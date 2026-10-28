"""Wire test for fair summoning: the test gates are gone, only the gates the
rail offers can be pulled, and pulls cost what the original game charged.

Checks, against a running server and its save database:
  A. GachaList and UserInfo carry no test gate (99005-99008).  Both list the
     tutorial gate 2000 while the stored end flag is 0 -- the client's own
     "in the tutorial" test -- and hide it once it is 1, also for a row left
     at chapter 50 with the flag set (what DebugCli unlock leaves).
  B. GachaAction refuses, each by its own rule: the removed test gates and
     gates without a pool, gate 2000 after the tutorial, a selector gate, more
     pulls than the Multi Summon cap (also a count whose 32-bit cost would
     wrap), an unknown payment code, Summon Tickets on an Honor gate, plain
     tickets on a gate with its own V2 tickets, and pulls the player cannot
     afford.  Refusals charge and grant nothing.  A gate the archive pools but
     no rail tile names must be refused by the rail rule; the shipped data has
     none, so that check needs a QA --archive.
  C. Costs, by the payment code `324b023k` the client sends (0 currency, 1
     plain Summon Ticket, 2 the gate's own V2 ticket): need_gems x count on a
     gem gate, need_friend_point x count on the Honor gate, one Brave Summon
     Ticket (V2 type 8) per pull on 11960 with no top-up from the plain
     counter, one plain ticket per pull on gate 2000 (the only rail gate with
     no V2 types), and the tuto15 pull (chapter 10, count 0) at need_gems.
  D. Races: eight pulls fired together at a balance that covers one -- gems,
     Brave Summon Tickets, plain tickets -- over several rounds: each round
     exactly one is paid and one unit granted.  (The V2 debit's own guard is
     defensive: with one database connection its read and write are already
     in one transaction, so this checks the outcome, not that guard.)
  E. Over many pulls the rarity split matches the gate's pool weights and the
     unit-type split matches F_UNIT_TYPE_MST (23/23/22/22/10, Rex 0), by a
     chi-square test at p = 0.001.

The client only gets "Invalid gacha request" or "Invalid gacha" for a refusal;
the rule that refused is read from the server's per-request dump log
(<src>/deploy/log/http_log_F7JvPk5H_*.log, written with log_level TRACE and
extra_log on, as deploy/config.docker.json has).

Usage: python3 -I tools/test_fair_summon.py --src wt/summon
           [--url http://127.0.0.1:9971] [--container NAME] [--archive DIR] [--pulls 3000]
--src is the server checkout the server runs from: its deploy/gme.sqlite is
edited to set balances and tutorial chapters, its deploy/ data gives the
expected costs and rates, and its deploy/log holds the dump logs.
--container is the Docker container the server runs in; by default the one
publishing the --url port, and '' for a server running on this machine.  The
database is then edited from inside it (docker exec python3): SQLite's locks
and page cache are not shared across a Docker Desktop bind mount, so edits made
on the host are not seen by the server, and its own writes can undo them.
--archive is the archive directory the server was started with, when that is
not <src>/deploy/archive.

Needs no pycryptodome: AES goes through the openssl command line, so it runs
under python3 -I.  Creates throwaway accounts.  Run it against a test server,
never a save with real players.
"""
import argparse
import base64
import gzip
import itertools
import json
import re
import secrets
import sqlite3
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from pathlib import Path

# Request group id and AES key, as registered in GmeControllerHandlers.cpp.
INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
GACHA_ACTION = ('F7JvPk5H', 'bL9fipzaSy7xN2w1')
GACHA_LIST = ('Uo86DcRh', '8JbxFvuSaB2CK7Ln')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')

TEST_GATES = {99005, 99006, 99007, 99008}
TUTORIAL_GATE = 2000
HONOR_GATE = 1000
GEM_GATE = 11960        # the All Star rare summon, 3 gems
SELECTOR_GATE = 17650   # a selector gate whose gacha_mst cost is a plain 5 gems
UNPOOLED_GATE = 1001    # Super Honor: a gacha_mst row, but no pool and no rail tile
BRAVE_TICKET = 8        # SummonTicketV2Mst type 8, "Brave Summon Ticket": 1 per pull on 11960

# (tutorial_status, tutorial_end_flag) rows.  The main tutorial runs chapters
# 1..12 with chapter 10 as tuto15, the free summon, and the client sets its end
# flag only on reaching 12.  MissionEnd writes chapter 2, then 50 (its own
# checkpoint), before the client reports 12.  DebugCli unlock sets the flag and
# leaves a chapter of 50 as it is.
IN_TUTORIAL = ((1, 0), (2, 0), (50, 0), (10, 0), (11, 0))
FINISHED = ((12, 1), (50, 1))
CHAPTER_FREE_SUMMON = 10
CHAPTER_DONE = 12

# `324b023k`, what pays: UserState::usingGachaTicket.
PAY_CURRENCY, PAY_PLAIN_TICKET, PAY_GATE_TICKET = 0, 1, 2

# What the client is told: HandleResult::error's first argument.
WIRE_RULE = 'Invalid gacha request'
WIRE_UNKNOWN = 'Invalid gacha'
# What the dump log says: its second argument.
NOT_OPEN = 'This gate is not open'
UNKNOWN_GATE = 'Unknown summon gate'
SELECTOR_ONLY = 'This gate is redeemed with its selector ticket'
TOO_MANY = 'Too many summons in one request'
NO_TICKETS_HERE = 'Summon tickets cannot be used on this gate'
NOT_ENOUGH = 'Not enough currency'
NOT_ENOUGH_TICKETS = 'Not enough summon tickets'
UNKNOWN_PAYMENT = 'Unknown summon payment'
OWN_TICKETS_ONLY = 'This gate takes its own summon tickets'
NO_OWN_TICKETS = 'This gate has no summon tickets of its own'

RACERS = 8
RACE_ROUNDS = 5

# chi-square critical values at p = 0.001, by degrees of freedom
CHI2_CRIT = {1: 10.83, 2: 13.82, 3: 16.27, 4: 18.47, 5: 20.52, 6: 22.46, 7: 24.32}

_markers = itertools.count(1)
_run = secrets.token_hex(3)


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


def seal(group: str, key: str, body: dict) -> bytes:
    """The request envelope, encrypted ahead of time so racers only POST."""
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    return json.dumps({
        'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'fairsummon'},
        'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(aes_ecb(key, raw + bytes([pad]) * pad, False)).decode()},
    }).encode()


def post(base: str, key: str, envelope: bytes) -> dict:
    status, data = fetch(f'{base}/bf/gme/action.php', envelope)
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = aes_ecb(key, base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']), True)
    return json.loads(plain[:-plain[-1]])


def gme_call(base: str, group: str, key: str, body: dict) -> dict:
    return post(base, key, seal(group, key, body))


def wire_error(reply: dict) -> str | None:
    """The error text the client would show, or None for a success."""
    if '_unencrypted' not in reply:
        return None
    error = reply['_unencrypted'].get('b5PH6mZa') or {}
    return error.get('ZC0msu2L') or json.dumps(reply['_unencrypted'])[:200]


def succeeded(reply: dict) -> bool:
    return wire_error(reply) is None and 'Km35HAXv' in reply


class DumpLogs:
    """Reads the refusal reason the server logged for one request."""

    def __init__(self, directory: Path):
        self.dir = directory

    @property
    def available(self) -> bool:
        return self.dir.is_dir()

    def reason(self, marker: str) -> str | None:
        """`ex:` text of the refusal logged for the request carrying `marker`;
        '' when it was logged as a success, None when no dump names it.  One
        file per request-second, truncated by the next request in that second,
        so read it before sending the next request."""
        dumps = sorted(self.dir.glob(f'http_log_{GACHA_ACTION[0]}_*.log'), reverse=True)
        for path in dumps[:20]:
            text = path.read_text(encoding='utf-8', errors='replace')
            if marker not in text:
                continue
            m = re.search(r'^RESPONSE IN ERROR: .*? ex: (.*)$', text, re.M)
            return m.group(1).strip() if m else ''
        return None


# Runs one statement against the save inside the server's container.
_IN_CONTAINER_SQL = r'''
import json, sqlite3, sys
req = json.load(sys.stdin)
con = sqlite3.connect(req["path"], timeout=30)
rows = con.execute(req["sql"], req["params"]).fetchall()
con.commit()
print(json.dumps(rows))
'''


class Database:
    """The server's save, edited where the server can see the edits."""

    def __init__(self, host_path: Path, container: str | None, container_path: str):
        self.host_path = host_path
        self.container = container
        self.container_path = container_path
        self.lock = threading.Lock()

    def rows(self, sql: str, *params) -> list:
        with self.lock:
            if self.container:
                done = subprocess.run(
                    ['docker', 'exec', '-i', self.container, 'python3', '-c', _IN_CONTAINER_SQL],
                    input=json.dumps({'path': self.container_path, 'sql': sql, 'params': list(params)}),
                    capture_output=True, text=True)
                if done.returncode != 0:
                    raise RuntimeError(f'SQL in {self.container} failed: {done.stderr.strip()[-300:]}')
                return json.loads(done.stdout)
            with sqlite3.connect(self.host_path, timeout=30) as con:
                return con.execute(sql, params).fetchall()

    def one(self, sql: str, *params):
        rows = self.rows(sql, *params)
        return rows[0] if rows else None


def container_on_port(url: str) -> str | None:
    """The one running container publishing the URL's port, if any."""
    port = urllib.parse.urlsplit(url).port
    try:
        out = subprocess.run(['docker', 'ps', '--filter', f'publish={port}', '--format', '{{.Names}}'],
                             capture_output=True, text=True, timeout=30).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out[0] if len(out) == 1 else None


class Pull:
    def __init__(self, reply: dict, marker: str):
        self.reply = reply
        self.marker = marker


class Player:
    def __init__(self, base: str, db: Database):
        self.base = base
        query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                        'dp': 'Android', 'dn': 'fair-summon-test', 'v': '16'})
        status, body = fetch(f'{base}/accounts/guest/login/?{query}')
        if status != 200:
            raise RuntimeError(f'guest login HTTP {status}')
        login = json.loads(body)
        node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}

        # A guest login is not a player yet: CreateUser makes the user_info row,
        # and only then does Initialize hand out the user id every gacha
        # request has to carry (getUserIdentity refuses an empty h7eY3sAK).
        created = gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j='Fair' + secrets.token_hex(2))],
                                                '9u45RGcV': [{'Kn51uR4Y': '1'}]})
        if wire_error(created) is not None:
            raise RuntimeError(f'CreateUser refused: {wire_error(created)}')
        init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]})
        user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
        if not user_id:
            raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
        self.login = [dict(node, h7eY3sAK=user_id)]

        self.db = db
        row = db.one('SELECT id FROM user_info WHERE gumi_user_id = ?', login['game_user_id'])
        if row is None or str(row[0]) != str(user_id):
            raise RuntimeError(f'user_info row {row} does not match user id {user_id}')
        self.user_id = row[0]

    def set(self, **values) -> None:
        cols = ', '.join(f'{k} = ?' for k in values)
        self.db.rows(f'UPDATE user_info SET {cols} WHERE id = ?', *values.values(), self.user_id)

    def set_brave(self, count: int) -> None:
        self.db.rows('INSERT INTO user_summon_tickets_v2 (user_id, ticket_id, count) VALUES (?, ?, ?)'
                     ' ON CONFLICT (user_id, ticket_id) DO UPDATE SET count = excluded.count',
                     self.user_id, BRAVE_TICKET, count)

    def balance(self) -> dict:
        row = self.db.one('SELECT gems, friend_points, summon_tickets FROM user_info WHERE id = ?', self.user_id)
        brave = self.db.one('SELECT count FROM user_summon_tickets_v2 WHERE user_id = ? AND ticket_id = ?',
                            self.user_id, BRAVE_TICKET)
        return {'gems': row[0], 'friend_points': row[1], 'summon_tickets': row[2], 'brave': brave[0] if brave else 0}

    def unit_count(self) -> int:
        return self.db.one('SELECT COUNT(*) FROM user_units WHERE user_id = ?', self.user_id)[0]

    def pull_envelope(self, gate: int, count: int = 1, pay: int = PAY_CURRENCY) -> tuple[bytes, str]:
        # The signal key is echoed into the request dump, so it finds this
        # request's refusal reason in the server's log.
        marker = f'fairsummon-{_run}-{next(_markers)}'
        body = {'6FrKacq7': [{'Kn51uR4Y': marker}], 'IKqx1Cn9': self.login,
                '1IR86sAv': [{'7Ffmi96v': str(gate), 'a329kbl8': str(count), '324b023k': str(pay)}]}
        return seal(*GACHA_ACTION, body), marker

    def pull(self, gate: int, count: int = 1, pay: int = PAY_CURRENCY) -> Pull:
        envelope, marker = self.pull_envelope(gate, count, pay)
        return Pull(post(self.base, GACHA_ACTION[1], envelope), marker)

    def gacha_list(self) -> dict:
        return gme_call(self.base, *GACHA_LIST, {'6FrKacq7': [{'Kn51uR4Y': 'fairsummon'}], 'IKqx1Cn9': self.login})

    def user_info(self) -> dict:
        return gme_call(self.base, *USER_INFO, {'6FrKacq7': [{'Kn51uR4Y': 'fairsummon'}], 'IKqx1Cn9': self.login})


def listed(reply: dict) -> tuple[set[int], set[int]]:
    """(gate ids in GachaInfo, gate ids named by any rail tile)."""
    gates = {int(g['7Ffmi96v']) for g in reply.get('1IR86sAv', [])}
    tiles = {int(x) for c in reply.get('IBs49NiH', []) for x in str(c.get('3rCmq58M', '')).split(',') if x}
    return gates, tiles


def chi_square(observed: Counter, expected_share: dict) -> tuple[float, int]:
    n = sum(observed.values())
    keys = [k for k, p in expected_share.items() if p > 0]
    if n == 0 or len(keys) < 2:
        return float('inf'), max(len(keys) - 1, 1)
    stat = sum((observed.get(k, 0) - n * expected_share[k]) ** 2 / (n * expected_share[k]) for k in keys)
    stat += sum(v for k, v in observed.items() if k not in keys) * 1e9  # an impossible outcome fails outright
    return stat, len(keys) - 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9971')
    ap.add_argument('--src', type=Path, required=True, help='server checkout the server runs from')
    ap.add_argument('--container', help="Docker container the server runs in (default: the one publishing "
                                        "the --url port; '' for a server on this machine)")
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite', help='save path inside the container')
    ap.add_argument('--archive', type=Path, help='archive dir the server runs with (default <src>/deploy/archive)')
    ap.add_argument('--pulls', type=int, default=3000, help='units rolled for the rate test')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    deploy = args.src / 'deploy'
    archive = args.archive or deploy / 'archive'
    container = container_on_port(args.url) if args.container is None else (args.container or None)
    db = Database(deploy / 'gme.sqlite', container, args.container_db)
    logs = DumpLogs(deploy / 'log')
    mst_rows = {int(r['7Ffmi96v']): r for r in json.loads(
        (deploy / 'mst/gacha_mst.json').read_text(encoding='utf-8'))['5Y4GJeo3']}
    pools = {g['id']: g['pool'] for g in json.loads((archive / 'gacha.json').read_text(encoding='utf-8'))}
    rarity = {u['id']: u['rarity'] for u in json.loads((archive / 'unit.json').read_text(encoding='utf-8'))}
    type_rows = next(iter(json.loads((deploy / 'mst/unit_type_mst.json').read_text(encoding='utf-8')).values()))
    rail = {int(x) for c in json.loads((deploy / 'mst/gacha_category_mst.json').read_text(encoding='utf-8'))['IBs49NiH']
            for x in str(c['3rCmq58M']).split(',') if x}
    selector_gates = {int(s['7Ffmi96v']) for s in json.loads(
        (deploy / 'mst/unit_selector_gacha_ticket_mst.json').read_text(encoding='utf-8'))['JukkSeNA']}
    features = json.loads((deploy / 'system/features.json').read_text(encoding='utf-8'))
    max_pulls = 10 if int(features.get('multisummon', 0)) else 1
    gem_cost = int(mst_rows[GEM_GATE]['03UGMHxF'])
    honor_cost = int(mst_rows[HONOR_GATE]['J3stQ7jd'])
    tutorial_cost = int(mst_rows[TUTORIAL_GATE]['03UGMHxF'])
    print(f'Multi Summon {"on" if max_pulls > 1 else "off"} (max {max_pulls} per request); '
          f'{GEM_GATE} costs {gem_cost} gems, {HONOR_GATE} {honor_cost} Honor, {TUTORIAL_GATE} {tutorial_cost} gems')
    print(f'Save edited {"inside container " + container if container else "on this machine"}')
    if not logs.available:
        print(f'NOTE  no dump-log directory at {logs.dir}: refusals are checked on the wire only')

    def refused(label: str, pull: Pull, wire: str, reason: str) -> None:
        """A refusal with the wire text `wire` and the logged rule `reason`."""
        text = wire_error(pull.reply)
        wire_ok = text is not None and f'Error: "{wire}"' in text
        if not logs.available:
            check(label, wire_ok, text)
            return
        logged = logs.reason(pull.marker)
        check(label, wire_ok and logged == reason, f'wire {text!r}, logged {logged!r}')

    # ---- A. what the lists offer ------------------------------------------------------
    check('A: no test gate in the data', not TEST_GATES & (set(mst_rows) | set(pools)))
    p = Player(args.url, db)
    for chapter, end_flag in IN_TUTORIAL:
        p.set(tutorial_status=chapter, tutorial_end_flag=end_flag)
        for name, reply in (('GachaList', p.gacha_list()), ('UserInfo', p.user_info())):
            gates, tiles = listed(reply)
            check(f'A: {name} at chapter {chapter}, end flag {end_flag}, lists gate {TUTORIAL_GATE}',
                  TUTORIAL_GATE in gates and TUTORIAL_GATE in tiles, sorted(tiles))
    for chapter, end_flag in FINISHED:
        p.set(tutorial_status=chapter, tutorial_end_flag=end_flag)
        for name, reply in (('GachaList', p.gacha_list()), ('UserInfo', p.user_info())):
            gates, tiles = listed(reply)
            check(f'A: {name} at chapter {chapter}, end flag {end_flag}, hides gate {TUTORIAL_GATE}',
                  TUTORIAL_GATE not in gates and TUTORIAL_GATE not in tiles, sorted(tiles))
    p.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1)
    for name, reply in (('GachaList', p.gacha_list()), ('UserInfo', p.user_info())):
        gates, tiles = listed(reply)
        check(f'A: {name} lists gates', bool(gates), sorted(gates))
        check(f'A: {name} carries no test gate', not TEST_GATES & (gates | tiles))
        check(f'A: {name} keeps the real rail (Honor {HONOR_GATE}, rare {GEM_GATE})',
              {HONOR_GATE, GEM_GATE} <= tiles, sorted(tiles))

    # ---- B. refusals ------------------------------------------------------------------
    p.set(gems=1000, friend_points=100000, summon_tickets=50)
    p.set_brave(0)
    control = p.pull(GEM_GATE)
    check('B: control: this account can pull 11960', succeeded(control.reply), wire_error(control.reply))
    before, units = p.balance(), p.unit_count()
    for gate in sorted(TEST_GATES):
        refused(f'B: removed test gate {gate} refused', p.pull(gate), WIRE_UNKNOWN, UNKNOWN_GATE)
    if UNPOOLED_GATE not in pools:  # a QA archive may pool it, to reach the rail rule below
        refused(f'B: gate {UNPOOLED_GATE} (gacha_mst row, no pool) refused', p.pull(UNPOOLED_GATE),
                WIRE_UNKNOWN, UNKNOWN_GATE)
    refused('B: unknown gate refused', p.pull(424242), WIRE_UNKNOWN, UNKNOWN_GATE)
    refused(f'B: gate {TUTORIAL_GATE} refused at chapter {CHAPTER_DONE}', p.pull(TUTORIAL_GATE), WIRE_RULE, NOT_OPEN)
    refused(f'B: selector gate {SELECTOR_GATE} refused for gems', p.pull(SELECTOR_GATE), WIRE_RULE, SELECTOR_ONLY)
    refused(f'B: {max_pulls + 1} pulls in one request refused', p.pull(GEM_GATE, max_pulls + 1), WIRE_RULE, TOO_MANY)
    wrap = 2 ** 32 // gem_cost + 1
    refused(f'B: {wrap} pulls (a cost that wraps 32 bits) refused', p.pull(GEM_GATE, wrap), WIRE_RULE, TOO_MANY)
    refused('B: payment code 3 refused', p.pull(GEM_GATE, pay=3), WIRE_RULE, UNKNOWN_PAYMENT)
    refused('B: a plain Summon Ticket on the Honor gate refused', p.pull(HONOR_GATE, pay=PAY_PLAIN_TICKET),
            WIRE_RULE, NO_TICKETS_HERE)
    refused(f'B: a plain Summon Ticket on {GEM_GATE}, which has its own (Brave) tickets, refused',
            p.pull(GEM_GATE, pay=PAY_PLAIN_TICKET), WIRE_RULE, OWN_TICKETS_ONLY)
    off_rail = sorted(set(pools) - rail - selector_gates)
    for gate in off_rail:
        refused(f'B: gate {gate}, pooled but on no rail tile, refused', p.pull(gate), WIRE_RULE, NOT_OPEN)
    if not off_rail:
        print('NOTE  B: every pooled gate is on the rail or is a selector gate, so the rail rule is not '
              'reachable with this archive (start the server on a QA archive and pass --archive)')
    check('B: refusals charged nothing and granted nothing',
          p.balance() == before and p.unit_count() == units, (before, p.balance()))
    p.set(gems=gem_cost - 1)
    refused(f'B: {gem_cost - 1} gems cannot buy a {gem_cost}-gem pull', p.pull(GEM_GATE), WIRE_RULE, NOT_ENOUGH)
    check('B: ...and the gems are untouched', p.balance()['gems'] == gem_cost - 1)
    if max_pulls > 1:
        p.set(gems=gem_cost * (max_pulls - 1))
        refused(f'B: {max_pulls} pulls refused with gems for {max_pulls - 1}',
                p.pull(GEM_GATE, max_pulls), WIRE_RULE, NOT_ENOUGH)
    p.set(gems=1000, summon_tickets=50)
    refused(f'B: a Brave Summon Ticket pull on {GEM_GATE} with none held refused',
            p.pull(GEM_GATE, pay=PAY_GATE_TICKET), WIRE_RULE, NOT_ENOUGH_TICKETS)
    check('B: ...and neither gems nor plain tickets were taken instead',
          p.balance()['gems'] == 1000 and p.balance()['summon_tickets'] == 50, p.balance())

    # ---- C. costs ---------------------------------------------------------------------
    p.set(gems=1000, friend_points=10000, summon_tickets=5)
    p.set_brave(0)
    honor_count = min(3, max_pulls)

    def cost_case(label: str, pull_args: dict, n: int, delta: dict, who: Player | None = None) -> None:
        who = who or p
        before, units = who.balance(), who.unit_count()
        pull = who.pull(**pull_args)
        after = who.balance()
        want = {k: before[k] + delta.get(k, 0) for k in before}
        got = len(pull.reply.get('qC2tJs4E', []))
        check(f'C: {label}', succeeded(pull.reply) and after == want and got == n and who.unit_count() == units + n,
              (wire_error(pull.reply), before, after, got))

    cost_case(f'one gem pull costs {gem_cost} gems', dict(gate=GEM_GATE), 1, {'gems': -gem_cost})
    cost_case(f'{max_pulls} gem pulls cost {max_pulls * gem_cost} gems (no Multi Summon discount)',
              dict(gate=GEM_GATE, count=max_pulls), max_pulls, {'gems': -max_pulls * gem_cost})
    cost_case('a request count of 0 is one pull', dict(gate=GEM_GATE, count=0), 1, {'gems': -gem_cost})
    cost_case(f'one Honor pull costs {honor_cost} Honor', dict(gate=HONOR_GATE), 1, {'friend_points': -honor_cost})
    cost_case(f'{honor_count} Honor pulls cost {honor_count * honor_cost} Honor', dict(gate=HONOR_GATE, count=honor_count),
              honor_count, {'friend_points': -honor_count * honor_cost})
    p.set_brave(3)
    cost_case(f'a Brave Summon Ticket pull (324b023k=2) on {GEM_GATE} costs one Brave ticket and nothing else',
              dict(gate=GEM_GATE, pay=PAY_GATE_TICKET), 1, {'brave': -1})
    if max_pulls > 1:
        cost_case('two Brave Summon Ticket pulls with two held spend both',
                  dict(gate=GEM_GATE, count=2, pay=PAY_GATE_TICKET), 2, {'brave': -2})
        p.set_brave(1)
        before, units = p.balance(), p.unit_count()
        refused('C: two Brave Summon Ticket pulls with one held are refused, not topped up from the plain counter',
                p.pull(GEM_GATE, 2, PAY_GATE_TICKET), WIRE_RULE, NOT_ENOUGH_TICKETS)
        check('C: ...and nothing was spent', p.balance() == before and p.unit_count() == units, p.balance())

    # Gate 2000 is the one rail gate with no V2 types, so the plain counter
    # pays only there, and only during the tutorial.
    t = Player(args.url, db)
    t.set(tutorial_status=CHAPTER_FREE_SUMMON, tutorial_end_flag=0, gems=tutorial_cost, summon_tickets=1)
    refused(f'C: 324b023k=2 on gate {TUTORIAL_GATE}, which has no V2 tickets, refused',
            t.pull(TUTORIAL_GATE, pay=PAY_GATE_TICKET), WIRE_RULE, NO_OWN_TICKETS)
    cost_case(f'a plain Summon Ticket pull (324b023k=1) on gate {TUTORIAL_GATE} costs one ticket and no gems',
              dict(gate=TUTORIAL_GATE, pay=PAY_PLAIN_TICKET), 1, {'summon_tickets': -1}, who=t)
    units = t.unit_count()
    pull = t.pull(TUTORIAL_GATE, 0)  # tuto15 sends a count of 0
    got = [int(u['pn16CNah']) for u in pull.reply.get('qC2tJs4E', [])]
    tutorial_rarities = {rarity[x['unit_id']] for x in pools[TUTORIAL_GATE]}
    check(f'C: the tuto15 pull (chapter {CHAPTER_FREE_SUMMON}, count 0) works and costs {tutorial_cost} gems',
          succeeded(pull.reply) and t.balance()['gems'] == 0 and t.unit_count() == units + 1
          and len(got) == 1 and rarity[got[0]] in tutorial_rarities,
          (wire_error(pull.reply), t.balance(), got))

    # ---- D. races on one balance -----------------------------------------------------
    r = Player(args.url, db)
    r.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, friend_points=0)
    rt = Player(args.url, db)  # still in the tutorial, for the plain counter on gate 2000
    rt.set(tutorial_status=CHAPTER_FREE_SUMMON, tutorial_end_flag=0, friend_points=0)
    races = [
        ('gems', r, GEM_GATE, PAY_CURRENCY, dict(gems=gem_cost, summon_tickets=0), 0, 'gems'),
        ('Brave Summon Tickets', r, GEM_GATE, PAY_GATE_TICKET, dict(gems=0, summon_tickets=0), 1, 'brave'),
        ('plain Summon Tickets', rt, TUTORIAL_GATE, PAY_PLAIN_TICKET, dict(gems=0, summon_tickets=1), 0,
         'summon_tickets'),
    ]
    for label, who, gate, pay, balance, brave, spent in races:
        rounds = []
        for _ in range(RACE_ROUNDS):
            who.set(**balance)
            who.set_brave(brave)
            units = who.unit_count()
            envelopes = [who.pull_envelope(gate, pay=pay)[0] for _ in range(RACERS)]
            replies = []
            barrier = threading.Barrier(RACERS)

            def racer(envelope: bytes) -> None:
                barrier.wait()
                replies.append(post(args.url, GACHA_ACTION[1], envelope))

            threads = [threading.Thread(target=racer, args=(e,)) for e in envelopes]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
            rounds.append((sum(succeeded(x) for x in replies), who.balance()[spent], who.unit_count() - units))
        check(f'D: {RACERS} pulls racing on {label} for one, {RACE_ROUNDS} rounds: exactly one paid each round',
              all(row == (1, 0, 1) for row in rounds), rounds)

    # ---- E. rates ---------------------------------------------------------------------
    rate = Player(args.url, db)
    rate.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1,
             gems=gem_cost * args.pulls, friend_points=0, summon_tickets=0)
    rarities, types = Counter(), Counter()
    rolled = 0
    while rolled < args.pulls:
        n = min(max_pulls, args.pulls - rolled)
        pull = rate.pull(GEM_GATE, n)
        if not succeeded(pull.reply):
            check('E: rate pulls succeed', False, wire_error(pull.reply))
            break
        for unit in pull.reply['qC2tJs4E']:
            rarities[rarity[int(unit['pn16CNah'])]] += 1
            types[int(unit['nBTx56W9'])] += 1
        rolled += n
    pool = [x for x in pools[GEM_GATE] if x['weight'] > 0]
    total = sum(x['weight'] for x in pool)
    expect_rarity = Counter()
    for x in pool:
        expect_rarity[rarity[x['unit_id']]] += x['weight'] / total
    stat, df = chi_square(rarities, expect_rarity)
    shown = {k: f'{rarities[k] / max(rolled, 1):.2%} (want {v:.2%})' for k, v in sorted(expect_rarity.items())}
    check(f'E: {rolled} pulls from {GEM_GATE} match the pool rarity weights (chi2 {stat:.2f} < {CHI2_CRIT[df]})',
          rolled == args.pulls and stat < CHI2_CRIT[df], shown)
    check(f'E: {rolled} pulls cost exactly {gem_cost} gems each', rate.balance()['gems'] == 0, rate.balance())

    type_total = sum(float(row['ChD5b0jR']) for row in type_rows)
    # nBTx56W9 is the type id, ChD5b0jR its appearance rate (mst/unit_ext.kdl)
    expect_type = {int(row['nBTx56W9']): float(row['ChD5b0jR']) / type_total for row in type_rows}
    stat, df = chi_square(types, expect_type)
    shown = {k: f'{types[k] / max(rolled, 1):.2%} (want {v:.2%})' for k, v in sorted(expect_type.items())}
    check(f'E: unit types match F_UNIT_TYPE_MST (chi2 {stat:.2f} < {CHI2_CRIT[df]})',
          stat < CHI2_CRIT[df] and types.get(6, 0) == 0, shown)

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
