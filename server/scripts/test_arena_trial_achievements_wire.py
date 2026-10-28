#!/usr/bin/env python3
"""Wire test: arena and "cleared in N turns" achievements count (ticket #46).

    .venv/bin/python -I <wt>/scripts/test_arena_trial_achievements_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-ach PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_arena_trial_achievements_wire.py --src <wt> --url http://127.0.0.1:9983

--prepare gives the top account an Arena record (12 wins of 55 battles, best streak 11, best rank 8),
9,999 energy, and no turn record on Trial No. 001 (mission 2000000).  The run reads GetAchievementInfo
(YPBU7MD8) and checks 28000 (10 wins), 29010 (10 in a row), 33010 (rank 8, Duelist) while 33020 (rank 14) is not and 600200 (50 battles)
are complete while 28010 (100 wins) is not; then clears Trial No. 001 reporting 3 turns
(Lu0pAk1R "1,1,1") and checks 400000 ("in 3 turns") completes but 630000 ("in 2 turns") does not; a
2-turn clear completes 630000; a 3-turn clear afterwards keeps it (best only goes down).
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

ACHIEVEMENT_INFO = ('YPBU7MD8', 'AKjzyZ81')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
TRIAL = 2000000
DONE = 2

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
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'ach'},
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


def states(url: str, acc: dict) -> dict[str, int]:
    reply = call(url, acc, ACHIEVEMENT_INFO, {})
    return {r['M7SXoc31']: int(r.get('0B5bQgZh', 0)) for r in reply.get('YTRJLG65') or []}


def clear(url: str, acc: dict, per_wave: str) -> None:
    started = call(url, acc, MISSION_START, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
        '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(TRIAL), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
                      'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
        'JzS3uxsZ': [{'0b4efi1W': '1'}]})
    fight = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT', 'U8uZLA34',
                              'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    fight.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': '', 'Lu0pAk1R': per_wave})
    ended = call(url, acc, MISSION_END, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
        'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'], 'j3g5P4cq': '2'}], 'rXvA1E5y': [fight]})
    if 'b5PH6mZa' in ended:
        raise SystemExit(f'MissionEnd refused: {ended["b5PH6mZa"]}')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    out = args.src / 'out/qa/achievements'
    if args.prepare:
        token = secrets.token_hex(16)
        with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:
            user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
            db.execute('UPDATE user_info SET energy=9999 WHERE id=?', (user,))
            db.execute('INSERT OR IGNORE INTO user_arena (user_id) VALUES (?)', (user,))
            db.execute('UPDATE user_arena SET win_cnt=12, battle_cnt=55, max_atk_series_win=11, max_rank_id=8'
                       ' WHERE user_id=?', (user,))
            cols = [r[1] for r in db.execute('PRAGMA table_info(user_campaign_missions)')]
            if 'best_turns' in cols:
                db.execute('UPDATE user_campaign_missions SET best_turns=0 WHERE user_id=? AND mission_id=?', (user, str(TRIAL)))
            db.execute("DELETE FROM user_achievement_subjects WHERE user_id=? AND subject_id IN"
                       " ('28000','28010','29010','33010','33020','600200','400000','630000')", (user,))
        out.mkdir(parents=True, exist_ok=True)
        (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
        print(f'prepared {user}: arena 12 wins / 55 battles / streak 11 / rank 8, no turn record on {TRIAL}')
        return 0

    acc = json.loads((out / 'account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    s = states(url, acc)
    for sid, what in (('28000', '10 arena wins'), ('29010', '10 wins in a row'), ('33010', 'Duelist rank (8)'),
                      ('600200', '(SP) 50 arena battles')):
        check(s.get(sid) == DONE, f'{sid} {what} is complete ({s.get(sid)})')
    check(s.get('28010') != DONE and s.get('33020') != DONE,
          f'28010 (100 wins) and 33020 (rank 14) are not ({s.get("28010")}, {s.get("33020")})')
    check(s.get('400000') != DONE and s.get('630000') != DONE, 'before any turn record: 400000 and 630000 are open')

    clear(url, acc, '1,1,1')
    s = states(url, acc)
    check(s.get('400000') == DONE, f'a 3-turn clear completes 400000 "in 3 turns" ({s.get("400000")})')
    check(s.get('630000') != DONE, f'... but not 630000 "in 2 turns" ({s.get("630000")})')

    clear(url, acc, '1,1')
    s = states(url, acc)
    check(s.get('630000') == DONE, f'a 2-turn clear completes 630000 ({s.get("630000")})')
    clear(url, acc, '2,1,1')
    s = states(url, acc)
    check(s.get('630000') == DONE, 'a slower clear afterwards keeps it')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
