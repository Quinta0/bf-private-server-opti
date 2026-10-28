"""Wire test: fusion is charged the server's Zel price, not the request's (ticket #166 audit).

UnitMix used to charge mCE3rUu5[0].Rs7bCE3t as sent, so a client could fuse
for 0 Zel.  The server now prices it like the client's GameUtils::getMixGold
@0x1EAF2C0:

    price = sum over materials of (base unit_lvl + material unit_lvl) * 100,
            x 0.25 when the base is at its UnitMst max_lv

and refuses a fusion the wallet cannot cover softly (ReturnToGame, cmd 6)
with nothing changed.  Checks:
  A. Rs7bCE3t "0" with random levels: charged the server price (DB zel, the
     reply's team info and result screen), materials gone, mismatch logged once.
  B. A normal request carrying the client's own price: charged that price.
  C. A maxed base: the quarter price.
  D. An inflated Rs7bCE3t: charged the server price, not more.
  E. A wallet one Zel short: refused softly, zel, base and materials untouched.
  F. A normal request after the refusal still works.

Usage: python3 -I scripts/test_batch8_fusion_zel_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite]
The test writes units and Zel with docker exec, so run the server with its
save INSIDE the container (a config whose db filename is /tmp/gme.sqlite): on
a Docker Desktop bind mount the server and an exec'd sqlite3 do not see each
other's writes reliably.
Creates throwaway accounts: never run it against a save with real players.
"""
import argparse
import base64
import gzip
import json
import random
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
SOFT_HOME = '6'   # GmeErrorCommand::ReturnToGame, HandleResult::refuseToHome


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


def new_player(base: str, label: str) -> dict:
    query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                    'dp': 'Android', 'dn': 'fusion-zel-test', 'v': '16'})
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    if status != 200:
        raise RuntimeError(f'guest login HTTP {status}')
    login = json.loads(body)
    node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
    gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=f'Z{label}{RUN}')],
                                  '9u45RGcV': [{'Kn51uR4Y': '1'}]}, secrets.token_hex(4))
    init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
    user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
    if not user_id:
        raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
    return {'base': base, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id}


def mix_gold(base_lv: int, mat_lvs: list[int], max_lv: int) -> int:
    """GameUtils::getMixGold, as the server now computes it."""
    gold = sum((base_lv + lv) * 100 for lv in mat_lvs)
    return gold // 4 if base_lv >= max_lv else gold   # always a multiple of 100: exact


def first(reply: dict, group: str) -> dict:
    return (reply.get(group) or [{}])[0]


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
    p = new_player(args.url, 'A')
    uid = p['id']
    print(f'account: {uid}')

    cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)')]
    template = dict(zip(cols, db.one('SELECT * FROM user_units WHERE user_id = ? LIMIT 1', uid)))
    template.pop('user_unit_id')
    mst_id = str(template['unit_id']).split('_')[0]
    units = {r['pn16CNah']: r for r in next(iter(json.loads(
        (ROOT / 'deploy/mst/unit_mst.json').read_text(encoding='utf-8')).values()))}
    max_lv = int(units[mst_id]['EI1DF8Yt'])
    print(f'base and fodder: unit {mst_id}, max level {max_lv}')
    if max_lv < 4:
        print('the starter unit cannot be levelled; nothing to test', file=sys.stderr)
        return 2

    def unit(level: int) -> int:
        row = dict(template, unit_lvl=level, total_exp=0, exp=0, favorite_flg=0)
        db.rows(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", *row.values())
        return db.value('SELECT max(user_unit_id) FROM user_units WHERE user_id = ?', uid)

    def set_zel(zel: int) -> None:
        db.rows('UPDATE user_info SET zel = ? WHERE id = ?', zel, uid)

    def zel() -> int:
        return db.value('SELECT zel FROM user_info WHERE id = ?', uid)

    def owned(ids: list[int]) -> int:
        return db.value(f"SELECT count(*) FROM user_units WHERE user_id = ? AND user_unit_id IN "
                        f"({','.join('?' for _ in ids)})", uid, *ids)

    def fuse(base_id: int, mat_ids: list[int], client_price: str) -> dict:
        return gme_call(args.url, *UNIT_MIX, {
            'IKqx1Cn9': p['login'],
            '60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}],
            'mCE3rUu5': [{'Rs7bCE3t': client_price}],
            'Km35HAXv': [{'edy7fq3L': str(base_id), 'mnZ5K4Ii': '1'}]
                        + [{'edy7fq3L': str(m), 'mnZ5K4Ii': '2'} for m in mat_ids]}, secrets.token_hex(4))

    def charged_case(tag: str, base_lv: int, mat_lvs: list[int], client_price) -> int:
        expected = mix_gold(base_lv, mat_lvs, max_lv)
        start = 1_000_000 + random.randrange(1000)
        set_zel(start)
        base_id, mats = unit(base_lv), [unit(lv) for lv in mat_lvs]
        sent = str(expected if client_price is None else client_price)
        reply = fuse(base_id, mats, sent)
        check(f'{tag}: the fusion succeeds', wire_error(reply) is None, wire_error(reply))
        check(f'{tag}: DB zel {start} -> {start - expected} (price {expected}, client sent {sent})',
              zel() == start - expected, zel())
        check(f'{tag}: the reply team info carries the new zel',
              str(first(reply, 'fEi17cnx').get('Najhr8m6')) == str(start - expected), first(reply, 'fEi17cnx'))
        check(f'{tag}: the result screen shows the charged price',
              str(first(reply, '1ZbHB6Im').get('Najhr8m6')) == str(expected), first(reply, '1ZbHB6Im'))
        check(f'{tag}: the materials are consumed', owned(mats) == 0, owned(mats))
        return expected

    print('\n-- A: Rs7bCE3t "0" is charged the server price')
    base_lv = random.randint(2, max_lv - 1)
    mat_lvs = [random.randint(1, max_lv) for _ in range(random.randint(2, 4))]
    since = str(int(time.time()) - 2)
    price = charged_case('A', base_lv, mat_lvs, '0')
    done = subprocess.run(['docker', 'logs', '--since', since, container], capture_output=True, text=True)
    logs = done.stdout + done.stderr
    line = f'UnitMix: client priced 0 zel, getMixGold gives {price} (base lv {base_lv}'
    check('A: the client/server mismatch is logged once', logs.count(line) == 1, logs.count(line))

    print('\n-- B: a normal request with the client\'s own price')
    charged_case('B', 3, [2], None)

    print('\n-- C: a maxed base pays a quarter')
    charged_case('C', max_lv, [1, 1, 1], '0')

    print('\n-- D: an inflated client price is not charged')
    charged_case('D', 1, [1], '99999')

    print('\n-- E: a wallet one Zel short is refused softly, nothing changes')
    expected = mix_gold(5, [4, 2], max_lv)
    set_zel(expected - 1)
    base_id, mats = unit(5), [unit(4), unit(2)]
    reply = fuse(base_id, mats, str(expected))
    err = wire_error(reply) or {}
    check('E: refused', wire_error(reply) is not None, reply)
    check('E: as ReturnToGame (cmd 6), not a session close', str(err.get('iPD12YCr')) == SOFT_HOME, err)
    check('E: zel untouched', zel() == expected - 1, zel())
    check('E: the materials are still owned', owned(mats) == len(mats), owned(mats))
    check('E: the base keeps its level',
          db.value('SELECT unit_lvl FROM user_units WHERE user_unit_id = ?', base_id) == 5)

    print('\n-- F: after the refusal a normal request still works')
    set_zel(expected)
    reply = fuse(base_id, mats, str(expected))
    check('F: the same fusion succeeds with exactly enough zel', wire_error(reply) is None, wire_error(reply))
    check('F: zel goes to 0', zel() == 0, zel())
    check('F: the materials are consumed', owned(mats) == 0, owned(mats))

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
