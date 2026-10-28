#!/usr/bin/env python3
"""Wire test: arena orbs refill on level-up and cost no gems; SP resets cost no gems (ticket #96).

    .venv/bin/python -I <wt>/scripts/test_arena_sp_qol_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-arenasp PORT=9985 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_arena_sp_qol_wire.py --src <wt> --url http://127.0.0.1:9985 \\
        --container bf-arenasp

--prepare writes the TEST save while no server holds it: four accounts past the tutorial get a known
session token, 9,999 energy and 0 of 10 Arena Orbs with the regeneration clock running.  One sits at
level 300 one EXP short of 301, one at level 300 with no EXP, one has 5 gems and one has none.

The run checks, each against the save read inside the container (WAL on a bind mount: host reads can
miss the server's writes):
  1. Initialize announces both refill prices, Arena Orbs (sy9G24Su) and SP reset (5csFoG1G), as 0.
  2. A clear that levels up fills the Arena Orbs and stops their clock; a clear that does not level up
     leaves them (and the clock) alone.
  3. ShopUse type 4 (restore Arena Orbs) fills them, charges nothing with 5 gems and with 0 gems, and a
     second tap at the cap changes nothing.
The SP reset charge itself is covered by test_fe_skill_qc_wire.py (ShopUse type 9, price 0 since 2026-10-20).
Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import secrets
import sqlite3
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
SHOP_USE = ('xe8tiSf4', 'qthMXTQSkz3KfH9R')
MISSION = 10
LEVEL = 300
MAX_ORBS = 10
SHOP_RESTORE_ARENA_ORBS = 4
ORBS_PRICE_KEY, SP_RESET_PRICE_KEY = 'sy9G24Su', '5csFoG1G'

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        failures.append(what)


def call(url: str, acc: dict, api: tuple[str, str], body: dict) -> dict:
    group, key = api
    body = {'IKqx1Cn9': [{'iN7buP2h': acc['login'], 'h7eY3sAK': acc['user'], 'iN7buP1i': acc['token']}], **body}
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'arenasp'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    req = urllib.request.Request(url, json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        raise SystemExit(f'{group} refused: {outer}')
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def quest(url: str, acc: dict) -> dict:
    started = call(url, acc, MISSION_START, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
        '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
                      'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
        'JzS3uxsZ': [{'0b4efi1W': '1'}]})
    fight = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT', 'U8uZLA34',
                              'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    fight.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    return call(url, acc, MISSION_END, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
        'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'], 'j3g5P4cq': '2'}],
        'rXvA1E5y': [fight]})


def restore_orbs(url: str, acc: dict) -> dict:
    return call(url, acc, SHOP_USE, {'32ibWjFG': [
        {'60IsqxDt': str(SHOP_RESTORE_ARENA_ORBS), '03UGMHxF': '0', '5gXxT7LZ': '0'}]})


def level_of(reply: dict) -> int:
    return int(reply['fEi17cnx'][0]['D9wXQI2V'])


def exp_for(src: Path, level: int) -> int:
    """The EXP a player at level - 1 needs to reach `level` (UserLevelMst d96tuT2E, a per-level chunk)."""
    rows = json.loads((src / 'deploy/mst/user_level_mst.json').read_text())['YDv9bJ3s']
    return next(int(r['d96tuT2E']) for r in rows if int(r['D9wXQI2V']) == level)


def prepare(src: Path) -> None:
    accounts = []
    almost = exp_for(src, LEVEL + 1) - 1
    # (level, exp, gems) per role: levels up, does not level up, 5 gems, 0 gems.
    roles = [(LEVEL, almost, 100), (LEVEL, 0, 100), (LEVEL, 0, 5), (LEVEL, 0, 0)]
    now = int(time.time())
    with sqlite3.connect(src / 'deploy/gme.sqlite', timeout=30) as db:
        picked = db.execute('SELECT u.id, u.gumi_user_id FROM user_info u JOIN gumi_live_users g ON g.id = u.gumi_user_id '
                            'WHERE u.tutorial_end_flag = 1 ORDER BY u.level DESC, u.id LIMIT 4').fetchall()
        if len(picked) < 4:
            raise SystemExit('the save needs four accounts past the tutorial')
        for (user, login), (level, exp, gems) in zip(picked, roles):
            token = secrets.token_hex(16)
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
            db.execute('UPDATE user_info SET energy=9999, energy_full_ts=0, level=?, exp=?, gems=?, '
                       'fight_point=0, max_fight_point=? WHERE id=?', (level, exp, gems, MAX_ORBS, user))
            db.execute('INSERT OR IGNORE INTO user_arena (user_id, updated_at) VALUES (?, ?)', (user, now))
            db.execute('UPDATE user_arena SET orb_ts=? WHERE user_id=?', (now, user))
            accounts.append({'login': str(login), 'user': str(user), 'token': token})
    out = src / 'out/qa/arenasp'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'accounts.json').write_text(json.dumps(accounts))
    print('prepared ' + ', '.join(a['user'] for a in accounts)
          + f' (level {LEVEL}; the first one EXP short of the next level; 0/{MAX_ORBS} orbs; gems 100/100/5/0)')


def read_save(container: str, sql: str) -> list:
    code = ('import json, sqlite3, sys; c = sqlite3.connect("file:/src/deploy/gme.sqlite?mode=ro", uri=True); '
            'print(json.dumps(c.execute(sys.argv[1]).fetchall()))')
    out = subprocess.run(['docker', 'exec', container, 'python3', '-I', '-c', code, sql],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def orbs(container: str, acc: dict) -> tuple[int, int, int]:
    """(fight_point, orb_ts, gems) of an account."""
    fp, gems = read_save(container, f"SELECT fight_point, gems FROM user_info WHERE id = '{acc['user']}'")[0]
    ts = read_save(container, f"SELECT orb_ts FROM user_arena WHERE user_id = '{acc['user']}'")[0][0]
    return fp, ts, gems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9985')
    ap.add_argument('--container', default='bf-arenasp')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        prepare(args.src)
        return 0
    accs = json.loads((args.src / 'out/qa/arenasp/accounts.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    levels, stays, five_gems, no_gems = accs

    init = call(url, levels, INITIALIZE, {})
    defines = init.get('VkoZ5t3K') or [{}]
    defines = defines[0] if isinstance(defines, list) else defines
    check(str(defines.get(ORBS_PRICE_KEY)) == '0', f'Initialize announces the Arena Orb refill as 0 gems ({defines.get(ORBS_PRICE_KEY)})')
    check(str(defines.get(SP_RESET_PRICE_KEY)) == '0', f'Initialize announces the SP reset as 0 gems ({defines.get(SP_RESET_PRICE_KEY)})')

    check(orbs(args.container, levels)[0] == 0, 'setup: the level-up account starts with 0 orbs')
    check(level_of(quest(url, levels)) == LEVEL + 1, f'one clear takes {levels["user"]} to level {LEVEL + 1}')
    fp, ts, _ = orbs(args.container, levels)
    check(fp == MAX_ORBS, f'level-up fills the Arena Orbs ({fp}/{MAX_ORBS})')
    check(ts == 0, f'level-up stops the orb clock ({ts})')

    before = orbs(args.container, stays)
    check(level_of(quest(url, stays)) == LEVEL, f'{stays["user"]} clears without a level-up')
    after = orbs(args.container, stays)
    check(after[0] == 0 and after[1] == before[1], f'no level-up, no refill: orbs and clock unchanged ({before} -> {after})')

    for label, acc, gems in (('5 gems', five_gems, 5), ('0 gems', no_gems, 0)):
        restore_orbs(url, acc)
        fp, ts, left = orbs(args.container, acc)
        check(fp == MAX_ORBS and ts == 0 and left == gems,
              f'ShopUse restore Arena Orbs with {label}: filled, clock stopped, gems stay {gems} ({fp}, {ts}, {left})')
    restore_orbs(url, five_gems)
    check(orbs(args.container, five_gems) == (MAX_ORBS, 0, 5), 'a second tap at the cap changes nothing')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
