"""Wire test: once-only claims pay once when the same request arrives in parallel (#166 audit).

Each of these was a read-check-pay-write race: N copies of one request, fired
together by a proxy or a modified client, all read "not claimed" and all paid.

  Daily Spin (4aClzokO)       load, award and consume now share one transaction:
                              six parallel spins use ONE spin and pay ONE prize,
                              on an ordinary day and on a guaranteed-Gem day.
  Welcome chests (2paswUpR)   the claim is a conditional UPDATE (claimed = 0):
                              six parallel claims of one chest open it once.
  Tutorial pack (T1nCVvx4)    the present check and the queueing share one
                              transaction: six parallel completions queue one pack.

Usage: python3 -I scripts/test_batch8_claim_races_a_wire.py [--url http://127.0.0.1:9975]
           [--container NAME] [--db /tmp/gme.sqlite] [--archive deploy/archive] [--copies 6]
The test writes the spin state with docker exec, so run the server with its save
INSIDE the container (a config whose db filename is /tmp/gme.sqlite): on a
Docker Desktop bind mount the server and an exec'd sqlite3 do not see each
other's writes reliably.  --archive is the server's archive_root as seen from
here (daily_login.json and mystery_chest.json), default this repo's deploy/archive.
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
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from pathlib import Path

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
TUTORIAL_UPDATE = ('T1nCVvx4', '7hqzmR3T')
MYSTERY_LIST = ('pAJ2Xesw', 'DaswA3rE')
MYSTERY_CLAIM = ('2paswUpR', 'kadRadU5')
DAILY_LOGIN = ('4aClzokO', 'stI81haQ')

SPACES = 6          # kDailySpinSpaces
GEM_GROUP = 32      # kDailySpinGemGroup
GEM_DAY = 7         # a day flagged guaranteed_gem_on_first_spin

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


def envelope(group: str, key: str, body: dict, tag: str) -> bytes:
    """The encrypted request.  `tag` is GmeHeader.client_id: the client's request
    object id, the same on a retry and new for every new request."""
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    return json.dumps({
        'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': tag},
        'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(aes_ecb(key, raw + bytes([pad]) * pad, False)).decode()},
    }).encode()


def deliver(base: str, key: str, sealed: bytes) -> dict:
    status, data = fetch(f'{base}/bf/gme/action.php', sealed)
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = aes_ecb(key, base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']), True)
    return json.loads(plain[:-plain[-1]])


def gme_call(base: str, group: str, key: str, body: dict, tag: str) -> dict:
    return deliver(base, key, envelope(group, key, body, tag))


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
                                    'dp': 'Android', 'dn': 'race-test', 'v': '16'})
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
    return {'base': base, 'login': [dict(node, h7eY3sAK=user_id)], 'id': user_id, 'name': name}


def body_for(p: dict, body: dict, login: list | None = None) -> dict:
    return dict(body, IKqx1Cn9=login or p['login'], **{'6FrKacq7': [{'Kn51uR4Y': 'racetest'}]})


def call(p: dict, request, body: dict, what: str, login: list | None = None) -> dict:
    reply = gme_call(p['base'], *request, body_for(p, body, login), secrets.token_hex(4))
    check(f'{what} answers without an error', wire_error(reply) is None, wire_error(reply))
    return reply


def race(p: dict, request, body: dict, copies: int, login: list | None = None) -> list[dict]:
    """The SAME request (one envelope, one tag) sent `copies` times at once: every
    copy is sealed first and a barrier releases them together."""
    group, key = request
    sealed = envelope(group, key, body_for(p, body, login), secrets.token_hex(4))
    gate = threading.Barrier(copies)

    def one(_):
        gate.wait()
        return deliver(p['base'], key, sealed)

    with concurrent.futures.ThreadPoolExecutor(max_workers=copies) as pool:
        return list(pool.map(one, range(copies)))


def check_replies(replies: list[dict], what: str) -> None:
    errors = [wire_error(r) for r in replies if wire_error(r) is not None]
    check(f'{what}: all {len(replies)} parallel copies answer without an error', not errors, errors[:2])


def wallet(db: Database, uid: str) -> dict:
    """Everything a claim can pay: the four currencies, the item stacks, the units."""
    gems, zel, karma, tickets = db.one('SELECT gems, zel, karma, summon_tickets FROM user_info WHERE id = ?', uid)
    items = {int(i): int(n) for i, n in db.rows('SELECT item_id, item_num FROM user_items WHERE user_id = ?', uid)}
    units = Counter(int(u) for (u,) in db.rows('SELECT unit_id FROM user_units WHERE user_id = ?', uid))
    return {'gem': gems, 'zel': zel, 'karma': karma, 'ticket': tickets, 'items': items, 'units': units}


def paid(before: dict, after: dict) -> dict:
    """What changed between two wallets: currency deltas, item deltas, unit deltas (non-zero only)."""
    out = {k: after[k] - before[k] for k in ('gem', 'zel', 'karma', 'ticket') if after[k] != before[k]}
    for i in set(before['items']) | set(after['items']):
        d = after['items'].get(i, 0) - before['items'].get(i, 0)
        if d:
            out[f'item {i}'] = d
    for u in set(before['units']) | set(after['units']):
        d = after['units'].get(u, 0) - before['units'].get(u, 0)
        if d:
            out[f'unit {u}'] = d
    return out


def spin_info(reply: dict) -> dict:
    """Drudr2w5 (DailyLoginRewardsUserInfo), an object; tolerates a one-row array."""
    info = reply.get('Drudr2w5') or {}
    return (info[0] if info else {}) if isinstance(info, list) else info


def spin_prize(day_rows: list, reward_id: int) -> dict:
    """The space a stored last_reward_id names: id = (group - 1) * 6 + space + 1."""
    group, space = (reward_id - 1) // SPACES + 1, (reward_id - 1) % SPACES
    row = next(r for r in day_rows if int(r['day']) == group)
    return row['spaces'][space]


def expected_spin_payout(prize: dict) -> dict:
    if not prize.get('available', True):
        return {}
    kind, count = prize['kind'], int(prize['count'])
    if kind in ('gem', 'zel', 'karma', 'ticket'):
        return {kind: count}
    return {f'{kind} {int(prize["id"])}': count}


def spin_race(p: dict, db: Database, day_rows: list, copies: int, day: int, what: str,
              reward_ids: range, gem_only: bool) -> None:
    # Yesterday's row (last_spin_utc_day 0): the spin rolls over and today's one spin is free.
    db.rows('INSERT OR REPLACE INTO user_daily_spin (user_id, spin_day, spins_used, last_spin_utc_day,'
            ' last_reward_id) VALUES (?, ?, 0, 0, 0)', p['id'], day)
    before = wallet(db, p['id'])
    replies = race(p, DAILY_LOGIN, {}, copies)
    check_replies(replies, what)
    after = wallet(db, p['id'])
    day_now, used, reward_id = db.one('SELECT spin_day, spins_used, last_reward_id FROM user_daily_spin'
                                      ' WHERE user_id = ?', p['id'])
    check(f'{what}: DB spins_used = 1', used == 1, used)
    check(f'{what}: the cycle advanced one day ({day} -> {day + 1})', day_now == day + 1, day_now)
    check(f'{what}: the stored reward id is on the expected wheel', reward_id in reward_ids, reward_id)
    got = paid(before, after)
    if reward_id in reward_ids:
        prize = spin_prize(day_rows, reward_id)
        want = expected_spin_payout(prize)
        # A unit missing from this server's unit_mst is logged and skipped, never paid twice.
        skipped = prize.get('kind') == 'unit' and got == {}
        check(f'{what}: exactly one prize was paid, the one the reward id names', got == want or skipped,
              f'paid {got}, expected {want}')
    if gem_only:
        check(f'{what}: the guaranteed Gem was paid once, not once per copy', got == {'gem': 1}, got)
    # XIvaD6Jp is i32::str on the wire; str() also tolerates a bare number.
    ids = {str(spin_info(r).get('XIvaD6Jp')) for r in replies if wire_error(r) is None}
    check(f'{what}: every reply names the one stored reward id', ids == {str(reward_id)}, ids)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9975')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/tmp/gme.sqlite')
    ap.add_argument('--archive', default=str(Path(__file__).resolve().parents[1] / 'deploy' / 'archive'))
    ap.add_argument('--copies', type=int, default=6)
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)
    archive = Path(args.archive)
    day_rows = json.loads((archive / 'daily_login.json').read_text())
    chests = json.loads((archive / 'mystery_chest.json').read_text())
    n = args.copies

    print('\n-- Daily Spin (4aClzokO)')
    p = new_player(args.url, 'S')
    print(f'account: {p["id"]}')
    spin_race(p, db, day_rows, n, 1, f'{n} parallel spins on day 1',
              range(1, SPACES + 1), gem_only=False)
    gem_day = next((int(r['day']) for r in day_rows if r.get('guaranteed_gem_on_first_spin')), GEM_DAY)
    first_gem = (GEM_GROUP - 1) * SPACES + 1
    spin_race(p, db, day_rows, n, gem_day, f'{n} parallel spins on guaranteed-Gem day {gem_day}',
              range(first_gem, first_gem + SPACES), gem_only=True)
    before = wallet(db, p['id'])
    call(p, DAILY_LOGIN, {}, 'one more spin the same day')
    check('a spin after the day\'s spin is used pays nothing', paid(before, wallet(db, p['id'])) == {},
          paid(before, wallet(db, p['id'])))
    check('and leaves spins_used at 1',
          db.value('SELECT spins_used FROM user_daily_spin WHERE user_id = ?', p['id']) == 1)

    print('\n-- welcome chests (2paswUpR)')
    c = new_player(args.url, 'C')
    print(f'account: {c["id"]}')
    listed = call(c, MYSTERY_LIST, {}, 'MysteryBoxList (grants the welcome chests)')
    held = [b.get('rEFRefr8') for b in listed.get('d0ajLeRi') or []]
    check('the welcome chests are listed', bool(held) and all(ch['key'] in held for ch in chests), held)
    for chest in chests:
        key = chest['key']
        if key not in held:
            continue
        before = wallet(db, c['id'])
        replies = race(c, MYSTERY_CLAIM, {'d0ajLeRi': [{'rEFRefr8': key}]}, n)
        check_replies(replies, f'{n} parallel claims of {key}')
        after = wallet(db, c['id'])
        check(f'{key}: DB marks it opened',
              db.value('SELECT claimed FROM user_mystery_boxes WHERE user_id = ? AND box_id = ?',
                       c['id'], key) == 1)
        winners = [r for r in replies if (r.get('CoAp2aph') or [])]
        check(f'{key}: exactly one copy reports the chest\'s rewards', len(winners) == 1, len(winners))
        got = paid(before, after)
        rewards = chest.get('rewards') or []
        zel = sum(int(r['target_cnt']) for r in rewards if int(r['present_type']) == 3)
        gems = sum(int(r['target_cnt']) for r in rewards if int(r['present_type']) == 8)
        check(f'{key}: its Zel is paid once', got.get('zel', 0) == zel, f'paid {got.get("zel", 0)}, chest {zel}')
        check(f'{key}: its Gems are paid once', got.get('gem', 0) == gems, f'paid {got.get("gem", 0)}, chest {gems}')
        for r in rewards:
            if int(r['present_type']) in (4, 5, 7):
                want = sum(int(x['target_cnt']) for x in rewards
                           if int(x['present_type']) in (4, 5, 7) and int(x['target_id']) == int(r['target_id']))
                got_items = got.get(f'item {int(r["target_id"])}', 0)
                check(f'{key}: item {r["target_id"]} is paid once', got_items == want,
                      f'paid {got_items}, chest {want}')
            elif int(r['present_type']) == 6:
                # A unit missing from this server's unit_mst is skipped, so at most once.
                want = sum(int(x['target_cnt']) for x in rewards
                           if int(x['present_type']) == 6 and int(x['target_id']) == int(r['target_id']))
                got_units = got.get(f'unit {int(r["target_id"])}', 0)
                check(f'{key}: unit {r["target_id"]} is paid at most once', 0 <= got_units <= want,
                      f'paid {got_units}, chest {want}')
        again_before = wallet(db, c['id'])
        call(c, MYSTERY_CLAIM, {'d0ajLeRi': [{'rEFRefr8': key}]}, f'claiming {key} again')
        check(f'{key}: a later claim pays nothing', paid(again_before, wallet(db, c['id'])) == {})

    print('\n-- tutorial starter pack (T1nCVvx4)')

    def finished(q: dict) -> list:
        return [dict(q['login'][0], **{'9sQM2XcN': '12', 'sv6BEI8X': '1'})]

    def pack(q: dict) -> Counter:
        return Counter(tuple(r) for r in db.rows(
            'SELECT present_type, target_id, target_cnt FROM user_presents WHERE user_id = ? AND receipt_type = 1',
            q['id']))

    control = new_player(args.url, 'T')
    call(control, TUTORIAL_UPDATE, {}, 'one TutorialUpdate finishing the tutorial (control account)',
         login=finished(control))
    one_pack = pack(control)
    check('the control account got a starter pack', sum(one_pack.values()) > 0, one_pack)
    t = new_player(args.url, 'U')
    print(f'account: {t["id"]}')
    check('a fresh account has no starter pack yet', sum(pack(t).values()) == 0, pack(t))
    replies = race(t, TUTORIAL_UPDATE, {}, n, login=finished(t))
    check_replies(replies, f'{n} parallel TutorialUpdates finishing the tutorial')
    check(f'{n} parallel completions queue exactly one pack', pack(t) == one_pack,
          f'{sum(pack(t).values())} present(s), one pack is {sum(one_pack.values())}')
    call(t, TUTORIAL_UPDATE, {}, 'a later TutorialUpdate still reporting end_flag=1', login=finished(t))
    check('a later completion queues nothing more', pack(t) == one_pack, sum(pack(t).values()))

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
