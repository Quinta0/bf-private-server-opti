"""Wire test: Grand Mission bonuses pay once, and a unit wears one sphere per category (ticket #166 audit).

  CampaignReceipt  The Serial Code screen's request no longer doubles as a Grand
                   Mission claim.  After CampaignEnd has paid a cleared mission's
                   First Quest Clear and Completion bonuses (rewards_got), a
                   crafted receipt naming that mission with no code -- once,
                   with an empty code, or 8 copies in parallel -- pays nothing
                   more (gems, zel, karma, items, units, presents unchanged) and
                   is still answered with the header.  A typed code is still a
                   serial-code redemption (an unknown one is refused plainly).
  ItemSphereEqp    The final pair a batch leaves on a two-slot unit may not be
                   the same sphere twice nor two spheres of one non-zero
                   sphere_type (ItemSphereSelectScene::checkEqpSphere): such a
                   batch is answered {} and changes nothing (unit, warehouse
                   rows, stock).  Two categories work, a one-slot change that
                   would make a same-category pair is refused, swapping slot 1
                   to the other same-category sphere next to a third category
                   works, and a one-slot unit still equips.

Usage: python3 -I scripts/test_batch8_campaign_sphere_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite]
The test writes sphere stock, a Sphere Frog slot and an open Grand Mission run
with docker exec, so run the server with its save INSIDE the container (a
config whose db filename is /tmp/gme.sqlite): on a Docker Desktop bind mount
the server and an exec'd sqlite3 do not see each other's writes reliably.
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
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
ITEM_SPHERE_EQP = ('0IXGiC9t', 'CZE56XAY')
CAMPAIGN_END = ('jF9Kkro4', '4X9tBSg8')
CAMPAIGN_RECEIPT = ('5Imq3wC0', '4DAgP80B')

MST = Path(__file__).resolve().parents[1] / 'deploy' / 'mst'
PARALLEL = 8
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


def mst_rows(name: str) -> list:
    """The rows of one deploy/mst table (a {wrapper: [rows]} file)."""
    data = json.loads((MST / name).read_text(encoding='utf-8'))
    return next(v for v in data.values() if isinstance(v, list))


def new_player(base: str, label: str) -> dict:
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


def send(p: dict, request, body: dict) -> dict:
    return gme_call(p['base'], *request, dict(body, IKqx1Cn9=p['login'],
                                              **{'6FrKacq7': [{'Kn51uR4Y': 'batch8test'}]}), secrets.token_hex(4))


def call(p: dict, request, body: dict, what: str) -> dict:
    reply = send(p, request, body)
    check(f'{what} answers without an error', wire_error(reply) is None, wire_error(reply))
    return reply


# ------------------------------------------------------------------ spheres --

def sphere_picks(held: set) -> tuple[int, int, int, dict]:
    """Two single-stack spheres of one non-zero sphere_type (a, b) and one of
    another (c), none of them already held."""
    types, by_type = {}, {}
    for r in mst_rows('item_mst.json'):
        kind = str(r.get('92udyUrJ', ''))
        if r.get('h0K7wjeH') != '3' or r.get('m9gd5h1u') != '1' or not kind.isdigit() or kind == '0':
            continue
        item = int(r['kixHbe54'])
        types[item] = int(kind)
        if item not in held:
            by_type.setdefault(int(kind), []).append(item)
    pair = next(t for t, items in sorted(by_type.items()) if len(items) >= 2)
    s_a, s_b = sorted(by_type[pair])[:2]
    s_c = sorted(by_type[next(t for t in sorted(by_type) if t != pair)])[0]
    return s_a, s_b, s_c, types


def sphere_tests(base: str, db: Database) -> None:
    print('\n-- ItemSphereEqp: one sphere per category')
    p = new_player(base, 'S')
    uid = p['id']
    print(f'account: {uid}')
    units = [r['edy7fq3L'] for r in call(p, USER_INFO, {}, 'UserInfo').get('4ceMWH6k') or []]
    check('the account owns a unit to test with', bool(units), units)
    if not units:
        return
    two, one = int(units[0]), (int(units[1]) if len(units) > 1 else None)
    held = {r[0] for r in db.rows('SELECT item_id FROM user_items WHERE user_id = ?', uid)} | \
           {r[0] for r in db.rows('SELECT item_id FROM user_warehouse_rows WHERE user_id = ?', uid)}
    s_a, s_b, s_c, types = sphere_picks(held)
    check('setup: a and b share a non-zero sphere_type, c has another',
          types[s_a] == types[s_b] != 0 and types[s_c] not in (0, types[s_a]), (s_a, s_b, s_c))
    # A Sphere Frog second slot on one unit; the other keeps one slot.
    db.rows('UPDATE user_units SET sphere_ext = 1, eqip_item_id = 0, eqip_item_id2 = 0,'
            ' eqip_item_frame_id = 0, eqip_item_frame_id2 = 0 WHERE user_unit_id = ?', two)
    if one is not None:
        db.rows('UPDATE user_units SET sphere_ext = 0, eqip_item_id = 0, eqip_item_id2 = 0,'
                ' eqip_item_frame_id = 0, eqip_item_frame_id2 = -1 WHERE user_unit_id = ?', one)
    # Two copies of a (so a doubled a is not refused for lack of stock), one of b and c.
    for item, count in ((s_a, 2), (s_b, 1), (s_c, 1)):
        db.rows('INSERT INTO user_items (user_id, item_id, item_num) VALUES (?, ?, ?)'
                ' ON CONFLICT(user_id, item_id) DO UPDATE SET item_num = excluded.item_num', uid, item, count)
    call(p, USER_INFO, {}, 'UserInfo after stocking (the login sync gives the copies rows)')

    def unit(u: int) -> list:
        return db.one('SELECT eqip_item_id, eqip_item_frame_id, eqip_item_id2, eqip_item_frame_id2'
                      ' FROM user_units WHERE user_unit_id = ?', u)

    def stock(item: int) -> int:
        return db.value('SELECT item_num FROM user_items WHERE user_id = ? AND item_id = ?', uid, item) or 0

    def snapshot():
        return (db.rows('SELECT user_unit_id, eqip_item_id, eqip_item_frame_id, eqip_item_id2, eqip_item_frame_id2'
                        ' FROM user_units WHERE user_id = ? ORDER BY user_unit_id', uid),
                db.rows('SELECT instance_id, item_id, item_num, equip_unit_id, equip_slot FROM user_warehouse_rows'
                        ' WHERE user_id = ? ORDER BY instance_id', uid),
                db.rows('SELECT item_id, item_num FROM user_items WHERE user_id = ? ORDER BY item_id', uid))

    def equip(packed: str, what: str) -> dict:
        return call(p, ITEM_SPHERE_EQP, {'wx1ZLFj9': [{'a2utCvs8': packed}]}, what)

    def refused(packed: str, what: str) -> None:
        before = snapshot()
        equip(packed, what)
        after = snapshot()
        check(f'{what}: refused, nothing changed (unit, rows, stock)', after == before, (before, after))

    refused(f'0:0:{two}:{s_a}:{s_a}', 'the same sphere in both slots')
    check('...the unit still wears nothing', unit(two) == [0, 0, 0, 0], unit(two))
    refused(f'0:0:{two}:{s_a}:{s_b}', 'two different spheres of one sphere_type')

    equip(f'0:0:{two}:{s_a}:{s_c}', 'two spheres of different sphere_types')
    worn = unit(two)
    check('two categories: both are worn, each on its own copy',
          worn[0] == s_a and worn[2] == s_c and worn[1] > 0 and worn[3] > 0 and worn[1] != worn[3], worn)
    check('two categories: one copy of each left storage', stock(s_a) == 1 and stock(s_c) == 0,
          (stock(s_a), stock(s_c)))
    frame_a, frame_c = worn[1], worn[3]

    refused(f'{frame_a}:0:{two}:{s_a}:{s_b}', 'changing only slot 2 to the same sphere_type as slot 1')
    refused(f'{frame_a}:0:{two}:{s_a}:{s_a}', 'changing only slot 2 to the sphere slot 1 wears')

    equip(f'0:{frame_c}:{two}:{s_b}:{s_c}', 'slot 1 to the other same-category sphere, next to a third category')
    worn = unit(two)
    check('the pair is judged as the batch leaves it: b and c are worn, c on the same copy',
          worn[0] == s_b and worn[2] == s_c and worn[3] == frame_c, worn)
    check('...and the a copy is back in storage', stock(s_a) == 2 and stock(s_b) == 0, (stock(s_a), stock(s_b)))

    if one is not None:
        equip(f'0:-1:{one}:{s_a}:0', 'a one-slot unit equipping slot 1')
        check('a one-slot unit still equips (the pair rule needs two spheres)',
              unit(one)[0] == s_a and unit(one)[3] == -1, unit(one))
    else:
        print('SKIP  a one-slot unit (the account has a single unit)')


# ------------------------------------------------------------ Grand Mission --

def grand_mission() -> tuple[str, list[str], list[dict]]:
    """The first Grand Mission with a once-only First Quest Clear bonus and
    Completion bonuses, every flag it has (so the run reaches 100 %), and its
    reward rows."""
    rewards = mst_rows('grand_mission_reward_mst.json')
    by_mission = {}
    for r in rewards:
        by_mission.setdefault(r['j28VNcUW'], []).append(r)
    mission = next(m for m, rows in sorted(by_mission.items(), key=lambda kv: int(kv[0]))
                   if any(r['IkmC8gG2'] == '4' and r['64hAvkZX'] == '1' for r in rows)
                   and any(r['IkmC8gG2'] == '1' for r in rows))
    flags = [r['NML08EGS'] for r in mst_rows('grand_mission_flg_mst.json') if r['j28VNcUW'] == mission]
    return mission, flags, by_mission[mission]


def earned_gems(rewards: list[dict], flags: list[str], percent: int) -> int:
    """Gems (present_type 8) CampaignEnd pays a first clear at `percent` with `flags` set."""
    total = 0
    for r in rewards:
        rt, cond = r['IkmC8gG2'], r['q7Nit8JW']
        earned = (rt == '4') or (rt == '1' and cond.isdigit() and percent >= int(cond)) or \
                 (rt == '2' and cond != '' and cond in flags)
        if earned and r['30Kw4WBa'] == '8':
            total += int(r['wJsB35iH'])
    return total


def campaign_tests(base: str, db: Database) -> None:
    print('\n-- CampaignReceipt: Grand Mission bonuses are CampaignEnd\'s alone')
    p = new_player(base, 'G')
    uid = p['id']
    print(f'account: {uid}')
    mission, flags, rewards = grand_mission()
    percents = sum(int(r['ug9xV4Fz']) for r in mst_rows('grand_mission_flg_mst.json') if r['j28VNcUW'] == mission)
    first_clear = next(r['N1b6SUW4'] for r in rewards if r['IkmC8gG2'] == '4' and r['64hAvkZX'] == '1')
    print(f'mission {mission}: {len(flags)} flags ({percents} %), first-clear reward {first_clear}')

    def wallet() -> dict:
        return {'info': db.one('SELECT zel, karma, gems FROM user_info WHERE id = ?', uid),
                'items': db.rows('SELECT item_id, item_num FROM user_items WHERE user_id = ? ORDER BY item_id', uid),
                'units': db.value('SELECT COUNT(*) FROM user_units WHERE user_id = ?', uid),
                'presents': db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ?', uid),
                'mission': db.rows('SELECT state, clear_count, attain_percent, rewards_got, reward_claimed'
                                   ' FROM user_campaign_missions WHERE user_id = ? ORDER BY mission_id', uid)}

    # An open run of the mission, as CampaignStart / DeckEdit leave it.
    db.rows('INSERT INTO user_campaign_state (user_id, active_mission_id, run_open) VALUES (?, ?, 1)'
            ' ON CONFLICT(user_id) DO UPDATE SET active_mission_id = excluded.active_mission_id, run_open = 1',
            uid, mission)
    gems_before = db.value('SELECT gems FROM user_info WHERE id = ?', uid)
    presents_before = db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ?', uid)
    call(p, CAMPAIGN_END, {'2I9V0o6J': [{'j28VNcUW': mission}],
                           'CHd0w9WU': [{'C03bapzk': '1', 'j3g5P4cq': '2'}],
                           'NjZ6ds1S': [], 'Nv94RF5q': [],
                           'RseDpY04': [{'JcKMjH64': ','.join(flags), '8n7oG4fS': '0', 'W1EkCyc3': '0'}]},
         'CampaignEnd clearing the mission with every flag')
    row = db.one('SELECT state, clear_count, attain_percent, rewards_got FROM user_campaign_missions'
                 ' WHERE user_id = ? AND mission_id = ?', uid, mission)
    got = set((row[3] if row else '').split(','))
    check('CampaignEnd: the mission is cleared at 100 % and the first-clear bonus recorded',
          row is not None and row[0] == 2 and row[1] == 1 and row[2] == min(percents, 100) and first_clear in got, row)
    expected = earned_gems(rewards, flags, min(percents, 100))
    gems_after = db.value('SELECT gems FROM user_info WHERE id = ?', uid)
    check(f'CampaignEnd: paid the bonus gems once (+{expected})', gems_after - gems_before == expected,
          (gems_before, gems_after))
    check('CampaignEnd: the other bonuses went to the present box',
          db.value('SELECT COUNT(*) FROM user_presents WHERE user_id = ?', uid) > presents_before)

    before = wallet()
    reply = call(p, CAMPAIGN_RECEIPT, {'j28VNcUW': mission}, 'a crafted CampaignReceipt naming the mission, no code')
    check('...answered with the header (team info)', bool(reply.get('fEi17cnx')), list(reply)[:8])
    check('...pays nothing more (gems, zel, karma, items, units, presents, mission row)', wallet() == before,
          (before, wallet()))
    call(p, CAMPAIGN_RECEIPT, {'j28VNcUW': mission, '4MCxgS5p': [{'pCIRMw04': '  '}]},
         'a crafted CampaignReceipt naming the mission, blank code')
    check('...pays nothing more', wallet() == before, (before, wallet()))

    with concurrent.futures.ThreadPoolExecutor(max_workers=PARALLEL) as pool:
        replies = list(pool.map(lambda _: send(p, CAMPAIGN_RECEIPT, {'j28VNcUW': mission}), range(PARALLEL)))
    check(f'{PARALLEL} parallel crafted receipts all answer without an error',
          all(wire_error(r) is None for r in replies), [wire_error(r) for r in replies if wire_error(r)])
    check(f'...and together pay nothing', wallet() == before, (before, wallet()))

    reply = send(p, CAMPAIGN_RECEIPT, {'j28VNcUW': mission, '4MCxgS5p': [{'pCIRMw04': 'NOTACODE'}]})
    check('a typed code is still a serial-code redemption: an unknown code is refused plainly',
          'This serial code is not valid.' in json.dumps(reply, ensure_ascii=False), reply)
    check('...and pays nothing', wallet() == before, (before, wallet()))


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
    sphere_tests(args.url, db)
    campaign_tests(args.url, db)
    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
