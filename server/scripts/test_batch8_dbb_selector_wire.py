"""Wire test: DBB bond rank and selector unit type are the server's (ticket #166 audit).

  DbbBond     The bond rank in the request (5eDLn6pr[0].M81OjLwr) is only an echo
              and is no longer stored as sent: a new pair starts at rank 1 even
              when the body claims 6, a re-bond keeps the rank already stored
              for that dbb_id (same partner or a new one), a stored rank outside
              1..6 is clamped, and after an unbond nothing is left to keep.
  UnitSelectorGachaTicket
              The confirm screen offers unit types 1-5 only.  A pick naming type
              6 (Rex) is not refused, but the granted unit is not type 6.

Usage: python3 -I scripts/test_batch8_dbb_selector_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite]
The test writes units, bond ranks and selector tickets with docker exec, so run
the server with its save INSIDE the container (a config whose db filename is
/tmp/gme.sqlite): on a Docker Desktop bind mount the server and an exec'd
sqlite3 do not see each other's writes reliably.
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
DBB_BOND = ('0EtanubR', 'Tr7dR4dR')
SELECTOR_PICK = ('k57TdKDj', '1IJ8SaNk')

MAX_BOND_LEVEL = 6       # gme::kDbbMaxBondLevel
UNIT_PICK_TICKETS = 9000  # 9001+ are the 50-gem / race pickers, not stored tickets


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
                                    'dp': 'Android', 'dn': 'batch8-test', 'v': '16'})
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    if status != 200:
        raise RuntimeError(f'guest login HTTP {status}')
    login = json.loads(body)
    node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
    name = f'B{label}{RUN}'
    gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=name)], '9u45RGcV': [{'Kn51uR4Y': '1'}]},
             secrets.token_hex(4))
    init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]}, secrets.token_hex(4))
    user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
    if not user_id:
        raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
    return {'base': base, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id, 'name': name}


def call(p: dict, request, body: dict, what: str) -> dict:
    reply = gme_call(p['base'], *request, dict(body, IKqx1Cn9=p['login'],
                                              **{'6FrKacq7': [{'Kn51uR4Y': 'batch8test'}]}), secrets.token_hex(4))
    check(f'{what} answers without an error', wire_error(reply) is None, wire_error(reply))
    return reply


def add_unit(db: Database, p: dict, species: int, dbb_open: bool) -> int:
    """Copy the account's first unit as a new row of `species`, so every NOT NULL
    column has a real value, optionally with its DBB slot already open."""
    template = db.value('SELECT MIN(user_unit_id) FROM user_units WHERE user_id = ?', p['id'])
    if template is None:
        raise RuntimeError('the account owns no unit to copy')
    cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)')
            if r[1] not in ('user_unit_id', 'unit_id', 'dbb_unlocked')]
    names = ', '.join(cols)
    db.rows(f'INSERT INTO user_units ({names}, unit_id, dbb_unlocked)'
            f' SELECT {names}, ?, ? FROM user_units WHERE user_unit_id = ?',
            species, 1 if dbb_open else 0, template)
    return db.value('SELECT MAX(user_unit_id) FROM user_units WHERE user_id = ? AND unit_id = ?',
                    p['id'], species)


def bond(p: dict, main: int, sub: int, claimed: str, what: str, kind: str = '1') -> dict:
    return call(p, DBB_BOND, {'5eDLn6pr': [{'KoC1ogir': str(main), 'd5enutLC': str(sub),
                                            'deKa4iva': kind, 'M81OjLwr': claimed}]}, what)


def stored_rank(db: Database, p: dict, main: int):
    return db.value('SELECT bond_level FROM user_unit_dbb WHERE user_id = ? AND user_unit_id = ?', p['id'], main)


def reply_rank(reply: dict, dbb_id: str):
    for row in reply.get('tR4katob') or []:
        if str(row.get('sp3Xi9hu')) == dbb_id:
            return str(row.get('M81OjLwr'))
    return None


def test_dbb(db: Database, p: dict, info: dict) -> None:
    print('\n-- DbbBond: the rank is the stored one, not the echoed one')
    pairs = [r for r in info.get('fTV3A0By') or []
             if str(r.get('3gZAK2vU', '')).isdigit() and str(r.get('zY9XKiSV', '')).isdigit()
             and r['3gZAK2vU'] != r['zY9XKiSV']]
    check('UserInfo sends the DbbMst catalogue', bool(pairs), len(info.get('fTV3A0By') or []))
    if not pairs:
        return
    dbb_id = str(pairs[0]['XIvaD6Jp'])
    species_a, species_b = int(pairs[0]['3gZAK2vU']), int(pairs[0]['zY9XKiSV'])
    print(f'pairing: DBB {dbb_id} = {species_a} + {species_b}')
    a = add_unit(db, p, species_a, True)
    b = add_unit(db, p, species_b, True)
    b2 = add_unit(db, p, species_b, True)

    reply = bond(p, a, b, str(MAX_BOND_LEVEL), 'a new bond claiming rank 6')
    check('DB: a new pair claiming rank 6 is stored at rank 1', stored_rank(db, p, a) == 1, stored_rank(db, p, a))
    check('the reply reports the new bond at rank 1', reply_rank(reply, dbb_id) == '1', reply_rank(reply, dbb_id))

    # What UnitBondBoost would have stored after two paid raises.
    db.rows('UPDATE user_unit_dbb SET bond_level = 3 WHERE user_id = ? AND dbb_id = ?', p['id'], dbb_id)
    reply = bond(p, a, b, str(MAX_BOND_LEVEL), 're-bonding the same pair claiming rank 6')
    check('DB: re-bonding keeps the earned rank 3, not the claimed 6', stored_rank(db, p, a) == 3,
          stored_rank(db, p, a))
    check('the reply reports rank 3', reply_rank(reply, dbb_id) == '3', reply_rank(reply, dbb_id))
    bond(p, a, b, '0', 're-bonding the same pair with the first-bond echo 0')
    check('DB: an echo of 0 does not reset the earned rank', stored_rank(db, p, a) == 3, stored_rank(db, p, a))

    bond(p, a, b2, '1', 're-bonding to another copy of the partner (same DBB) claiming rank 1')
    check('DB: the rank of the replaced row is kept for the new partner', stored_rank(db, p, a) == 3,
          stored_rank(db, p, a))
    check('DB: the old pair is gone, the new partner holds the bond',
          db.rows('SELECT bonded_user_unit_id FROM user_unit_dbb WHERE user_id = ? AND dbb_id = ?',
                  p['id'], dbb_id) == [[b2]])

    db.rows('UPDATE user_unit_dbb SET bond_level = 9 WHERE user_id = ? AND dbb_id = ?', p['id'], dbb_id)
    bond(p, a, b2, '9', 're-bonding over an out-of-range stored rank 9')
    check(f'DB: a stored rank above {MAX_BOND_LEVEL} is clamped to {MAX_BOND_LEVEL}',
          stored_rank(db, p, a) == MAX_BOND_LEVEL, stored_rank(db, p, a))

    bond(p, a, b2, str(MAX_BOND_LEVEL), 'unbonding the pair', kind='2')
    check('DB: the unbond leaves no row', stored_rank(db, p, a) is None, stored_rank(db, p, a))
    bond(p, a, b2, str(MAX_BOND_LEVEL), 'bonding again after the unbond claiming rank 6')
    check('DB: with nothing stored, a claimed rank 6 starts at 1', stored_rank(db, p, a) == 1,
          stored_rank(db, p, a))


def pool_of(row: dict) -> list[int]:
    raw = row.get('r7MSYD2c')
    items = raw.split(',') if isinstance(raw, str) else (raw or [])
    return [int(x) for x in (str(i).strip() for i in items) if x.isdigit()]


def pick(p: dict, selector: dict, unit: int, unit_type: str, what: str) -> dict:
    return call(p, SELECTOR_PICK, {'CGHaOZda': [{'7Ffmi96v': str(selector['7Ffmi96v']),
                                                 'XIvaD6Jp': str(selector['XIvaD6Jp']),
                                                 'pn16CNah': str(unit), 'H6k1LIxC': '1',
                                                 'C5QbG2DM': unit_type}]}, what)


def granted_type(db: Database, p: dict, reply: dict, unit: int):
    granted = (reply.get('qC2tJs4E') or [{}])[0]
    user_unit_id = granted.get('edy7fq3L')
    if user_unit_id is None:
        user_unit_id = db.value('SELECT MAX(user_unit_id) FROM user_units WHERE user_id = ? AND unit_id = ?',
                                p['id'], unit)
    stored = db.value('SELECT unit_type_id FROM user_units WHERE user_unit_id = ?', int(user_unit_id))
    return granted.get('nBTx56W9'), stored


def test_selector(db: Database, p: dict, info: dict) -> None:
    print('\n-- UnitSelectorGachaTicket: unit types 1-5 only')
    selectors = [r for r in info.get('JukkSeNA') or []
                 if str(r.get('XIvaD6Jp', '')).isdigit() and int(r['XIvaD6Jp']) < UNIT_PICK_TICKETS
                 and pool_of(r)]
    check('UserInfo sends a ticket selector with a pool', bool(selectors), len(info.get('JukkSeNA') or []))
    if not selectors:
        return
    selector = selectors[0]
    unit = pool_of(selector)[0]
    print(f'selector: ticket {selector["XIvaD6Jp"]}, gate {selector["7Ffmi96v"]}, unit {unit}')
    db.rows('INSERT OR REPLACE INTO user_selector_tickets (user_id, ticket_id, count) VALUES (?, ?, 2)',
            p['id'], str(selector['XIvaD6Jp']))

    reply = pick(p, selector, unit, '3', 'a pick choosing type 3')
    wire, stored = granted_type(db, p, reply, unit)
    check('a type-3 pick grants a type-3 unit (reply and DB)', str(wire) == '3' and stored == 3, (wire, stored))

    reply = pick(p, selector, unit, '6', 'a pick naming type 6 (Rex), which has no button')
    wire, stored = granted_type(db, p, reply, unit)
    check('DB: the type-6 pick did not grant a type-6 unit', stored is not None and 1 <= stored <= 5, stored)
    check('the reply does not send a type-6 unit', str(wire) != '6', wire)
    check('DB: both picks spent a ticket',
          db.value('SELECT count FROM user_selector_tickets WHERE user_id = ? AND ticket_id = ?',
                   p['id'], str(selector['XIvaD6Jp'])) == 0)


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
    info = call(p, USER_INFO, {}, 'UserInfo')

    test_dbb(db, p, info)
    test_selector(db, p, info)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
