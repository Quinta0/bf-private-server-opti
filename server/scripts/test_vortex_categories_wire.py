#!/usr/bin/env python3
"""Wire test: the Vortex category children are permitted and play (ticket #19).

    # server STOPPED; the save may sit in a Docker volume, so prepare inside the toolchain image:
    docker run --rm -v <save volume>:/save -v $PWD/wt/vortex:/src bf-server-build \
        python3 -I /src/scripts/test_vortex_categories_wire.py --src /src --prepare --save /save/gme.sqlite
    # server running:
    .venv/bin/python -I wt/vortex/scripts/test_vortex_categories_wire.py \
        --src wt/vortex --url http://127.0.0.1:9990

Travellers (100002), Vortex Trials (100003) and the two Seasons Past tiles
(100004, 100005) are categories.  73 of their child areas carry ids far above
the Vortex range (700000 .. 25600001); the client lists them from AreaMst, but
the server only permitted areas below 200000, so tapping one dropped the player
back on the Vortex list.  Entering an area sends no request of its own: what
the client needs is the permit rows, so "entering" is checked on UserInfo.

Checks:
  1. UserInfo permits every nested child that has a battle, its dungeons and its
     entry missions, and the four category tiles;
  2. no other land-99 area above the range is permitted (Frontier Hunter, Frontier
     Gate, Grand Quest and the trials keep their own collectors);
  3. one mission per category (and one two levels deep) starts and ends, and the
     MissionEnd permit refresh opens the next mission of its chain.

--prepare gives the save's highest-level account a known session token and
9,999 energy (out/qa/vortex-categories/account.json).  Requires pycryptodome
for the run (not for --prepare).
"""
from __future__ import annotations

import argparse
import base64
import collections
import gzip
import hashlib
import json
import secrets
import sqlite3
import sys
import urllib.request
from pathlib import Path

USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
ERROR = 'b5PH6mZa'
NUM, SERIAL = 'Kz7qfSs5', 'k9cxD7Ba'
PERMIT_CHANNELS = ('yXNM8kL3', 'Y73tHKS8', 'Y73mHKS8')
AREA, DUNGEON, MISSION = 'VjCY7rX4', 'MHx05sXt', 'j28VNcUW'

VORTEX_LAND, VORTEX_AREA_MAX = 99, 200000
CATEGORIES = (100002, 100003, 100004, 100005)
TRIAL_DUNGEONS = (800051, 2000000, 2100000)
# Land-99 areas above the range that OTHER collectors permit: Frontier Hunter,
# the Research Lab trials, Frontier Gate, Grand Quest.
OTHER_COLLECTORS = ((1000000, 1999999), (2000000, 2999999), (3000000, 3999999), (5000000, 5999999))
# (mission, what it stands for): one per category, one two levels deep.
SAMPLES = ((8301541, 'Vortex Trials > Weaver of Friendship'),
           (8350401, 'Seasons Past (100004) > Easter Madness'),
           (8301530, 'Seasons Past (100005) > Candy Crash'),
           (9104200, "Travellers > A Dark Ritual"),
           (8301470, 'Vortex Trials > Scrolls of Urzyma > Burden of Aesys'),
           # The event areas re-parented under Seasons Past from the community
           # megathread (they had no parent, so nothing collected them).
           (20001201, 'Seasons Past > Brave Frontier Heroes > Battlefield of Glory'),
           (20001010, 'Seasons Past > Summer 2020 Daily > Daily Tan'),
           (20000909, 'Seasons Past > Defend the Gates > Against the Darkness'),
           (409581, 'Seasons Past > Gortius Daily > Training Grounds'),
           (83018000, 'Seasons Past > Treasure Paradise'),
           (8001721, 'Seasons Past > Trigun Collaboration > Parallel Festivals'),
           (8301781, 'Seasons Past > Trigun: Bounties > Bounty Searcher'),
           (8301791, 'Seasons Past > Trigun: Routine Bounty'))


def rows(path: Path, key: str) -> list:
    data = json.loads(path.read_text())
    return data[key] if isinstance(data, dict) else data


def call(url: str, account: dict, group: str, key: str, body: dict) -> dict:
    from Crypto.Cipher import AES

    body = dict(body, IKqx1Cn9=[{'iN7buP2h': account['login'], 'h7eY3sAK': account['user'],
                                 'iN7buP1i': account['token']}])
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'vortexcat'},
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


def end_body(serial: int) -> dict:
    battle = {k: '0' for k in ('Najhr8m6', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                               'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9', 'HTVh8a65')}
    battle.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    return {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}], 'rXvA1E5y': [battle]}


def permitted(reply: dict, key: str) -> set[int]:
    return {int(r[key]) for ch in PERMIT_CHANNELS for r in (reply.get(ch) or []) if key in r}


class World:
    """The Vortex tree as the server's boot code sees it (ServerCache nestedVortexAreas)."""

    def __init__(self, deploy: Path):
        self.archived = {int(m['id']) for m in json.loads((deploy / 'archive/mission.json').read_text())}
        self.areas = {int(a[AREA]): a for a in rows(deploy / 'mst/area_mst.json', '3SG2wX0R')}
        dungeons = rows(deploy / 'mst/dungeon_mst.json', '01pzrTCm')
        missions = rows(deploy / 'mst/mission_mst.json', 'oXeC1Ak9')
        self.dungeons_in = collections.defaultdict(list)
        for d in dungeons:
            self.dungeons_in[int(d[AREA])].append(int(d[DUNGEON]))
        self.missions_in = collections.defaultdict(list)
        self.needs = {}
        for m in missions:
            self.missions_in[int(m[DUNGEON])].append(int(m[MISSION]))
            self.needs[int(m[MISSION])] = [int(n) for n in str(m.get('HSRhkf70') or '0').split(',') if n.strip()]
        trial_areas = {int(d[AREA]) for d in dungeons if int(d[DUNGEON]) in TRIAL_DUNGEONS}
        self.nested = {a for a in self.areas if self.land(a) == VORTEX_LAND and a >= VORTEX_AREA_MAX
                       and a not in trial_areas and self._reaches_range(a)}

    def land(self, area: int) -> int:
        return int(self.areas[area]['9C64Qwe0'])

    def parent(self, area: int) -> int:
        p = str(self.areas[area].get('sHMai2ZG') or '0')
        return int(p) if p.isdigit() else 0

    def _reaches_range(self, area: int) -> bool:
        at = area
        for _ in range(8):
            at = self.parent(at)
            if at not in self.areas or self.land(at) != VORTEX_LAND:
                return False
            if 0 < at < VORTEX_AREA_MAX:
                return True
        return False

    def servable(self, dungeon: int) -> bool:
        return any(m in self.archived for m in self.missions_in.get(dungeon, []))

    def next_in_chain(self, mission: int) -> int | None:
        for d, ms in self.missions_in.items():
            if mission in ms:
                return next((m for m in ms if mission in self.needs.get(m, [])), None)
        return None


class Checks:
    def __init__(self):
        self.failed = 0

    def __call__(self, label: str, ok: bool, detail: str = '') -> bool:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f' ({detail})' if detail and not ok else ''))
        self.failed += not ok
        return ok


def prepare(save: Path, account_file: Path) -> int:
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


def check_permits(world: World, reply: dict, check: Checks) -> None:
    areas, dungeons, missions = (permitted(reply, k) for k in (AREA, DUNGEON, MISSION))
    playable = {a for a in world.nested if any(world.servable(d) for d in world.dungeons_in.get(a, []))}
    categories = {a for a in world.nested if not world.dungeons_in.get(a)}
    print(f'  {len(world.nested)} nested Vortex area(s), {len(playable)} with a battle, '
          f'{len(categories)} nested categor(y/ies) without dungeons')

    check('every category tile is permitted', set(CATEGORIES) <= areas, str(sorted(set(CATEGORIES) - areas)))
    missing = sorted((playable | categories) - areas)
    check('every nested child area is permitted', not missing, str(missing[:15]))
    no_dungeon = sorted(a for a in playable if not any(d in dungeons for d in world.dungeons_in.get(a, [])))
    check('every nested area has a permitted dungeon', not no_dungeon, str(no_dungeon[:15]))
    entries = {m for a in playable for d in world.dungeons_in.get(a, []) for m in world.missions_in.get(d, [])
               if m in world.archived and not any(world.needs.get(m, []))}
    check(f'every entry mission is permitted ({len(entries)})', entries <= missions,
          str(sorted(entries - missions)[:15]))
    unbuilt = sorted(m for a in world.nested for d in world.dungeons_in.get(a, []) for m in world.missions_in.get(d, [])
                     if m in missions and m not in world.archived)
    check('no permitted nested mission lacks a battle', not unbuilt, str(unbuilt[:15]))
    stray = sorted(a for a in areas if a in world.areas and world.land(a) == VORTEX_LAND
                   and a >= VORTEX_AREA_MAX and a not in world.nested
                   and not any(lo <= a <= hi for lo, hi in OTHER_COLLECTORS) and a != 800051)
    check('no other land-99 area above the range is opened', not stray, str(stray[:15]))


def run_mission(url: str, account: dict, world: World, mission: int, label: str, check: Checks) -> None:
    print(f'\n{mission} {label}')
    start = call(url, account, *MISSION_START, start_body(mission))
    if not check('MissionStart serves the battle', ERROR not in start and NUM in start,
                 json.dumps(start)[:300]):
        return
    serial = int(start[NUM][0][SERIAL])
    end = call(url, account, *MISSION_END, end_body(serial))
    if not check('MissionEnd accepts the clear', ERROR not in end, json.dumps(end)[:300]):
        return
    area = next(a for a, ds in world.dungeons_in.items() for d in ds if mission in world.missions_in.get(d, []))
    check(f'area {area} stays permitted after MissionEnd', area in permitted(end, AREA))
    nxt = world.next_in_chain(mission)
    if nxt is not None and nxt in world.archived:
        check(f'the clear opens the next mission {nxt}', nxt in permitted(end, MISSION))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9990')
    ap.add_argument('--prepare', action='store_true', help='write the TEST save (server stopped) and exit')
    ap.add_argument('--save', type=Path, help='default: <src>/deploy/gme.sqlite')
    args = ap.parse_args()
    src = args.src.resolve()
    account_file = src / 'out' / 'qa' / 'vortex-categories' / 'account.json'
    if args.prepare:
        return prepare(args.save or src / 'deploy' / 'gme.sqlite', account_file)

    world = World(src / 'deploy')
    account = json.loads(account_file.read_text())
    url = f"{args.url.rstrip('/')}/bf/gme/action.php"
    check = Checks()
    print('UserInfo')
    check_permits(world, call(url, account, *USER_INFO, {}), check)
    for mission, label in SAMPLES:
        run_mission(url, account, world, mission, label, check)
    print(f"\n{'ALL PASS' if not check.failed else f'{check.failed} FAILED'}")
    return 1 if check.failed else 0


if __name__ == '__main__':
    sys.exit(main())
