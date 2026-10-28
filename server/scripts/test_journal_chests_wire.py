#!/usr/bin/env python3
"""Wire test: every Summoner's Journal chest names a reward the client can draw (ticket #41).

    .venv/bin/python -I <wt>/scripts/test_journal_chests_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-journal PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_journal_chests_wire.py --src <wt> --url http://127.0.0.1:9983

Tapping a chest hands the milestone row's present_type / present_id (S1B82FHK) to
PresentCommon::createThumbnailBack, which looks an item or unit id up and crashed on the
milestone id ("4", "5") it used to get.  Checks every milestone and task-reward row of
SummonerJournalInfo (32Gwida0): an item or unit reward must name a row of item_mst / unit_mst,
and a currency reward (zel 3, gem 8, karma 11) needs no id.
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

JOURNAL = ('32Gwida0', '66B2pDki')
MILESTONES, REWARDS = 'ad52Diwq', 'b2DjiaXp'
CURRENCY = {3, 8, 11}
UNIT = 6


def call(url: str, acc: dict, api: tuple[str, str], body: dict) -> dict:
    group, key = api
    body = {'IKqx1Cn9': [{'iN7buP2h': acc['login'], 'h7eY3sAK': acc['user'], 'iN7buP1i': acc['token']}], **body}
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'journal'},
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


def ids(path: Path, key: str) -> set[str]:
    data = json.loads(path.read_text())
    rows = data[next(iter(data))] if isinstance(data, dict) else data
    return {str(r[key]) for r in rows if key in r}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    out = args.src / 'out/qa/journal'
    if args.prepare:
        token = secrets.token_hex(16)
        with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:
            user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
        out.mkdir(parents=True, exist_ok=True)
        (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
        print(f'prepared {user}')
        return 0

    acc = json.loads((out / 'account.json').read_text())
    items = ids(args.src / 'deploy/mst/item_mst.json', 'kixHbe54')
    units = ids(args.src / 'deploy/mst/unit_mst.json', 'pn16CNah')
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    reply = call(url, acc, JOURNAL, {})
    bad, seen = [], 0
    for key in (MILESTONES, REWARDS):
        for row in reply.get(key, []):
            seen += 1
            kind, pid = int(row.get('30Kw4WBa', 0)), str(row.get('S1B82FHK', ''))
            label = f'{key} {row.get("Uiwd28Vq") or row.get("23DaiBpe")}: type {kind} id {pid!r}'
            if kind in CURRENCY or kind == 0:
                continue
            if pid not in (units if kind == UNIT else items):
                bad.append(label)
            elif key == MILESTONES:
                print(f'ok    {label} ({row.get("ZC0msu2L", "")})')
    for b in bad:
        print('FAIL  ' + b)
    print(f'{seen} row(s), {len(bad)} failure(s)')
    return 1 if bad or not seen else 0


if __name__ == '__main__':
    sys.exit(main())
