"""Wire test: a first clear's Zel and Karma are part of the result (ticket #134).

The result screen shows a quest's own Zel as reward_info.zel minus the clear
bonus's Zel (MissionResultScene::updateEvent @0x18CB3C8, getRewardBonusZel),
so the first-clear Zel must be inside reward_info.zel and in the balance.  It
used to go to the present box, and a 120,000 Zel first clear of The Longest
Night (83015900-83015904) showed a negative Zel reward.

  - first clear of 83015900: reward Zel = balance change, and includes the
    120,000; the quest's own Zel the client derives is not negative; the reply's
    team Zel is the new balance; no Zel present is queued.
  - a repeat clear: no bonus, reward Zel = balance change.
  - 83015903 (a Gem plus 120,000 Zel): the Gem still goes to the present box.

Usage: python3 -I scripts/test_first_clear_zel_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite]
Run the server with its save INSIDE the container (db filename /tmp/gme.sqlite):
docker exec reads of a Docker Desktop bind-mounted save can be stale.
Creates throwaway accounts: never run it against a save with real players.
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
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')

STORY_MISSION = 10

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
    def start(self, mission: int, helper: str = '0', tag: str | None = None) -> dict:
        body = {'9Q1Lq5FS': [{'h7eY3sAK': helper, 'J3stQ7jd': '0', 'j28VNcUW': str(mission),
                              'jkldTrhL': '0', 'Z0Y4RoD7': '0', 'nA95Bdj6': '0',
                              '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
                'JzS3uxsZ': [{'0b4efi1W': '1'}]}
        return self.raw(MISSION_START, body, tag)






LONGEST_NIGHT, LONGEST_NIGHT_GEM = 83015900, 83015903
BONUS = 120000
REWARD, TEAM = 'F5Vs19mb', 'fEi17cnx'
R_ZEL, R_BONUS = 'Najhr8m6', '92ms7J5M'


def clear(p: Player, mission: int, zel: int = 500) -> dict:
    started = p.start(mission)
    check(f'MissionStart {mission} answers', wire_error(started) is None, wire_error(started))
    serial = started['Kz7qfSs5'][0]['k9cxD7Ba']
    battle = {'47cJBUxz': '', 'Najhr8m6': str(zel), 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
              'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
              'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0'}
    return p.call(MISSION_END, {'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}],
                                'rXvA1E5y': [battle]}, f'MissionEnd {mission}')


def zel(db: Database, p: Player) -> int:
    return db.value('SELECT zel FROM user_info WHERE id = ?', p.user_id)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/tmp/gme.sqlite')
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)
    p = Player(args.url, db, 'Z')
    print(f'account: {p.user_id}')

    before = zel(db, p)
    reply = clear(p, LONGEST_NIGHT)
    reward = (reply.get(REWARD) or [{}])[0]
    got = int(reward.get(R_ZEL, -1))
    after = zel(db, p)
    check('first clear: the clear bonus line carries the 120,000 Zel', f'3:0:{BONUS}' in reward.get(R_BONUS, ''),
          reward.get(R_BONUS))
    check('first clear: reward Zel includes the 120,000', got >= BONUS + 500, got)
    check('first clear: the quest\'s own Zel the result screen shows is not negative', got - BONUS >= 0, got - BONUS)
    check('first clear: the balance rose by exactly the reward Zel', after - before == got, (before, after, got))
    team = int(((reply.get(TEAM) or [{}])[0]).get(R_ZEL, -1))
    check('first clear: the reply\'s team Zel is the new balance', team == after, (team, after))
    presents = db.value("SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = 3"
                        " AND description = 'First clear reward'", p.user_id)
    check('first clear: no Zel present is queued any more', presents == 0, presents)

    before = zel(db, p)
    reply = clear(p, LONGEST_NIGHT)
    reward = (reply.get(REWARD) or [{}])[0]
    got = int(reward.get(R_ZEL, -1))
    check('repeat clear: no clear bonus', reward.get(R_BONUS, '') == '', reward.get(R_BONUS))
    check('repeat clear: reward Zel is the balance change and below the bonus', zel(db, p) - before == got < BONUS,
          (before, zel(db, p), got))

    gems = db.value("SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = 8", p.user_id)
    before = zel(db, p)
    reply = clear(p, LONGEST_NIGHT_GEM)
    got = int(((reply.get(REWARD) or [{}])[0]).get(R_ZEL, -1))
    check('Gem + Zel first clear: the Zel is paid with the result', zel(db, p) - before == got >= BONUS,
          (before, zel(db, p), got))
    check('Gem + Zel first clear: the Gem still goes to the present box', db.value(
        "SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = 8", p.user_id) == gems + 1)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
