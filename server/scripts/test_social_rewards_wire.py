"""Wire test for the social-rewards fixes: friend gifts, lender Honor, Hunter Orbs.

Creates fresh accounts on a running test server:
  A. Friend gifts.  A and B are real friends, C is a stranger, D has only a
     pending request.  A's one FixGiftInfo sends to B, C, D, itself, a bad gift
     id and garbage: only B's arrives.  The reply carries A's sent row (that is
     what keeps B off A's Send list once the client wipes its own), a resend
     delivers nothing twice, A cannot claim what it sent, and B sees the gift
     under A's name next to the simulated friend's daily gift, claims it once
     and is paid once.
  B. Lender Honor.  B clears with A's unit as helper: A, the lender, earns the
     friend rate and the trophy counter, and sees it in UserInfo.  A second
     clear the same day pays A nothing more; a stranger's borrow pays the
     stranger rate; a lost battle, an NPC helper, a made-up id and lending to
     yourself pay no lender; the daily cap stops the credit.
  B2. Rows dated a LATER day than today (what a rollback past the 08:00 UTC
     day switch, or a clock step back, leaves behind) are never paid around:
     a gift to B dated tomorrow blocks today's send, and a lend row dated
     tomorrow counts for the pair limit and for the daily cap.
  C. Hunter Orbs.  A Frontier Hunter MissionStart spends one orb (reply and
     database), starts the refill clock, and a RESEND of the same request (same
     GmeHeader.client_id) is not charged again -- but only while the battle it
     paid for is still open: once settled, the same tag pays again.  At 0 orbs the start is refused
     with a soft Retry and changes nothing; the Survey Office (ChallengeBase)
     reports the spent count; orbs regenerate on the 3-hour clock; ShopUse
     type 5 refills them; an ordinary quest leaves them alone.

No reply may close the session: every refusal must be a soft one.

Usage: python3 -I scripts/test_social_rewards_wire.py [--url http://127.0.0.1:9969]
           [--container NAME] [--db /src/deploy/gme.sqlite]
The database is read from inside the server's container (docker exec), like
scripts/test_guilds_wire.py.  Creates throwaway accounts: never run it against
a save with real players.  Run it away from 08:00 UTC, when giftToday()'s day
turns (part B fills one day's lend cap).
"""
import argparse
import base64
import gzip
import json
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
FRIEND_APPLY = ('WUNi08YL', 'gbpdAEZuv8IP40UF')
FRIEND_AGREE = ('NYKBH4V6', '8PFVDiUIZyES2vzR')
GET_GIFT_INFO = ('ifYoPJ46', '6zHEYa9U')
FIX_GIFT_INFO = ('gLRIn74v', '15gTE9ft')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
CHALLENGE_BASE = ('nUAW2B0a', 'uE5Tsv6P')
SHOP_USE = ('xe8tiSf4', 'qthMXTQSkz3KfH9R')

GIFTS = '30uygM9m'
G_ID, G_FROM, G_TO, G_GIFT, G_TYPE, G_ITEM, G_POSS, G_FLAG, G_NAME, G_UNIT = (
    'gNE76SLp', 'yDiG31Fe', '1px2dbX3', 'eDj82kMN', '2bvT9hKt', 'kixHbe54', 'wgV86x1q',
    'fa6WwE9u', 'B5JQyV8j', 'pn16CNah')
ORBS, ORB_COUNT, ORB_TIMER = 'kN2i7qds', '38sHatGk', 'RHKA30s5'
SIMULATED_SENDER = 'n9ZMPC0t'
STORY_MISSION = 10          # needs 1 and 2 cleared; the test records those
HUNTER_MISSION = 1009700    # the active Frontier Hunter expedition's first quest
# giftToday()'s key in SQL (helper lends count days by it): the 08:00 UTC game
# day, numbered with the shift its switch recorded (GameDay.hpp).
GAME_DAY = ("date('now', '-8 hours', '+' || (SELECT value FROM server_state"
            " WHERE name = 'gift_day_shift') || ' days')")
HONOR_FRIEND, HONOR_STRANGER, LEND_CAP = 50, 10, 300
ORB_CAP, ORB_REGEN = 3, 10800

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
    """One encrypted request.  `tag` is GmeHeader.client_id: the client's request
    object id, the same on a retry and new for every new request."""
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
    return json.loads(plain[:-plain[-1]])


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

    def one(self, sql: str, *params):
        rows = self.rows(sql, *params)
        return rows[0] if rows else None

    def value(self, sql: str, *params):
        row = self.one(sql, *params)
        return row[0] if row else None


def container_on_port(url: str) -> str | None:
    port = urllib.parse.urlsplit(url).port
    out = subprocess.run(['docker', 'ps', '--filter', f'publish={port}', '--format', '{{.Names}}'],
                         capture_output=True, text=True, timeout=30).stdout.split()
    return out[0] if len(out) == 1 else None


class Player:
    def __init__(self, base: str, db: Database, label: str):
        self.base = base
        self.db = db
        self.label = label
        query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                        'dp': 'Android', 'dn': 'social-test', 'v': '16'})
        status, body = fetch(f'{base}/accounts/guest/login/?{query}')
        if status != 200:
            raise RuntimeError(f'guest login HTTP {status}')
        login = json.loads(body)
        node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
        self.name = f'S{label}{RUN}'
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
        # Story missions 1 and 2 cleared (mission 10 needs them), energy and gems to spend.
        for mission in ('1', '2'):
            db.rows("INSERT INTO user_campaign_missions (user_id, mission_id, state) VALUES (?, ?, 2)"
                    ' ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2', user_id, mission)
        db.rows('UPDATE user_info SET energy = 20000, energy_full_ts = 0, gems = 100 WHERE id = ?', user_id)

    def raw(self, request: tuple[str, str], body: dict | None = None, tag: str | None = None) -> dict:
        return gme_call(self.base, *request, dict(body or {}, IKqx1Cn9=self.login,
                                                  **{'6FrKacq7': [{'Kn51uR4Y': 'socialtest'}]}),
                        tag or secrets.token_hex(4))

    def call(self, request: tuple[str, str], body: dict | None = None, what: str = '',
             tag: str | None = None) -> dict:
        reply = self.raw(request, body, tag)
        err = wire_error(reply)
        check(f'{self.label}: {what or request[0]} answers without an error', err is None, err)
        return reply

    # --- helpers
    def honor(self) -> int:
        return self.db.value('SELECT friend_points FROM user_info WHERE id = ?', self.user_id)

    def honor_trophy(self) -> int:
        return self.db.value('SELECT COALESCE((SELECT friend_p_get FROM user_team_archive'
                             ' WHERE user_id = ?), 0)', self.user_id)

    def gifts(self, reply: dict) -> list:
        return reply.get(GIFTS) or []

    def fix_gift(self, send: str = '', receive: str = '', tag: str | None = None, what='FixGiftInfo') -> dict:
        return self.call(FIX_GIFT_INFO, {'Gg5c3eM6': [{'s2WnRw9N': '', '23Xi0jom': receive, 'qD74LPXb': send}]},
                         what, tag)

    def befriend(self, other: 'Player') -> None:
        self.call(FRIEND_APPLY, {'BPz1e5tU': [{'h7eY3sAK': other.user_id}]}, 'FriendApply')
        other.call(FRIEND_AGREE, {'BPz1e5tU': [{'h7eY3sAK': self.user_id}]}, 'FriendAgree')

    def start(self, mission: int, helper: str = '0', tag: str | None = None) -> dict:
        body = {'9Q1Lq5FS': [{'h7eY3sAK': helper, 'J3stQ7jd': '0', 'j28VNcUW': str(mission),
                              'jkldTrhL': '0', 'Z0Y4RoD7': '0', 'nA95Bdj6': '0',
                              '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
                'JzS3uxsZ': [{'0b4efi1W': '1'}]}
        return self.raw(MISSION_START, body, tag)

    def end(self, start_reply: dict, status: int = 2) -> dict:
        serial = start_reply['Kz7qfSs5'][0]['k9cxD7Ba']
        battle = {'47cJBUxz': '', 'Najhr8m6': '0', 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
                  'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
                  'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0'}
        return self.call(MISSION_END, {'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': str(status)}],
                                       'rXvA1E5y': [battle]}, 'MissionEnd')

    def clear_with(self, helper: str, status: int = 2) -> None:
        started = self.start(STORY_MISSION, helper)
        check(f'{self.label}: MissionStart with helper {helper} answers', wire_error(started) is None,
              wire_error(started))
        if wire_error(started) is None:
            self.end(started, status)

    def orbs(self) -> tuple[int, int]:
        row = self.db.one('SELECT hunter_orbs, hunter_orb_rest_ts FROM user_info WHERE id = ?', self.user_id)
        return row[0], row[1]

    def survey_office(self) -> dict:
        return (self.call(CHALLENGE_BASE, what='ChallengeBase').get(ORBS) or {})


def gift_payout(db: Database, player: Player, gift_type: int, item_id: str) -> int:
    if gift_type == 1:
        return db.value('SELECT zel FROM user_info WHERE id = ?', player.user_id)
    if gift_type == 2:
        return db.value('SELECT friend_points FROM user_info WHERE id = ?', player.user_id)
    if gift_type == 4:
        return db.value('SELECT karma FROM user_info WHERE id = ?', player.user_id)
    return db.value('SELECT COALESCE(SUM(item_num), 0) FROM user_items WHERE user_id = ? AND item_id = ?',
                    player.user_id, item_id)


def test_gifts(db: Database, a: Player, b: Player, c: Player, d: Player) -> None:
    print('\n-- A. friend gifts')
    # A's own list first: the simulated friend's gift gives a valid catalogue id.
    own = a.gifts(a.call(GET_GIFT_INFO, what='GetGiftInfo'))
    npc = [g for g in own if g.get(G_FROM) == SIMULATED_SENDER and g.get(G_TO) == a.user_id]
    check('A: the simulated friend\'s daily gift is in the list', len(npc) == 1, own)
    gift_id = npc[0][G_GIFT] if npc else '1'

    d.call(FRIEND_APPLY, {'BPz1e5tU': [{'h7eY3sAK': a.user_id}]}, 'FriendApply (left pending)')
    pairs = ','.join([f'{b.user_id}:{gift_id}', f'{c.user_id}:{gift_id}', f'{d.user_id}:{gift_id}',
                      f'{a.user_id}:{gift_id}', f'{b.user_id}:99999', 'garbage', ':5', f'{b.user_id}:'])
    reply = a.fix_gift(send=pairs, tag='gift-1')
    sent = [g for g in a.gifts(reply) if g.get(G_FROM) == a.user_id]
    check('A: the reply carries the gift list (30uygM9m)', GIFTS in reply, list(reply))
    check('A: the reply lists exactly one sent gift, to B', [g.get(G_TO) for g in sent] == [b.user_id], sent)
    check('A: the sent row is marked sent and carries the gift', sent and sent[0].get(G_FLAG) == '1'
          and sent[0].get(G_GIFT) == gift_id, sent)
    rows = db.rows('SELECT user_id, gift_id, received FROM user_gifts WHERE from_user_id = ?', a.user_id)
    check('DB: one gift row from A, to B, unclaimed', rows == [[b.user_id, int(gift_id), 0]], rows)

    a.fix_gift(send=pairs, tag='gift-1', what='FixGiftInfo resent')
    a.fix_gift(send=f'{b.user_id}:{gift_id}', what='FixGiftInfo second visit')
    count = db.value('SELECT COUNT(*) FROM user_gifts WHERE from_user_id = ?', a.user_id)
    check('A: a resend and a second visit the same day deliver nothing more', count == 1, count)
    again = a.gifts(a.call(GET_GIFT_INFO, what='GetGiftInfo'))
    check('A: GetGiftInfo keeps the sent row (B stays off the Send list after a relogin)',
          any(g.get(G_FROM) == a.user_id and g.get(G_TO) == b.user_id for g in again), again)

    row_id = db.value('SELECT id FROM user_gifts WHERE from_user_id = ? AND user_id = ?', a.user_id, b.user_id)
    a.fix_gift(receive=str(row_id), what='FixGiftInfo claiming its own sent gift')
    check('A: cannot claim the gift it sent',
          db.value('SELECT received FROM user_gifts WHERE id = ?', row_id) == 0)

    inbox = b.gifts(b.call(GET_GIFT_INFO, what='GetGiftInfo'))
    from_a = [g for g in inbox if g.get(G_FROM) == a.user_id and g.get(G_TO) == b.user_id]
    check('B: A\'s gift is in B\'s list', len(from_a) == 1, inbox)
    check('B: drawn under A\'s name', from_a and from_a[0].get(G_NAME) == a.name, from_a)
    check('B: with A\'s leader as the thumbnail', from_a and int(from_a[0].get(G_UNIT, 0)) > 0, from_a)
    check('B: the simulated friend\'s gift still came although A\'s landed first',
          any(g.get(G_FROM) == SIMULATED_SENDER for g in inbox), inbox)

    if from_a:
        gift = from_a[0]
        gift_type, item, amount = int(gift[G_TYPE]), gift[G_ITEM], int(gift[G_POSS])
        before = gift_payout(db, b, gift_type, item)
        after_claim = b.gifts(b.fix_gift(receive=gift[G_ID], tag='claim-1', what='FixGiftInfo claim'))
        paid = gift_payout(db, b, gift_type, item)
        check(f'B: claiming pays the gift once (type {gift_type}, +{amount})', paid - before == amount,
              (before, paid))
        check('B: the claimed gift leaves B\'s list', not any(g.get(G_ID) == gift[G_ID] and g.get(G_TO) == b.user_id
                                                            for g in after_claim), after_claim)
        b.fix_gift(receive=gift[G_ID], tag='claim-1', what='FixGiftInfo claim resent')
        check('B: a resent claim pays nothing more', gift_payout(db, b, gift_type, item) == paid)


def test_lender_honor(db: Database, a: Player, b: Player, c: Player, d: Player, e: Player) -> None:
    print('\n-- B. Honor for the lender')
    a0, trophy0, b0 = a.honor(), a.honor_trophy(), b.honor()
    b.clear_with(a.user_id)
    check(f'A (lender): +{HONOR_FRIEND} Honor when friend B clears with A\'s unit', a.honor() - a0 == HONOR_FRIEND,
          (a0, a.honor()))
    check('A (lender): the Honor trophy counter (friend_p_get) rises too',
          a.honor_trophy() - trophy0 == HONOR_FRIEND, (trophy0, a.honor_trophy()))
    check(f'B (borrower): still +{HONOR_FRIEND} as before', b.honor() - b0 == HONOR_FRIEND, (b0, b.honor()))
    # Read before the call: the day's first UserInfo also pays the login
    # calendar (LoginCampaign.cpp), after its team_info has been built.
    lent = a.honor()
    team = (a.call(USER_INFO, what='UserInfo').get('fEi17cnx') or [{}])[0]
    check('A: UserInfo shows the new balance (J3stQ7jd)', int(team.get('J3stQ7jd', -1)) == lent,
          (team.get('J3stQ7jd'), lent))

    a1 = a.honor()
    b.clear_with(a.user_id)
    check('A: a second clear by B the same day pays the lender nothing', a.honor() == a1, (a1, a.honor()))

    c.clear_with(a.user_id)
    check(f'A: a stranger\'s borrow pays the stranger rate (+{HONOR_STRANGER})', a.honor() - a1 == HONOR_STRANGER,
          (a1, a.honor()))

    a2 = a.honor()
    d.clear_with(a.user_id, status=3)
    check('A: a lost battle pays the lender nothing', a.honor() == a2, (a2, a.honor()))
    lends = db.value('SELECT COUNT(*) FROM user_helper_lends WHERE user_id = ? AND borrower_id = ?',
                     a.user_id, d.user_id)
    check('DB: and records no lend', lends == 0, lends)

    e0 = e.honor()
    npc = db.value('SELECT friend_id FROM user_friends WHERE user_id = ? LIMIT 1', e.user_id) or 'DCF00001'
    e.clear_with(npc)
    e.clear_with('notaplayer')
    check('an NPC or made-up helper credits no lender row',
          db.value('SELECT COUNT(*) FROM user_helper_lends WHERE borrower_id = ?', e.user_id) == 0)
    # The NPC friend pays the friend rate; a made-up helper is recorded as solo
    # (MissionStart's recordableHelper) and pays nothing.
    check(f'E: the borrower is paid for the NPC helper (+{HONOR_FRIEND}), not the made-up one',
          e.honor() - e0 == HONOR_FRIEND, (e0, e.honor()))

    a.clear_with(a.user_id)
    check('lending to yourself records no lend',
          db.value('SELECT COUNT(*) FROM user_helper_lends WHERE user_id = ? AND borrower_id = ?',
                   a.user_id, a.user_id) == 0)

    # The daily cap: fill A's day to 5 short of it, then two more borrowers.
    used = db.value('SELECT COALESCE(SUM(honor), 0) FROM user_helper_lends WHERE user_id = ?'
                    f" AND day = {GAME_DAY}", a.user_id)
    filler = LEND_CAP - 5 - used
    db.rows("INSERT INTO user_helper_lends (user_id, borrower_id, day, honor, created_at)"
            f" VALUES (?, 'capfill', {GAME_DAY}, ?, 0)", a.user_id, filler)
    a4 = a.honor()
    e.clear_with(a.user_id)
    check('A: the borrow that reaches the daily cap pays only what is left (+5)', a.honor() - a4 == 5,
          (a4, a.honor()))
    a5 = a.honor()
    f = Player(a.base, db, 'F')
    f.clear_with(a.user_id)
    check(f'A: past the cap ({LEND_CAP}/day) a borrow pays the lender nothing', a.honor() == a5, (a5, a.honor()))
    total = db.value("SELECT SUM(honor) FROM user_helper_lends WHERE user_id = ?"
                     f" AND day = {GAME_DAY}", a.user_id)
    check('DB: the day\'s lends add up to the cap', total == LEND_CAP, total)


def test_later_day_guards(db: Database, a: Player, b: Player) -> None:
    print('\n-- B2. rows dated a later day are not paid around')
    tomorrow = f"date({GAME_DAY}, '+1 day')"

    gift_id = db.value('SELECT gift_id FROM user_gifts WHERE from_user_id = ? AND user_id = ?', a.user_id, b.user_id)
    db.rows(f'UPDATE user_gifts SET day = {tomorrow} WHERE from_user_id = ? AND user_id = ?', a.user_id, b.user_id)
    a.fix_gift(send=f'{b.user_id}:{gift_id}', what='FixGiftInfo after a gift dated tomorrow')
    count = db.value('SELECT COUNT(*) FROM user_gifts WHERE from_user_id = ? AND user_id = ?', a.user_id, b.user_id)
    check('A: a gift to B dated a later day blocks today\'s send', count == 1, count)

    lender, borrower = Player(a.base, db, 'L'), Player(a.base, db, 'M')
    db.rows(f'INSERT INTO user_helper_lends (user_id, borrower_id, day, honor, created_at)'
            f' VALUES (?, ?, {tomorrow}, ?, 0)', lender.user_id, borrower.user_id, HONOR_STRANGER)
    before = lender.honor()
    borrower.clear_with(lender.user_id)
    check('L: a lend to M dated a later day pays no second credit today', lender.honor() == before,
          (before, lender.honor()))
    check('DB: and adds no row for today',
          db.value('SELECT COUNT(*) FROM user_helper_lends WHERE user_id = ? AND borrower_id = ?',
                   lender.user_id, borrower.user_id) == 1)

    capped, other = Player(a.base, db, 'N'), Player(a.base, db, 'O')
    db.rows(f"INSERT INTO user_helper_lends (user_id, borrower_id, day, honor, created_at)"
            f" VALUES (?, 'capfill', {tomorrow}, ?, 0)", capped.user_id, LEND_CAP)
    before = capped.honor()
    other.clear_with(capped.user_id)
    check('N: a full cap dated a later day still stops the credit', capped.honor() == before,
          (before, capped.honor()))


def test_hunter_orbs(db: Database, a: Player) -> None:
    print('\n-- C. Hunter Orbs')
    office = a.survey_office()
    check(f'Survey Office: a fresh account holds {ORB_CAP} orbs', office.get(ORB_COUNT) == str(ORB_CAP), office)

    before = time.time()
    first = a.start(HUNTER_MISSION, tag='fh-1')
    check('Frontier Hunter MissionStart answers without an error', wire_error(first) is None, wire_error(first))
    check('the reply carries the orbs after the charge (2)', (first.get(ORBS) or {}).get(ORB_COUNT) == '2',
          first.get(ORBS))
    check('the reply carries the refill timer (~3 h)',
          ORB_REGEN - 5 <= int((first.get(ORBS) or {}).get(ORB_TIMER, -1)) <= ORB_REGEN, first.get(ORBS))
    orbs, rest = a.orbs()
    check('DB: one orb spent', orbs == ORB_CAP - 1, orbs)
    check('DB: the refill clock started', before + ORB_REGEN - 5 <= rest <= time.time() + ORB_REGEN + 5, rest)

    resent = a.start(HUNTER_MISSION, tag='fh-1')
    check('a RESEND of the same start answers', wire_error(resent) is None, wire_error(resent))
    check('a resend is not charged again', a.orbs()[0] == ORB_CAP - 1, a.orbs())
    check('the resend reply still shows 2', (resent.get(ORBS) or {}).get(ORB_COUNT) == '2', resent.get(ORBS))
    a.end(resent)

    check('Survey Office: the count stays spent (2), not full again',
          a.survey_office().get(ORB_COUNT) == '2')

    # The tag only covers the battle it paid for: once that battle has settled,
    # re-using it (a crafted client, or a relaunch that reuses the id) pays again.
    replay = a.start(HUNTER_MISSION, tag='fh-1')
    check('re-using a settled start\'s tag is a new entry and costs an orb',
          wire_error(replay) is None and a.orbs()[0] == 1, a.orbs())
    check('spending while refilling keeps the pending orb\'s deadline', a.orbs()[1] == rest, (rest, a.orbs()))
    a.end(replay)
    third = a.start(HUNTER_MISSION, tag='fh-3')
    check('and the last one', wire_error(third) is None and a.orbs()[0] == 0, a.orbs())

    serials = db.value('SELECT COUNT(*) FROM user_mission_runs WHERE user_id = ?', a.user_id)
    refused = a.start(HUNTER_MISSION, tag='fh-4')
    err = wire_error(refused) or {}
    check('at 0 orbs the start is refused', bool(err), refused)
    check('as a soft Retry (cmd 2), not a session close', str(err.get('iPD12YCr')) == '2', err)
    check('the refusal says why', 'Hunter Orb' in str(err.get('ZC0msu2L', '')), err)
    check('a refused start changes nothing (orbs, runs)', a.orbs()[0] == 0 and db.value(
        'SELECT COUNT(*) FROM user_mission_runs WHERE user_id = ?', a.user_id) == serials, a.orbs())
    again = a.start(HUNTER_MISSION, tag='fh-3')
    check('a resend of the start that took the last orb still goes through', wire_error(again) is None,
          wire_error(again))
    check('and opens its battle for the result', db.value('SELECT open_mission_serial FROM user_info WHERE id = ?',
          a.user_id) == int(again['Kz7qfSs5'][0]['k9cxD7Ba']) if wire_error(again) is None else False)
    a.end(again)
    replayed = a.start(HUNTER_MISSION, tag='fh-3')
    check('after that battle settles, re-using its tag at 0 orbs is refused',
          str((wire_error(replayed) or {}).get('iPD12YCr')) == '2' and a.orbs()[0] == 0, (replayed, a.orbs()))
    check('Survey Office: 0 orbs with the refill timer running', (lambda o: o.get(ORB_COUNT) == '0'
          and 0 < int(o.get(ORB_TIMER, 0)) <= ORB_REGEN)(a.survey_office()))

    # Regeneration: the pending orb was due 3 h + 5 s ago, so it and one more have landed.
    now = int(time.time())
    db.rows('UPDATE user_info SET hunter_orbs = 0, hunter_orb_rest_ts = ? WHERE id = ?',
            now - ORB_REGEN - 5, a.user_id)
    office = a.survey_office()
    check('orbs regenerate on the 3-hour clock (2 after 6 h)', office.get(ORB_COUNT) == '2', office)
    check('with the next one due in under 3 h', 0 < int(office.get(ORB_TIMER, 0)) < ORB_REGEN, office)

    db.rows('UPDATE user_info SET hunter_orbs = 0, hunter_orb_rest_ts = ? WHERE id = ?', now + 600, a.user_id)
    gems = db.value('SELECT gems FROM user_info WHERE id = ?', a.user_id)
    shop = a.call(SHOP_USE, {'32ibWjFG': [{'60IsqxDt': '5', '03UGMHxF': '1', '5gXxT7LZ': '0'}]},
                  'ShopUse type 5 (restore Hunter Orbs)')
    check('ShopUse type 5 refills to the cap', (shop.get(ORBS) or {}).get(ORB_COUNT) == str(ORB_CAP)
          and a.orbs() == (ORB_CAP, 0), (shop.get(ORBS), a.orbs()))
    check('for one Gem', gems - db.value('SELECT gems FROM user_info WHERE id = ?', a.user_id) == 1)

    story = a.start(STORY_MISSION)
    check('an ordinary quest leaves the orbs alone', wire_error(story) is None and a.orbs()[0] == ORB_CAP
          and ORBS not in story, (a.orbs(), list(story)[:5]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9969')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)

    a, b, c, d, e = (Player(args.url, db, label) for label in 'ABCDE')
    print(f'accounts: A={a.user_id} B={b.user_id} C={c.user_id} D={d.user_id} E={e.user_id}')
    a.befriend(b)

    test_gifts(db, a, b, c, d)
    test_lender_honor(db, a, b, c, d, e)
    test_later_day_guards(db, a, b)
    test_hunter_orbs(db, a)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
