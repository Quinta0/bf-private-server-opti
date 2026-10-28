#!/usr/bin/env python3
"""Wire test: every Vortex tile UserInfo permits leads to a playable mission.

    python3 -I tools/test_vortex_tiles.py --src wt/vortex --url http://127.0.0.1:9974

Runs against a server already started from --src (its deploy/ holds the MSTs,
the mission archive and the save DB it uses).  Logs in as that DB's highest-level
account (run it on a TEST copy: it rewrites that account's session token), asks UserInfo, and checks:
  - every permitted Vortex mission has an authored battle in archive/mission.json,
  - every permitted Vortex leaf area holds a dungeon with an authored battle,
  - every permitted Vortex category has a permitted child area,
  - the Vortex still has tiles (the fix must not hide everything).
"""
import argparse
import base64
import collections
import gzip
import json
import hashlib
import secrets
import sqlite3
import sys
import urllib.request
from pathlib import Path


from Crypto.Cipher import AES

USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
PERMIT_CHANNELS = ('yXNM8kL3', 'Y73tHKS8', 'Y73mHKS8')
VORTEX_LAND, VORTEX_AREA_MAX = 99, 200000


def rows(path: Path, key: str) -> list:
    data = json.loads(path.read_text())
    return data[key] if isinstance(data, dict) else data


def call(url: str, user: str, login: str, token: str, group: str, key: str, body: dict) -> dict:
    body = dict(body, IKqx1Cn9=[{'iN7buP2h': login, 'h7eY3sAK': user, 'iN7buP1i': token}])
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'vortex'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    req = urllib.request.Request(url, json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        raise SystemExit(f'server refused UserInfo: {outer}')
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def borrow_test_account(deploy: Path) -> tuple[str, str, str]:
    """(gumi id, user id, token) of the highest-level account in this TEST save.

    Gives it a known session token (the server stores only its SHA-256), so
    never point this at a save real players use.
    """
    token = secrets.token_hex(16)
    with sqlite3.connect(deploy / 'gme.sqlite', timeout=30) as db:
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
                   (hashlib.sha256(token.encode()).hexdigest(), login))
    return str(login), str(user), token


def permitted(reply: dict, key: str) -> set:
    return {int(r[key]) for ch in PERMIT_CHANNELS for r in (reply.get(ch) or []) if key in r}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9974')
    args = ap.parse_args()
    deploy = args.src / 'deploy'

    archived = {m['id'] for m in json.loads((deploy / 'archive/mission.json').read_text())}
    areas = rows(deploy / 'mst/area_mst.json', '3SG2wX0R')
    dungeons = rows(deploy / 'mst/dungeon_mst.json', '01pzrTCm')
    missions = rows(deploy / 'mst/mission_mst.json', 'oXeC1Ak9')

    by_dungeon = collections.defaultdict(list)
    for m in missions:
        by_dungeon[int(m['MHx05sXt'])].append(int(m['j28VNcUW']))
    dungeons_in_area = collections.defaultdict(list)
    for d in dungeons:
        dungeons_in_area[int(d['VjCY7rX4'])].append(int(d['MHx05sXt']))
    vortex_areas = {int(d['VjCY7rX4']) for d in dungeons
                    if int(d['9C64Qwe0']) == VORTEX_LAND and int(d['VjCY7rX4']) < VORTEX_AREA_MAX}
    children = collections.defaultdict(set)
    for a in areas:
        parent = str(a.get('sHMai2ZG') or '0')  # AreaMst.parent_area_id
        if parent.isdigit() and parent != '0':
            children[int(parent)].add(int(a['VjCY7rX4']))

    login, user, token = borrow_test_account(deploy)
    base = args.url.rstrip('/')
    reply = call(f'{base}/bf/gme/action.php', user, login, token, *USER_INFO, {})
    p_areas, p_missions = permitted(reply, 'VjCY7rX4'), permitted(reply, 'j28VNcUW')

    fails = []
    def check(ok: bool, what: str):
        print(('PASS ' if ok else 'FAIL ') + what)
        if not ok:
            fails.append(what)

    vortex_missions = {i for d in dungeons if int(d['9C64Qwe0']) == VORTEX_LAND
                       and int(d['VjCY7rX4']) < VORTEX_AREA_MAX for i in by_dungeon[int(d['MHx05sXt'])]}
    bad = sorted((p_missions & vortex_missions) - archived)
    check(not bad, f'every permitted Vortex mission has a battle (bad: {bad[:10]})')

    servable_areas = {a for a, ds in dungeons_in_area.items()
                      if any(m in archived for d in ds for m in by_dungeon[d])}
    shown = p_areas & vortex_areas
    empty_leaves = sorted(a for a in shown if a not in children and a not in servable_areas)
    check(not empty_leaves, f'every permitted Vortex leaf area has a playable dungeon (empty: {empty_leaves[:10]})')
    empty_cats = sorted(a for a in p_areas if a in children and a >= 100000
                        and not (children[a] & p_areas) and a not in servable_areas)
    check(not empty_cats, f'every permitted category has a permitted child or its own quest (empty: {empty_cats})')
    check(len(shown) > 0, f'the Vortex still has tiles ({len(shown)} area(s), '
                          f'{len(p_missions & vortex_missions)} mission(s))')

    print(f'{len(fails)} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
