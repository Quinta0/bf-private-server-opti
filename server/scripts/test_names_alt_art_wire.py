"""Wire test: renames and per-unit alternate art survive a restart (#144, #145).

  #144  A rename arrives in DeckEdit's team node (Ti62XfZK[0].B5JQyV8j) and is
        stored; UserInfo then sends the new name.  Unusable names (too long,
        blank, markup characters) are skipped and the DeckEdit still succeeds.
  #145  UnitFavorite lists every unit showing alt art (2pAyFjmZ "2").  It is
        stored only when the player owns that unit's alt art, cleared when the
        unit is no longer listed, and UserInfo's unit list sends it back.

Usage: python3 -I scripts/test_names_alt_art_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite]
The test writes an alt-art unlock with docker exec, so run the server with its
save INSIDE the container (a config whose db filename is /tmp/gme.sqlite): on
a Docker Desktop bind mount the server and an exec'd sqlite3 do not see each
other's writes reliably.
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
DECK_EDIT = ('m2Ve9PkJ', 'd7UuQsq8')
UNIT_FAVORITE = ('2p9LHCNh', 'cb4ESLa1')
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


def rename(p: dict, name: str, what: str) -> None:
    call(p, DECK_EDIT, {'Ti62XfZK': [{'Z0Y4RoD7': '0', 'gKNfIZiA': '0', 'bM7RLu5K': '', 'B5JQyV8j': name}]}, what)


def login_name(p: dict) -> str:
    reply = call(p, USER_INFO, {}, 'UserInfo')
    return ((reply.get('IKqx1Cn9') or [{}])[0]).get('B5JQyV8j', '')


def unit_rows(p: dict) -> dict:
    reply = call(p, USER_INFO, {}, 'UserInfo')
    return {r['edy7fq3L']: r for r in reply.get('4ceMWH6k') or []}


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
    p = new_player(args.url, db, 'A')
    print(f'account: {p["id"]}')

    print('\n-- #144 renames')
    check('UserInfo sends the name chosen at creation', login_name(p) == p['name'], login_name(p))
    rename(p, 'Renamed1', 'DeckEdit with a rename')
    check('DB: the new name is stored', db.value('SELECT username FROM user_info WHERE id = ?', p['id']) == 'Renamed1')
    check('UserInfo (next login) sends the new name', login_name(p) == 'Renamed1', login_name(p))
    call(p, DECK_EDIT, {'Ti62XfZK': [{'Z0Y4RoD7': '0', 'gKNfIZiA': '0', 'bM7RLu5K': ''}]}, 'DeckEdit without a rename')
    check('a DeckEdit without a name leaves it alone', login_name(p) == 'Renamed1', login_name(p))
    for bad, why in (('NineChars', 'over 8 characters'), ('   ', 'blank'), ('a<3', 'markup character'),
                     ('tab\tname', 'control character'), ('\u3000\u3000', 'only ideographic spaces'),
                     ('a\u200bb', 'zero-width character'), ('\u202eabc', 'bidi override')):
        rename(p, bad, f'DeckEdit renaming to a name that is {why}')
        check(f'a name that is {why} is not stored', login_name(p) == 'Renamed1', login_name(p))
    rename(p, 'あいうえおかきく', 'DeckEdit with an 8-character Japanese name')
    check('8 multi-byte characters fit (characters, not bytes)', login_name(p) == 'あいうえおかきく', login_name(p))

    print('\n-- #145 alternate art per unit')
    units = unit_rows(p)
    check('the account owns a unit to test with', bool(units), units)
    uid, row = next(iter(units.items()))
    species = int(row['pn16CNah'])
    check('a fresh unit shows its base art', str(row.get('2pAyFjmZ')) in ('0', '1'), row.get('2pAyFjmZ'))

    def show(img: str, what: str) -> None:
        call(p, UNIT_FAVORITE, {'3kcmQy7B': [{'edy7fq3L': uid, '5JbjC3Pp': '0', '2pAyFjmZ': img}]}, what)

    show('2', 'UnitFavorite asking for alt art the player does not own')
    check('alt art that is not owned is not stored', str(unit_rows(p)[uid].get('2pAyFjmZ')) == '0')
    db.rows('INSERT OR IGNORE INTO user_unit_alt_art (user_id, unit_id) VALUES (?, ?)', p['id'], species)
    show('2', 'UnitFavorite with the unit on owned alt art')
    check('DB: the unit is stored on alt art', db.value('SELECT unit_img_type FROM user_units WHERE user_unit_id = ?',
                                                        int(uid)) == 2)
    check('UserInfo (next login) sends the unit on alt art', str(unit_rows(p)[uid].get('2pAyFjmZ')) == '2')
    call(p, UNIT_FAVORITE, {'3kcmQy7B': [{'edy7fq3L': uid, '5JbjC3Pp': '1', '2pAyFjmZ': '2'}]},
         'UnitFavorite favouriting the alt-art unit')
    check('favouriting keeps the alt art', str(unit_rows(p)[uid].get('2pAyFjmZ')) == '2')
    check('and the favourite', db.value('SELECT favorite_flg FROM user_units WHERE user_unit_id = ?', int(uid)) == 1)
    call(p, UNIT_FAVORITE, {'3kcmQy7B': []}, 'UnitFavorite with the unit back on base art (not listed)')
    check('a unit no longer listed is back on its base art', str(unit_rows(p)[uid].get('2pAyFjmZ')) == '0')

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
