"""Wire test for Classic Raid round 2, band "multiplayer" (docs/features/RAIDS_ROUND2_MULTIPLAYER.md 7.1).

Fake devices A..E (plus F, the fourth entrant the full-room race needs) walk
the lobby: make / search / auto-join, password and idempotent RaidEnter, the
RoomInfo poll with its chat delta, stamp chat (validation, rate limit, resend
dedupe, non-members), ready, kick and the expel poll, the last-slot race,
master promotion, the absence drop, blacklist, edit sanitising, helper
select/remove and the player's records; then the hand-over to the field (5.2)
and the shared boss HP contract (5.1) on a second, full room.

Usage: python -I raid_multi_test.py --url http://127.0.0.1:9965 --db <worktree>/deploy/gme.sqlite
       [--rounds 20] [--no-mission] [--keep-rooms]
Needs pycryptodome, the shared helpers in tools/ and the round-1 / core raid
tests next to this file.  Creates throwaway accounts, CLOSES every open
non-quick room already in the DB (step 2 needs "a fresh world state"; skip with
--keep-rooms) and writes rows directly for the absence step and C's spare
unit: back up deploy/gme.sqlite first.
"""
import argparse
import re
import sqlite3
import sys
import threading
import time
from pathlib import Path

# -I leaves the script's own directory off sys.path; the two raid tests next to
# this one carry Player / Run and find tools/ themselves.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from raid_entry_test import (Player, RAID_AGREEMENT, RAID_EXIT, RAID_ROOM_INFO,  # noqa: E402
                             RAID_USER_INFO, RAID_WORLD_INFO, user_info)
from raid_field_test import (RAID_ENTER, RAID_MISSION_END, RAID_MISSION_EXIT, RAID_MISSION_START, Run, first,  # noqa: E402
                             mine, party, refused)

# Request ids: the REGISTER list of MULTIPLAYER 6.
RAID_ROOM_LIST = ('YF9UQpn6', 'c0Qf94Ir')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')   # the game's UserInfo: seeds raid squads
RAID_ROOM_EDIT = ('52H0dkjv', '9KdeuMX8')
RAID_READY = ('vmdA2GD8', 'esR7NZL9')
RAID_KICK = ('Xbgen5J8', 'FrX0T2QJ')
RAID_PLAYER_INFO = ('81zkMjfN', 'V7M6Bph4')
RAID_SEND_CHAT = ('dYIy6Eu8', 'x2Fm1LSt')
RAID_CHAT_LOG = ('k5pMN68B', 'cy0oLna7')
RAID_BLACKLIST = ('4igfcyIm', 'XIgQ4UHm')
RAID_FRIEND_REMOVE = ('u59zdKCi', '0HJ9ShKp')
RAID_FRIEND_SELECT = ('u59zdKCw', '0HJ9ShKg')

# Mission 2000 (RC 1, member limit 4): boss 1 Edelma, main body 210001, at point 4.
WORLD, MISSION, PASSWORD, BOSS, BOSS_BODY = '1', '2000', '1234', '1', '210001'
RETRY = 2                 # GmeErrorCommand::Retry = HandleResult::refuseToRetry (1.2)
CHAT_GAP_S = 1.6          # kChatMinGapMs 1500, plus margin
CHAT_PER_MINUTE = 20
# Stamp 49 is one of the gaps in F_STAMP_MST_Ver8 (ids 1..101 minus 49, 66..69; 8000..8003).
NOT_A_STAMP = '49'
# RaidUserTeamArchiveInfo (6nqGvdY1) keys besides h7eY3sAK, MULTIPLAYER 4.7.
ARCHIVE_KEYS = ('T7BvL1kh', '8CEu9Kcm', '58rGsYvL', '2dVBPNQ5', 'ai0gh9Vo', 'UNycbmV0', 'x0s78E4G',
                'y6fVdTch', 'Js8Amd2N', '0LwvAF3H', 'rQ3TAy6I', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU',
                'XP06YWdT', 'Wnc6A31E', 'kqI45VU1', 'kHbYZ9w1', '4vx39EIB', 'c3Bo97kI', '1VbdgX76',
                'E5S7XgR6', 'bprW7k3f', 'D8EZfi4p', 'nLb68MSq', '54imIPa2', 'nbu1dyP5', 'b28wpmvq', 'b5LZj7fy')
BOSS_FINISH, WIN = 'bprW7k3f', '8CEu9Kcm'


# ---- reply readers ---------------------------------------------------------

def retried(reply: dict) -> bool:
    """A refuseToRetry refusal: the raid scenes' own noticeOK handles -4000."""
    error = (reply.get('_unencrypted') or {}).get('b5PH6mZa') or {}
    return str(error.get('iPD12YCr', '')) == str(RETRY)


def rows(reply: dict, group: str) -> list:
    found = reply.get(group) or []
    return found if isinstance(found, list) else [found]


def member_ids(reply: dict) -> list:
    return [m.get('h7eY3sAK') for m in rows(reply, 'uwH62EZy')]


def listed(reply: dict, room: str):
    return next((r for r in rows(reply, '8iBuZa3n') if r.get('8VYd6xSX') == room), None)


def chat(reply: dict) -> list:
    return rows(reply, 'WCA9VhL3')


def log_ids(reply: dict) -> list:
    return [num(x.get('NLKce95n')) for x in chat(reply)]


def black_ids(reply: dict) -> list:
    return [x.get('h7eY3sAK') for x in rows(reply, 'Qazc0Qv8')]


def boss_hp(reply: dict) -> int:
    row = next((x for x in rows(reply, 'Ts2zJ6gj') if x.get('SP29fLtH') == BOSS), {})
    return num(row.get('4x7IS9Vg'))


def num(value) -> int:
    text = str(value if value is not None else '')
    return int(text) if text.lstrip('-').isdigit() else -1


def rid(room: str) -> int:
    """raid_room.room_id is an INTEGER; the wire carries it as a string."""
    return int(room) if str(room).isdigit() else -1


# ---- requests --------------------------------------------------------------

def key(room: str, **extra) -> dict:
    return {'8iBuZa3n': [{'43hMuY2I': WORLD, '8VYd6xSX': room, **extra}]}


def poll(p: Player, room: str, cursor: int = 0) -> dict:
    """RaidRoomInfo as roomConnectReadyUp sends it (4.1), cursor = newest chat log id held."""
    return p.call(RAID_ROOM_INFO, **key(room), pw38qdui=[{'NLKce95n': str(cursor)}],
                  T49LxzKr=[{'wbo6qS5P': '1', 'c13QLNRf': '0'}])


def enter(p: Player, room: str, password: str = '') -> dict:
    return p.call(RAID_ENTER, **key(room, **({'4WSu1irc': password} if password else {})))


def leave(p: Player, room: str) -> dict:
    return p.call(RAID_EXIT, **key(room), uwH62EZy=[{'h7eY3sAK': p.user_id}])


def room_form(op: int, room: str = '', **keys) -> dict:
    """A RaidUpDataRoomInfo node as the make/edit scenes send it (4.2); None drops a key."""
    node = {'mnZ5K4Ii': str(op), '43hMuY2I': WORLD, 'NQR1Vd3t': '1', 'wYmg52xp': '1', 'nXCf2FE9': '1',
            '0TfXnP3m': '1', 'j28VNcUW': MISSION, 'j3sGA1zt': '1', '1Txpza2u': '1', 'KQA1oWj7': '1',
            'TdDHf59J': ''}
    if room:
        node['8VYd6xSX'] = room
    node.update(keys)
    return {'8iBuZa3n': [{k: v for k, v in node.items() if v is not None}]}


def search(p: Player, stype: int, world: str = WORLD, **filters) -> dict:
    """RaidRoomList (4.4); kick/joinable -1 = any, level "1" as the menu sends it."""
    node = {'ouU5H7qm': str(stype), 'j3sGA1zt': '-1', '1Txpza2u': '-1', '0TfXnP3m': '1'}
    if world:
        node['43hMuY2I'] = world
    node.update(filters)
    return p.call(RAID_ROOM_LIST, **{'8iBuZa3n': [node]})


def auto_join(p: Player) -> dict:
    """RaidRoomAutoEnterScene::connectRoomAuto: type 3, RC -1, kick -1, level "0"."""
    return search(p, 3, wYmg52xp='-1', **{'0TfXnP3m': '0'})


def ready(p: Player, room: str, value: int, target: str = '') -> dict:
    return p.call(RAID_READY, **key(room), uwH62EZy=[{'h7eY3sAK': target or p.user_id, 'uX87tAZB': str(value)}])


def kick(p: Player, room: str, target: str) -> dict:
    return p.call(RAID_KICK, **key(room), uwH62EZy=[{'h7eY3sAK': target}])


def say(p: Player, room: str, chat_type: str, content: str, cursor: int) -> dict:
    # The client sends chat with createDungeonEventUserInfoTag; the same login
    # keys resolve the identity, which is all the server reads from that group.
    return p.call(RAID_SEND_CHAT, **key(room), JQ6hW0Ma=[{'8Fj06n3K': chat_type, '3kQUaGx0': content}],
                  pw38qdui=[{'NLKce95n': str(cursor)}])


def chat_log(p: Player, room: str, cursor: int) -> dict:
    return p.call(RAID_CHAT_LOG, **key(room), pw38qdui=[{'NLKce95n': str(cursor)}])


def block(p: Player, target: str, op: int) -> dict:
    return p.call(RAID_BLACKLIST, kKpAdqAu=[{'KpTZlmkh': target, 'mnZ5K4Ii': str(op)}])


def records(p: Player) -> dict:
    return first(p.call(RAID_PLAYER_INFO), '6nqGvdY1')


def forged_helper(owner: str, user_unit_id: str, unit_id: str) -> dict:
    """A FriendInfo as the client sends it in ZX7gzQ6s, with stats the server must not trust."""
    return {'h7eY3sAK': owner, 'B5JQyV8j': 'Forged', '2Fh3J7ng': '999', '96Nxs2WQ': '1', 'pn16CNah': unit_id,
            '4A6LzBxr': '999', 'e7DK0FQT': '999999', '67CApcti': '999999', 'edy7fq3L': user_unit_id,
            'zsiAn9P1': '0', '2pAyFjmZ': '0'}


def concurrently(call, keys: list) -> dict:
    """call(k) for every k released by one barrier (raid_race_test.py style)."""
    barrier, out = threading.Barrier(len(keys)), {}

    def go(k):
        barrier.wait()
        try:
            out[k] = call(k)
        except Exception as ex:  # a dropped connection must fail the check, not hang the run
            out[k] = {'_unencrypted': {'exception': repr(ex)}}
    threads = [threading.Thread(target=go, args=(k,)) for k in keys]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    return out


class RoomRun(Run):
    """One member's side of a shared room run: no QuickStart, the master's id in MissionStart."""

    def __init__(self, player: Player, room: str, master: str):
        self.p, self.mission, self.room, self.master = player, MISSION, room, master
        self.rm, self.serial, self.event = '', '', 1

    def start(self) -> dict:
        r = self.p.call(RAID_MISSION_START, **self.room_node(**{'8ZK3w0Np': self.master}),
                        wc2DiMY1=party(self.p.user_id, start=True))
        if not refused(r):
            self.rm = mine(r, 'uwH62EZy', self.p.user_id).get('FV6MU5nQ', self.rm)
        return r

    def battle_start(self, point: int, boss: str = '') -> dict:
        r = super().battle_start(point, boss)
        nxt = num(first(r, 'yL3Yrf1M').get('Ez52fI60'))
        self.event = max(self.event, nxt)   # ids never repeat (CORE 3.1)
        return r

    def end(self) -> dict:
        return self.p.call(RAID_MISSION_END, **self.nodes(), GWfpjED4=[{'GWfpjED4': '0'}])


class Ctx:
    """The players, the lobby room under test, the DB handle and the tally."""

    def __init__(self, url: str, db_path: str):
        self.db = sqlite3.connect(db_path, timeout=10, isolation_level=None)
        self.failures, self.room, self.cursor = 0, '', 0
        self.a, self.b, self.c, self.d, self.e, self.f = (Player(url, f'M{x}') for x in 'ABCDEF')
        for p in (self.a, self.b, self.c, self.d, self.e, self.f):
            p.call(RAID_USER_INFO)
            p.call(RAID_AGREEMENT)

    def check(self, label: str, ok: bool, detail='') -> None:
        self.failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail)[:600] if detail and not ok else ''}")

    def one(self, sql: str, *args):
        row = self.db.execute(sql, args).fetchone()
        return row[0] if row else None

    def member(self, room: str, uid: str, column: str = 'kicked'):
        return self.one(f'SELECT {column} FROM raid_room_member WHERE room_id = ? AND user_id = ?', rid(room), uid)

    def members(self, room: str) -> int:
        return self.one('SELECT COUNT(*) FROM raid_room_member WHERE room_id = ? AND kicked = 0', rid(room))

    def room_col(self, room: str, column: str):
        return self.one(f'SELECT {column} FROM raid_room WHERE room_id = ?', rid(room))

    def chat_count(self, uid: str = '', content: str = '') -> int:
        sql, args = 'SELECT COUNT(*) FROM raid_chat WHERE room_id = ?', [rid(self.room)]
        if uid:
            sql, args = sql + ' AND user_id = ? AND content = ?', args + [uid, content]
        return self.one(sql, *args)

    def add_unit(self, p: Player) -> tuple:
        """A unit outside every deck: FrlendSelect's own-unit source (4.11). A fresh account has none."""
        cur = self.db.execute('INSERT INTO user_units(user_id, unit_id, unit_type_id, unit_lvl, base_hp, base_atk,'
                              ' base_def, base_rec) VALUES (?, 10017, 1, 30, 1500, 600, 600, 600)', (p.user_id,))
        return str(cur.lastrowid), '10017', 30

    def kill_credits(self, rm: str):
        """defeated_by of the run's bosses, or None when the mission band keeps no such column (5.1)."""
        if 'defeated_by' not in [r[1] for r in self.db.execute('PRAGMA table_info(raid_mission_boss)')]:
            return None
        return [r[0] for r in self.db.execute("SELECT defeated_by FROM raid_mission_boss"
                                              " WHERE room_mission_id = ? AND defeated_by != ''", (rid(rm),))]


def reset_world(t: Ctx) -> None:
    """Step 2's "fresh world state": close every open listed room, blank the login ids naming it."""
    ids = [r[0] for r in t.db.execute('SELECT room_id FROM raid_room WHERE status != 3 AND quick_start = 0')]
    for room_id in ids:
        t.db.execute("UPDATE raid_user SET login_world_id = '', login_room_id = '' WHERE login_room_id = ?",
                     (str(room_id),))
        t.db.execute('UPDATE raid_room SET status = 3, closed_at = ? WHERE room_id = ?', (int(time.time()), room_id))
    print(f'NOTE  closed {len(ids)} open room(s) already in the DB')


# ---- steps 1-4 -------------------------------------------------------------

def step_empty_world(t: Ctx) -> None:
    """Step 2, C's half, run first: the reply shapes that mean "no rooms" (1.3, 4.4)."""
    r = auto_join(t.c)
    found = rows(r, '8iBuZa3n')
    t.check('2a C auto-join, no rooms: one placeholder (8VYd6xSX "0", KQA1oWj7 0, C1vG0iKh 0)', len(found) == 1
            and found[0].get('8VYd6xSX') == '0' and found[0].get('KQA1oWj7') == '0'
            and found[0].get('C1vG0iKh') == '0' and found[0].get('rPIya39u') == '0', r)
    r = search(t.c, 1)
    t.check('2b C search, no rooms: empty 8iBuZa3n', not refused(r) and not rows(r, '8iBuZa3n'), r)
    for stype in (2, 4):
        found = rows(search(t.c, stype, world=''), '8iBuZa3n')
        t.check(f'2c type {stype} with nobody to follow: one placeholder (C1vG0iKh 0, 3tWuc7de 1)', len(found) == 1
                and found[0].get('C1vG0iKh') == '0' and found[0].get('3tWuc7de') == '1', found)


def step_make(t: Ctx) -> None:
    a = t.a
    r = a.call(RAID_ROOM_EDIT, **room_form(1, **{'4WSu1irc': PASSWORD}))
    room = first(r, '8iBuZa3n')
    t.room = room.get('8VYd6xSX', '')
    t.check('1a make: one row, master A, mission 2000, kick + auto-join on, not quick', len(rows(r, '8iBuZa3n')) == 1
            and room.get('8ZK3w0Np') == a.user_id and room.get('j28VNcUW') == MISSION and room.get('j3sGA1zt') == '1'
            and room.get('KQA1oWj7') == '1' and room.get('GWfpjED4') == '0' and room.get('rPIya39u') == '1', r)
    t.check('1b XqMsnE89 1 and the master sees the password', room.get('XqMsnE89') == '1'
            and room.get('4WSu1irc') == PASSWORD, room)
    t.check('1c members = [A]', member_ids(r) == [a.user_id], r.get('uwH62EZy'))
    t.check('1d USER login ids name the room', user_info(r).get('8VYd6xSX') == t.room != ''
            and user_info(r).get('43hMuY2I') == WORLD, user_info(r))
    db_row = t.db.execute('SELECT status, quick_start, password_flg FROM raid_room WHERE room_id = ?',
                          (rid(t.room),)).fetchone()
    t.check('1e DB: lobby (0), not quick, password_flg 1', db_row == (0, 0, 1), db_row)


def step_search(t: Ctx) -> None:
    r = search(t.b, 1)
    row = listed(r, t.room) or {}
    t.check("2d B search lists A's room, password blank for B", row.get('4WSu1irc') == '' and row.get('XqMsnE89') == '1'
            and row.get('ouU5H7qm') == '1' and row.get('C1vG0iKh') == '1' and row.get('3tWuc7de') == '0', row or r)
    t.check('2e a search reply carries no member rows', not rows(r, 'uwH62EZy'), r.get('uwH62EZy'))
    # Auto-join never picks a password room: its RaidEnter sends no password
    # and would always be refused (RaidRoomSearch candidateRooms).
    r = auto_join(t.b)
    found = rows(r, '8iBuZa3n')
    t.check("2f B auto-join skips A's password room: the single placeholder", len(found) == 1
            and found[0].get('8VYd6xSX') == '0' and found[0].get('KQA1oWj7') == '0', found or r)
    t.db.execute("UPDATE raid_room SET password = '' WHERE room_id = ?", (t.room,))
    r = auto_join(t.b)
    found = rows(r, '8iBuZa3n')
    t.check("2f' without the password: B auto-join gets exactly A's room", [x.get('8VYd6xSX') for x in found] == [t.room]
            and found[0].get('KQA1oWj7') == '1', found or r)
    t.db.execute("UPDATE raid_room SET password = ? WHERE room_id = ?", (PASSWORD, t.room))


def step_enter(t: Ctx) -> None:
    a, b = t.a, t.b
    r = enter(b, t.room, '0000')
    t.check('3a wrong password refused (Retry), B not a member', retried(r) and t.member(t.room, b.user_id) is None, r)
    r = enter(b, t.room, PASSWORD)
    t.check('3b right password: ROOM is the joined room only, members A, B', [x.get('8VYd6xSX') for x in
            rows(r, '8iBuZa3n')] == [t.room] and member_ids(r) == [a.user_id, b.user_id]
            and first(r, '8iBuZa3n').get('4WSu1irc') == '', r)
    t.check("3c B's USER login ids name the room", user_info(r).get('8VYd6xSX') == t.room, user_info(r))
    r = enter(b, t.room, PASSWORD)
    t.check('3d entering again: success, still 2 members', not refused(r) and member_ids(r) == [a.user_id, b.user_id]
            and first(r, '8iBuZa3n').get('rPIya39u') == '2' and t.members(t.room) == 2, r)


def step_poll(t: Ctx) -> None:
    a, b = t.a, t.b
    r = poll(a, t.room, 0)
    t.check('4a A polls from cursor 0: members A, B', member_ids(r) == [a.user_id, b.user_id], r)
    t.check('4b no chat rows yet', listed(r, t.room) is not None and not chat(r), r.get('WCA9VhL3'))
    row = first(r, '8iBuZa3n')
    t.check('4c the master still sees the password', row.get('4WSu1irc') == PASSWORD, row)
    row = first(poll(b, t.room, 0), '8iBuZa3n')
    t.check("4d B's poll hides the password, flag still set", row.get('4WSu1irc') == ''
            and row.get('XqMsnE89') == '1', row)


# ---- step 5: chat ----------------------------------------------------------

def step_chat(t: Ctx) -> None:
    a, b = t.a, t.b
    r = say(b, t.room, '2', '1', 0)
    own = [x for x in chat(r) if x.get('h7eY3sAK') == b.user_id and x.get('8Fj06n3K') == '2'
           and x.get('3kQUaGx0') == '1']
    t.check("5a B sends stamp 1: the reply carries B's own row", len(own) == 1, r)
    row = own[0] if own else {}
    t.check('5b row: sender name, HH:MM date, numeric log id', row.get('B5JQyV8j') == 'RaiderMB'
            and re.fullmatch(r'\d\d:\d\d', str(row.get('ToSxkU34', ''))) is not None
            and num(row.get('NLKce95n')) > 0, row)
    t.cursor = num(row.get('NLKce95n'))
    t.check('5c A polls from the old cursor: the row once', log_ids(poll(a, t.room, 0)) == [t.cursor])
    r = poll(a, t.room, t.cursor)
    t.check('5d A polls from the new cursor: no rows', listed(r, t.room) is not None and not chat(r), r.get('WCA9VhL3'))
    t.check('5e GetChatLog from 0: the row once', log_ids(chat_log(a, t.room, 0)) == [t.cursor])
    r = chat_log(a, t.room, t.cursor)
    t.check('5f GetChatLog from the newest id: no rows', not refused(r) and not chat(r), r)
    # 4.1 item 5: the member card draws each member's squad from 9PS8BLiC.
    # UserInfo seeds raid squads (deck type 3) from the quest decks first.
    for p in (t.a, t.b):
        p.call(USER_INFO)
    party = rows(poll(t.a, t.room), '9PS8BLiC')
    owners = {x.get('h7eY3sAK') for x in party}
    t.check("4e RaidRoomInfo carries both members' squads (9PS8BLiC)", {t.a.user_id, t.b.user_id} <= owners
            and all(x.get('pn16CNah') and x.get('edy7fq3L') and x.get('zsiAn9P1') == '0' for x in party),
            party[:2] or party)


def step_chat_filters(t: Ctx) -> None:
    b, c = t.b, t.c
    before = t.chat_count()
    for chat_type, content in (('1', '<color=255:0:0>hi'), ('2', '999999'), ('2', NOT_A_STAMP), ('3', '1')):
        time.sleep(CHAT_GAP_S)   # past the rate limit, so only validation can drop it
        r = say(b, t.room, chat_type, content, t.cursor)
        t.check(f'5g ({chat_type}, {content!r}) dropped silently: success, no rows, nothing stored',
                not refused(r) and not chat(r) and t.chat_count() == before, r)
    r = say(c, t.room, '2', '1', 0)
    t.check('5h non-member SendChat: success, no rows, nothing stored', not refused(r) and not chat(r)
            and t.chat_count() == before, r)
    r = chat_log(c, t.room, 0)
    t.check('5i non-member GetChatLog: success, no rows', not refused(r) and not chat(r), r)


def step_chat_rate(t: Ctx) -> None:
    b = t.b
    time.sleep(CHAT_GAP_S)
    before, started = t.chat_count(), time.monotonic()
    for stamp in range(2, 27):   # 25 valid StampMst ids
        say(b, t.room, '2', str(stamp), t.cursor)
    elapsed = time.monotonic() - started
    stored = t.chat_count() - before
    allowance = min(CHAT_PER_MINUTE, int(elapsed / 1.5) + 1)
    t.check('5j 25 stamps in a burst: 1..rate allowance stored', 1 <= stored <= allowance and stored < 25,
            (stored, allowance, round(elapsed, 2)))
    time.sleep(CHAT_GAP_S)
    cursor = t.one('SELECT MAX(log_id) FROM raid_chat WHERE room_id = ?', rid(t.room)) or 0
    sent = say(b, t.room, '2', '30', cursor)
    time.sleep(CHAT_GAP_S)   # the client resends after >= 1 s (checkSendSuccess)
    resent = say(b, t.room, '2', '30', cursor)
    seen = [sum(1 for x in chat(r) if x.get('3kQUaGx0') == '30') for r in (sent, resent)]
    t.check('5k a resend within 10 s is stored once; both replies carry the row',
            t.chat_count(b.user_id, '30') == 1 and seen == [1, 1], (t.chat_count(b.user_id, '30'), seen))


# ---- steps 6-8 -------------------------------------------------------------

def step_ready(t: Ctx) -> None:
    b, c = t.b, t.c
    r = ready(b, t.room, 1)
    t.check("6a B ready 1: B's row uX87tAZB 1", mine(r, 'uwH62EZy', b.user_id).get('uX87tAZB') == '1', r)
    r = ready(b, t.room, 1, target=c.user_id)
    t.check("6b B sending C's id: refused (Retry)", retried(r), r)
    r = ready(b, t.room, 2)
    t.check('6c ready 2 while the room is in the lobby: refused (Retry), B still ready 1', retried(r)
            and t.member(t.room, b.user_id, 'ready') == 1, r)
    t.check("6d a non-member's ReadyUp: refused (Retry)", retried(ready(c, t.room, 1)))
    r = ready(b, t.room, 0)
    t.check('6e B ready 0 un-readies', mine(r, 'uwH62EZy', b.user_id).get('uX87tAZB') == '0', r)
    # Readying commits to setting out, which costs an orb: with none, refused.
    t.db.execute('UPDATE raid_user SET aube = 0, aube_ts = ? WHERE user_id = ?', (int(time.time()), t.b.user_id))
    t.check('6f B with no raid orb: ready 1 refused (Retry)', retried(ready(t.b, t.room, 1)))
    t.db.execute('UPDATE raid_user SET aube = 3, aube_ts = 0 WHERE user_id = ?', (t.b.user_id,))


def step_kick(t: Ctx) -> None:
    a, b = t.a, t.b
    r = kick(b, t.room, a.user_id)
    t.check('7a B (not master) kicking A: refused (Retry)', retried(r) and t.member(t.room, a.user_id) == 0, r)
    r = kick(a, t.room, b.user_id)
    t.check('7b A kicks B: ROOM without B', not refused(r) and member_ids(r) == [a.user_id]
            and first(r, '8iBuZa3n').get('rPIya39u') == '1', r)
    u = user_info(b.call(RAID_USER_INFO))
    t.check("7c B's RaidUserInfo login ids are blank", u.get('8VYd6xSX') == '' and u.get('43hMuY2I') == '', u)
    for n in (1, 2):
        r = poll(b, t.room)
        t.check(f'7d B poll {n}: success, own row D2ZKtGR9 1', not refused(r)
                and mine(r, 'uwH62EZy', b.user_id).get('D2ZKtGR9') == '1', r)
    r = leave(b, t.room)
    t.check('7e kicked B RaidExit succeeds', not refused(r) and user_info(r).get('8VYd6xSX') == '', r)
    t.check('7f B re-entering: refused (Retry), kicked row kept', retried(enter(b, t.room, PASSWORD))
            and t.member(t.room, b.user_id) == 1)
    r = poll(a, t.room)
    t.check("7g A's poll never shows B", member_ids(r) == [a.user_id], r)


def step_race(t: Ctx, rounds: int) -> None:
    """Four entrants for A's last three slots; the INSERT alone decides (4.3 rule 8)."""
    entrants, bad = [t.c, t.d, t.e, t.f], []
    for rnd in range(rounds):
        poll(t.a, t.room)   # the master's heartbeat: 20 rounds outlast the 180 s absence rule
        res = concurrently(lambda p: enter(p, t.room, PASSWORD), entrants)
        joined = [p for p in entrants if not refused(res[p])]
        widest = max((num(first(res[p], '8iBuZa3n').get('rPIya39u')) for p in joined), default=0)
        losers_told = all(retried(res[p]) for p in entrants if p not in joined)
        count = t.members(t.room)
        if count > 4 or widest > 4 or len(joined) != 3 or not losers_told:
            bad.append({'round': rnd, 'members': count, 'reply_max': widest, 'joined': len(joined),
                        'losers_retry': losers_told})
        for p in joined:
            leave(p, t.room)
    t.check(f'8 {rounds} rounds, 4 racing entrants for 3 slots: never more than 4 members, loser refused (Retry)',
            not bad, bad)


# ---- steps 9-14 ------------------------------------------------------------

def step_promotion(t: Ctx) -> None:
    a, c, d = t.a, t.c, t.d
    enter(c, t.room, PASSWORD)
    time.sleep(1.1)   # joined_at is in seconds: C must be the earliest joiner
    r = enter(d, t.room, PASSWORD)
    t.check('9a C then D join: members A, C, D', member_ids(r) == [a.user_id, c.user_id, d.user_id], r)
    ready(c, t.room, 1)
    r = leave(a, t.room)
    t.check("9b master A leaves: success, A's login ids blank", not refused(r)
            and user_info(r).get('8VYd6xSX') == '', r)
    r = poll(c, t.room)
    t.check('9c C (earliest joiner) is master, room open', first(r, '8iBuZa3n').get('8ZK3w0Np') == c.user_id
            and member_ids(r) == [c.user_id, d.user_id] and t.room_col(t.room, 'status') == 0, r)
    t.check("9d the new master's ready is reset to 0", mine(r, 'uwH62EZy', c.user_id).get('uX87tAZB') == '0',
            mine(r, 'uwH62EZy', c.user_id))
    t.check('9e A polling the room it left: refused (Retry)', retried(poll(a, t.room)))


def step_absence(t: Ctx) -> None:
    c, d = t.c, t.d
    t.db.execute('UPDATE raid_room_member SET last_seen = ? WHERE room_id = ? AND user_id = ?',
                 (int(time.time()) - 200, rid(t.room), d.user_id))
    r = poll(c, t.room)
    t.check('10a D silent for 200 s: gone from the next room read', member_ids(r) == [c.user_id], r)
    login = t.one('SELECT login_room_id FROM raid_user WHERE user_id = ?', d.user_id)
    t.check("10b D's member row deleted, login ids blank", t.member(t.room, d.user_id) is None and login == ''
            and user_info(d.call(RAID_USER_INFO)).get('8VYd6xSX') == '', login)


def step_blacklist(t: Ctx) -> None:
    c, e = t.c, t.e
    t.check("11a before any block, E's search lists C's room", listed(search(e, 1), t.room) is not None)
    r = block(c, e.user_id, 1)
    t.check('11b C blocks E (op 1): Qazc0Qv8 = [E]', black_ids(r) == [e.user_id], r)
    t.check('11c blocking again is idempotent', black_ids(block(c, e.user_id, 1)) == [e.user_id])
    r = search(e, 1)
    t.check("11d E's search no longer lists C's room", not refused(r) and listed(r, t.room) is None, r)
    t.check("11e E cannot enter C's room: refused (Retry)", retried(enter(e, t.room, PASSWORD)))
    ui, wi = c.call(RAID_USER_INFO), c.call(RAID_WORLD_INFO)
    t.check('11f RaidUserInfo and RaidWorloInfo carry the list', black_ids(ui) == [e.user_id]
            and black_ids(wi) == [e.user_id], (ui.get('Qazc0Qv8'), wi.get('Qazc0Qv8')))
    r = block(c, e.user_id, 3)
    t.check('11g C unblocks E (op 3): list without E', not refused(r) and e.user_id not in black_ids(r), r)
    t.check("11h E's search lists C's room again", listed(search(e, 1), t.room) is not None)


def step_edit(t: Ctx) -> None:
    c, e = t.c, t.e
    t.check('12a E joins once unblocked (the non-master below)', e.user_id in member_ids(enter(e, t.room, PASSWORD)))
    r = c.call(RAID_ROOM_EDIT, **room_form(2, t.room, **{'WDUyI85C': '<b>x</b>\x01', 'j3sGA1zt': '0',
                                                         '4WSu1irc': PASSWORD}))
    row, stored = first(r, '8iBuZa3n'), t.room_col(t.room, 'comment') or ''
    t.check('12b comment stored without <, > and control characters', not refused(r) and 'x' in stored
            and re.search(r'[<>\x00-\x1f\x7f]', stored) is None and row.get('WDUyI85C') == stored, (stored, r))
    t.check('12c the kick flag is not editable: still 1', row.get('j3sGA1zt') == '1'
            and t.room_col(t.room, 'kick_flg') == 1, row)
    r = e.call(RAID_ROOM_EDIT, **room_form(2, t.room, **{'WDUyI85C': 'mine now'}))
    t.check('12d non-master edit: refused (Retry), nothing changed', retried(r)
            and t.room_col(t.room, 'comment') == stored, r)


def step_helper(t: Ctx) -> None:
    b, c, d = t.b, t.c, t.d
    r = c.call(RAID_FRIEND_SELECT, **key(t.room), ZX7gzQ6s=[forged_helper(d.user_id, '1000', '10017')])
    t.check('13a helper of a non-friend: refused (Retry), nothing stored', retried(r)
            and not t.member(t.room, c.user_id, 'helper_json'), r)
    uuid, unit_id, level = t.add_unit(c)
    r = c.call(RAID_FRIEND_SELECT, **key(t.room), ZX7gzQ6s=[forged_helper(c.user_id, uuid, unit_id)])
    sel = first(r, 'ZX7gzQ6s')
    t.check('13b own non-party unit accepted; ZX7gzQ6s built by the server (real name, level, stats)',
            not refused(r) and sel.get('h7eY3sAK') == c.user_id and sel.get('pn16CNah') == unit_id
            and sel.get('4A6LzBxr') == str(level) and sel.get('B5JQyV8j') == 'RaiderMC'
            and sel.get('e7DK0FQT') != '999999', sel or r)
    t.check('13c helper_json stored, reply carries ROOM', unit_id in (t.member(t.room, c.user_id, 'helper_json') or '')
            and first(r, '8iBuZa3n').get('8VYd6xSX') == t.room, t.member(t.room, c.user_id, 'helper_json'))
    r = c.call(RAID_FRIEND_REMOVE, **key(t.room))
    t.check('13d FriendRemove in the room: success, helper_json cleared', not refused(r)
            and not t.member(t.room, c.user_id, 'helper_json'), r)
    # PartyEditTopScene2 sends it with no room: the empty room id is not sent at all.
    r = b.call(RAID_FRIEND_REMOVE, **{'8iBuZa3n': [{'43hMuY2I': WORLD}]})
    t.check('13e FriendRemove outside a room: {} success', not refused(r) and not rows(r, '8iBuZa3n'), r)


def step_close(t: Ctx) -> None:
    """Step 9, second half: the last member leaves -> closed; every stale poll is refused (4.1 rule 1)."""
    leave(t.e, t.room)
    r = leave(t.c, t.room)
    t.check('9f last member leaves: room closed (status 3)', not refused(r) and t.room_col(t.room, 'status') == 3, r)
    for who, p in (('C (left)', t.c), ('D (dropped)', t.d)):
        t.check(f'9g RaidRoomInfo of the closed room for {who}: refused (Retry)', retried(poll(p, t.room)))
    t.check('9h RaidRoomInfo of a room that never existed: refused (Retry)', retried(poll(t.c, '999999999')))


def step_player_info(t: Ctx) -> None:
    info = records(t.b)
    rest = {k: v for k, v in info.items() if k != 'h7eY3sAK'}
    t.check('14 PlayerInfo: one 6nqGvdY1 node for the caller, every record "0"', info.get('h7eY3sAK') == t.b.user_id
            and set(ARCHIVE_KEYS) <= set(rest) and all(v == '0' for v in rest.values()), info)


# ---- 5.2 hand-over and step 15 ---------------------------------------------

def step_handover(t: Ctx) -> dict:
    """A second room (expel off) filled to 4: A master, B and D ready, F not (MULTIPLAYER 5.2)."""
    a, b, d, f = t.a, t.b, t.d, t.f
    room = first(a.call(RAID_ROOM_EDIT, **room_form(1, **{'j3sGA1zt': '0', 'KQA1oWj7': '0'})),
                 '8iBuZa3n').get('8VYd6xSX', '')
    for p in (b, d, f):
        enter(p, room)
    t.check('7h kick in a room without the Expel feature: refused (Retry)', retried(kick(a, room, b.user_id))
            and t.member(room, b.user_id) == 0)
    ready(b, room, 1)
    ready(d, room, 1)
    runs = {p.user_id: RoomRun(p, room, a.user_id) for p in (a, b, d, f)}
    r = runs[a.user_id].start()
    t.check('5.2a master MissionStart: room in the field (status 2, mission_started_at set)',
            first(r, '8iBuZa3n').get('3tWuc7de') == '1' and t.room_col(room, 'status') == 2
            and (t.room_col(room, 'mission_started_at') or 0) > 0 and runs[a.user_id].rm != '', r)
    r = poll(b, room)
    status = {m.get('h7eY3sAK'): m.get('j3g5P4cq') for m in rows(r, 'uwH62EZy')}
    t.check('5.2b ready members -> MissionStatus 1, unready F stays 0', status == {a.user_id: '1', b.user_id: '1',
            d.user_id: '1', f.user_id: '0'}, status or r)
    row = first(r, '8iBuZa3n')
    t.check('5.2c room row in the field: 3tWuc7de 1, 4Qf8RePm in 1..300', row.get('3tWuc7de') == '1'
            and 0 < num(row.get('4Qf8RePm')) <= 300, row)
    t.check("5.2d ready B's MissionStart accepted into the same run", not refused(runs[b.user_id].start())
            and runs[b.user_id].rm == runs[a.user_id].rm != '', runs[b.user_id].rm)
    # In the field the member card only keeps rows it finds in the run party
    # (setUnitInfoParty): B's 9PS8BLiC rows must be B's run party rows.
    want = {x.get('edy7fq3L') for x in party(b.user_id, start=True)}
    got = {x.get('edy7fq3L') for x in rows(poll(a, room), '9PS8BLiC') if x.get('h7eY3sAK') == b.user_id}
    t.check("5.2d' on the run, B's card rows are B's run party", got == want and len(got) > 0, (got, want))
    t.check("5.2e unready F's MissionStart refused", refused(runs[f.user_id].start()))
    me = mine(ready(f, room, 2), 'uwH62EZy', f.user_id)
    t.check('5.2f F mid-quest ReadyUp 2: ready 2, MissionStatus 1', me.get('uX87tAZB') == '2'
            and me.get('j3g5P4cq') == '1', me)
    for label, p in (('F', f), ('D', d)):
        t.check(f"5.2g {label}'s MissionStart accepted into the same run", not refused(runs[p.user_id].start())
                and runs[p.user_id].rm == runs[a.user_id].rm != '', runs[p.user_id].rm)
    return runs


def step_collect_later(t: Ctx, runs: dict, order: list) -> None:
    """16: F steps away for 3+ min while the others collect; the room moves on, F still collects later."""
    a, f = t.a, t.f
    room, old_rm = runs[a.user_id].room, runs[a.user_id].rm
    for uid in order[:-1]:
        runs[uid].end()
    t.db.execute('UPDATE raid_room_member SET last_seen = last_seen - 400 WHERE room_id = ? AND user_id = ?',
                 (rid(room), f.user_id))
    poll(a, room)
    r = poll(a, room)
    t.check('16a the others collected: room back in the lobby, away F kept at MissionStatus 2',
            t.room_col(room, 'status') == 0 and mine(r, 'uwH62EZy', f.user_id).get('j3g5P4cq') == '2', r)
    r = runs[a.user_id].start()
    new_rm = runs[a.user_id].rm
    t.check('16b the master starts the next run', not refused(r) and new_rm not in ('', old_rm), (new_rm, old_rm))
    me = mine(poll(f, room), 'uwH62EZy', f.user_id)
    t.check("16c F's row still names the run F has not collected", me.get('FV6MU5nQ') == old_rm
            and me.get('j3g5P4cq') == '2', me)
    runs[f.user_id].rm = old_rm
    r = runs[f.user_id].end()
    t.check('16d F collects the earlier run (MissionEnd named it), back to the lobby', not refused(r)
            and t.member(room, f.user_id, 'mission_status') == 0, r)
    t.check('16e collecting it again is the stored reply, nothing paid twice', not refused(runs[f.user_id].end()))
    runs[a.user_id].p.call(RAID_MISSION_EXIT, **runs[a.user_id].nodes())


def step_shared_boss(t: Ctx, runs: dict) -> None:
    """Step 15 (5.1): one boss HP for the room, one kill credit, clear side effects once."""
    order = [t.a.user_id, t.b.user_id, t.d.user_id, t.f.user_id]
    for uid in order:
        runs[uid].info(2)
    at = [mine(runs[uid].info(4), '81B4TvEK', uid).get('pjX27bKB') for uid in order]
    t.check('15a all four walk 1 -> 2 -> 4 (the boss point)', at == ['4'] * 4, at)
    ra = runs[t.a.user_id]
    hp0 = boss_hp(ra.info())
    ra.battle_start(4, boss=BOSS)
    hit = ra.new_event(1, f'{BOSS}:{BOSS_BODY}:10000')
    once, twice = ra.battle_end([hit], boss=BOSS), ra.battle_end([hit], boss=BOSS)
    t.check('15b a 10000 BattleEnd lands once; its replay adds nothing', hp0 > 10000
            and boss_hp(once) == boss_hp(twice) == hp0 - 10000, (hp0, boss_hp(once), boss_hp(twice)))
    for uid in order:
        runs[uid].battle_start(4, boss=BOSS)
    t.check('15c every member holds an open boss battle', all(runs[u].serial not in ('', '0') for u in order),
            [runs[u].serial for u in order])
    blows = {uid: runs[uid].new_event(1, f'{BOSS}:{BOSS_BODY}:{hp0}') for uid in order}
    ends = concurrently(lambda uid: runs[uid].battle_end([blows[uid]], boss=BOSS), order)
    state = ra.info()
    t.check('15d four simultaneous killing blows: none refused, boss HP 0, run won',
            not any(refused(x) for x in ends.values()) and boss_hp(state) == 0
            and first(state, 'g97TWISb').get('3tWuc7de') == '2', [x for x in ends.values() if refused(x)] or state)
    status = [mine(state, 'uwH62EZy', uid).get('j3g5P4cq') for uid in order]
    t.check('15e every member MissionStatus 2', status == ['2'] * 4, status)
    again = runs[t.b.user_id].battle_end([blows[t.b.user_id]], boss=BOSS)
    t.check('15f replaying a killing BattleEnd is harmless', not refused(again) and boss_hp(again) == 0, again)
    step_collect_later(t, runs, order)
    players = [t.a, t.b, t.d, t.f]
    credit = [num(records(p).get(BOSS_FINISH)) for p in players]
    killers = t.kill_credits(ra.rm)
    t.check('15g exactly one kill credit: BossFinishCnt over the four = 1 (and one defeated_by)', sum(credit) == 1
            and (killers is None or (len(killers) == 1 and killers[0] in order)), (credit, killers))
    wins = [num(records(p).get(WIN)) for p in players]
    t.check('15h clear side effects once: Win = 1 for each member', wins == [1] * 4, wins)


def step_mission(t: Ctx) -> None:
    runs = step_handover(t)
    step_shared_boss(t, runs)
    for run in runs.values():   # leave the world empty for the next run
        leave(run.p, run.room)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9965')
    ap.add_argument('--db', default='/deploy/gme.sqlite')
    ap.add_argument('--rounds', type=int, default=20, help='race rounds for step 8 (0 skips it)')
    ap.add_argument('--no-mission', action='store_true', help='skip 5.2 and step 15 (they need the mission band)')
    ap.add_argument('--keep-rooms', action='store_true', help='do not close the open rooms already in the DB')
    args = ap.parse_args()

    t = Ctx(args.url, args.db)
    if not args.keep_rooms:
        reset_world(t)
    step_empty_world(t)
    step_make(t)
    if not t.room:
        print(f'{t.failures} failure(s); no room was made, stopping')
        return 1
    for step in (step_search, step_enter, step_poll, step_chat, step_chat_filters, step_chat_rate,
                 step_ready, step_kick):
        step(t)
    if args.rounds > 0:
        step_race(t, args.rounds)
    for step in (step_promotion, step_absence, step_blacklist, step_edit, step_helper, step_close,
                 step_player_info):
        step(t)
    if not args.no_mission:
        step_mission(t)

    print(f'{t.failures} failure(s)')
    return 1 if t.failures else 0


if __name__ == '__main__':
    sys.exit(main())
