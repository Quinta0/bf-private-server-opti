"""Wire test for Classic Raid round 2, band "camp_items" (docs/features/RAIDS_ROUND2_CAMP_ITEMS.md 8).

Two fake devices.  A stores a raid item set with RoonItemEdit, then plays a
REAL quick-start run of mission 2000 (QuickStart + MissionStart, exactly what
raid_field_test.py does; no fixture rows for the run itself) and exercises the
in-field RaidItemEdit / RaidItemMix / RaidItemDelete, camp rest (start, poll,
end) and RaidShopUse 101 (lives).  B is the outsider whose camp poll of A's run
is refused, and the RaidShopUse 100 (raid orbs) buyer.

State the wire cannot cheaply produce is written to --db directly: A's
warehouse (user_items), the synthesis facility level, the mission bag
(raid_mission_bag), unit HP (raid_mission_party), the camp clock
(raid_mission_user.healed_until, to skip 12 s of waiting), gems
(user_info.gems) and raid orbs (raid_user.aube).  Lives reach 0 through three
real wipes, not a DB write.

Usage: python -I raid_camp_items_test.py --url http://127.0.0.1:9965 --db <worktree>/deploy/gme.sqlite
Needs pycryptodome and the shared helpers in tools/ (found the way
raid_entry_test.py does).  Creates throwaway accounts and edits rows directly:
back up deploy/gme.sqlite first.
"""
import argparse
import math
import sqlite3
import sys
import time
from pathlib import Path

# -I leaves the script's own directory off sys.path; the two raid tests next to
# this one carry Player / Run and find tools/ themselves.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from raid_entry_test import (Player, RAID_AGREEMENT, RAID_ITEM_EDIT as ROON_ITEM_EDIT,  # noqa: E402
                             RAID_USER_INFO, user_info)
from raid_field_test import RAID_MISSION_EXIT, Run, first, mine, refused  # noqa: E402

# Request ids: the REGISTER list of CAMP_ITEMS 7.2.
CAMP_START = ('I7nVUv0h', 'qy27SYu9')
CAMP_GET = ('diIe8ux7', 'G5uind2r')
CAMP_END = ('f9Dh4djx', '0TCX6S4R')
ITEM_EDIT = ('DZLI83hx', '3Lxpf5KE')
ITEM_MIX = ('K17JUYR2', 'c6wBM9sH')
ITEM_DELETE = ('N43JmIf2', 'jvm3q24f')
SHOP_USE = ('4zJ0WmTX', 'RSvU7dh6')

# Master data this script relies on, checked against the checkout's deploy/mst/:
#   recipe_mst.json            1037 -> 70000 Beacon from "11016:1,10610:1";
#                              1038 -> 70100 Smoke Bomb from "11016:1,10309:1";
#                              1010 -> 20001 from "10001:1,10302:1,20000:1"
#   town_facility_lv_mst.json  facility 2 (Synthesis, no unlock mission) lv 1 releases
#                              1009/1037/1038; 1010 only at lv 14
#   item_mst.json              20000 Cure, 20200 Antidote: max_stack 99, max_equipped 10;
#                              70000 Beacon item_type 1; 11016/10610/10309/10000 materials, stack 999
#   raid_mission_mst.json      mission 2000 plays map 1 (H9sx5D08)
#   raid_point_mst.json        map 1: point 1 "Camp" (AZFM14In 1), point 2 is not a camp
#   raid_world_mst.json        camp tick Rqc8pi0x = 4 s
MISSION = '2000'
CAMP_POINT, ROUTE_POINT = 1, 2
NO_CAMP_POINT = 4   # mission 2000: routes from 4 lead to 2/3/4 only, never the camp
CURE, ANTIDOTE = 20000, 20200
LOCKED_MATERIALS = (10001, 10302)   # recipe 1010's materials besides a Cure
STONE, EYE, SMOKY_DROP, GRASS = 11016, 10610, 10309, 10000
BEACON, SMOKE_BOMB = 70000, 70100
SYNTHESIS_FACILITY = 2

CAMP_TICK = 4            # RaidWorldMst Rqc8pi0x
HEAL_PERCENT = 10        # kCampHealPercentPerTick (3.5)
CAMP_MAX_HP, CAMP_LOW_HP = 1000, 100
BAG_CAP = 50             # DefineMst raid_bag_cnt
MAX_LIVES = 3            # DefineMst raid_max_p
SERVER_PRICE = 1         # kRaidAubeRefillGems / kRaidLivesRefillGems (5.2)
SHOP_AUBE, SHOP_LIVES, SHOP_UNUSED = 100, 101, 102
RETRY = 2                # GmeErrorCommand::Retry (packet-generator/assets/net/gme.kdl)


class Db:
    """The server's SQLite file.  Autocommit, so no lock is held between steps."""

    def __init__(self, path: str):
        self.conn = sqlite3.connect(path, timeout=10, isolation_level=None)

    def rows(self, sql: str, *args) -> list:
        return self.conn.execute(sql, args).fetchall()

    def value(self, sql: str, *args, default=None):
        rows = self.rows(sql, *args)
        return rows[0][0] if rows else default

    def write(self, *statements) -> None:
        """(sql, args) pairs applied in one transaction."""
        self.conn.execute('BEGIN IMMEDIATE')
        try:
            for sql, args in statements:
                self.conn.execute(sql, args)
            self.conn.execute('COMMIT')
        except Exception:
            self.conn.execute('ROLLBACK')
            raise

    # Account state.
    def warehouse(self, uid: str, item: int) -> int:
        return self.value('SELECT item_num FROM user_items WHERE user_id = ? AND item_id = ?', uid, item, default=0)

    def all_items(self, uid: str) -> dict:
        return dict(self.rows('SELECT item_id, item_num FROM user_items WHERE user_id = ?', uid))

    def set_warehouse(self, uid: str, counts: dict) -> None:
        self.write(*[('INSERT INTO user_items(user_id, item_id, item_num) VALUES (?, ?, ?)'
                      ' ON CONFLICT(user_id, item_id) DO UPDATE SET item_num = excluded.item_num', (uid, item, num))
                     for item, num in counts.items()])

    def equip_rows(self, uid: str, kind: int) -> list:
        return self.rows('SELECT disp_order, item_id, item_num FROM raid_equip_items WHERE user_id = ? AND kind = ?'
                         ' ORDER BY disp_order', uid, kind)

    def gems(self, uid: str) -> int:
        return self.value('SELECT gems FROM user_info WHERE id = ?', uid)

    def set_gems(self, uid: str, gems: int) -> None:
        self.write(('UPDATE user_info SET gems = ? WHERE id = ?', (gems, uid)))

    def aube(self, uid: str) -> tuple:
        rows = self.rows('SELECT aube, aube_ts FROM raid_user WHERE user_id = ?', uid)
        return tuple(rows[0]) if rows else (None, None)

    def set_aube(self, uid: str, aube: int) -> None:
        # aube_ts = now: the lazy regen (Raids.cpp deriveAube) adds nothing for an hour.
        self.write(('UPDATE raid_user SET aube = ?, aube_ts = ? WHERE user_id = ?', (aube, int(time.time()), uid)))

    # Run state.
    def bag(self, rm: int, uid: str) -> dict:
        """frame -> (item, num) of the stacks with num > 0."""
        rows = self.rows('SELECT frame_id, CAST(item_id AS INTEGER), num FROM raid_mission_bag'
                         ' WHERE room_mission_id = ? AND user_id = ? AND num > 0', rm, uid)
        return {frame: (item, num) for frame, item, num in rows}

    def add_to_bag(self, rm: int, uid: str, stacks: list, replace: bool = False) -> None:
        """Append (item, num) stacks at max(frame_id) + 1, ... (the server's frame rule, 4.1)."""
        statements = [('DELETE FROM raid_mission_bag WHERE room_mission_id = ? AND user_id = ?', (rm, uid))] if replace else []
        top = 0 if replace else self.value('SELECT COALESCE(MAX(frame_id), 0) FROM raid_mission_bag'
                                           ' WHERE room_mission_id = ? AND user_id = ?', rm, uid)
        statements += [('INSERT INTO raid_mission_bag(room_mission_id, user_id, frame_id, item_id, num) VALUES (?, ?, ?, ?, ?)',
                        (rm, uid, top + i, str(item), num)) for i, (item, num) in enumerate(stacks, start=1)]
        self.write(*statements)

    def member(self, rm: int, uid: str) -> dict:
        rows = self.conn.execute('SELECT camp_flg, camp_since, healed_until, player_rest, now_point FROM raid_mission_user'
                                 ' WHERE room_mission_id = ? AND user_id = ?', (rm, uid))
        names = [d[0] for d in rows.description]
        found = rows.fetchall()
        return dict(zip(names, found[0])) if found else {}

    def party_hp(self, rm: int, uid: str) -> list:
        return self.rows('SELECT disp_order, now_hp, max_hp FROM raid_mission_party'
                         ' WHERE room_mission_id = ? AND user_id = ? ORDER BY disp_order', rm, uid)


def bag_totals(bag: dict) -> dict:
    totals = {}
    for item, num in bag.values():
        totals[item] = totals.get(item, 0) + num
    return totals


def frames_of(bag: dict, item: int) -> dict:
    return {frame: num for frame, (it, num) in bag.items() if it == item}


def reply_bag(reply: dict) -> dict:
    return {int(r.get('n6E8iMf3', -1)): (int(r.get('kixHbe54', -1)), int(r.get('wgV86x1q', 0)))
            for r in reply.get('UsDq1E6o', []) if int(r.get('wgV86x1q', 0)) > 0}


def reply_set(reply: dict, group: str) -> list:
    return sorted((int(r.get('XuJL4pc5', -1)), int(r.get('kixHbe54', -1)), int(r.get('wgV86x1q', -1)))
                  for r in reply.get(group, []))


def my_party(reply: dict, uid: str) -> list:
    return [r for r in reply.get('wc2DiMY1', []) if r.get('h7eY3sAK') == uid]


def retry_refused(reply: dict) -> bool:
    """A refuseToRetry (GmeErrorCommand::Retry), the only refusal this band uses (CAMP_ITEMS 2)."""
    error = reply.get('_unencrypted', {}).get('b5PH6mZa') or {}
    return int(error.get('iPD12YCr', -1)) == RETRY


def slots(rows: list) -> list:
    """(slot, item, num) -> 71U5wzhI / Fihk73ZR / GvV1w4yM nodes; ints as addParam(const char*, int) writes them."""
    return [{'kixHbe54': str(item), 'XuJL4pc5': slot, 'wgV86x1q': num} for slot, item, num in rows]


def healed(hp: int, since: int, now: int) -> int:
    """The 3.5 heal rule for one unit at CAMP_MAX_HP."""
    per_tick = math.ceil(CAMP_MAX_HP * HEAL_PERCENT / 100)
    return min(CAMP_MAX_HP, hp + max(0, now - since) // CAMP_TICK * per_tick)


class Test:
    def __init__(self, url: str, db_path: str):
        self.db = Db(db_path)
        self.failures = 0
        self.a, self.b = Player(url, 'A'), Player(url, 'B')
        for p in (self.a, self.b):
            p.call(RAID_AGREEMENT)
        self.run = None

    def check(self, label: str, ok: bool, detail='') -> bool:
        self.failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail)[:600] if detail and not ok else ''}")
        return ok

    @property
    def rm(self) -> int:
        return int(self.run.rm or 0)

    def bag(self) -> dict:
        return self.db.bag(self.rm, self.a.user_id)

    def field(self, request: tuple, player=None, rm=None, **groups) -> dict:
        """A request carrying the run's room and room mission nodes (CAMP_ITEMS 3.2)."""
        nodes = self.run.nodes()
        if rm is not None:
            nodes['g97TWISb'] = [{'ja39HMLq': rm}]
        return (player or self.a).call(request, **nodes, **groups)

    def shop(self, player, shop_type: int, cost: int, nodes: dict) -> dict:
        return player.call(SHOP_USE, **nodes, **{'32ibWjFG': [{'60IsqxDt': shop_type, '03UGMHxF': cost}]})


def step_room_items(t: Test) -> None:
    """1-2: RoonItemEdit moves the set + reserve out of / back into user_items (4.5)."""
    a, db, uid = t.a, t.db, t.a.user_id

    def edit(equip: list, reserve: list) -> dict:
        return a.call(ROON_ITEM_EDIT, Fihk73ZR=slots(equip), GvV1w4yM=slots(reserve))

    db.set_warehouse(uid, {CURE: 5, ANTIDOTE: 3})
    r = edit([(0, CURE, 3)], [(0, ANTIDOTE, 2)])
    t.check('1a RoonItemEdit acknowledged', not refused(r), r)
    t.check('1b set + reserve left the warehouse (Cure 2, Antidote 1)',
            (db.warehouse(uid, CURE), db.warehouse(uid, ANTIDOTE)) == (2, 1), db.all_items(uid))
    t.check('1c raid_equip_items holds the set (kind 0) and the reserve (kind 1)',
            db.equip_rows(uid, 0) == [(0, CURE, 3)] and db.equip_rows(uid, 1) == [(0, ANTIDOTE, 2)],
            (db.equip_rows(uid, 0), db.equip_rows(uid, 1)))
    r = a.call(RAID_USER_INFO)
    t.check('2 RaidUserInfo carries Fihk73ZR and GvV1w4yM', reply_set(r, 'Fihk73ZR') == [(0, CURE, 3)]
            and reply_set(r, 'GvV1w4yM') == [(0, ANTIDOTE, 2)], (r.get('Fihk73ZR'), r.get('GvV1w4yM')))
    edit([], [])
    t.check('1d emptying set and reserve restores the warehouse (5 / 3)',
            (db.warehouse(uid, CURE), db.warehouse(uid, ANTIDOTE)) == (5, 3)
            and db.equip_rows(uid, 0) == [] and db.equip_rows(uid, 1) == [], db.all_items(uid))
    r = edit([(0, CURE, 6)], [])
    t.check('1e over-count (6 Cure, 5 owned): acknowledged, nothing changed', not refused(r)
            and db.warehouse(uid, CURE) == 5 and db.equip_rows(uid, 0) == [], (r, db.equip_rows(uid, 0)))
    edit([(0, CURE, 3)], [])
    t.check('1f A carries 3 Cure into the run (warehouse 2)',
            db.warehouse(uid, CURE) == 2 and db.equip_rows(uid, 0) == [(0, CURE, 3)], db.equip_rows(uid, 0))


def step_open_run(t: Test) -> bool:
    """3: a real run, then the spec's bag seeded: 11016 x2, 10610, 10309, Cure as two stacks (3 at frame 4, 2 at frame 5)."""
    uid = t.a.user_id
    t.run = Run(t.a, MISSION)
    r = t.run.start()
    ok = t.check('3a QuickStart + MissionStart opened a run, A at the camp point',
                 t.run.rm not in ('', '0') and mine(r, '81B4TvEK', uid).get('pjX27bKB') == str(CAMP_POINT),
                 (t.run.rm, mine(r, '81B4TvEK', uid)))
    if not ok:
        return False
    t.db.add_to_bag(t.rm, uid, [(STONE, 2), (EYE, 1), (SMOKY_DROP, 1), (CURE, 3), (CURE, 2)], replace=True)
    return t.check('3b bag seeded', len(t.bag()) == 5, t.bag())


def step_field_edit(t: Test) -> None:
    """4-6: RaidItemEdit trades the set with the bag only, all or nothing, never refuses (4.2)."""
    db, uid = t.db, t.a.user_id
    warehouse = db.warehouse(uid, CURE)
    r = t.field(ITEM_EDIT, **{'71U5wzhI': slots([(0, CURE, 5)])})
    bag = t.bag()
    t.check('4a RaidItemEdit replies Fihk73ZR + UsDq1E6o', not refused(r) and 'Fihk73ZR' in r and 'UsDq1E6o' in r, r)
    t.check('4b set slot 0 = 5 Cure (reply and DB)', reply_set(r, 'Fihk73ZR') == [(0, CURE, 5)]
            and db.equip_rows(uid, 0) == [(0, CURE, 5)], (r.get('Fihk73ZR'), db.equip_rows(uid, 0)))
    t.check('4c the 2 more Cure came from the smallest stack (frame 5 emptied, frame 4 keeps 3)',
            frames_of(bag, CURE) == {4: 3}, bag)
    t.check('4d reply bag = server bag', reply_bag(r) == bag, (reply_bag(r), bag))

    r = t.field(ITEM_EDIT, **{'71U5wzhI': []})
    bag = t.bag()
    t.check('5a empty set: 5 Cure back into the bag, topped up onto frame 4 (8)',
            not refused(r) and frames_of(bag, CURE) == {4: 8} and db.equip_rows(uid, 0) == [], bag)
    t.check('5b reply bag = server bag', reply_bag(r) == bag, (reply_bag(r), bag))
    t.check('5c in-field edits leave user_items alone', db.warehouse(uid, CURE) == warehouse,
            (warehouse, db.warehouse(uid, CURE)))

    r = t.field(ITEM_EDIT, **{'71U5wzhI': slots([(0, CURE, 2), (1, ANTIDOTE, 9)])})
    t.check('6 set needing 9 Antidote the bag lacks: success, nothing changed (not even the Cure part)',
            not refused(r) and t.bag() == bag and db.equip_rows(uid, 0) == [] and reply_bag(r) == bag
            and reply_set(r, 'Fihk73ZR') == [], (r, t.bag(), db.equip_rows(uid, 0)))


def step_mix(t: Test) -> None:
    """7-8: RaidItemMix crafts from bag materials only: 1 product per craft, no karma, no user_items (4.3)."""
    db, uid = t.db, t.a.user_id
    db.write(('INSERT INTO user_town_facilities(user_id, facility_id, lv) VALUES (?, ?, 1)'
              ' ON CONFLICT(user_id, facility_id) DO UPDATE SET lv = 1', (uid, SYNTHESIS_FACILITY)))
    karma, items = db.value('SELECT karma FROM user_info WHERE id = ?', uid), db.all_items(uid)

    r = t.field(ITEM_MIX, JTf2jY5o=[{'4HqhTf3a': '1037:1,1038:1'}])
    totals = bag_totals(t.bag())
    t.check('7a mix 1037 + 1038: materials gone, +1 Beacon, +1 Smoke Bomb', not refused(r)
            and all(totals.get(i, 0) == 0 for i in (STONE, EYE, SMOKY_DROP))
            and totals.get(BEACON) == 1 and totals.get(SMOKE_BOMB) == 1 and totals.get(CURE) == 8, totals)
    t.check('7b reply bag = server bag', reply_bag(r) == t.bag(), (reply_bag(r), t.bag()))

    db.add_to_bag(t.rm, uid, [(STONE, 1), (EYE, 1)])
    r = t.field(ITEM_MIX, JTf2jY5o=[{'4HqhTf3a': '1037:5'}])
    totals = bag_totals(t.bag())
    t.check('7c "1037:5" with materials for one crafts exactly one', not refused(r) and totals.get(BEACON) == 2
            and totals.get(STONE, 0) == 0 and totals.get(EYE, 0) == 0, totals)

    # Materials for 1037 and for the locked 1010 are on hand, so a misread
    # order would craft something.
    db.add_to_bag(t.rm, uid, [(STONE, 1), (EYE, 1)] + [(item, 1) for item in LOCKED_MATERIALS])
    for label, csv in (('unknown recipe', '999999:1'), ('malformed list', '1037'),
                       ('recipe locked (1010 needs Synthesis lv 14)', '1010:1')):
        before = t.bag()
        r = t.field(ITEM_MIX, JTf2jY5o=[{'4HqhTf3a': csv}])
        t.check(f'7d {label}: success, bag unchanged', not refused(r) and t.bag() == before, (r, t.bag()))
    t.check('7e karma and user_items untouched by raid mixing',
            db.value('SELECT karma FROM user_info WHERE id = ?', uid) == karma and db.all_items(uid) == items)

    db.add_to_bag(t.rm, uid, [(GRASS, 1)] * (BAG_CAP - len(t.bag())))
    before = t.bag()
    r = t.field(ITEM_MIX, JTf2jY5o=[{'4HqhTf3a': '1037:1'}])
    t.check('8 bag at 50 active stacks: mix crafts nothing', not refused(r) and len(before) == BAG_CAP
            and t.bag() == before, (len(before), bag_totals(t.bag())))
    db.write(('DELETE FROM raid_mission_bag WHERE room_mission_id = ? AND user_id = ? AND CAST(item_id AS INTEGER) = ?',
              (t.rm, uid, GRASS)))


def step_delete(t: Test) -> None:
    """9: RaidItemDelete (dead in the client, registered anyway) never refuses (4.4)."""
    beacons = frames_of(t.bag(), BEACON)
    if not t.check('9 a Beacon stack to delete', len(beacons) == 1, t.bag()):
        return
    frame, num = next(iter(beacons.items()))
    r = t.a.call(ITEM_DELETE, qa3iN58C=[{'n6E8iMf3': str(frame), 'wgV86x1q': num}])
    bag = t.bag()
    t.check('9a RaidItemDelete of the Beacon frame: stack gone, reply = server bag', not refused(r)
            and BEACON not in bag_totals(bag) and reply_bag(r) == bag, (r, bag))
    r = t.a.call(ITEM_DELETE, qa3iN58C=[{'n6E8iMf3': '999999', 'wgV86x1q': 1}])
    t.check('9b unknown frame: success, nothing changed', not refused(r) and t.bag() == bag, r)


def unit_hp(t: Test) -> int:
    """A's one now_hp when every unit has the same, else -1."""
    hps = {now for _, now, _ in t.db.party_hp(t.rm, t.a.user_id)}
    return next(iter(hps)) if len(hps) == 1 else -1


def rewind_camp_clock(t: Test, seconds: int) -> int:
    since = int(time.time()) - seconds
    t.db.write(('UPDATE raid_mission_user SET healed_until = ? WHERE room_mission_id = ? AND user_id = ?',
                (since, t.rm, t.a.user_id)))
    return since


def step_camp_start(t: Test) -> int:
    """10a-c: rest at the camp point (3.4).  Returns A's unit count (0: nothing to heal, stop)."""
    db, uid, rm = t.db, t.a.user_id, t.rm
    db.write(('UPDATE raid_mission_party SET max_hp = ?, now_hp = ? WHERE room_mission_id = ? AND user_id = ?',
              (CAMP_MAX_HP, CAMP_LOW_HP, rm, uid)))
    units = len(db.party_hp(rm, uid))
    r = t.field(CAMP_START)
    me = db.member(rm, uid)
    t.check('10a CampRestStart at point 1: camp_flg 1, clock started', not refused(r) and me.get('camp_flg') == 1
            and me.get('camp_since', 0) > 0 and me.get('healed_until', 0) > 0, (r, me))
    t.check("10b reply: A's 81B4TvEK row has MFz8YRS6 1, wc2DiMY1 has A's units", units > 0
            and mine(r, '81B4TvEK', uid).get('MFz8YRS6') == '1' and len(my_party(r, uid)) == units,
            (mine(r, '81B4TvEK', uid), r.get('wc2DiMY1')))
    since = rewind_camp_clock(t, 3 * CAMP_TICK)
    r = t.field(CAMP_START)
    t.check('10c CampRestStart while camping keeps the clock', not refused(r)
            and db.member(rm, uid).get('healed_until') == since, db.member(rm, uid))
    return units


def step_camp_poll(t: Test, units: int) -> None:
    """10d-h: CampRestGet applies the lazy heal, ceil(10 % of max) per whole 4 s tick (3.5)."""
    db, uid, rm = t.db, t.a.user_id, t.rm
    since = db.member(rm, uid).get('healed_until', 0)
    before = int(time.time())
    r = t.field(CAMP_GET)
    after = int(time.time())
    hp = unit_hp(t)
    t.check(f'10d CampRestGet: 3 ticks x {HEAL_PERCENT} % -> now_hp {healed(CAMP_LOW_HP, since, since + 12)} in the DB',
            not refused(r) and healed(CAMP_LOW_HP, since, before) <= hp <= healed(CAMP_LOW_HP, since, after),
            (r if refused(r) else '', db.party_hp(rm, uid)))
    t.check('10e reply wc2DiMY1 shows the healed HP',
            [int(x.get('e7DK0FQT', -1)) for x in my_party(r, uid)] == [hp] * units, my_party(r, uid))
    ticks = (hp - CAMP_LOW_HP) // math.ceil(CAMP_MAX_HP * HEAL_PERCENT / 100)
    t.check('10f healed_until advanced by whole ticks only',
            db.member(rm, uid).get('healed_until') == since + ticks * CAMP_TICK, (since, ticks, db.member(rm, uid)))
    mission = first(r, 'g97TWISb')
    t.check('10g poll carries the mission payload (81B4TvEK, full g97TWISb)', bool(mine(r, '81B4TvEK', uid))
            and mission.get('3tWuc7de') == '1' and '8n7oG4fS' in mission, (list(r), mission))
    r = t.field(CAMP_GET, rm='0')
    t.check('10h CampRestGet with ja39HMLq "0" means the live run (3.2)', not refused(r)
            and len(my_party(r, uid)) == units, r)


def step_camp_leave(t: Test, units: int) -> None:
    """12 while A camps (an outsider's poll must not heal or touch A), then 10i-j CampRestEnd (3.1, 3.4)."""
    db, uid, rm = t.db, t.a.user_id, t.rm
    since = rewind_camp_clock(t, 3 * CAMP_TICK)
    hp, snapshot = unit_hp(t), (db.member(rm, uid), db.party_hp(rm, uid))
    r = t.field(CAMP_GET, player=t.b)
    t.check("12 B polling CampRestGet for A's run: Retry refusal, A untouched", retry_refused(r)
            and (db.member(rm, uid), db.party_hp(rm, uid)) == snapshot, r)

    before = int(time.time())
    r = t.field(CAMP_END)
    after = int(time.time())
    end_hp = unit_hp(t)
    t.check('10i CampRestEnd: final heal applied, camp_flg 0, MFz8YRS6 "0", wc2DiMY1 sent', not refused(r)
            and healed(hp, since, before) <= end_hp <= healed(hp, since, after)
            and db.member(rm, uid).get('camp_flg') == 0
            and mine(r, '81B4TvEK', uid).get('MFz8YRS6') == '0' and len(my_party(r, uid)) == units,
            (r if refused(r) else mine(r, '81B4TvEK', uid), hp, end_hp))
    r = t.field(CAMP_END)
    t.check('10j CampRestEnd when not camping: same reply, still 0', not refused(r)
            and mine(r, '81B4TvEK', uid).get('MFz8YRS6') == '0' and db.member(rm, uid).get('camp_flg') == 0, r)


def step_not_camp(t: Test) -> None:
    """11: Rest one route step from the camp catches up the walk; with no route to a camp it is refused."""
    uid = t.a.user_id
    r = t.run.info(ROUTE_POINT)
    moved = mine(r, '81B4TvEK', uid).get('pjX27bKB') == str(ROUTE_POINT)
    # The Rest button is drawn on arrival, before the poll that reports the
    # walk (as Harvest): from point 2 the camp (point 1) is one route step.
    r = t.field(CAMP_START)
    member = t.db.member(t.rm, uid)
    t.check('11a CampRestStart from point 2 (route 2 -> 1): walk caught up, camping at point 1', moved
            and not refused(r) and member.get('camp_flg') == 1 and member.get('now_point') == 1, (r, member))
    t.field(CAMP_END)
    t.run.info(ROUTE_POINT)
    r = t.run.info(NO_CAMP_POINT)
    at_far = mine(r, '81B4TvEK', uid).get('pjX27bKB') == str(NO_CAMP_POINT)
    r = t.field(CAMP_START)
    t.check('11b CampRestStart at point 4 (no route to a camp): Retry refusal, camp_flg stays 0', at_far
            and retry_refused(r) and t.db.member(t.rm, uid).get('camp_flg') == 0, (at_far, r))
    t.run.info(ROUTE_POINT)   # back on the route battle point for step 14


def step_shop_aube(t: Test) -> None:
    """13: RaidShopUse 100 refills raid orbs for the server's price; B is in no room (5.2)."""
    b, db, uid = t.b, t.db, t.b.user_id
    nodes = {'8iBuZa3n': [{'43hMuY2I': '1'}], 'g97TWISb': [{}]}   # 5.1: no room, empty mission node
    db.set_aube(uid, 0)
    db.set_gems(uid, 3)
    r = t.shop(b, SHOP_AUBE, SERVER_PRICE, nodes)
    t.check('13a aube 0, 3 gems: aube 3 (aube_ts 0), gems 2', not refused(r)
            and db.aube(uid) == (MAX_LIVES, 0) and db.gems(uid) == 3 - SERVER_PRICE, (r, db.aube(uid), db.gems(uid)))
    t.check('13b reply: fEi17cnx with the new balance, full 1ry6BKoe with aube 3',
            int(first(r, 'fEi17cnx').get('03UGMHxF', -1)) == db.gems(uid)
            and user_info(r).get('C45mRxnZ') == '3' and 'S92Hcor3' in user_info(r), (first(r, 'fEi17cnx'), user_info(r)))
    r = t.shop(b, SHOP_AUBE, SERVER_PRICE, nodes)
    t.check('13c again with aube 3: no charge', db.gems(uid) == 3 - SERVER_PRICE and db.aube(uid)[0] == MAX_LIVES,
            (r, db.gems(uid)))
    db.set_aube(uid, 0)
    db.set_gems(uid, 0)
    r = t.shop(b, SHOP_AUBE, SERVER_PRICE, nodes)
    t.check('13d 0 gems, aube 0: Retry refusal, nothing changed', retry_refused(r)
            and db.aube(uid)[0] == 0 and db.gems(uid) == 0, (r, db.aube(uid)))
    db.set_gems(uid, 3)
    r = t.shop(b, SHOP_AUBE, 99, nodes)
    t.check('13e claimed cost 99: charged the server price', not refused(r) and db.gems(uid) == 3 - SERVER_PRICE
            and db.aube(uid)[0] == MAX_LIVES, (r, db.gems(uid), db.aube(uid)))


def wipe(t: Test) -> str:
    """One real route battle at point 2 lost with the whole party at 0 HP (costs a life)."""
    t.run.battle_start(ROUTE_POINT)
    return mine(t.run.battle_end([], hp=0), '81B4TvEK', t.a.user_id).get('fP1r39bd', '')


def step_shop_lives(t: Test) -> None:
    """14: RaidShopUse 101 refills lives inside the run for the server's price (5.2)."""
    db, uid, rm = t.db, t.a.user_id, t.rm
    lives = [wipe(t) for _ in range(MAX_LIVES)]
    t.check('14a three real wipes spend every life', lives == ['2', '1', '0']
            and db.member(rm, uid).get('player_rest') == 0, (lives, db.member(rm, uid)))
    # CORE 8: BattleStart has no heart check (a refusal on the boss's point
    # looped); the field's game-over check sends the player to the heart shop.
    t.check('14b no lives: the field reports 0 (fP1r39bd)', lives[-1] == '0')
    db.set_gems(uid, 5)
    r = t.shop(t.a, SHOP_LIVES, 99, t.run.nodes())
    t.check('14c 0 lives: +1 life ("1 Gem to recover 1 Life"), charged the server price (claim 99)', not refused(r)
            and db.member(rm, uid).get('player_rest') == 1 and db.gems(uid) == 5 - SERVER_PRICE,
            (r, db.member(rm, uid), db.gems(uid)))
    t.check("14d reply: fEi17cnx and A's 81B4TvEK row with fP1r39bd 1", 'fEi17cnx' in r
            and mine(r, '81B4TvEK', uid).get('fP1r39bd') == '1', mine(r, '81B4TvEK', uid))
    for _ in range(MAX_LIVES - 1):
        t.shop(t.a, SHOP_LIVES, SERVER_PRICE, t.run.nodes())
    r = t.shop(t.a, SHOP_LIVES, SERVER_PRICE, t.run.nodes())
    t.check('14e two more buys reach 3; at 3 lives no charge', db.gems(uid) == 5 - MAX_LIVES * SERVER_PRICE
            and db.member(rm, uid).get('player_rest') == MAX_LIVES, (r, db.gems(uid)))
    db.set_gems(uid, 0)
    wipe(t)
    r = t.shop(t.a, SHOP_LIVES, SERVER_PRICE, t.run.nodes())
    t.check('14f 2 lives, 0 gems: Retry refusal, nothing changed', retry_refused(r)
            and db.member(rm, uid).get('player_rest') == MAX_LIVES - 1 and db.gems(uid) == 0, (r, db.member(rm, uid)))
    db.set_gems(uid, 3)
    r = t.shop(t.a, SHOP_UNUSED, SERVER_PRICE, t.run.nodes())
    t.check('14g type 102: fEi17cnx only, no charge', not refused(r) and 'fEi17cnx' in r
            and db.gems(uid) == 3 and db.member(rm, uid).get('player_rest') == MAX_LIVES - 1, (r, db.gems(uid)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9965')
    ap.add_argument('--db', default='/deploy/gme.sqlite')
    args = ap.parse_args()

    t = Test(args.url, args.db)
    step_room_items(t)
    if step_open_run(t):
        step_field_edit(t)
        step_mix(t)
        step_delete(t)
        units = step_camp_start(t)
        if units:
            step_camp_poll(t, units)
            step_camp_leave(t, units)
        step_not_camp(t)
        step_shop_lives(t)
        t.a.call(RAID_MISSION_EXIT, **t.run.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])
    step_shop_aube(t)

    print(f'{t.failures} failure(s)')
    return 1 if t.failures else 0


if __name__ == '__main__':
    sys.exit(main())
