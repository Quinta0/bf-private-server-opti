"""Wire test for Classic Raid round 2, band "core" (docs/features/RAIDS_ROUND2_CORE.md 6.5).

Fake devices play solo quick-start raids end to end: MissionStart (new and
resume), walking, route and boss battles with per-turn events, wipes,
harvest, item use, mission end with rewards and RC-up, retire, time-out, and
the room-path MissionSelect / RaidEnter refusals.

Usage: python -I raid_field_test.py --url http://127.0.0.1:9965 [--db /deploy/gme.sqlite]
Needs pycryptodome and the shared helpers in tools/ (mounted at /tools).
Creates throwaway accounts and edits run rows directly for the time-out and
teleport steps: back up deploy/gme.sqlite first.
"""
import argparse
import sqlite3
import sys
import time
from pathlib import Path

# -I leaves the script's own directory off sys.path; the round-1 test next to
# this one carries the Player helper (and finds tools/ itself).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from raid_entry_test import (Player, RAID_AGREEMENT, RAID_QUICK_START, RAID_ROOM_INFO,  # noqa: E402
                             RAID_USER_INFO, user_info)

RAID_MISSION_START = ('2qDW9NBU', 'kY5y1zBj')
RAID_MISSION_INFO = ('BqF2x3EC', '6SyFs0Du')
RAID_MISSION_INFO_LIGHT = ('Argz1U6B', '1hJrYxs7')
RAID_BATTLE_START = ('8UiQj0uF', '1Jhavqb7')
RAID_BATTLE_END = ('5Ko2BjPA', 'gi6B0m2p')
RAID_BATTLE_RESTART = ('La24KGAp', 'ecJRzX16')
RAID_UPDATE_PROGRESS = ('7geDL5Vb', '7DkVMG8Y')
RAID_HARVEST = ('9QdRVwB1', 'b1ScyFv9')
RAID_USE_ITEM = ('o03kCtb1', 'Ys8ro9Sq')
RAID_LIMITED_USE_ITEM = ('SaN7v13E', 'btR9Jkw7')
RAID_MISSION_END = ('vgin9Mf1', '95ULkowE')
RAID_MISSION_EXIT = ('a12v0RcA', '14GMJFIN')
RAID_MISSION_SELECT = ('8iQL3IKA', 'ZBex5ju9')
RAID_ENTER = ('yZReG75i', 'N0Azw6ob')

UNIT_HP = 3000


def party(uid: str, hp: int = UNIT_HP, start: bool = False) -> list:
    rows = []
    for order in range(4):
        row = {'edy7fq3L': str(100 + order), 'pn16CNah': '10017', 'e7DK0FQT': str(hp),
               'XuJL4pc5': str(order), 'gr48vsdJ': '0' if order == 0 else '1', '4A6LzBxr': '30',
               '2pAyFjmZ': '0', 'nBTx56W9': '1'}
        if start:
            row['PWXu25cg'] = '500'
        else:
            row.update({'h7eY3sAK': uid, 'gU2xtQ0V': '5', 'nIGZ1X9C': '0', '98WfKiyA': ''})
        rows.append(row)
    return rows


def first(reply: dict, group: str) -> dict:
    rows = reply.get(group) or [{}]
    return rows[0] if isinstance(rows, list) else rows


def mine(reply: dict, group: str, uid: str) -> dict:
    return next((r for r in reply.get(group, []) if r.get('h7eY3sAK') == uid), {})


def refused(reply: dict) -> bool:
    return '_unencrypted' in reply


class Run:
    """One quick-start run of a player, with the request nodes it keeps sending."""

    def __init__(self, player: Player, mission: str):
        self.p, self.mission = player, mission
        r = player.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': mission}]})
        self.room = first(r, '8iBuZa3n').get('8VYd6xSX', '')
        self.rm = ''
        self.serial = ''
        self.event = 1

    def room_node(self, **extra) -> dict:
        return {'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': self.room, **extra}]}

    def nodes(self) -> dict:
        return {**self.room_node(), 'g97TWISb': [{'ja39HMLq': self.rm}]}

    def start(self) -> dict:
        r = self.p.call(RAID_MISSION_START, **self.room_node(**{'8ZK3w0Np': self.p.user_id}),
                        wc2DiMY1=party(self.p.user_id, start=True))
        self.rm = mine(r, 'uwH62EZy', self.p.user_id).get('FV6MU5nQ', self.rm)
        return r

    def info(self, point=None) -> dict:
        extra = {'pw38qdui': [{'NLKce95n': '0'}]}
        if point is not None:
            extra['81B4TvEK'] = [{'pjX27bKB': str(point)}]
        return self.p.call(RAID_MISSION_INFO, **self.nodes(), **extra)

    def battle_start(self, point: int, boss: str = '') -> dict:
        r = self.p.call(RAID_BATTLE_START, **self.room_node(),
                        **{'9Q1Lq5FS': [{'h7eY3sAK': self.p.user_id, 'j28VNcUW': self.mission}],
                           '2cBk5WH8': [{'ja39HMLq': self.rm, 'SP29fLtH': boss, 'qbJcuC60': str(point)}]})
        self.serial = first(r, 'yL3Yrf1M').get('1kisc6IF', '')
        return r

    def new_event(self, etype: int, params: str) -> dict:
        ev = {'NH65Wj0f': str(etype), 'o96QPNZn': str(self.event), 'xIB0C8PN': params}
        self.event += 1
        return ev

    def progress(self, events: list, hp: int = UNIT_HP, boss: str = '') -> dict:
        return self.p.call(RAID_UPDATE_PROGRESS, **self.nodes(),
                           yL3Yrf1M=[{'1kisc6IF': self.serial, 'SP29fLtH': boss}],
                           QP4cwt7n=events, wc2DiMY1=party(self.p.user_id, hp), pw38qdui=[{'NLKce95n': '0'}])

    def battle_end(self, events: list, hp: int = UNIT_HP, boss: str = '') -> dict:
        return self.p.call(RAID_BATTLE_END, **self.room_node(),
                           yL3Yrf1M=[{'1kisc6IF': self.serial, 'SP29fLtH': boss, 'x69GYheC': '0'}],
                           zS9s5bEI=[{'M4NAYyc6': '2', '6RxkwBv7': '', 'g5L3bKPu': '', '4T0Q2Bh5': ''}],
                           QP4cwt7n=events, wc2DiMY1=party(self.p.user_id, hp))

    def end(self) -> dict:
        return self.p.call(RAID_MISSION_END, **self.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9965')
    ap.add_argument('--db', default='/deploy/gme.sqlite')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail)[:600] if detail and not ok else ''}")

    db = sqlite3.connect(args.db, timeout=10)
    a, b = Player(args.url, 'A'), Player(args.url, 'B')
    for p in (a, b):
        p.call(RAID_AGREEMENT)
    uid = a.user_id

    # 1. QuickStart + MissionStart opens the run.
    run = Run(a, '2000')
    r = run.start()
    me = mine(r, 'uwH62EZy', uid)
    rm = first(r, 'g97TWISb')
    pos = mine(r, '81B4TvEK', uid)
    boss = first(r, 'Ts2zJ6gj')
    check('1a room flagged in mission', first(r, '8iBuZa3n').get('3tWuc7de') == '1', first(r, '8iBuZa3n'))
    check('1b member in mission with a room-mission id', me.get('j3g5P4cq') == '1' and me.get('FV6MU5nQ') not in ('', '0', None), me)
    check('1c run is mission 2000, running, ~1800 s', rm.get('j28VNcUW') == '2000' and rm.get('3tWuc7de') == '1'
          and 1790 <= int(rm.get('8n7oG4fS', 0)) <= 1800, rm)
    check('1d player at the camp (point 1), 3 hearts, no battle', pos.get('pjX27bKB') == '1' and pos.get('fP1r39bd') == '3'
          and pos.get('EqgF1H8M') == '0', pos)
    check('1e boss 1 at point 4, visible, full HP', boss.get('SP29fLtH') == '1' and boss.get('7Ex5mKRP') == '4'
          and boss.get('7z6dqvuc') == '1' and boss.get('4x7IS9Vg') == '40000', boss)
    check('1f party stored (4 rows, HP 3000)', len(r.get('wc2DiMY1', [])) == 4
          and all(x.get('e7DK0FQT') == '3000' and x.get('3WMz78t6') == '3000' for x in r['wc2DiMY1']), r.get('wc2DiMY1'))
    bag = r.get('UsDq1E6o', [])
    check('1g empty bag still sent: one blank row (overwrites the last run\'s bag)', len(bag) == 1
          and bag[0].get('kixHbe54') == '' and bag[0].get('wgV86x1q') == '0', bag)

    # 2. MissionStart again resumes the same run.
    first_rm = run.rm
    time.sleep(1.1)
    r = run.start()
    check('2 MissionStart again resumes (same run, clock running)', run.rm == first_rm
          and int(first(r, 'g97TWISb').get('8n7oG4fS', 0)) < 1800, first(r, 'g97TWISb'))

    # 3. Walking.
    check('3a walk 1 -> 2', mine(run.info(2), '81B4TvEK', uid).get('pjX27bKB') == '2')
    check('3b walk 2 -> 4', mine(run.info(4), '81B4TvEK', uid).get('pjX27bKB') == '4')
    check('3c point off the map (99) keeps point 4, no error', mine(run.info(99), '81B4TvEK', uid).get('pjX27bKB') == '4')
    # Two walks between polls: the client reports only the last point (tickets#125).
    check('3d 4 -> 1 in one report (2 hops via 2) is accepted', mine(run.info(1), '81B4TvEK', uid).get('pjX27bKB') == '1')
    check('3e walk 1 -> 2', mine(run.info(2), '81B4TvEK', uid).get('pjX27bKB') == '2')
    r = a.call(RAID_MISSION_INFO_LIGHT, **run.nodes())
    check('3e MissionInfoLight answers the field', first(r, 'g97TWISb').get('j28VNcUW') == '2000', list(r)[:8])

    # 4. Route battle at point 2.
    r = run.battle_start(2)
    groups = r.get('pj41dy9g', [])
    check('4a route battle content for raid mission 2000', len(groups) == 1 and groups[0].get('j28VNcUW') == '2000'
          and groups[0].get('VETu07N6') == '1' and len(r.get('U0v5IeJo', [])) >= 1 and len(r.get('75t0sx9z', [])) >= 1,
          groups)
    check('4b serial issued and in MissionNumInfo + yL3Yrf1M', run.serial not in ('', '0')
          and first(r, 'Kz7qfSs5').get('k9cxD7Ba', run.serial) == run.serial and first(r, 'yL3Yrf1M').get('Ez52fI60') == '1',
          (first(r, 'yL3Yrf1M'), first(r, 'Kz7qfSs5')))
    check('4c member shows the open battle', mine(r, '81B4TvEK', uid).get('EqgF1H8M') == run.serial)
    check('4d 9Q1Lq5FS echoed, 2cBk5WH8 says point 2', first(r, '9Q1Lq5FS').get('j28VNcUW') == '2000'
          and first(r, '2cBk5WH8').get('qbJcuC60') == '2', (first(r, '9Q1Lq5FS'), first(r, '2cBk5WH8')))
    check('4e no boss part HP on a route battle', 'Fx60pCaU' not in r)
    rb = b.call(RAID_BATTLE_START, **run.room_node(),
                **{'2cBk5WH8': [{'ja39HMLq': run.rm, 'SP29fLtH': '', 'qbJcuC60': '2'}]})
    check('4f B (not a member) cannot start a battle in A\'s room', refused(rb), rb)
    r = run.battle_start(99)
    check('4g battle at a point off the map (99) is refused', refused(r), r)
    r = run.battle_start(2)

    # 5. Events are acked and applied once; HP carries over.
    e1, e2 = run.new_event(2, '0:20000:1'), run.new_event(2, '0:20000:1')
    r = run.progress([e1, e2], hp=2500)
    acks = sorted(x.get('o96QPNZn') for x in r.get('QP4cwt7n', []))
    check('5a UpdateProgress acks events 1,2', acks == ['1', '2'], r.get('QP4cwt7n'))
    r = run.progress([e1, e2], hp=2400)
    check('5b re-sent events acked again', sorted(x.get('o96QPNZn') for x in r.get('QP4cwt7n', [])) == ['1', '2'])
    r = run.battle_end([], hp=2000)
    check('5c BattleEnd stores HP 2000 and closes the battle', all(x.get('e7DK0FQT') == '2000' for x in r.get('wc2DiMY1', []))
          and mine(r, '81B4TvEK', uid).get('EqgF1H8M') == '0' and mine(r, '81B4TvEK', uid).get('fP1r39bd') == '3',
          (r.get('wc2DiMY1'), mine(r, '81B4TvEK', uid)))
    r = run.battle_end([], hp=100)
    check('5d duplicate BattleEnd harmless (no open battle)', mine(r, '81B4TvEK', uid).get('fP1r39bd') == '3')
    r = run.battle_start(2)
    check('5e next battle starts with the stored HP and event id after the last', first(r, 'yL3Yrf1M').get('Ez52fI60') == '3'
          and all(x.get('e7DK0FQT') == '2000' for x in r.get('wc2DiMY1', [])), first(r, 'yL3Yrf1M'))

    # 6. Wipe costs a heart and restores the party.
    r = run.battle_end([], hp=0)
    me = mine(r, '81B4TvEK', uid)
    check('6 wipe: 2 hearts, party back to full', me.get('fP1r39bd') == '2'
          and all(x.get('e7DK0FQT') == '3000' for x in r.get('wc2DiMY1', [])), (me, r.get('wc2DiMY1')))

    # 11 (early). Harvest at point 2, three times, then refused.
    bag_sizes = []
    for _ in range(3):
        r = a.call(RAID_HARVEST, **run.nodes(), **{'81B4TvEK': [{'zHs17TE2': '2:1'}]})
        bag_sizes.append(sum(int(x.get('wgV86x1q', 0)) for x in r.get('UsDq1E6o', [])))
    check('11a three harvests fill the bag', bag_sizes == [1, 2, 3] and mine(r, '81B4TvEK', uid).get('zHs17TE2') == '2:3',
          (bag_sizes, mine(r, '81B4TvEK', uid)))
    r = a.call(RAID_HARVEST, **run.nodes(), **{'81B4TvEK': [{'zHs17TE2': '2:1'}]})
    check('11b fourth harvest refused', refused(r), r)
    # A point the player could have walked to counts as the walk (tickets#125);
    # one that is not on the map does not.
    r = a.call(RAID_HARVEST, **run.nodes(), **{'81B4TvEK': [{'zHs17TE2': '99:1'}]})
    check('11c harvest at a point off the map (99) refused', refused(r), r)

    # Item use from the bag.
    bag = run.info().get('UsDq1E6o', [])
    item = bag[0]['kixHbe54'] if bag else '0'
    before = sum(int(x['wgV86x1q']) for x in bag)
    r = a.call(RAID_USE_ITEM, **run.nodes(), igAhC0R5=[{'kixHbe54': item, 'sx9S4gZF': uid, 'H6k1LIxC': '1', 'Y5k8LJs9': '1'}],
               wc2DiMY1=party(uid, 2800))
    after = sum(int(x['wgV86x1q']) for x in r.get('UsDq1E6o', []))
    check('11d UseItem takes one from the bag and stores the healed party', after == before - 1
          and all(x.get('e7DK0FQT') == '2800' for x in r.get('wc2DiMY1', [])), (before, after))
    r = a.call(RAID_LIMITED_USE_ITEM, **run.nodes(), igAhC0R5=[{'kixHbe54': '99999', 'sx9S4gZF': uid, 'H6k1LIxC': '1', 'Y5k8LJs9': '1'}],
               wc2DiMY1=party(uid, 2800))
    check('11e LimitedUseItem of an item not in the bag refused', refused(r), r)
    r = a.call(RAID_LIMITED_USE_ITEM, **run.nodes(), igAhC0R5=[{'kixHbe54': '22100', 'sx9S4gZF': uid, 'H6k1LIxC': '1', 'Y5k8LJs9': '0'}],
               wc2DiMY1=party(uid, 2900))
    check('11f LimitedUseItem from the equipped list accepted', not refused(r)
          and all(x.get('e7DK0FQT') == '2900' for x in r.get('wc2DiMY1', [])), r if refused(r) else '')

    # 7. Boss: persisted HP across battles, then the kill.
    run.info(4)
    r = run.battle_start(4, boss='1')
    parts = r.get('Fx60pCaU', [])
    check('7a boss battle: part 210001 at 40000', [(x.get('o49dYfpH'), x.get('e7DK0FQT')) for x in parts]
          == [('210001', '40000')], parts)
    check('7b boss groups use RaidBossMst group 20000000', first(r, 'pj41dy9g').get('Qzhp8B40') == '20000000'
          and first(r, 'pj41dy9g').get('etM5TCb9') in ('1', 'true', True)
          and all(x.get('Qzhp8B40') == '20000000' for x in r.get('75t0sx9z', [])),
          (r.get('pj41dy9g'), r.get('75t0sx9z')))
    check('7c boss monster row carries the raid sprite', any('monster_anime_raid02.png' in str(m.values())
                                                          for m in r.get('U0v5IeJo', [])), r.get('U0v5IeJo'))
    hit = run.new_event(1, '1:210001:15000')
    r = run.progress([hit], boss='1')
    check('7d 15000 damage applied once', first(r, 'Ts2zJ6gj').get('4x7IS9Vg') == '25000', first(r, 'Ts2zJ6gj'))
    r = run.battle_end([hit], boss='1')
    check('7e BattleEnd re-sending the same event does not double it', first(r, 'Ts2zJ6gj').get('4x7IS9Vg') == '25000')
    r = run.progress([run.new_event(1, '1:210001:5000')], boss='1')
    check('7f damage outside an open boss battle ignored', first(r, 'Ts2zJ6gj').get('4x7IS9Vg') == '25000')
    r = run.battle_start(4, boss='1')
    check('7g second boss battle starts at 25000', [x.get('e7DK0FQT') for x in r.get('Fx60pCaU', [])] == ['25000'])
    check('7g2 boss encounters counted (5soj5vGI)', mine(r, '81B4TvEK', uid).get('5soj5vGI') == '1:2',
          mine(r, '81B4TvEK', uid))
    r = run.progress([run.new_event(1, '1:210001:30000')], boss='1')
    check('7h boss down: run won, member status 2', first(r, 'g97TWISb').get('3tWuc7de') == '2'
          and mine(r, 'uwH62EZy', uid).get('j3g5P4cq') == '2' and first(r, 'Ts2zJ6gj').get('4x7IS9Vg') == '0',
          (first(r, 'g97TWISb'), mine(r, 'uwH62EZy', uid)))
    r = run.battle_end([], boss='1')
    check('7i BattleEnd after the win answers the finished field', first(r, 'g97TWISb').get('3tWuc7de') == '2')
    r = run.battle_start(4, boss='1')
    check('7j BattleStart after the win: no battle content (client skips to BattleEnd)', 'pj41dy9g' not in r and not refused(r), r)

    # 8. MissionEnd pays once.
    zel_before = db.execute('SELECT zel, karma FROM user_info WHERE id = ?', (uid,)).fetchone()
    end1 = run.end()
    res = first(end1, 'UC8S1AiX')
    rewards = end1.get('4sbU8kdW', [])
    zel_after = db.execute('SELECT zel, karma FROM user_info WHERE id = ?', (uid,)).fetchone()
    check('8a first clear, boss named', res.get('3fHunNe4') == '1' and res.get('6dkEYp04') == 'Edelma', res)
    check('8b zel 8000 / karma 2000 rows and credit', any(x.get('30Kw4WBa') == '3' and x.get('wJsB35iH') == '8000' for x in rewards)
          and zel_after[0] - zel_before[0] == 8000 and zel_after[1] - zel_before[1] == 2000, (rewards, zel_before, zel_after))
    # The result scenes list items only as reward type 2/5/6 (page 1285);
    # type 1 feeds the zel/karma count-up alone (ticket #154).
    check('8c harvested items to the present box, listed as materials (reward type 2)',
          sum(1 for x in rewards if x.get('30Kw4WBa') == '4') >= 1
          and all(x.get('IkmC8gG2') == '2' for x in rewards if x.get('30Kw4WBa') == '4'), rewards)
    check('8c2 zel/karma rows are reward type 1; mission 2000 (b5yeVr61 1:0) pays no medals',
          all(x.get('IkmC8gG2') == '1' for x in rewards if x.get('30Kw4WBa') in ('3', '11'))
          and not any(x.get('30Kw4WBa') == '12' for x in rewards), rewards)
    check('8d login ids blank, header team info present', first(end1, '1ry6BKoe').get('8VYd6xSX') == ''
          and 'fEi17cnx' in end1 and any(c.get('j28VNcUW') == '2000' for c in end1.get('8LF2ohCY', [])), first(end1, '1ry6BKoe'))
    check('8e no RC-up on mission 2000', first(end1, 'awQFCW03').get('UJTR1h5r') == '0')
    end2 = run.end()
    zel_again = db.execute('SELECT zel FROM user_info WHERE id = ?', (uid,)).fetchone()
    # Both carry the achievement badges (h23iRjGN): the retry's is computed
    # anew, and nothing changed in between.
    check('8f second MissionEnd: same reply, no double pay',
          end2 == end1 and 'h23iRjGN' in end1 and zel_again[0] == zel_after[0], sorted(set(end1) ^ set(end2)))
    r = a.call(RAID_ROOM_INFO, **run.room_node())
    check('8g RaidRoomInfo still answers the retired room (grace)', len(r.get('8iBuZa3n', [])) == 1, r)
    u = user_info(a.call(RAID_USER_INFO))
    check('8h RaidUserInfo: no login room after the run', u.get('8VYd6xSX') == '', u)

    # 9. Retire.
    run2 = Run(a, '2000')
    run2.start()
    r = a.call(RAID_MISSION_EXIT, **run2.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])
    check('9a MissionExit (quick): login ids blank', user_info(r).get('8VYd6xSX') == '', r)
    check('9b field refuses after retire', refused(run2.info()))
    check('9c MissionEnd after retire pays nothing', refused(run2.end()))

    # 10. Time-out.
    run3 = Run(a, '2000')
    run3.start()
    db.execute('UPDATE raid_room_mission SET started_at = started_at - 1801 WHERE room_mission_id = ?', (run3.rm,))
    db.commit()
    r = run3.info()
    check('10a time-out derived on the next poll', first(r, 'g97TWISb').get('3tWuc7de') == '4'
          and mine(r, 'uwH62EZy', uid).get('j3g5P4cq') == '4', first(r, 'g97TWISb'))
    r = a.call(RAID_ROOM_INFO, **run3.room_node())
    check('10b resume path sees MissionStatus 4', mine(r, 'uwH62EZy', uid).get('j3g5P4cq') == '4', r)
    end = run3.end()
    # One blank row, not none: the client keeps the last list it was given.
    check('10c MissionEnd after time-up: one blank reward row, no first clear',
          [x.get('IkmC8gG2') for x in end.get('4sbU8kdW', [])] == ['0']
          and first(end, 'UC8S1AiX').get('3fHunNe4') == '0', end)

    # 12. RC-up mission 2007 (UJTR1h5r = 1): teleport onto the boss and kill it.
    run4 = Run(b, '2007')
    r = run4.start()
    bosses = r.get('Ts2zJ6gj', [])
    check('12a mission 2007 started with a boss', len(bosses) >= 1, r.get('Ts2zJ6gj'))
    target = bosses[0] if bosses else {}
    db.execute('UPDATE raid_mission_user SET now_point = ?, dst_point = ? WHERE room_mission_id = ? AND user_id = ?',
               (int(target.get('7Ex5mKRP', 0)), int(target.get('7Ex5mKRP', 0)), run4.rm, b.user_id))
    db.commit()
    r = run4.battle_start(int(target.get('7Ex5mKRP', 0)), boss=target.get('SP29fLtH', ''))
    body = [x for x in r.get('Fx60pCaU', []) if x.get('7Hf3DnVj') == '1']
    check('12b boss battle on 2007', len(body) == 1, r.get('Fx60pCaU') if not refused(r) else r)
    if body:
        dmg = ','.join(f"{target['SP29fLtH']}:{x['o49dYfpH']}:{x['e7DK0FQT']}" for x in r.get('Fx60pCaU', []))
        r = run4.battle_end([run4.new_event(1, dmg)], boss=target['SP29fLtH'])
        check('12c run won', first(r, 'g97TWISb').get('3tWuc7de') == '2', first(r, 'g97TWISb'))
        medals = lambda: (db.execute("SELECT COALESCE(SUM(target_cnt), 0) FROM user_presents WHERE user_id = ?"
                                     " AND present_type = 12 AND target_id = '1'", (b.user_id,)).fetchone()[0])
        medals_before = medals()
        end = run4.end()
        # The Raid Class-up scene (1263) lists reward type 3; 4 is the clear scene's.
        rc_rows = [x for x in end.get('4sbU8kdW', []) if x.get('IkmC8gG2') == '3']
        check('12d RC-up flag, before = 1, RC 2 and RC 2 rewards as type 3 (Sphere Frog + 3 gems)',
              first(end, 'awQFCW03').get('UJTR1h5r') == '1'
              and first(end, 'awQFCW03').get('3Y9cKkuV') == '1' and first(end, '1ry6BKoe').get('7x3pPB2C') == '2'
              and sorted((x.get('30Kw4WBa'), x.get('TdDHf59J'), x.get('wJsB35iH')) for x in rc_rows)
              == [('6', '20302', '1'), ('8', '0', '3')]
              and not any(x.get('IkmC8gG2') == '4' for x in end.get('4sbU8kdW', [])),
              (first(end, 'awQFCW03'), end.get('4sbU8kdW')))
        medal_rows = [x for x in end.get('4sbU8kdW', []) if x.get('30Kw4WBa') == '12']
        check('12d2 Brave Medal from b5yeVr61 1:1: one type-1 row for medal 1, in the present box',
              [(x.get('TdDHf59J'), x.get('wJsB35iH'), x.get('IkmC8gG2')) for x in medal_rows] == [('1', '1', '1')]
              and medals() - medals_before == 1, (medal_rows, medals_before, medals()))
        permits = {p.get('j28VNcUW') for p in end.get('2WqoMm4d', [])}
        check('12e RC-2 missions now permitted', '3011' in permits, len(permits))

    # 11. Raid orbs: each run set out on cost one (A: 3 runs above), a resume
    # costs nothing, and at 0 a new start is refused (the client offers the
    # orb shop first; the server only stops a stale or forged start).
    def aube() -> int:
        return int(user_info(a.call(RAID_USER_INFO)).get('C45mRxnZ') or -1)
    check('11o-a three runs set out: three orbs spent', aube() == 0, aube())
    blocked = Run(a, '2000')
    check('11o-b no orb left: MissionStart refused (Retry)', refused(blocked.start()))
    db.execute('UPDATE raid_user SET aube = 1, aube_ts = 0 WHERE user_id = ?', (uid,))
    db.commit()
    check('11o-c with an orb: MissionStart opens the run, orb spent', not refused(blocked.start()) and aube() == 0)
    check('11o-d resuming the open run costs nothing', not refused(blocked.start()) and aube() == 0)
    a.call(RAID_MISSION_EXIT, **blocked.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])
    db.execute('UPDATE raid_user SET aube = 3, aube_ts = 0 WHERE user_id = ?', (uid,))
    db.commit()

    # 13. Room path guards.
    run5 = Run(a, '2000')
    r = b.call(RAID_ENTER, **run5.room_node(**{'4WSu1irc': ''}))
    check('13a RaidEnter into a solo quick room refused', refused(r), r)
    r = b.call(RAID_MISSION_SELECT, **run5.room_node(**{'j28VNcUW': '2001', '8ZK3w0Np': uid}))
    check('13b MissionSelect by a non-master refused', refused(r), r)
    r = a.call(RAID_MISSION_SELECT, **run5.room_node(**{'j28VNcUW': '2001', '8ZK3w0Np': uid}))
    check('13c master MissionSelect changes the room mission', first(r, '8iBuZa3n').get('j28VNcUW') == '2001', r)
    r = a.call(RAID_MISSION_SELECT, **run5.room_node(**{'j28VNcUW': '87100', '8ZK3w0Np': uid}))
    check('13d MissionSelect above RC refused', refused(r), r)

    # 14. Crash mid battle: MissionStart reports the open battle; BattleRestart rebuilds it.
    run5.mission = '2001'
    r = run5.start()
    pos = mine(r, '81B4TvEK', uid)
    point = int(pos.get('pjX27bKB', 0))
    db.execute('UPDATE raid_mission_user SET now_point = 2, dst_point = 2 WHERE room_mission_id = ? AND user_id = ?',
               (run5.rm, uid))
    db.commit()
    r = run5.battle_start(2)
    serial = run5.serial
    r = run5.start()
    check('14a resumed MissionStart shows BattleNum = the open battle', mine(r, '81B4TvEK', uid).get('EqgF1H8M') == serial,
          (serial, mine(r, '81B4TvEK', uid), point))
    r = a.call(RAID_BATTLE_RESTART, **run5.nodes(), yL3Yrf1M=[{'1kisc6IF': serial}])
    check('14b BattleRestart serves the battle again', len(r.get('pj41dy9g', [])) == 1
          and first(r, 'yL3Yrf1M').get('1kisc6IF') == serial, r if refused(r) else '')
    r = a.call(RAID_BATTLE_RESTART, **run5.nodes(), yL3Yrf1M=[{'1kisc6IF': '123'}])
    check('14c BattleRestart with a stale serial refused', refused(r), r)
    a.call(RAID_MISSION_EXIT, **run5.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])

    # 15. tickets#125: mission 4011 (RC 3), boss 37 at point 11, four hops from
    # the camp.  The client reports only the last point a quick walker reached,
    # and the boss encounter fires on arrival: both must be accepted.
    db.execute('UPDATE raid_user SET rc = 3, aube = 3, aube_ts = 0 WHERE user_id = ?', (b.user_id,))
    db.commit()
    run6 = Run(b, '4011')
    run6.start()
    r = run6.info(11)
    check('15a 1 -> 11 in one report (4 hops) is accepted', mine(r, '81B4TvEK', b.user_id).get('pjX27bKB') == '11',
          mine(r, '81B4TvEK', b.user_id))
    r = run6.battle_start(11, boss='37')
    check('15b boss battle at the far point starts', not refused(r) and len(r.get('Fx60pCaU', [])) >= 1,
          r if refused(r) else r.get('Fx60pCaU'))
    b.call(RAID_MISSION_EXIT, **run6.nodes(), GWfpjED4=[{'GWfpjED4': '1'}])

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
