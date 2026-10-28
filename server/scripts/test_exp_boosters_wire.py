#!/usr/bin/env python3
"""Wire test: Experience Token / Badge in the bonus bar multiply quest EXP and are used up (ticket #50).

    .venv/bin/python -I <wt>/scripts/test_exp_boosters_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-boost PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_exp_boosters_wire.py --src <wt> --url http://127.0.0.1:9983

--prepare writes the TEST save while no server holds it: the top account gets a known session token,
9,999 energy, an empty bonus bar and 2 Experience Tokens + 1 Experience Badge in storage
(out/qa/boosters/account.json).

The run clears mission 10 with no booster (the base EXP), then through ItemEdit (ruoB7bD8) puts a
Token in the bonus bar and checks that a clear pays 1.5x, sends the bar back with the slot at 0
(nAligJSQ), and leaves storage alone; that a lost quest keeps the booster; and that a Badge pays 2x.
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
import sys
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
ITEM_EDIT = ('ruoB7bD8', 'DHEfRexCu0q5TAQm')
MISSION = 10
TOKEN, BADGE = '120000', '120001'

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
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'boost'},
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


def exp_of(reply: dict) -> tuple[int, int]:
    team = reply['fEi17cnx'][0]
    return int(team['D9wXQI2V']), int(team['d96tuT2E'])


def gained(before: tuple[int, int], after: tuple[int, int]) -> int | None:
    return after[1] - before[1] if after[0] == before[0] else None  # a level-up resets the counter


def equip(url: str, acc: dict, item: str | None) -> None:
    bonus = [{'h7eY3sAK': acc['user'], 'kixHbe54': item, 'XuJL4pc5': '0', 'wgV86x1q': '1'}] if item else []
    call(url, acc, ITEM_EDIT, {'71U5wzhI': [], 'nAligJSQ': bonus})


def bar(reply: dict) -> list:
    return [(b['kixHbe54'], b['wgV86x1q']) for b in reply.get('nAligJSQ', [])]


def prepare(src: Path) -> None:
    token = secrets.token_hex(16)
    with sqlite3.connect(src / 'deploy/gme.sqlite', timeout=30) as db:
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
        db.execute('UPDATE user_info SET energy=9999 WHERE id=?', (user,))
        db.execute('DELETE FROM user_equip_bonus_items WHERE user_id=?', (user,))
        db.execute('DELETE FROM user_equip_items WHERE user_id=?', (user,))
        for item, n in ((int(TOKEN), 2), (int(BADGE), 1)):
            if db.execute('UPDATE user_items SET item_num=? WHERE user_id=? AND item_id=?', (n, user, item)).rowcount == 0:
                db.execute('INSERT INTO user_items (user_id, item_id, item_num) VALUES (?, ?, ?)', (user, item, n))
    out = src / 'out/qa/boosters'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
    print(f'prepared {user}: 2 Experience Tokens and 1 Experience Badge in storage, empty bonus bar')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        prepare(args.src)
        return 0
    acc = json.loads((args.src / 'out/qa/boosters/account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'

    first = quest(url, acc)
    plain = quest(url, acc)
    base = gained(exp_of(first), exp_of(plain))
    check(bool(base) and 'nAligJSQ' not in plain, f'a clear with no booster pays {base} EXP and sends no bar')

    equip(url, acc, TOKEN)
    boosted = quest(url, acc)
    got = gained(exp_of(plain), exp_of(boosted))
    check(base is not None and got == base * 3 // 2, f'Experience Token: {got} EXP (want {base * 3 // 2 if base else "?"})')
    check(bar(boosted) == [(TOKEN, '0')], f'the Token is used up and the bar comes back at 0 ({bar(boosted)})')
    after = quest(url, acc)
    check(gained(exp_of(boosted), exp_of(after)) == base, 'the next clear is unboosted (nothing left in the slot)')

    equip(url, acc, TOKEN)  # Auto Equip's refill: the second Token from storage
    lost = quest(url, acc, won=False)
    check('nAligJSQ' not in lost, 'a lost quest keeps the Token')
    kept = quest(url, acc)
    check(gained(exp_of(lost), exp_of(kept)) == base * 3 // 2 and bar(kept) == [(TOKEN, '0')],
          'the kept Token boosts the next clear')

    equip(url, acc, BADGE)
    badge = quest(url, acc)
    check(gained(exp_of(kept), exp_of(badge)) == base * 2 and bar(badge) == [(BADGE, '0')],
          f'Experience Badge: {gained(exp_of(kept), exp_of(badge))} EXP (want {base * 2 if base else "?"})')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
