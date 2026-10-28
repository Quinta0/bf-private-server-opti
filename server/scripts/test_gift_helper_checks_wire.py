"""Wire test for two social checks: wanted-gift weighting and the MissionStart helper id.

Creates fresh accounts on a running test server:
  A. Wanted gifts.  want_gift is the client's three-slot comma list ("a,b,c"),
     and every wish in it is weighted kWantedGiftWeight (4) times in the
     simulated friend's daily roll.  The roll is deterministic per (user, day):
     its draw index depends only on the pool size, so re-minting today's gift
     under different three-wish lists must land exactly where the weighted
     pool says.  Wishing for the first three gifts and for the last three
     gives different drops (the old whole-string compare never matched, so
     both drew from the same unweighted pool).  Read as the gift screen reads
     it: "0" or a blank is an empty slot, an unknown id drops out and the next
     moves up, only three slots count, a repeat is weighted once, order does
     not matter; no wish, "0,0,0" and unknown ids are all the unweighted roll.
     Every check runs on 8 fresh accounts, each drawing its own index.
  B. Helper ids.  MissionStart records 9Q1Lq5FS.h7eY3sAK for MissionEnd's
     Honor only when it names someone borrowable: a real player other than
     the borrower, or an NPC (roster friend, developer, stranger) that
     gme::helperLeader resolves.  Anything else -- made up, yourself, too
     long, a NUL inside, the wrong case, the retired synthetic helper, a
     stranger id naming no unit, a story guest's handle name -- is recorded as
     solo over whatever was on record, and pays no Honor.  No start is refused.

Usage: python3 -I scripts/test_gift_helper_checks_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /src/deploy/gme.sqlite]
           [--catalogue /src/deploy/mst/gift_item_mst.json]
The database and the gift catalogue are read from inside the server's
container (docker exec), like scripts/test_social_rewards_wire.py.  Creates
throwaway accounts: never run it against a save with real players.  Run it
away from 08:00 UTC, when giftToday()'s day turns: part A re-rolls one day's
gift.
"""
import argparse
import base64
import gzip
import json
import secrets
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
FRIEND_GET = ('2o4axPIC', 'EoYuZ2nbImhCU1c0')
FRIEND_APPLY = ('WUNi08YL', 'gbpdAEZuv8IP40UF')
FRIEND_AGREE = ('NYKBH4V6', '8PFVDiUIZyES2vzR')
GET_GIFT_INFO = ('ifYoPJ46', '6zHEYa9U')
FIX_GIFT_INFO = ('gLRIn74v', '15gTE9ft')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')

GIFTS, G_FROM, G_TO, G_GIFT = '30uygM9m', 'yDiG31Fe', '1px2dbX3', 'eDj82kMN'
TEAM_INFO, WANT_GIFT = 'fEi17cnx', 's2WnRw9N'
PICKER = 'xZH6EIQ7'
SIMULATED_SENDER = 'n9ZMPC0t'
STORY_MISSION = 10          # needs 1 and 2 cleared; the test records those
HONOR_FRIEND, HONOR_STRANGER = 50, 10
WANTED_WEIGHT, WISH_SLOTS = 4, 3
# giftToday()'s key in SQL: the 08:00 UTC game day, numbered with the shift
# its switch recorded (GameDay.hpp), not the UTC date.
TODAY = ("date('now', '-8 hours', '+' || (SELECT value FROM server_state"
         " WHERE name = 'gift_day_shift') || ' days')")

RUN = secrets.token_hex(2)
failures = 0
checks = 0


def check(label: str, cond: bool, detail='') -> None:
    global failures, checks
    checks += 1
    print(('PASS  ' if cond else 'FAIL  ') + label + ('' if cond else f'  -- {detail}'))
    failures += 0 if cond else 1


def fetch(url: str, data: bytes | None = None) -> tuple[int, bytes]:
    req = urllib.request.Request(url, data, {'Content-Type': 'application/json'} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            body = r.read()
            if r.headers.get('Content-Encoding') == 'gzip':
                body = gzip.decompress(body)
            return r.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def aes_ecb(key: str, data: bytes, decrypt: bool) -> bytes:
    hexkey = key.encode().ljust(16, b'\0')[:16].hex()
    cmd = ['openssl', 'enc', '-aes-128-ecb', '-K', hexkey, '-nopad'] + (['-d'] if decrypt else [])
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def gme_call(base: str, group: str, key: str, body: dict, tag: str) -> dict:
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    envelope = json.dumps({
        'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': tag},
        'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(aes_ecb(key, raw + bytes([pad]) * pad, False)).decode()},
    }).encode()
    status, data = fetch(f'{base}/bf/gme/action.php', envelope)
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = aes_ecb(key, base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']), True)
    try:
        return json.loads(plain[:-plain[-1]])
    except json.JSONDecodeError:
        # A success reply that echoes a NUL the request carried: the server's
        # reply encryption stops at it, so the JSON is cut short.  Still not an
        # error reply (those are unencrypted).
        return {'_truncated': plain[:-plain[-1]][-80:].decode(errors='replace')}


def wire_error(reply: dict) -> dict | None:
    if '_unencrypted' not in reply:
        return None
    return reply['_unencrypted'].get('b5PH6mZa') or {'raw': json.dumps(reply['_unencrypted'])[:200]}


_IN_CONTAINER_SQL = r'''
import json, sqlite3, sys
req = json.load(sys.stdin)
con = sqlite3.connect(req["path"], timeout=30)
rows = con.execute(req["sql"], req["params"]).fetchall()
con.commit()
print(json.dumps(rows))
'''


class Database:
    def __init__(self, container: str, path: str):
        self.container = container
        self.path = path

    def rows(self, sql: str, *params) -> list:
        done = subprocess.run(
            ['docker', 'exec', '-i', self.container, 'python3', '-c', _IN_CONTAINER_SQL],
            input=json.dumps({'path': self.path, 'sql': sql, 'params': list(params)}),
            capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f'SQL in {self.container} failed: {done.stderr.strip()[-300:]}')
        return json.loads(done.stdout)

    def value(self, sql: str, *params):
        rows = self.rows(sql, *params)
        return rows[0][0] if rows else None


def container_on_port(url: str) -> str | None:
    port = urllib.parse.urlsplit(url).port
    out = subprocess.run(['docker', 'ps', '--filter', f'publish={port}', '--format', '{{.Names}}'],
                         capture_output=True, text=True, timeout=30).stdout.split()
    return out[0] if len(out) == 1 else None


def gift_catalogue(container: str, path: str) -> list[str]:
    """Gift ids in the order the server loads them (ServerCache, gift_item_mst.json)."""
    done = subprocess.run(['docker', 'exec', container, 'cat', path], capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f'cannot read {path} in {container}: {done.stderr.strip()[-200:]}')
    return [row['eDj82kMN'] for row in json.loads(done.stdout)['Bm1WNDQ0']]


class Player:
    def __init__(self, base: str, db: Database, label: str):
        self.base = base
        self.db = db
        self.label = label
        query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                        'dp': 'Android', 'dn': 'gifthelper-test', 'v': '16'})
        status, body = fetch(f'{base}/accounts/guest/login/?{query}')
        if status != 200:
            raise RuntimeError(f'guest login HTTP {status}')
        login = json.loads(body)
        node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
        self.name = f'G{label}{RUN}'
        created = gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=self.name)],
                                                '9u45RGcV': [{'Kn51uR4Y': '1'}]}, secrets.token_hex(4))
        if wire_error(created) is not None:
            raise RuntimeError(f'CreateUser refused: {wire_error(created)}')
        init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
        user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
        if not user_id:
            raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
        self.login = [dict(node, h7eY3sAK=user_id)]
        self.user_id = user_id
        for mission in ('1', '2'):
            db.rows("INSERT INTO user_campaign_missions (user_id, mission_id, state) VALUES (?, ?, 2)"
                    ' ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2', user_id, mission)
        db.rows('UPDATE user_info SET energy = 20000, energy_full_ts = 0 WHERE id = ?', user_id)

    def raw(self, request: tuple[str, str], body: dict | None = None) -> dict:
        return gme_call(self.base, *request, dict(body or {}, IKqx1Cn9=self.login,
                                                  **{'6FrKacq7': [{'Kn51uR4Y': 'gifthelpertest'}]}),
                        secrets.token_hex(4))

    def call(self, request: tuple[str, str], body: dict | None = None, what: str = '') -> dict:
        reply = self.raw(request, body)
        err = wire_error(reply)
        check(f'{self.label}: {what or request[0]} answers without an error', err is None, err)
        return reply

    def honor(self) -> int:
        return self.db.value('SELECT friend_points FROM user_info WHERE id = ?', self.user_id)

    def recorded_helper(self) -> str:
        return self.db.value('SELECT reinforce_user_id FROM user_info WHERE id = ?', self.user_id)

    def befriend(self, other: 'Player') -> None:
        self.call(FRIEND_APPLY, {'BPz1e5tU': [{'h7eY3sAK': other.user_id}]}, 'FriendApply')
        other.call(FRIEND_AGREE, {'BPz1e5tU': [{'h7eY3sAK': self.user_id}]}, 'FriendAgree')

    def start(self, helper: str) -> dict:
        body = {'9Q1Lq5FS': [{'h7eY3sAK': helper, 'J3stQ7jd': '0', 'j28VNcUW': str(STORY_MISSION),
                              'jkldTrhL': '0', 'Z0Y4RoD7': '0', 'nA95Bdj6': '0',
                              '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
                'JzS3uxsZ': [{'0b4efi1W': '1'}]}
        return self.raw(MISSION_START, body)

    def end(self, start_reply: dict) -> dict:
        serial = (start_reply['Kz7qfSs5'][0]['k9cxD7Ba'] if 'Kz7qfSs5' in start_reply else
                  self.db.value('SELECT open_mission_serial FROM user_info WHERE id = ?', self.user_id))
        battle = {'47cJBUxz': '', 'Najhr8m6': '0', 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
                  'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
                  'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0'}
        return self.call(MISSION_END, {'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}],
                                       'rXvA1E5y': [battle]}, 'MissionEnd')

    # --- the wanted-gift roll
    def reroll(self, want: str | None) -> str | None:
        """Forget today's simulated gift and mint it again under `want`.

        A string goes through FixGiftInfo's s2WnRw9N, as the client sends it, and
        that reply mints the gift.  None clears want_gift in the database (the
        client never sends an empty one) and mints through GetGiftInfo."""
        self.db.rows(f'DELETE FROM user_gifts WHERE user_id = ? AND from_user_id = ? AND day = {TODAY}',
                     self.user_id, SIMULATED_SENDER)
        if want is None:
            self.db.rows("UPDATE user_info SET want_gift = '' WHERE id = ?", self.user_id)
            reply = self.raw(GET_GIFT_INFO)
        else:
            reply = self.raw(FIX_GIFT_INFO, {'Gg5c3eM6': [{'s2WnRw9N': want, '23Xi0jom': '', 'qD74LPXb': ''}]})
        if wire_error(reply) is not None:
            check(f'{self.label}: gift reroll under {want!r} answers', False, wire_error(reply))
            return None
        npc = [g for g in reply.get(GIFTS) or []
               if g.get(G_FROM) == SIMULATED_SENDER and g.get(G_TO) == self.user_id]
        return npc[-1][G_GIFT] if npc else None


def weighted_pool(catalogue: list[str], wanted: list[str]) -> list[str]:
    """The server's pool: catalogue order, each wish in place WANTED_WEIGHT times."""
    pool = []
    for gift in catalogue:
        pool += [gift] * (WANTED_WEIGHT if gift in wanted else 1)
    return pool


class Tally:
    """One PASS/FAIL line per check, over every account: one account cannot vouch alone.

    Each account draws one index per pool size for the day, and two lists that
    happen to share a pool size meet at the same index, so a wrong parser
    agrees with the right one on some accounts by luck.  Across 8 accounts that
    luck runs out (simulated: under 1% for a missing dedupe, a wrong weight, an
    unknown id taking a slot, a first-wish-only parser; 0% for the old bug)."""

    def __init__(self):
        self.results: dict[str, list] = {}

    def add(self, label: str, ok: bool, detail='') -> None:
        self.results.setdefault(label, []).append((ok, detail))

    def report(self) -> None:
        for label, results in self.results.items():
            bad = [detail for ok, detail in results if not ok]
            check(f'{label} ({len(results) - len(bad)}/{len(results)} accounts)', not bad, bad[:3])


def test_wanted_gifts(base: str, db: Database, catalogue: list[str], accounts: int) -> None:
    print('\n-- A. wanted gifts')
    check('the catalogue has ten gifts, enough for disjoint wish lists', len(catalogue) >= 2 * WISH_SLOTS + 1,
          catalogue)
    c = catalogue
    front, back = c[:3], c[-3:]
    three_wish_lists = [front, back, c[3:6], [c[0], c[4], c[9]], [c[2], c[5], c[8]], [c[1], c[6], c[7]]]
    size = len(catalogue) + WISH_SLOTS * (WANTED_WEIGHT - 1)
    b0, b1, b2 = back
    one = c[4]
    tally = Tally()
    movable = 0

    for n in range(1, accounts + 1):
        p = Player(base, db, f'W{n}')
        team = (p.call(USER_INFO, what='UserInfo').get(TEAM_INFO) or [{}])[0]
        tally.add('UserInfo pads want_gift with the catalogue\'s first three, in the server\'s order',
                  team.get(WANT_GIFT) == ','.join(front), (team.get(WANT_GIFT), front))

        # Every three-wish list makes a pool of the same size, so all of them
        # are read at the one index this account draws today.
        got = {tuple(w): p.reroll(','.join(w)) for w in three_wish_lists}
        index = set(range(size))
        for w in three_wish_lists:
            index &= {i for i, g in enumerate(weighted_pool(catalogue, w)) if g == got[tuple(w)]}
        tally.add(f'six three-wish lists all land on one draw from the weighted {size}-entry pool',
                  bool(index), (p.label, list(got.values())))
        tally.add('FixGiftInfo stores the list as sent', db.value(
            'SELECT want_gift FROM user_info WHERE id = ?', p.user_id) == ','.join(three_wish_lists[-1]))
        # Index 0 and the last index name the first and last gift whatever the
        # wishes: on those accounts no wish list can move the drop.
        got_front, got_back = got[tuple(front)], got[tuple(back)]
        if index and not index <= {0, size - 1}:
            movable += 1
            tally.add('wishing for the first three and for the last three changes the drop',
                      got_front != got_back, (p.label, got_front, got_back))
        tally.add('the same list the same day gives the same gift', p.reroll(','.join(front)) == got_front)

        # Read as the gift screen reads it: an unknown id is dropped and the
        # next piece moves up; blanks before a number are skipped, and digits
        # count up to the first non-digit.
        tally.add('unknown ids drop out and the next wish moves up',
                  p.reroll(f' {b0} ,99999, {b1},-5,{b2}x,{b0}') == got_back)
        tally.add('the order of the wishes does not matter', p.reroll(f'{b2},{b0},{b1}') == got_back)
        tally.add('only the first three slots count (a list of the whole catalogue)',
                  p.reroll(','.join(catalogue)) == got_front)

        unweighted = p.reroll(None)
        tally.add('no want_gift draws from the unweighted pool', unweighted in catalogue, unweighted)
        tally.add('"0,0,0" is three empty slots, no wish', p.reroll('0,0,0') == unweighted)
        tally.add('ids the catalogue lacks are no wish', p.reroll('99999,-5') == unweighted)
        tally.add('a fourth piece after three empty slots is no wish', p.reroll(f'0,,abc,{b0}') == unweighted)

        single = p.reroll(one)
        tally.add('one wish in any slot is the same wish',
                  p.reroll(f'0,{one},0') == single and p.reroll(f'abc,0,{one}') == single, single)
        tally.add('a gift in two slots is weighted once', p.reroll(f'{one},{one},0') == single, single)
        pair = p.reroll(f'{b0},0,{b1}')
        tally.add('two wishes and a repeat are the same two wishes', p.reroll(f'{b1},{b0},{b0}') == pair, pair)

    tally.report()
    check(f'most accounts drew an index the wishes can move ({movable}/{accounts})', movable >= accounts // 2)


def test_helper_ids(base: str, db: Database) -> None:
    print('\n-- B. helper ids')
    b, friend, stranger = (Player(base, db, label) for label in ('B', 'F', 'S'))
    b.befriend(friend)

    picker = b.call(FRIEND_GET, what='FriendGet').get(PICKER) or []
    npc_stranger = next((h['h7eY3sAK'] for h in picker if str(h.get('h7eY3sAK', '')).startswith('NEW')), None)
    check('B: the picker offers an NPC stranger', npc_stranger is not None, [h.get('h7eY3sAK') for h in picker])
    npc_friend = db.value('SELECT friend_id FROM user_friends WHERE user_id = ? ORDER BY friend_id LIMIT 1',
                          b.user_id)
    check('B: the roster is seeded', bool(npc_friend))

    # (helper sent, recorded, Honor for the clear)
    cases = [
        (friend.user_id, friend.user_id, HONOR_FRIEND, 'a real friend'),
        ('notaplayer', '', 0, 'a made-up id'),
        (stranger.user_id, stranger.user_id, HONOR_STRANGER, 'a real stranger'),
        (b.user_id, '', 0, 'the borrower themself'),
        ('0', '', 0, 'a solo run ("0")'),
        (npc_friend or 'DCF00001', npc_friend or 'DCF00001', HONOR_FRIEND, 'an NPC roster friend'),
        ('', '', 0, 'a solo run ("")'),
        (npc_stranger or 'NEW00010011', npc_stranger or 'NEW00010011', HONOR_STRANGER, 'an NPC stranger'),
        ('DEVEVAN', 'DEVEVAN', HONOR_STRANGER, 'a developer met in the picker'),
        ('NEW99999999', '', 0, 'a stranger id naming no unit'),
        ('DCF99999', '', 0, 'an NPC friend id not on the roster'),
        (SIMULATED_SENDER, '', 0, 'the retired synthetic helper'),
        ('Q' * 40, '', 0, 'an overlong id'),
        (b.user_id + '\0', '', 0, 'the borrower\'s own id with a NUL after it'),
        (friend.user_id + '\0x', '', 0, 'a real id with a NUL and more after it'),
        (friend.user_id.swapcase(), '', 0, 'a real id in the wrong case'),
        # Story guests (FixedReinforcementInfo): the client sends the handle
        # name, or the unit id when UnitMst has no name.  No Honor by decision.
        ('Fencer Vargas', '', 0, 'a story guest (handle name)'),
        ('Selena', '', 0, 'a story guest (one-word handle name)'),
        ('10011', '', 0, 'a story guest (unit id fallback)'),
    ]
    for sent, recorded, honor, what in cases:
        # A real friend already on record: a start that skipped the write would
        # leave them there and the clear would pay the friend rate.
        db.rows('UPDATE user_info SET reinforce_user_id = ? WHERE id = ?', friend.user_id, b.user_id)
        before = b.honor()
        started = b.start(sent)
        check(f'MissionStart with {what} answers without an error', wire_error(started) is None,
              wire_error(started))
        if wire_error(started) is not None:
            continue
        check(f'  records {recorded or "no helper"!r}, replacing the one on record', b.recorded_helper() == recorded,
              (sent[:40], b.recorded_helper()))
        b.end(started)
        check(f'  and the clear pays {honor} Honor', b.honor() - before == honor, (before, b.honor()))

    check('MissionEnd cleared the record', b.recorded_helper() == '', b.recorded_helper())
    lends = db.value('SELECT COUNT(*) FROM user_helper_lends WHERE borrower_id = ?', b.user_id)
    check('only the two real helpers were credited as lenders', lends == 2, lends)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/src/deploy/gme.sqlite')
    ap.add_argument('--catalogue', default='/src/deploy/mst/gift_item_mst.json')
    ap.add_argument('--accounts', type=int, default=8, help='accounts part A rolls on (see Tally)')
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)

    test_wanted_gifts(args.url, db, gift_catalogue(container, args.catalogue), args.accounts)
    test_helper_ids(args.url, db)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
