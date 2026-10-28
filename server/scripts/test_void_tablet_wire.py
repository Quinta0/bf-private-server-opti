#!/usr/bin/env python3
"""Wire test: a Void Tablet Emgif takes the Elgif skill off the base unit and returns it (ticket #78).

    .venv/bin/python -I <wt>/scripts/test_void_tablet_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-void PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_void_tablet_wire.py --src <wt> --url http://127.0.0.1:9983

--prepare copies one of the top account's units four times: a base carrying Elgif skill 1012700
(extra_passive_skill_id2), a plain base, and two Void Tablets (unit 60812).  The run fuses a tablet into
each base (UnitMix Mw08CIg2) and checks that the reply's unit rebuild (4ceMWH6k) has the base with no
Elgif skill (LjY4DfRg) and no tablet, that the present box (PresentList) holds an Imbued Tablet Elgif
(50792), and that claiming it (PresentReceipt) gives an Elgif carrying 1012700 (T4rewHa9).  The tablet
fused into the plain base had nothing to take and must come back to the present box.
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

UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
PRESENT_LIST = ('nhjvB52R', '6F9sMzBxEv8jXpau')
PRESENT_RECEIPT = ('bV5xa0ZW', 'X2QFqAKfomPIg3rG')
VOID_TABLET, ELGIF, ELGIF_SKILL = 60812, 50792, 1012700

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
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'void'},
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


def units(reply: dict) -> dict[str, dict]:
    return {u['edy7fq3L']: u for u in reply.get('4ceMWH6k', [])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    out = args.src / 'out/qa/void'
    if args.prepare:
        token = secrets.token_hex(16)
        with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:
            db.row_factory = sqlite3.Row
            user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
            db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
            db.execute('UPDATE user_info SET zel=99999999 WHERE id=?', (user,))
            template = dict(db.execute('SELECT * FROM user_units WHERE user_id=? ORDER BY user_unit_id LIMIT 1', (user,)).fetchone())
            template.pop('user_unit_id')

            def insert(**row):
                data = dict(template, **row)
                cur = db.execute(f"INSERT INTO user_units ({','.join(data)}) VALUES ({','.join('?' for _ in data)})",
                                 tuple(data.values()))
                return cur.lastrowid
            base = insert(favorite_flg=0, extra_passive_skill_id2=ELGIF_SKILL)
            tablet, plain, tablet2 = (
                insert(unit_id=str(VOID_TABLET), favorite_flg=0, extra_passive_skill_id2=0, eqip_item_id=0,
                       eqip_item_id2=0, eqip_item_frame_id=0, eqip_item_frame_id2=0),
                insert(favorite_flg=0, extra_passive_skill_id2=0),
                insert(unit_id=str(VOID_TABLET), favorite_flg=0, extra_passive_skill_id2=0, eqip_item_id=0,
                       eqip_item_id2=0, eqip_item_frame_id=0, eqip_item_frame_id2=0))
        out.mkdir(parents=True, exist_ok=True)
        (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token,
                                                      'base': str(base), 'tablet': str(tablet),
                                                      'plain': str(plain), 'tablet2': str(tablet2)}))
        print(f'prepared {user}: base {base} with Elgif skill {ELGIF_SKILL}, Void Tablet {tablet}')
        return 0

    acc = json.loads((out / 'account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'

    def fuse(base: str, tablet: str) -> dict:
        return call(url, acc, UNIT_MIX, {
            '60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}], 'mCE3rUu5': [{'Rs7bCE3t': '0'}],
            'Km35HAXv': [{'edy7fq3L': base, 'mnZ5K4Ii': '1'}, {'edy7fq3L': tablet, 'mnZ5K4Ii': '2'}]})

    def presents() -> dict[str, dict]:
        return {p['S1B82FHK']: p for p in call(url, acc, PRESENT_LIST, {}).get('sEA41vFK') or []}

    before = presents()
    reply = fuse(acc['base'], acc['tablet'])
    check('b5PH6mZa' not in reply, f'UnitMix accepted ({reply.get("b5PH6mZa")})')

    after = units(reply)
    check(acc['base'] in after, 'the reply rebuilds the base unit')
    skill = after.get(acc['base'], {}).get('LjY4DfRg')
    check(skill in (None, '', '0'), f'after: the Elgif skill is gone ({skill!r})')
    check(acc['tablet'] not in after, 'the Void Tablet was used up')

    new = [p for pid, p in presents().items() if pid not in before]
    elgifs = [p for p in new if p.get('TdDHf59J') == str(ELGIF)]
    check(len(new) == 1 and len(elgifs) == 1, f'the present box got one Imbued Tablet Elgif ({new})')
    if elgifs:
        owned = set(after)
        claimed = call(url, acc, PRESENT_RECEIPT, {'o6uWU0Z7': [{'i1WQkh4G': '0', 'S1B82FHK': elgifs[0]['S1B82FHK']}]})
        check('b5PH6mZa' not in claimed, f'PresentReceipt accepted ({claimed.get("b5PH6mZa")})')
        got = [u for key in ('4ceMWH6k', 'qC2tJs4E') for u in claimed.get(key) or []
               if u['edy7fq3L'] not in owned and u.get('pn16CNah') == str(ELGIF)]
        check(len(got) >= 1 and got[0].get('T4rewHa9') == str(ELGIF_SKILL),
              f'the claimed Elgif carries skill {ELGIF_SKILL} '
              f'({[(u["edy7fq3L"], u.get("T4rewHa9")) for u in got]})')

    before = presents()
    reply = fuse(acc['plain'], acc['tablet2'])
    check('b5PH6mZa' not in reply, f'UnitMix (plain base) accepted ({reply.get("b5PH6mZa")})')
    new = [p for pid, p in presents().items() if pid not in before]
    check([p.get('TdDHf59J') for p in new] == [str(VOID_TABLET)],
          f'a tablet with no Elgif to take comes back ({[p.get("TdDHf59J") for p in new]})')

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
