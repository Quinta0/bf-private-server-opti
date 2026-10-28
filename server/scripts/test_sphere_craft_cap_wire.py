"""Wire test: a once-only recipe cannot be crafted twice (ticket #166).

ReceipeMst.required_material_amount is the craft cap (1 = craftable once).
Recipe 8229 makes Iridescent Hope (819284) from 6 x 890049 and 300,000 Karma.
With materials for two:
  - asking for two at once crafts ONE, and charges for one;
  - asking again crafts nothing and charges nothing;
  - with no craft recorded, a copy EQUIPPED on a unit still blocks the craft;
  - with no copy and no craft recorded, it crafts again (the cap is per copy made).

Usage: python3 -I scripts/test_sphere_craft_cap_wire.py [--url http://127.0.0.1:9975] [--db /tmp/gme.sqlite]
Run the server with its save INSIDE the container (db filename /tmp/gme.sqlite).
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
ITEM_MIX = ('4P5GELTF', 'AFqKIJ8Z4mHPB9xg')
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


def call(p: dict, request, body: dict, what: str) -> dict:
    reply = gme_call(p['base'], *request, dict(body, IKqx1Cn9=p['login'],
                                              **{'6FrKacq7': [{'Kn51uR4Y': 'namestest'}]}), secrets.token_hex(4))
    check(f'{what} answers without an error', wire_error(reply) is None, wire_error(reply))
    return reply



RECIPE, SPHERE, MATERIAL, KARMA = 8229, 819284, 890049, 300000


def mix(p: dict, csv: str, what: str) -> None:
    call(p, ITEM_MIX, {'JTf2jY5o': [{'4HqhTf3a': csv}]}, what)


def state(db: Database, p: dict) -> tuple:
    held = db.value('SELECT COALESCE(SUM(item_num), 0) FROM user_items WHERE user_id = ? AND item_id = ?', p['id'], SPHERE)
    mats = db.value('SELECT COALESCE(SUM(item_num), 0) FROM user_items WHERE user_id = ? AND item_id = ?', p['id'], MATERIAL)
    karma = db.value('SELECT karma FROM user_info WHERE id = ?', p['id'])
    crafts = db.value('SELECT COALESCE(SUM(craft_count), 0) FROM user_recipe_crafts WHERE user_id = ? AND recipe_id = ?',
                      p['id'], RECIPE)
    return held, mats, karma, crafts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/tmp/gme.sqlite')
    args = ap.parse_args()
    db = Database(args.container or container_on_port(args.url), args.db)
    p = new_player(args.url, db, 'H')
    print(f'account: {p["id"]}')
    db.rows('INSERT INTO user_town_facilities (user_id, facility_id, lv) VALUES (?, 1, 1)'
            ' ON CONFLICT(user_id, facility_id) DO UPDATE SET lv = 1', p['id'])
    db.rows("INSERT INTO user_campaign_missions (user_id, mission_id, state) VALUES (?, '21', 2)"
            ' ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2', p['id'])
    db.rows('INSERT INTO user_items (user_id, item_id, item_num) VALUES (?, ?, 12)'
            ' ON CONFLICT(user_id, item_id) DO UPDATE SET item_num = 12', p['id'], MATERIAL)
    db.rows('UPDATE user_info SET karma = ? WHERE id = ?', 2 * KARMA, p['id'])

    mix(p, f'{RECIPE}:2', 'ItemMix asking for two Iridescent Hope at once')
    held, mats, karma, crafts = state(db, p)
    check('two at once: ONE Iridescent Hope is made', held == 1, held)
    check('...and only one craft is charged (6 materials, 300,000 Karma)', mats == 6 and karma == KARMA, (mats, karma))
    check('...and recorded once', crafts == 1, crafts)

    mix(p, f'{RECIPE}:1', 'ItemMix asking for another')
    check('a second craft makes nothing and charges nothing', state(db, p) == (1, 6, KARMA, 1), state(db, p))

    mix(p, f'{RECIPE}:1,{RECIPE}:1', 'ItemMix listing the recipe twice')
    check('listing it twice in one request makes nothing more', state(db, p)[0] == 1, state(db, p))

    # A copy on a unit, no craft recorded (as for a craft before the count existed).
    unit = db.value('SELECT user_unit_id FROM user_units WHERE user_id = ? LIMIT 1', p['id'])
    db.rows('DELETE FROM user_items WHERE user_id = ? AND item_id = ?', p['id'], SPHERE)
    db.rows('DELETE FROM user_recipe_crafts WHERE user_id = ? AND recipe_id = ?', p['id'], RECIPE)
    db.rows('UPDATE user_units SET eqip_item_id = ? WHERE user_unit_id = ?', SPHERE, unit)
    mix(p, f'{RECIPE}:1', 'ItemMix with the only copy equipped')
    check('an equipped copy blocks the craft', state(db, p)[:2] == (0, 6), state(db, p))

    db.rows('UPDATE user_units SET eqip_item_id = 0 WHERE user_unit_id = ?', unit)
    mix(p, f'{RECIPE}:1', 'ItemMix with no copy and no craft recorded')
    check('with no copy and no record it crafts one', state(db, p)[:2] == (1, 0), state(db, p))

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
