"""Unit-balance overlay (balance_root): client table announce/serve + owned-unit stat migration.

python scripts/test_balance_overlay_wire.py [--port 9964] [--name bf-overlay-test]

Runs against the Docker server for THIS checkout (docker/run.sh with SRC set
to it) and restarts that container itself: the server reads
<balance_root>/server/unit_mst.json and re-curves owned units only at boot.
config.docker.json must carry "balance_root": "./balance", i.e.
deploy/balance on the host.  The save is deploy/gme.sqlite; the test only
touches user_units rows it inserts for user BALANCE_WIRE and removes them,
and the staged table / override file, when it finishes.

(a) A staged <balance_root>/content/F_UNIT_MST (manifest + 2 encrypted parts)
    is announced by Initialize (KeC10fuL) at its version, and every part is
    served byte for byte from GET /mst/<name>.
(b) An override raising unit 10011's ATK curve re-curves owned copies of it
    at the next boot exactly per
        g = base - statAt(old, lv);  base' = statAt(new, lv) + g
    leaves add_* / limit_over_* alone, clamps ext_def to a lowered imp cap,
    updates unit_stat_basis, and patches the archive's Lv1 stats.  Removing
    the override and restarting puts base_atk back (the clamp stays: that one
    is lossy by design).

Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Util.Padding import pad, unpad

from bf_testkit import Checker

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1] if ROOT.parent.name == 'wt' else ROOT.parent
RUN_SH = REPO / 'docker' / 'run.sh'
DB = ROOT / 'deploy' / 'gme.sqlite'
BALANCE = ROOT / 'deploy' / 'balance'
UNIT_MST = ROOT / 'deploy' / 'mst' / 'unit_mst.json'

TABLE, FILE_KEY = 'F_UNIT_MST', '2r9cNSdt'
UNIT_KEY = b'7nL1WTUb'.ljust(16, b'\0')
# A test-only version: >= 1100 and far above any version an operator will publish.
VERSION = 9100

UNIT_ID = 10011                 # Fencer Vargas: max_lv 12, ATK 500->700, caps 400:160:80:80
TEST_USER = 'BALANCE_WIRE'
INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')

# Wire keys: unit_mst row.
K_ID, K_MAXLV, K_PARAM_MAX = 'pn16CNah', 'EI1DF8Yt', 'imQJdg64'
K_MIN = {'hp': 'UZ1Bj7w2', 'atk': 'i9Tn7kYr', 'def': 'q78KoWsg', 'rec': '92ij6UGB'}
K_MAX = {'hp': '3WMz78t6', 'atk': 'omuyP54D', 'def': '32INDST4', 'rec': 'X9P3AN5d'}


def stat_at(lo: int, hi: int, lv: int, max_lv: int) -> int:
    """UnitMix.cpp unitMix_statAtLevel, bit for bit (truncating cast)."""
    if max_lv <= 1 or lv <= 1:
        return lo
    if lv >= max_lv:
        return hi
    return lo + int(float(hi - lo) * float(lv - 1) / float(max_lv - 1))


# ---------------------------------------------------------------- server control

def stop_server(name: str) -> None:
    subprocess.run(['docker', 'stop', name], capture_output=True)
    for _ in range(60):
        out = subprocess.run(['docker', 'ps', '-aq', '-f', f'name=^{name}$'],
                             capture_output=True, text=True).stdout.strip()
        if not out:
            return
        time.sleep(0.5)
    raise RuntimeError(f'container {name} did not go away')


def start_server(name: str, port: int) -> None:
    env = dict(os.environ, SRC=str(ROOT), NAME=name, PORT=str(port), DETACH='1')
    subprocess.run(['bash', str(RUN_SH)], env=env, check=True, capture_output=True)
    deadline = time.time() + 120
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f'http://127.0.0.1:{port}/mst/Ver0_probe.dat', timeout=3)
            return
        except urllib.error.HTTPError:
            return          # any HTTP answer means the listeners are up
        except (urllib.error.URLError, ConnectionError, TimeoutError, OSError):
            time.sleep(1)
    raise RuntimeError('server did not come up:\n' + server_log(name)[-3000:])


def restart(name: str, port: int) -> None:
    stop_server(name)
    start_server(name, port)


def server_log(name: str) -> str:
    r = subprocess.run(['docker', 'logs', name], capture_output=True, text=True, errors='replace')
    return r.stdout + r.stderr


# ---------------------------------------------------------------- wire helpers

def http_get(port: int, path: str) -> tuple[int, bytes]:
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{port}{path}', timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, b''


def guest_login(port: int) -> tuple[str, str]:
    status, body = http_get(port, '/accounts/guest/login/?device_id=balance-wire-test-device')
    assert status == 200, status
    reply = json.loads(body)
    return reply['game_user_id'], reply['token']


def gme_call(port: int, group: str, key: str, body: dict) -> dict:
    k = key.encode().ljust(16, b'\0')[:16]
    cipher = AES.new(k, AES.MODE_ECB)
    raw = json.dumps(body).encode()
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'balance'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(pad(raw, 16))).decode()}}
    req = urllib.request.Request(f'http://127.0.0.1:{port}/bf/gme/action.php',
                                 json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'b5PH6mZa' in outer:
        return {'error': outer['b5PH6mZa']}
    return json.loads(unpad(cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y'])), 16))


def announced(port: int) -> dict:
    """{table: row} from Initialize's KeC10fuL for a fresh guest."""
    gumi, token = guest_login(port)
    reply = gme_call(port, *INITIALIZE, {'IKqx1Cn9': [{'iN7buP2h': gumi, 'iN7buP1i': token}],
                                         'KeC10fuL': []})
    assert 'error' not in reply, reply
    return {row['moWQ30GH']: row for row in reply.get('KeC10fuL', [])}


# ---------------------------------------------------------------- staging

def stage_client_table(rows: list) -> dict:
    """Two-part F_UNIT_MST under <balance_root>/content; returns {name: bytes}."""
    dest = BALANCE / 'content' / TABLE
    dest.mkdir(parents=True, exist_ok=True)
    half = len(rows) // 2
    parts, recovered = {}, []
    for index, chunk in enumerate((rows[:half], rows[half:]), 1):
        plain = json.dumps(chunk, ensure_ascii=False, separators=(',', ':')).encode()
        blob = base64.b64encode(AES.new(UNIT_KEY, AES.MODE_ECB).encrypt(pad(plain, 16)))
        name = f'Ver{VERSION}_{FILE_KEY}_{index}.dat'
        (dest / name).write_bytes(blob)
        parts[name] = blob
        recovered.extend(json.loads(unpad(AES.new(UNIT_KEY, AES.MODE_ECB).decrypt(base64.b64decode(blob)), 16)))
    assert recovered == rows
    manifest = {'table': TABLE, 'file_key': FILE_KEY, 'version': VERSION,
                'file_count': len(parts), 'record_num': len(rows)}
    tmp = dest / 'manifest.json.tmp'
    tmp.write_text(json.dumps(manifest), encoding='utf-8')
    tmp.replace(dest / 'manifest.json')
    return parts


def write_override(rows: list) -> None:
    path = BALANCE / 'server' / 'unit_mst.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.json.tmp')
    tmp.write_text(json.dumps({FILE_KEY: rows}, ensure_ascii=False), encoding='utf-8')
    tmp.replace(path)


def remove_override() -> None:
    (BALANCE / 'server' / 'unit_mst.json').unlink(missing_ok=True)


# ---------------------------------------------------------------- db

def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB, timeout=30)
    conn.row_factory = sqlite3.Row
    return conn


def owned_rows() -> list[sqlite3.Row]:
    with db() as conn:
        return conn.execute('SELECT * FROM user_units WHERE user_id=? ORDER BY unit_lvl',
                            (TEST_USER,)).fetchall()


def basis_row() -> sqlite3.Row | None:
    with db() as conn:
        return conn.execute('SELECT * FROM unit_stat_basis WHERE unit_id=?', (UNIT_ID,)).fetchone()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=9964)
    ap.add_argument('--name', default='bf-overlay-test')
    args = ap.parse_args()
    check = Checker()

    shipped_rows = json.loads(UNIT_MST.read_text(encoding='utf-8'))[FILE_KEY]
    shipped = next(r for r in shipped_rows if int(r[K_ID]) == UNIT_ID)
    max_lv = int(shipped[K_MAXLV])
    lo = {s: int(shipped[K_MIN[s]]) for s in K_MIN}
    hi = {s: int(shipped[K_MAX[s]]) for s in K_MAX}

    # Owned copies at Lv1, a mid level carrying type growth, and max level.
    # (level, atk growth, ext_atk, ext_def, add_atk, limit_over_atk)
    owned = [(1, 0, 10, 0, 7, 0), (7, 9, 120, 60, 0, 3), (max_lv, -4, 160, 80, 2, 5)]

    stop_server(args.name)
    remove_override()
    shutil.rmtree(BALANCE / 'content' / TABLE, ignore_errors=True)
    try:
        # ---- (a) client table announce + serve --------------------------------
        parts = stage_client_table(shipped_rows)
        start_server(args.name, args.port)

        rows = announced(args.port)
        row = rows.get(TABLE, {})
        check('Initialize announces F_UNIT_MST at the staged version',
              int(row.get('d2RFtP8T', 0)) == VERSION, row)
        check('... with the staged part count', int(row.get('H6k1LIxC', 0)) == 2, row)
        check('... and record count', int(row.get('5kbnkTp0', 0)) == len(shipped_rows), row)
        check('baseline rows are kept beside it', 'F_SKILL_MST' in rows and len(rows) >= 169, len(rows))
        for name, blob in parts.items():
            status, body = http_get(args.port, f'/mst/{name}')
            check(f'GET /mst/{name} serves the staged bytes', status == 200 and body == blob,
                  f'status {status}, {len(body)} vs {len(blob)} bytes')
        status, _ = http_get(args.port, f'/mst/Ver{VERSION}_{FILE_KEY}_3.dat')
        check('an unstaged part still 404s', status == 404, status)
        status, _ = http_get(args.port, '/mst/Ver1..%2F..%2Fgme.sqlite')
        check('a non-part name is refused', status == 404, status)
        offered = [line for line in server_log(args.name).splitlines()
                   if f'mstVersions: offering {TABLE} v{VERSION}' in line]
        check('the log names balance_root as the source',
              offered and 'from ./balance/content ' in offered[-1], offered[-1:])

        # ---- (b) owned-unit migration ------------------------------------------
        stop_server(args.name)
        b = basis_row()
        check('first boot seeded unit_stat_basis from the shipped row',
              b is not None and (b['max_lv'], b['min_atk'], b['max_atk'], b['imp_caps'])
              == (max_lv, lo['atk'], hi['atk'], shipped[K_PARAM_MAX]), dict(b) if b else None)

        with db() as conn:
            conn.execute('DELETE FROM user_units WHERE user_id=?', (TEST_USER,))
            for lv, g, ext_atk, ext_def, add_atk, lo_atk in owned:
                conn.execute(
                    'INSERT INTO user_units(user_id, unit_id, unit_type_id, unit_lvl,'
                    ' base_hp, base_atk, base_def, base_rec, ext_atk, ext_def, add_atk, limit_over_atk)'
                    ' VALUES (?,?,?,?,?,?,?,?,?,?,?,?)',
                    (TEST_USER, UNIT_ID, 1, lv,
                     stat_at(lo['hp'], hi['hp'], lv, max_lv),
                     stat_at(lo['atk'], hi['atk'], lv, max_lv) + g,
                     stat_at(lo['def'], hi['def'], lv, max_lv),
                     stat_at(lo['rec'], hi['rec'], lv, max_lv),
                     ext_atk, ext_def, add_atk, lo_atk))
        before = {r['unit_lvl']: dict(r) for r in owned_rows()}

        tuned = dict(shipped)
        new_lo_atk, new_hi_atk, new_def_cap = lo['atk'] + 100, hi['atk'] + 213, 40
        tuned[K_MIN['atk']] = str(new_lo_atk)
        tuned[K_MAX['atk']] = str(new_hi_atk)
        caps = shipped[K_PARAM_MAX].split(':')
        caps[2] = str(new_def_cap)
        tuned[K_PARAM_MAX] = ':'.join(caps)
        bogus = dict(shipped)
        bogus[K_ID] = '99999999'
        write_override([tuned, bogus])
        start_server(args.name, args.port)
        log = server_log(args.name)

        after = {r['unit_lvl']: dict(r) for r in owned_rows()}
        for lv, g, ext_atk, ext_def, add_atk, lo_atk in owned:
            want = stat_at(new_lo_atk, new_hi_atk, lv, max_lv) + g
            got = after[lv]
            check(f'Lv{lv}: base_atk re-curved keeping growth {g:+d} ({before[lv]["base_atk"]} -> {want})',
                  got['base_atk'] == want, got['base_atk'])
            check(f'Lv{lv}: base_hp/def/rec untouched',
                  (got['base_hp'], got['base_def'], got['base_rec'])
                  == (before[lv]['base_hp'], before[lv]['base_def'], before[lv]['base_rec']))
            check(f'Lv{lv}: add_atk / limit_over_atk / ext_atk untouched',
                  (got['add_atk'], got['limit_over_atk'], got['ext_atk']) == (add_atk, lo_atk, ext_atk))
            check(f'Lv{lv}: ext_def clamped to the lowered cap ({ext_def} -> {min(ext_def, new_def_cap)})',
                  got['ext_def'] == min(ext_def, new_def_cap), got['ext_def'])
        b = basis_row()
        check('unit_stat_basis now holds the overridden curve',
              b is not None and (b['min_atk'], b['max_atk'], b['imp_caps'])
              == (new_lo_atk, new_hi_atk, tuned[K_PARAM_MAX]), dict(b) if b else None)
        check('override log: 1 replaced, 1 ignored', 'balance: 1 unit row(s) overridden, 1 ignored' in log)
        check('unknown id is logged and ignored', 'balance: unit 99999999 is not in the unit table' in log)
        check('archive Lv1 stats patched', 'balance: patched archive stats for 1 unit(s)' in log)
        check('migration summary logged',
              f'balance: unit {UNIT_ID} curve changed; {len(owned)} owned row(s) re-curved' in log)
        check('the server still answers Initialize after migrating', TABLE in announced(args.port))

        # ---- revert --------------------------------------------------------------
        stop_server(args.name)
        remove_override()
        start_server(args.name, args.port)
        reverted = {r['unit_lvl']: dict(r) for r in owned_rows()}
        for lv, *_ in owned:
            check(f'Lv{lv}: removing the override restores base_atk {before[lv]["base_atk"]}',
                  reverted[lv]['base_atk'] == before[lv]['base_atk'], reverted[lv]['base_atk'])
        b = basis_row()
        check('unit_stat_basis back on the shipped curve',
              b is not None and (b['min_atk'], b['max_atk'], b['imp_caps'])
              == (lo['atk'], hi['atk'], shipped[K_PARAM_MAX]), dict(b) if b else None)
        check('the revert boot re-curved exactly the one unit',
              'balance: unit stat sync -- 1 unit(s) changed, 3 owned row(s) migrated'
              in server_log(args.name))
        restart(args.name, args.port)
        check('a reboot with nothing changed migrates nothing',
              'balance: unit stat sync -- 0 unit(s) changed, 0 owned row(s) migrated'
              in server_log(args.name))
        check('... and leaves the owned rows alone',
              {r['unit_lvl']: r['base_atk'] for r in owned_rows()}
              == {lv: before[lv]['base_atk'] for lv in before})

        # ---- first boot with a publish already staged ------------------------------
        # An empty basis is seeded from the PRE-override rows (what owned units
        # were really computed from), so the staged publish still migrates.
        stop_server(args.name)
        with db() as conn:
            conn.execute('DELETE FROM unit_stat_basis')
        write_override([tuned])
        start_server(args.name, args.port)
        log = server_log(args.name)
        check('empty basis + staged override: basis seeded',
              'balance: seeded unit_stat_basis with' in log)
        check('... and the owned rows still follow the override',
              {r['unit_lvl']: r['base_atk'] for r in owned_rows()}
              == {lv: stat_at(new_lo_atk, new_hi_atk, lv, max_lv) + g for lv, g, *_ in owned})
        stop_server(args.name)
        remove_override()
        start_server(args.name, args.port)
        check('... and revert cleanly',
              {r['unit_lvl']: r['base_atk'] for r in owned_rows()}
              == {lv: before[lv]['base_atk'] for lv in before})
    finally:
        stop_server(args.name)
        remove_override()
        shutil.rmtree(BALANCE / 'content' / TABLE, ignore_errors=True)
        if DB.exists():
            with db() as conn:
                conn.execute('DELETE FROM user_units WHERE user_id=?', (TEST_USER,))

    return check.summary('balance overlay')


if __name__ == '__main__':
    sys.exit(main())
