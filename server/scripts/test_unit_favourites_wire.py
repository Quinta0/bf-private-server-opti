#!/usr/bin/env python3
"""Wire test: UnitFavorite stores the client's whole favourite list and echoes all of it (ticket #39).

    .venv/bin/python -I <wt>/scripts/test_unit_favourites_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-favs PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_unit_favourites_wire.py --src <wt> --url http://127.0.0.1:9983

The client lists every unit that is a favourite (UnitFavoriteRequest::createBody: getFavorite == 1 or
image type 2), so a unit it leaves out was unfavourited.  --prepare locks the top account's first
four units (A-D) in the save.  The run then sends the list [A, E], as a player who unfavourited B
to D and favourited E would, and checks the reply carries both rows and UserInfo (3kcmQy7B) now
reports exactly A and E; then an empty list clears them all.
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

FAVOURITE = ('2p9LHCNh', 'cb4ESLa1')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')

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
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'favs'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    req = urllib.request.Request(url, json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'refused': outer}
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def favourites(url: str, acc: dict) -> set[str]:
    reply = call(url, acc, USER_INFO, {})
    return {r['edy7fq3L'] for r in reply.get('3kcmQy7B', []) if r.get('5JbjC3Pp') == '1'}


def send(url: str, acc: dict, ids: list[str]) -> dict:
    return call(url, acc, FAVOURITE, {'3kcmQy7B': [{'edy7fq3L': i, '5JbjC3Pp': '1', '2pAyFjmZ': '0'} for i in ids]})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    out = args.src / 'out/qa/favs'
    if args.prepare:
        token = secrets.token_hex(16)
        with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:
            user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
            units = [str(r[0]) for r in db.execute(
                'SELECT user_unit_id FROM user_units WHERE user_id=? ORDER BY user_unit_id LIMIT 5', (user,))]
            db.execute('UPDATE user_units SET favorite_flg=0 WHERE user_id=?', (user,))
            db.executemany('UPDATE user_units SET favorite_flg=1 WHERE user_id=? AND user_unit_id=?',
                           [(user, u) for u in units[:4]])
        out.mkdir(parents=True, exist_ok=True)
        (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token, 'units': units}))
        print(f'prepared {user}: units {units[:4]} locked, {units[4]} free')
        return 0

    acc = json.loads((out / 'account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    a, b, c, d, e = acc['units']
    check(favourites(url, acc) == {a, b, c, d}, 'before: UserInfo reports the four locked units')

    reply = send(url, acc, [a, e])
    echoed = reply.get('3kcmQy7B')
    check(isinstance(echoed, list) and [r.get('edy7fq3L') for r in echoed] == [a, e],
          f'the reply echoes the whole list as an array ({echoed})')
    now = favourites(url, acc)
    check(now == {a, e}, f'after [A, E]: exactly A and E are favourites ({sorted(now)})')

    reply = send(url, acc, [])
    check('refused' not in reply and 'b5PH6mZa' not in reply, f'an empty list is accepted ({reply})')
    check(favourites(url, acc) == set(), 'after []: no favourites left')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
