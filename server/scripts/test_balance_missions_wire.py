#!/usr/bin/env python3
"""Wire test: hand-built battles in <balance_root>/server/missions go live without a restart.

    .venv/bin/python -I wt/battles/scripts/test_balance_missions_wire.py \
        --src wt/battles --url http://127.0.0.1:9978 [--mission 100700] [--fence 100701] \
        [--container bf-battles]

Runs against a server already started from --src (docker/run.sh with SRC set to
it): config.docker.json has "balance_root": "./balance", so the overlay folder
is <src>/deploy/balance/server/missions on the host.  Logs in as the save's
highest-level account (a TEST save: its session token is rewritten and its
energy topped up) and polls MissionStart (1 s apart, up to 15 s) instead of
sleeping.

  1. the archive record with sentinel HPs -> every sentinel HP and the wave
     count are served, and MissionEnd is accepted
  2. the same file with an unknown key -> step 1's version is still served
  3. the file removed -> the archive HPs again
  4. a misnamed 0<id>.json and a dotfile temp -> ignored
  5. an unknown ai_id -> rejected, the archive record is served
  6. a chest weight of 0, then of 2000 -> rejected, the archive record is served
  7. an energy_cost other than mission_mst's energy_use -> rejected
  8. a chest of 1,000,001 Zel, then of 10,000 items -> rejected; a chest of
     9,999 items -> served, and MissionEnd is accepted
  9. (--container only) after a restart, step 1's file is served from boot
     and a broken file for the fence mission leaves that one on the archive

A step whose file must NOT change what is served is fenced: after writing it
the test rewrites the fence mission's file with new sentinel HPs and waits
until those are served.  One server pass lists the whole folder and publishes
once at its end, so by then the step's file has been read too.  With
--container every step also checks the server's log line.

Always removes its files (and the folders it created).  Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import base64
import copy
import gzip
import hashlib
import json
import os
import re
import secrets
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
MONSTERS, MONSTER_HP, BATTLE_GROUPS, ERROR = 'U0v5IeJo', 'e7DK0FQT', 'pj41dy9g', 'b5PH6mZa'
POLL_SECONDS, POLL_TRIES = 1.0, 15
BOOT_TRIES = 120
UNKNOWN_AI = 999_999_999


# --- Wire helpers (as in tools/test_vortex_tiles.py / test_captured_missions.py) ---

def call(url: str, user: str, login: str, token: str, group: str, key: str, body: dict) -> dict:
    body = dict(body, IKqx1Cn9=[{'iN7buP2h': login, 'h7eY3sAK': user, 'iN7buP1i': token}])
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'battles'},
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


def borrow_test_account(db_path: Path) -> tuple[str, str, str]:
    """(gumi id, user id, token) of the highest-level account in this TEST save."""
    token = secrets.token_hex(16)
    with sqlite3.connect(db_path, timeout=30) as db:
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
                   (hashlib.sha256(token.encode()).hexdigest(), login))
    return str(login), str(user), token


def start_body(mission_id: int) -> dict:
    return {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission_id), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]}


def end_body(serial: int) -> dict:
    battle = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                               'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    battle.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    return {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}], 'rXvA1E5y': [battle]}


class Game:
    """One logged-in TEST account on the server under test."""

    def __init__(self, url: str, db_path: Path):
        self.url = f'{url.rstrip("/")}/bf/gme/action.php'
        self.db_path = db_path
        self.login, self.user, self.token = borrow_test_account(db_path)

    def battle(self, mission_id: int) -> dict:
        """Start and clear one battle: {'hps': set, 'waves': int, 'end_ok': bool}."""
        with sqlite3.connect(self.db_path, timeout=30) as db:  # a TEST save
            db.execute('UPDATE user_info SET energy=999 WHERE id=?', (self.user,))
        started = call(self.url, self.user, self.login, self.token, *MISSION_START, start_body(mission_id))
        if ERROR in started or 'Kz7qfSs5' not in started:
            raise RuntimeError(f'MissionStart {mission_id} failed: {json.dumps(started)[:200]}')
        serial = int(started['Kz7qfSs5'][0]['k9cxD7Ba'])
        ended = call(self.url, self.user, self.login, self.token, *MISSION_END, end_body(serial))
        return {'hps': {int(m[MONSTER_HP]) for m in started.get(MONSTERS) or []},
                'waves': len(started.get(BATTLE_GROUPS) or []),
                'end_ok': ERROR not in ended}


# --- Records and files ---

def with_hps(record: dict, base: int) -> dict:
    """A copy whose wave monsters carry HPs base, base+1, ... (unique sentinels)."""
    out = copy.deepcopy(record)
    for i, m in enumerate(m for s in out['stages'] for m in s['battle_monsters']):
        m['hp'] = base + i
    return out


def wave_hps(record: dict) -> set:
    return {m['hp'] for s in record['stages'] for m in s['battle_monsters']}


class Folder:
    """The overlay folder; remembers what it wrote so cleanup removes exactly that."""

    def __init__(self, deploy: Path):
        self.path = deploy / 'balance' / 'server' / 'missions'
        self.created = [p for p in (self.path.parents[1], self.path.parent, self.path) if not p.exists()]
        self.path.mkdir(mode=0o755, parents=True, exist_ok=True)
        self.written: set[Path] = set()

    def write(self, name: str, payload) -> None:
        """Atomic like Balance Forge's store.write_atomic: a dotfile temp, then os.replace."""
        target, tmp = self.path / name, self.path / f'.{name}.wiretest'
        tmp.write_text(payload if isinstance(payload, str) else json.dumps(payload))
        os.replace(tmp, target)
        self.written.add(target)

    def remove(self, name: str) -> None:
        (self.path / name).unlink(missing_ok=True)

    def cleanup(self) -> None:
        for path in self.written:
            path.unlink(missing_ok=True)
        for path in reversed(self.created):
            try:
                path.rmdir()
            except OSError:
                pass  # something else lives there now: leave it


# --- Polling and log checks ---

def poll(game: Game, mission_id: int, want, what: str, tries: int = POLL_TRIES) -> dict:
    """MissionStart every POLL_SECONDS until want(result); raises with the last result."""
    last = None
    for _ in range(tries):
        try:
            last = game.battle(mission_id)
            if want(last):
                return last
        except (urllib.error.URLError, ConnectionError, TimeoutError, RuntimeError, ValueError) as e:
            last = {'error': str(e)[:160]}
        time.sleep(POLL_SECONDS)
    raise AssertionError(f'{what}: not served after {tries} tries (last: {summary(last)})')


def summary(result) -> str:
    if not result or 'hps' not in result:
        return str(result)
    return f"{result['waves']} waves, hps {sorted(result['hps'])[:6]}..."


def serves(hps: set):
    return lambda r: hps <= r['hps']


def serves_archive(archive_hps: set, *sentinel_bases: int):
    def want(r):
        stray = {h for h in r['hps'] for b in sentinel_bases if b <= h < b + 1000}
        return archive_hps <= r['hps'] and not stray
    return want


class Logs:
    """The container's log since a mark; disabled without --container."""

    def __init__(self, container: str | None):
        self.container = container
        self.since = time.time()

    def mark(self) -> None:
        self.since = time.time() - 2  # slack for the Docker VM's clock

    def text(self) -> str:
        if not self.container:
            return ''
        out = subprocess.run(['docker', 'logs', '--since', f'{self.since:.3f}', self.container],
                             capture_output=True, text=True, timeout=60)
        return out.stdout + out.stderr

    def lines(self, pattern: str) -> list[str]:
        return [line for line in self.text().splitlines() if re.search(pattern, line)]


# --- The steps ---

STEP1, STEP2, MISNAMED, DOTFILE, STEP5, WEIGHT0, WEIGHT2000, BOOT_BROKEN = (
    123_457, 223_457, 323_457, 423_457, 523_457, 623_457, 723_457, 923_457)
ENERGY, ZEL_CHEST, ITEM_CHEST, ITEM_OK = 1_023_457, 1_123_457, 1_223_457, 1_323_457
FENCE_BASE = 800_000
MAX_CHEST_AMOUNT, MAX_CHEST_ITEMS = 1_000_000, 9_999  # MissionOverlay.cpp treasureProblem


class Wire:
    def __init__(self, args):
        deploy = args.src / 'deploy'
        archive = {r['id']: r for r in json.loads((deploy / 'archive' / 'mission.json').read_text())}
        self.mid, self.fid = args.mission, args.fence
        self.record, self.fence_record = archive[self.mid], archive[self.fid]
        # An item the archive's own chests drop, so it is in item_mst.
        self.item_id = next(d['target_id'] for r in archive.values() for s in r['stages']
                            for m in s['battle_monsters'] for d in m['treasure_drops'] if d['target_type'] == 4)
        self.archive_hps = wave_hps(self.record)
        self.name = f'{self.mid}.json'
        self.game = Game(args.url, deploy / 'gme.sqlite')
        self.folder = Folder(deploy)
        self.logs = Logs(args.container)
        self.fails: list[str] = []
        self.fence_round = 0

    def check(self, ok: bool, label: str, detail: str = '') -> None:
        print(f'{"PASS" if ok else "FAIL"} {label}' + (f' -- {detail}' if detail else ''))
        if not ok:
            self.fails.append(label)

    def logged(self, pattern: str, label: str) -> None:
        if not self.logs.container:
            return
        found = self.logs.lines(pattern)
        self.check(bool(found), f'log: {label}',
                   found[0].strip()[-220:] if found else f'no line matching {pattern!r}')

    def fence(self) -> None:
        """Rewrite the fence mission's file and wait until it is served (see the docstring)."""
        self.fence_round += 1
        record = with_hps(self.fence_record, FENCE_BASE + self.fence_round * 1000)
        self.folder.write(f'{self.fid}.json', record)
        poll(self.game, self.fid, serves(wave_hps(record)), f'fence round {self.fence_round}')

    def step1(self) -> set:
        self.logs.mark()
        record = with_hps(self.record, STEP1)
        self.folder.write(self.name, record)
        got = poll(self.game, self.mid, serves(wave_hps(record)), 'step 1: hand-built battle')
        self.check(True, 'step 1: every sentinel HP served', summary(got))
        self.check(got['waves'] == len(record['stages']), 'step 1: wave count',
                   f"{got['waves']} of {len(record['stages'])}")
        self.check(got['end_ok'], 'step 1: MissionEnd accepted')
        self.logged(rf'balance: mission {self.mid} from server/missions/{self.mid}\.json '
                    r'\(replaces the archive record\)', 'step 1 accepted')
        return wave_hps(record)

    def step2(self, kept: set) -> None:
        self.logs.mark()
        self.folder.write(self.name, dict(with_hps(self.record, STEP2), not_a_record_key=1))
        self.fence()
        got = self.game.battle(self.mid)
        self.check(kept <= got['hps'], 'step 2: unknown key rejected, step 1 version kept', summary(got))
        self.logged(rf'balance: mission {self.mid} rejected \(does not parse.*keeping the previous version',
                    'step 2 rejected')

    def step3(self) -> None:
        self.logs.mark()
        self.folder.remove(self.name)
        got = poll(self.game, self.mid, serves_archive(self.archive_hps, STEP1, STEP2), 'step 3: archive again')
        self.check(True, 'step 3: file removed, archive record served', summary(got))
        self.logged(rf'balance: mission {self.mid} file removed; serving the archive record', 'step 3 removed')

    def step4(self) -> None:
        self.logs.mark()
        misnamed, dotfile = f'0{self.mid}.json', f'.{self.mid}.json.Xy12ab'
        self.folder.write(misnamed, with_hps(self.record, MISNAMED))
        self.folder.write(dotfile, with_hps(self.record, DOTFILE))
        self.fence()
        got = self.game.battle(self.mid)
        self.check(serves_archive(self.archive_hps, MISNAMED, DOTFILE)(got),
                   'step 4: misnamed file and dotfile ignored', summary(got))
        self.logged(rf'balance: ignoring server/missions/0{self.mid}\.json', 'step 4 misnamed reported once')
        self.folder.remove(misnamed)
        self.folder.remove(dotfile)

    def rejected(self, record: dict, base: int, label: str, reason: str) -> None:
        self.logs.mark()
        self.folder.write(self.name, record)
        self.fence()
        got = self.game.battle(self.mid)
        self.check(serves_archive(self.archive_hps, base)(got), f'{label}: rejected, archive record served',
                   summary(got))
        self.logged(rf'balance: mission {self.mid} rejected \({reason}.*serving the archive record', label)

    def step5(self) -> None:
        record = with_hps(self.record, STEP5)
        record['stages'][0]['battle_monsters'][0]['ai_id'] = UNKNOWN_AI
        self.rejected(record, STEP5, 'step 5: unknown ai_id', r'dry run: ais')

    def with_chest(self, base: int, **row) -> dict | None:
        """with_hps(base) with `row` merged into its first chest row; None (a FAIL) without a chest."""
        record = with_hps(self.record, base)
        drops = next((m['treasure_drops'] for s in record['stages'] for m in s['battle_monsters']
                      if m['treasure_drops']), None)
        if drops is None:
            self.check(False, f'mission {self.mid} has no chest to change')
            return None
        drops[0].update(row)
        return record

    def step6(self) -> None:
        for weight, base in ((0, WEIGHT0), (2000, WEIGHT2000)):
            record = self.with_chest(base, weight=weight)
            if record is None:
                return
            self.rejected(record, base, f'step 6: chest weight {weight}', rf'.*treasure weight {weight} \(')
        self.folder.remove(self.name)

    def step7(self) -> None:
        mst = self.record['energy_cost']   # the archive matches mission_mst everywhere
        record = dict(with_hps(self.record, ENERGY), energy_cost=mst + 1)
        self.rejected(record, ENERGY, 'step 7: energy_cost differs from mission_mst',
                      rf'energy_cost {mst + 1} but mission_mst energy_use is {mst}\)')

    def step8(self) -> None:
        zel = {'target_type': 1, 'target_id': '', 'amount': MAX_CHEST_AMOUNT + 1}
        item = {'target_type': 4, 'target_id': self.item_id}
        for row, base, cap in ((zel, ZEL_CHEST, MAX_CHEST_AMOUNT),
                               (dict(item, amount=MAX_CHEST_ITEMS + 1), ITEM_CHEST, MAX_CHEST_ITEMS)):
            record = self.with_chest(base, **row)
            if record is None:
                return
            self.rejected(record, base, f'step 8: chest of {row["amount"]} (type {row["target_type"]})',
                          rf'.*treasure amount {row["amount"]} \(1-{cap} for target_type {row["target_type"]}\)')

        record = self.with_chest(ITEM_OK, **dict(item, amount=MAX_CHEST_ITEMS))
        if record is None:
            return
        self.logs.mark()
        self.folder.write(self.name, record)
        got = poll(self.game, self.mid, serves(wave_hps(record)), f'step 8: chest of {MAX_CHEST_ITEMS} items')
        self.check(got['end_ok'], f'step 8: chest of {MAX_CHEST_ITEMS} items served, MissionEnd accepted',
                   summary(got))
        self.logged(rf'balance: mission {self.mid} from server/missions/{self.mid}\.json', 'step 8 accepted')
        self.folder.remove(self.name)

    def step9(self, container: str) -> None:
        """Boot: a good file is served from the first answer; a broken one only costs its own mission."""
        good = with_hps(self.record, STEP1)
        self.folder.write(self.name, good)
        self.folder.write(f'{self.fid}.json', dict(with_hps(self.fence_record, BOOT_BROKEN), not_a_record_key=1))
        self.logs.mark()
        subprocess.run(['docker', 'restart', container], check=True, capture_output=True, timeout=180)
        got = poll(self.game, self.mid, lambda r: True, 'step 9: server answering after restart', BOOT_TRIES)
        self.check(wave_hps(good) <= got['hps'], 'step 9: hand-built battle served from boot', summary(got))
        fence = self.game.battle(self.fid)
        self.check(serves_archive(wave_hps(self.fence_record), BOOT_BROKEN, FENCE_BASE)(fence),
                   'step 9: broken file at boot leaves its mission on the archive record', summary(fence))
        self.logged(r'balance: 1 mission record\(s\) from \S+ \(1 replace archive records, 0 new\), 1 rejected',
                    'step 9 boot summary')

    def back_to_archive(self) -> None:
        """After cleanup: both missions serve their archive records again."""
        poll(self.game, self.mid, serves_archive(self.archive_hps, STEP1, STEP2, MISNAMED, DOTFILE, STEP5,
                                                 WEIGHT0, WEIGHT2000, ENERGY, ZEL_CHEST, ITEM_CHEST, ITEM_OK),
             'cleanup: mission back on the archive')
        poll(self.game, self.fid, serves_archive(wave_hps(self.fence_record), FENCE_BASE, BOOT_BROKEN),
             'cleanup: fence back on the archive')
        self.check(True, 'cleanup: files removed, both missions back on their archive records')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, required=True, help='server checkout the container runs')
    ap.add_argument('--url', default='http://127.0.0.1:9960')
    ap.add_argument('--mission', type=int, default=100700)
    ap.add_argument('--fence', type=int, default=100701, help='a second archived mission used as a barrier')
    ap.add_argument('--container', help='docker container name: also check log lines and run the boot step')
    args = ap.parse_args()

    wire = Wire(args)
    try:
        kept = wire.step1()
        wire.step2(kept)
        wire.step3()
        wire.step4()
        wire.step5()
        wire.step6()
        wire.step7()
        wire.step8()
        if args.container:
            wire.step9(args.container)
    except Exception as e:  # noqa: BLE001 - report it, then clean up
        wire.check(False, f'{type(e).__name__}: {e}')
    finally:
        wire.folder.cleanup()
    try:
        wire.back_to_archive()
    except AssertionError as e:
        wire.check(False, str(e))
    print(f'{len(wire.fails)} failure(s)')
    return 1 if wire.fails else 0


if __name__ == '__main__':
    sys.exit(main())
