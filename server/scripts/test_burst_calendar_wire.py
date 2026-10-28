"""Wire test: the Brave Burst login calendar pays Burst Tokens (ticket #44).

  default mode (server running, save INSIDE its container, --db /tmp/gme.sqlite):
    a new account's first UserInfo pays calendar day 1: a Gem and 200 Burst Tokens
    (present type 8004, target 61), and no Honor; the Initialize reply's campaign
    MST ends in the future, so "Campaign Ends In" is no longer 0D 0H 0M.
  prepare-backfill SAVE (server STOPPED): two existing accounts on calendar days 1
    and 25, and the 25102026_BurstCalendarTokens migration marked as not run.
  check-backfill SAVE (after one server start, then stopped again): those accounts
    each hold one Burst Token present for 200 and 5000.

Usage: python3 -I scripts/test_burst_calendar_wire.py [--url http://127.0.0.1:9975] [--db /tmp/gme.sqlite]
       python3 -I scripts/test_burst_calendar_wire.py prepare-backfill|check-backfill deploy/gme.sqlite
Creates throwaway accounts: never run it against a save with real players.
"""
import argparse
import base64
import gzip
import json
import secrets
import subprocess
import time
import sys
import urllib.error
import urllib.parse
import urllib.request

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')


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



def new_player(base: str, db: Database, label: str) -> dict:
    query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                    'dp': 'Android', 'dn': 'names-test', 'v': '16'})
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    if status != 200:
        raise RuntimeError(f'guest login HTTP {status}')
    login = json.loads(body)
    node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
    name = f'N{label}{RUN}'
    gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=name)], '9u45RGcV': [{'Kn51uR4Y': '1'}]},
             secrets.token_hex(4))
    init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
    user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
    if not user_id:
        raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
    return {'base': base, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id, 'name': name}



def new_player(base: str, label: str) -> dict:
    query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                    'dp': 'Android', 'dn': 'calendar-test', 'v': '16'})
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    login = json.loads(body)
    node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
    gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=f'C{label}{RUN}')], '9u45RGcV': [{'Kn51uR4Y': '1'}]},
             secrets.token_hex(4))
    init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
    user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
    return {'base': base, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id, 'init': init}


def wire(args) -> int:
    container = args.container or container_on_port(args.url)
    db = Database(container, args.db)
    p = new_player(args.url, 'A')
    print(f'account: {p["id"]}')
    campaign = (p['init'].get('Bd29Pqw0') or [{}])[0]
    start, days = int(campaign.get('qA7M9EjP', 0)), int(campaign.get('1adb38d5', 0))
    end = (start // 1000 if start > 10**11 else start) + days * 86400
    check('Initialize: the calendar countdown ends in the future', end > time.time(), (start, days))
    check('...within one campaign period', end - time.time() <= days * 86400 + 60, (start, days))
    honor = db.value('SELECT friend_points FROM user_info WHERE id = ?', p['id'])
    reply = gme_call(p['base'], *USER_INFO, {'IKqx1Cn9': p['login']}, secrets.token_hex(4))
    check('UserInfo (first login of the day) answers', wire_error(reply) is None, wire_error(reply))
    tokens = db.rows("SELECT target_cnt FROM user_presents WHERE user_id = ? AND present_type = 8004"
                     " AND target_id = '61'", p['id'])
    check('day 1 pays 200 Burst Tokens (present 8004, token 61)', tokens == [[200]], tokens)
    gems = db.value("SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = 8", p['id'])
    check('day 1 still pays its Gem', gems >= 1, gems)
    after = db.value('SELECT friend_points FROM user_info WHERE id = ?', p['id'])
    check('day 1 no longer pays Honor', after == honor, (honor, after))
    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


def backfill(mode: str, save: str) -> int:
    import sqlite3
    con = sqlite3.connect(save)
    users = [r[0] for r in con.execute('SELECT id FROM user_info ORDER BY rowid LIMIT 2')]
    if len(users) < 2:
        print('the save needs two accounts', file=sys.stderr)
        return 2
    if mode == 'prepare-backfill':
        for user, day in zip(users, (1, 25)):
            con.execute('INSERT INTO user_login_campaign (user_id, current_day, last_day) VALUES (?, ?, ?)'
                        ' ON CONFLICT(user_id) DO UPDATE SET current_day = excluded.current_day', (user, day, '2026-10-01'))
            con.execute("DELETE FROM user_presents WHERE user_id = ? AND present_type = 8004 AND target_id = '61'", (user,))
        con.execute("DELETE FROM migration_status WHERE hash = '25102026_BurstCalendarTokens'")
        con.commit()
        print('prepared', users)
        return 0
    for user, want in zip(users, (200, 5000)):
        got = [r[0] for r in con.execute("SELECT target_cnt FROM user_presents WHERE user_id = ? AND present_type = 8004"
                                         " AND target_id = '61'", (user,))]
        check(f'backfill: {user} gets one Burst Token present of {want}', got == [want], got)
    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] in ('prepare-backfill', 'check-backfill'):
        return backfill(sys.argv[1], sys.argv[2])
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/tmp/gme.sqlite')
    return wire(ap.parse_args())


if __name__ == '__main__':
    sys.exit(main())
