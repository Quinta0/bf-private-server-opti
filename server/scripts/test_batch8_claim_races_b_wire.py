"""Wire test: once-only claims pay once under parallel copies (ticket #166 audit).

Each of these read "not claimed yet", paid, then wrote the mark, with awaits in
between, so N copies of the same request sent at once all passed the read and
all paid.  Six identical requests (each with its own client id, as a proxy
would send them) are fired together and the payout is counted in the save.

  Journal task       SummonerJournalTaskRewards for one finished mission queues
                     its reward once; the row ends claimed.
  Journal milestone  SummonerJournalMilestoneRewards with every rung reached
                     queues each rung's reward once.
  Starter selector   UpdateInfo at Summoner Rank 50 mints the one Rank-50
                     selector ticket (present 8005 / selector 17) once.
  Daily medals       Initialize queues the daily 120 Brave Medal gift
                     (receipt_type 2) once.
Every copy must answer without a wire error: the losers are refused softly.

Usage: python3 -I scripts/test_batch8_claim_races_b_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite] [--copies 6]
The test writes journal progress, levels and presents with docker exec, so run
the server with its save INSIDE the container (a config whose db filename is
/tmp/gme.sqlite): on a Docker Desktop bind mount the server and an exec'd
sqlite3 do not see each other's writes reliably.
Creates throwaway accounts: never run it against a save with real players.
"""
import argparse
import base64
import concurrent.futures
import gzip
import json
import secrets
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
UPDATE_INFO = ('RUV94Dqz', 'hy0P9xjsGJ6MAgb2')
JOURNAL_TASK_REWARDS = ('2y48D13d', '7nm3Dqe9')
JOURNAL_MILESTONE_REWARDS = ('3a83iY3r', '98Tw0ubW')

JOURNAL_RECEIPT_TYPE = 3        # kJournalRewardReceiptType
STARTER_SELECTOR_LEVEL = 50     # kStarterSelectorLevel
SELECTOR_PRESENT_TYPE = 8005
WEEKLY_SELECTOR_ID = '17'       # kWeeklySelectorId
MEDAL_GIFT_RECEIPT_TYPE = 2     # kBraveSlotDailyGiftReceiptType
MEDAL_PRESENT_TYPE = 12
MEDAL_ID = '1'                  # kBraveSlotMedalId
RACE_TASK_KEY = 'friendship'    # a fresh account has no friends: only the stored counter counts

JOURNAL = json.loads((Path(__file__).resolve().parents[1] / 'deploy' / 'archive'
                      / 'summoner_journal.json').read_text(encoding='utf-8'))

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


def gme_envelope(group: str, key: str, body: dict, tag: str) -> bytes:
    """The encrypted request body.  `tag` is GmeHeader.client_id: the client's
    request object id, the same on a retry and new for every new request."""
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    return json.dumps({
        'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': tag},
        'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(aes_ecb(key, raw + bytes([pad]) * pad, False)).decode()},
    }).encode()


def gme_post(base: str, key: str, envelope: bytes) -> dict:
    status, data = fetch(f'{base}/bf/gme/action.php', envelope)
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = aes_ecb(key, base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']), True)
    return json.loads(plain[:-plain[-1]])


def gme_call(base: str, group: str, key: str, body: dict, tag: str) -> dict:
    return gme_post(base, key, gme_envelope(group, key, body, tag))


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


def new_player(base: str, label: str) -> dict:
    query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                    'dp': 'Android', 'dn': 'races-test', 'v': '16'})
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    if status != 200:
        raise RuntimeError(f'guest login HTTP {status}')
    login = json.loads(body)
    node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
    name = f'R{label}{RUN}'
    gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=name)], '9u45RGcV': [{'Kn51uR4Y': '1'}]},
             secrets.token_hex(4))
    init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
    user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
    if not user_id:
        raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
    return {'base': base, 'node': node, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id, 'name': name}


def fire_together(p: dict, request, body: dict, copies: int, what: str) -> None:
    """`copies` identical requests, released at once by a barrier, each with its
    own client id.  The envelopes are encrypted first so the posts line up."""
    group, key = request
    envelopes = [gme_envelope(group, key, body, secrets.token_hex(4)) for _ in range(copies)]
    gate = threading.Barrier(copies)

    def one(envelope: bytes) -> dict:
        gate.wait()
        return gme_post(p['base'], key, envelope)

    with concurrent.futures.ThreadPoolExecutor(max_workers=copies) as pool:
        outcomes = [f.exception() or f.result() for f in [pool.submit(one, e) for e in envelopes]]
    broken = [str(o) for o in outcomes if isinstance(o, BaseException)]
    errors = [wire_error(o) for o in outcomes if isinstance(o, dict) and wire_error(o) is not None]
    check(f'{what}: all {copies} copies answered', not broken, broken[:2])
    check(f'{what}: no copy got a wire error (losers are refused softly)', not errors, errors[:2])


def signed(p: dict, body: dict) -> dict:
    return dict(body, IKqx1Cn9=p['login'], **{'6FrKacq7': [{'Kn51uR4Y': 'racestest'}]})


def journal_presents(db: Database, p: dict, description: str) -> int:
    return db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND receipt_type = ?'
                    ' AND description = ?', p['id'], JOURNAL_RECEIPT_TYPE, description)


def test_journal(base: str, db: Database, copies: int) -> None:
    print('\n-- Summoner Journal task and milestone claims')
    p = new_player(base, 'J')
    task = next(t for t in JOURNAL['tasks'] if t['key'] == RACE_TASK_KEY)
    db.rows('INSERT INTO user_journal_tasks (user_id, task_key, progress, claimed) VALUES (?, ?, ?, 0)'
            ' ON CONFLICT(user_id, task_key) DO UPDATE SET progress = excluded.progress, claimed = 0',
            p['id'], task['key'], task['target'])
    check('precondition: the mission is finished and unpaid', journal_presents(db, p, task['name']) == 0)

    fire_together(p, JOURNAL_TASK_REWARDS, signed(p, {'da38tRai': [{'23DaiBpe': task['task_id']}]}), copies,
                  f'SummonerJournalTaskRewards for "{task["name"]}"')
    paid = journal_presents(db, p, task['name'])
    check(f'the "{task["name"]}" reward is queued once', paid == 1, f'{paid} present(s)')
    claimed = db.value('SELECT claimed FROM user_journal_tasks WHERE user_id = ? AND task_key = ?',
                       p['id'], task['key'])
    check('DB: the mission ends claimed', claimed == 1, claimed)

    # Every mission claimed puts the points past the top rung, so every
    # milestone is due at once.
    total = sum(t['points'] for t in JOURNAL['tasks'])
    rungs = JOURNAL['milestones']
    check('precondition: the missions are worth the top rung', total >= rungs[-1]['points'],
          f'{total} < {rungs[-1]["points"]}')
    values = ', '.join('(?, ?, 0, 1)' for _ in JOURNAL['tasks'])
    params = [v for t in JOURNAL['tasks'] for v in (p['id'], t['key'])]
    db.rows('INSERT INTO user_journal_tasks (user_id, task_key, progress, claimed) VALUES ' + values
            + ' ON CONFLICT(user_id, task_key) DO UPDATE SET claimed = 1', *params)

    fire_together(p, JOURNAL_MILESTONE_REWARDS, signed(p, {}), copies, 'SummonerJournalMilestoneRewards')
    for rung in rungs:
        paid = journal_presents(db, p, f'Journal Milestone {rung["points"]}')
        check(f'milestone {rung["points"]} ({rung["reward_note"]}) is queued once', paid == 1,
              f'{paid} present(s)')
        claimed = db.value('SELECT claimed FROM user_journal_tasks WHERE user_id = ? AND task_key = ?',
                           p['id'], f'milestone:{rung["milestone_id"]}')
        check(f'DB: milestone {rung["points"]} ends claimed', claimed == 1, claimed)


def selector_presents(db: Database, p: dict) -> int:
    return db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = ? AND target_id = ?',
                    p['id'], SELECTOR_PRESENT_TYPE, WEEKLY_SELECTOR_ID)


def test_starter_selector(base: str, db: Database, copies: int) -> None:
    print('\n-- Rank-50 starter selector ticket')
    p = new_player(base, 'S')
    check('precondition: a fresh account has no starter selector', selector_presents(db, p) == 0,
          selector_presents(db, p))
    db.rows('UPDATE user_info SET level = ?, starter_selector_granted = 0 WHERE id = ?',
            STARTER_SELECTOR_LEVEL, p['id'])

    fire_together(p, UPDATE_INFO, signed(p, {}), copies, f'UpdateInfo at Summoner Rank {STARTER_SELECTOR_LEVEL}')
    minted = selector_presents(db, p)
    check('the starter selector ticket is granted once', minted == 1, f'{minted} ticket(s)')
    granted = db.value('SELECT starter_selector_granted FROM user_info WHERE id = ?', p['id'])
    check('DB: the account is marked granted', granted == 1, granted)


def medal_gifts(db: Database, p: dict) -> int:
    start_of_day = int(time.time()) // 86400 * 86400     # the gift's day turns at 00:00 UTC
    return db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND receipt_type = ?'
                    ' AND present_type = ? AND target_id = ? AND present_date >= ?',
                    p['id'], MEDAL_GIFT_RECEIPT_TYPE, MEDAL_PRESENT_TYPE, MEDAL_ID, start_of_day)


def test_daily_medals(base: str, db: Database, copies: int) -> None:
    print('\n-- Daily 120 Brave Medal gift')
    p = new_player(base, 'M')
    check('the first Initialize of the day queued the gift once', medal_gifts(db, p) == 1, medal_gifts(db, p))
    db.rows('DELETE FROM user_presents WHERE user_id = ? AND receipt_type = ?', p['id'], MEDAL_GIFT_RECEIPT_TYPE)
    check('precondition: today\'s gift is gone again', medal_gifts(db, p) == 0, medal_gifts(db, p))

    fire_together(p, INITIALIZE, {'IKqx1Cn9': [p['node']]}, copies, 'Initialize')
    queued = medal_gifts(db, p)
    check('the daily medal gift is queued once', queued == 1, f'{queued} gift(s)')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/tmp/gme.sqlite')
    ap.add_argument('--copies', type=int, default=6)
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)

    test_journal(args.url, db, args.copies)
    test_starter_selector(args.url, db, args.copies)
    test_daily_medals(args.url, db, args.copies)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
