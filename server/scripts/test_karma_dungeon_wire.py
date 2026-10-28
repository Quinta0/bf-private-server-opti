#!/usr/bin/env python3
"""Wire test: the rebuilt Monday Karma dungeons are served and pay their Karma (ticket #43).

    .venv/bin/python -I wt/karma-dungeon/scripts/test_karma_dungeon_wire.py \
        --src wt/karma-dungeon --url http://127.0.0.1:9979 --prepare      # server STOPPED
    SRC=$PWD/wt/karma-dungeon NAME=bf-karma PORT=9979 DETACH=1 docker/run.sh
    .venv/bin/python -I wt/karma-dungeon/scripts/test_karma_dungeon_wire.py \
        --src wt/karma-dungeon --url http://127.0.0.1:9979

--prepare writes the TEST save while no server holds it (a host write next to a
running container's WAL can split it): the top account gets a known session
token and 9,999 energy, kept in out/qa/karma-dungeon/account.json.

The run itself never touches the save.  For every mission in
deploy/balance-overlays/karma-dungeon it

  1. shows what the archive record serves (before),
  2. installs the file into <src>/deploy/balance/server/missions and polls
     MissionStart until the file's waves are served,
  3. checks the reply against the file: wave count and boss flag, every
     wave's monster ids in order, HP and Karma drop numbers per monster, no
     capture anywhere, and every Karma chest the file promises present in
     the drop info with its amount (chests are 100%, Mimics are off),
  4. ends the battle reporting all of it (every enemy's karma_max_drop plus
     every chest) and checks MissionEnd credits exactly that Karma,
  5. removes the file again (unless --keep) and sees the archive record back.

Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import base64
import collections
import gzip
import hashlib
import json
import os
import secrets
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
ERROR = 'b5PH6mZa'
# MissionStartResp
GROUPS, MONSTER_GROUPS, MONSTERS, NUM = 'pj41dy9g', '75t0sx9z', 'U0v5IeJo', 'Kz7qfSs5'
# BattleGroupMst / BattleMonsterGroupMst / MonsterMst / MissionNumInfo
G_ID, G_ORDER, G_BOSS = 'Qzhp8B40', 'VETu07N6', 'etM5TCb9'
MG_GROUP, MG_MONSTER, MG_ORDER, MG_DROP = 'Qzhp8B40', 'o49dYfpH', 'hZtF1s8B', 'hw3L0uVj'
M_ID, M_HP, M_KARMA, M_KARMA_N, M_ZEL = 'o49dYfpH', 'e7DK0FQT', 'HTVh8a65', 'vNk2sI4X', 'Najhr8m6'
SERIAL, DROP_INFO = 'k9cxD7Ba', 'LE6JkUp7'
# MissionEndResp
REWARD, TEAM, KARMA = 'F5Vs19mb', 'fEi17cnx', 'HTVh8a65'
KARMA_CHEST = 2
POLL_SECONDS, POLL_TRIES = 1.0, 20


def call(url: str, account: dict, group: str, key: str, body: dict) -> dict:
    body = dict(body, IKqx1Cn9=[{'iN7buP2h': account['login'], 'h7eY3sAK': account['user'],
                                 'iN7buP1i': account['token']}])
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'karma'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    req = urllib.request.Request(url, json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        raise RuntimeError(f'server refused {group}: {outer}')
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def start_body(mission_id: int) -> dict:
    return {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission_id), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]}


def end_body(serial: int, karma: int) -> dict:
    battle = {k: '0' for k in ('Najhr8m6', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                               'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    battle.update({'HTVh8a65': str(karma), '47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    return {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}], 'rXvA1E5y': [battle]}


# --- What a MissionStart reply serves ---

def parse_drop_info(text: str) -> list[dict]:
    """battleOrder|unitDrops|treasureDrops per wave, '@'-separated (MissionArchiver::encodeMissionDropInfo)."""
    waves = []
    for chunk in text.split('@'):
        order, units, chests = (chunk.split('|') + ['', ''])[:3]
        parsed = []
        for entry in chests.strip().split('-') if chests.strip() else []:
            idx, kind, params = entry.split('/', 2)
            fields = params.split(':')
            parsed.append({'monster': int(idx), 'kind': int(kind), 'target_type': int(fields[0]),
                           'amount': int(fields[-1])})
        waves.append({'order': int(order), 'units': units.strip(), 'chests': parsed})
    return waves


def served(reply: dict) -> dict:
    """Waves as the client will build them: [{'boss', 'monsters': [{id, hp, karma, ...}], 'chests'}]."""
    groups = sorted(reply.get(GROUPS) or [], key=lambda g: int(g[G_ORDER]))
    members = collections.defaultdict(list)
    for row in reply.get(MONSTER_GROUPS) or []:
        members[row[MG_GROUP]].append(row)
    # One MonsterMst row per monster occurrence, wave by wave (MissionArchiver::populatePacket);
    # the client keeps the first row of an id, so that is what this reads too.
    rows = {}
    for row in reply.get(MONSTERS) or []:
        rows.setdefault(row[M_ID], row)
    drops = parse_drop_info(reply[NUM][0].get(DROP_INFO, '')) if reply.get(NUM) else []
    waves = []
    for i, group in enumerate(groups):
        monsters = []
        for row in sorted(members[group[G_ID]], key=lambda r: int(r[MG_ORDER])):
            mst = rows[row[MG_MONSTER]]
            monsters.append({'id': int(row[MG_MONSTER]), 'hp': int(mst[M_HP]), 'karma': int(mst[M_KARMA]),
                             'karma_n': int(mst[M_KARMA_N]), 'zel': int(mst[M_ZEL]),
                             'capture': row.get(MG_DROP, '')})
        wave_drops = drops[i] if i < len(drops) else {'units': '', 'chests': []}
        waves.append({'boss': group[G_BOSS] in ('1', 'true', True), 'monsters': monsters,
                      'units': wave_drops['units'], 'chests': wave_drops['chests']})
    return {'serial': int(reply[NUM][0][SERIAL]), 'waves': waves}


def expected(record: dict) -> list[dict]:
    return [{'boss': s['is_boss'],
             'monsters': [{'id': m['id'], 'name': m['name'], 'hp': m['hp'], 'karma': m['karma_max_drop'],
                           'karma_n': m['karma_drop_count'],
                           'chest': sum(t['amount'] for t in m['treasure_drops'] if t['target_type'] == KARMA_CHEST)
                           if m['treasure_chest_chance'] else 0}
                          for m in s['battle_monsters']]}
            for s in record['stages']]


def karma_ceiling(waves: list[dict]) -> int:
    """Every Karma piece and every Karma chest the server rolled.  The client
    pays karma_max_drop as karma_drop_count pieces of max/count, floored
    (MonsterParty::entryMonsters @0x10c1eec), so 8 Karma in 5 pieces is 5."""
    return sum(m['karma_n'] * (m['karma'] // m['karma_n']) for w in waves for m in w['monsters'] if m['karma_n']) + \
        sum(c['amount'] for w in waves for c in w['chests'] if c['target_type'] == KARMA_CHEST)


# --- Harness ---

class Server:
    def __init__(self, url: str, account: dict):
        self.url = f'{url.rstrip("/")}/bf/gme/action.php'
        self.account = account

    def start(self, mission_id: int) -> dict:
        reply = call(self.url, self.account, *MISSION_START, start_body(mission_id))
        if ERROR in reply or NUM not in reply:
            raise RuntimeError(f'MissionStart {mission_id} failed: {json.dumps(reply)[:300]}')
        return reply

    def end(self, serial: int, karma: int) -> dict:
        reply = call(self.url, self.account, *MISSION_END, end_body(serial, karma))
        if ERROR in reply:
            raise RuntimeError(f'MissionEnd failed: {json.dumps(reply)[:300]}')
        return reply

    def poll(self, mission_id: int, want) -> dict:
        last = None
        for _ in range(POLL_TRIES):
            try:
                last = served(self.start(mission_id))
                if want(last):
                    return last
            except (urllib.error.URLError, ConnectionError, TimeoutError, RuntimeError, ValueError, KeyError) as e:
                last = {'error': str(e)[:200]}
            time.sleep(POLL_SECONDS)
        raise AssertionError(f'mission {mission_id}: not served after {POLL_TRIES} tries; last {str(last)[:300]}')


def same_waves(record: dict):
    want = [[m['id'] for m in w['monsters']] for w in expected(record)]
    hps = [[m['hp'] for m in w['monsters']] for w in expected(record)]
    return lambda got: 'waves' in got and [[m['id'] for m in w['monsters']] for w in got['waves']] == want \
        and [[m['hp'] for m in w['monsters']] for w in got['waves']] == hps


def describe(waves: list[dict], names: dict) -> str:
    lines = []
    for i, w in enumerate(waves, 1):
        counts = collections.Counter(names.get(m['id'], str(m['id'])) for m in w['monsters'])
        chest = sum(c['amount'] for c in w['chests'] if c['target_type'] == KARMA_CHEST)
        hp = '/'.join(f"{m['hp']:,}" for m in w['monsters'])
        lines.append(f"    wave {i}{' BOSS' if w['boss'] else '     '} "
                     + ', '.join(f'{n} x{c}' if c > 1 else n for n, c in counts.items())
                     + f"  HP {hp}  Karma drops {sum(m['karma'] for m in w['monsters']):,}"
                     + (f"  chests {chest:,}" if chest else '')
                     + (f"  CAPTURE {w['units']}" if w['units'] else ''))
    return '\n'.join(lines)


class Checks:
    def __init__(self):
        self.failed = 0

    def __call__(self, label: str, ok: bool, detail: str = '') -> bool:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f' ({detail})' if detail and not ok else ''))
        self.failed += not ok
        return ok


def prepare(src: Path, account_file: Path) -> int:
    save = src / 'deploy' / 'gme.sqlite'
    token = secrets.token_hex(16)
    with sqlite3.connect(save, timeout=30) as db:   # a TEST save, server stopped
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
                   (hashlib.sha256(token.encode()).hexdigest(), login))
        db.execute('UPDATE user_info SET energy=9999 WHERE id=?', (user,))
    account_file.parent.mkdir(parents=True, exist_ok=True)
    account_file.write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
    print(f'prepared user {user} in {save}: token in {account_file}')
    return 0


def check_mission(server: Server, folder: Path, path: Path, keep: bool, check: Checks) -> None:
    record = json.loads(path.read_text())
    mid = record['id']
    want = expected(record)
    names = {m['id']: m['name'] for s in record['stages'] for m in s['battle_monsters']}
    print(f"\n{mid} {record['name']} ({record['energy_cost']} energy)")

    before = served(server.start(mid))
    archive_names = {}
    print('  before (archive record):')
    print(describe(before['waves'], archive_names))
    old_karma = karma_ceiling(before['waves'])
    print(f'    up to {old_karma:,} Karma')

    target = folder / path.name
    tmp = folder / f'.{path.name}.wiretest'
    tmp.write_text(path.read_text())
    os.chmod(tmp, 0o644)
    os.replace(tmp, target)
    got = server.poll(mid, same_waves(record))
    print('  after (balance file):')
    print(describe(got['waves'], names))

    check('wave count and boss wave', [w['boss'] for w in got['waves']] == [w['boss'] for w in want])
    check('monster ids, HP, Karma drops per wave',
          all([(m['id'], m['hp'], m['karma'], m['karma_n']) for m in g['monsters']]
              == [(m['id'], m['hp'], m['karma'], m['karma_n']) for m in w['monsters']]
              for g, w in zip(got['waves'], want)))
    check('nothing can be caught', all(not w['units'] for w in got['waves'])
          # hw3L0uVj only names unit assets to load: "0:0:0:0" when the monster drops no unit.
          and all(m['capture'] in ('', '0:0:0:0') for w in got['waves'] for m in w['monsters']),
          str([w['units'] for w in got['waves']]))
    chests_ok = all(sorted((c['monster'], c['amount']) for c in g['chests'] if c['target_type'] == KARMA_CHEST and c['kind'] == 1)
                    == sorted((i, m['chest']) for i, m in enumerate(w['monsters']) if m['chest'])
                    and all(c['kind'] == 1 for c in g['chests'])
                    for g, w in zip(got['waves'], want))
    check('every Karma chest present with its amount, no Mimic', chests_ok,
          str([g['chests'] for g in got['waves']]))
    no_imps = not any(m['id'] in (14550, 24400, 34250, 45550) for w in got['waves'] for m in w['monsters'])
    check('no stat Imps', no_imps)
    boss = got['waves'][-1]['monsters'][0]
    check(f"boss is {names[boss['id']]} with {boss['hp']:,} HP", boss['id'] == want[-1]['monsters'][0]['id'])

    karma = karma_ceiling(got['waves'])
    ended = server.end(got['serial'], karma)
    paid = int(ended[REWARD][0][KARMA]) if ended.get(REWARD) else -1
    total = int(ended[TEAM][0][KARMA]) if ended.get(TEAM) else -1
    print(f'  MissionEnd reporting every drop and chest: {karma:,} Karma (archive record: up to {old_karma:,})')
    check(f'MissionEnd credits {karma:,} Karma (total now {total:,})', paid == karma + record['karma'],
          f'reward said {paid}')

    if not keep:
        target.unlink()
        back = server.poll(mid, lambda r: 'waves' in r and [[m['id'] for m in w['monsters']] for w in r['waves']]
                           == [[m['id'] for m in w['monsters']] for w in before['waves']])
        check('file removed: the archive record is served again', bool(back))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9979')
    ap.add_argument('--files', type=Path, help='default: <src>/deploy/balance-overlays/karma-dungeon')
    ap.add_argument('--only', type=int, nargs='*')
    ap.add_argument('--prepare', action='store_true', help='write the TEST save (server stopped) and exit')
    ap.add_argument('--keep', action='store_true', help='leave the files installed')
    args = ap.parse_args()
    src = args.src.resolve()
    account_file = src / 'out' / 'qa' / 'karma-dungeon' / 'account.json'
    if args.prepare:
        return prepare(src, account_file)

    files = sorted((args.files or src / 'deploy' / 'balance-overlays' / 'karma-dungeon').glob('*.json'))
    if args.only:
        files = [f for f in files if int(f.stem) in args.only]
    folder = src / 'deploy' / 'balance' / 'server' / 'missions'
    created = [p for p in (folder.parents[1], folder.parent, folder) if not p.exists()]
    folder.mkdir(mode=0o755, parents=True, exist_ok=True)
    for p in created:
        os.chmod(p, 0o755)
    server = Server(args.url, json.loads(account_file.read_text()))
    check = Checks()
    try:
        for path in files:
            check_mission(server, folder, path, args.keep, check)
    finally:
        if not args.keep:
            for path in files:
                (folder / path.name).unlink(missing_ok=True)
            for p in reversed(created):
                try:
                    p.rmdir()
                except OSError:
                    pass
    print(f"\n{'ALL PASS' if not check.failed else f'{check.failed} FAILED'} ({len(files)} missions)")
    return 1 if check.failed else 0


if __name__ == '__main__':
    sys.exit(main())
