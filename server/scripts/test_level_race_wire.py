#!/usr/bin/env python3
"""Wire test: the race to level 999 records who got there, once, in the order they did (21102026_LevelRace).

    .venv/bin/python -I <wt>/scripts/test_level_race_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-levelrace PORT=9987 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_level_race_wire.py --src <wt> --url http://127.0.0.1:9987 \\
        --container bf-levelrace

--prepare writes the TEST save while no server holds it: five accounts past the tutorial get a known
session token and 9,999 energy; four of them sit at level 998 one EXP short of 999 (deploy/mst/
user_level_mst.json), the fifth is at 999 already (out/qa/levelrace/accounts.json).

The run clears mission 10 with the 998s in the order 2, 1, 4, loses a quest with the third, clears with
the one already at 999 and clears again with the first finisher.  level_race must then hold exactly
2, 1, 4 in that order, timed within the run, and the migration's start mark must be there.  The save is
read inside the container (WAL on a bind mount: host reads can miss the server's writes).
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

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
MISSION = 10

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
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'race'},
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


def quest(url: str, acc: dict, won: bool = True) -> dict:
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
        'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'], 'j3g5P4cq': '2' if won else '3'}],
        'rXvA1E5y': [fight]})


def level_of(reply: dict) -> int:
    return int(reply['fEi17cnx'][0]['D9wXQI2V'])


def exp_for(src: Path, level: int) -> int:
    """The EXP a player at level - 1 needs to reach `level` (UserLevelMst d96tuT2E, a per-level chunk)."""
    rows = json.loads((src / 'deploy/mst/user_level_mst.json').read_text())['YDv9bJ3s']
    return next(int(r['d96tuT2E']) for r in rows if int(r['D9wXQI2V']) == level)


def prepare(src: Path) -> None:
    accounts = []
    almost = exp_for(src, 999) - 1
    with sqlite3.connect(src / 'deploy/gme.sqlite', timeout=30) as db:
        picked = db.execute('SELECT u.id, u.gumi_user_id FROM user_info u JOIN gumi_live_users g ON g.id = u.gumi_user_id '
                            'WHERE u.tutorial_end_flag = 1 ORDER BY u.level DESC, u.id LIMIT 5').fetchall()
        if len(picked) < 5:
            raise SystemExit('the save needs five accounts past the tutorial')
        for n, (user, login) in enumerate(picked):
            token = secrets.token_hex(16)
            level, exp = (999, 0) if n == 4 else (998, almost)
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
            db.execute('UPDATE user_info SET energy=9999, level=?, exp=? WHERE id=?', (level, exp, user))
            accounts.append({'login': str(login), 'user': str(user), 'token': token})
        if db.execute("SELECT 1 FROM sqlite_master WHERE name = 'level_race'").fetchone():
            db.execute('DELETE FROM level_race')
    out = src / 'out/qa/levelrace'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'accounts.json').write_text(json.dumps(accounts))
    print('prepared ' + ', '.join(a['user'] for a in accounts) + ' (four at 998, one EXP short of 999; the last at 999)')


def read_save(container: str, sql: str) -> list:
    code = ('import json, sqlite3, sys; c = sqlite3.connect("file:/src/deploy/gme.sqlite?mode=ro", uri=True); '
            'print(json.dumps(c.execute(sys.argv[1]).fetchall()))')
    out = subprocess.run(['docker', 'exec', container, 'python3', '-I', '-c', code, sql],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9987')
    ap.add_argument('--container', default='bf-levelrace')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        prepare(args.src)
        return 0
    accs = json.loads((args.src / 'out/qa/levelrace/accounts.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    first, second, third, fourth, capped = accs
    started = int(time.time())

    for acc in (second, first, fourth):
        check(level_of(quest(url, acc)) == 999, f'{acc["user"]} reaches 999 with one clear')
    check(level_of(quest(url, third, won=False)) == 998, f'{third["user"]} loses a quest and stays at 998')
    check(level_of(quest(url, capped)) == 999, f'{capped["user"]}, at 999 before the race, clears again')
    check(level_of(quest(url, second)) == 999, f'{second["user"]} clears again at 999')
    ended = int(time.time())

    rows = read_save(args.container, 'SELECT seq, user_id, reached_at FROM level_race ORDER BY seq')
    order = [r[1] for r in rows]
    want = [second['user'], first['user'], fourth['user']]
    check(order == want, f'level_race holds the three who crossed, in order ({order})')
    check([r[0] for r in rows] == sorted(r[0] for r in rows), 'seq grows with each finisher')
    check(all(started <= r[2] <= ended for r in rows), 'each row is timed within the run')
    mark = read_save(args.container, "SELECT value FROM server_state WHERE name = 'level_race_started'")
    check(bool(mark) and 0 < mark[0][0] <= ended, f'the migration marked the race start ({mark})')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
