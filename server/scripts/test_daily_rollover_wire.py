"""Wire test for the daily rollover at 08:00 UTC (server branch
feat/daily-task-0800): the Daily Tasks (Brave Points), the login spin, and
giftToday()'s day (the login calendar and the simulated friend's gift).

The client's day is UTC-8 all year (ServerTimeInfo stores server time - 28800),
so Today's Tasks counts down to 08:00 UTC.  The server used to turn these days
at 00:00 UTC; it now turns them at 08:00 UTC, and the migrations that switch
them record a day shift per clock so the day running at the deploy keeps its
number (GameDay.hpp, DayClock).  At every step below the spin, the calendar
and the gift are checked alongside the task board: granted once per day,
refused again the same day, never repeated across the deploy or midnight.

This test starts the server itself, in Docker, with its clock set by
libfaketime, so it can cross the boundaries for real:

  A. A fresh save on the new server.
     A1 07:59 -> 08:00: the board (which tasks, how far along) is day N's before
        08:00 and day N+1's after, with a fresh count; a craft after 08:00
        starts at its own size.
     A2 23:59 -> 00:00: nothing changes: same tasks, same progress, and crafts
        keep adding up; finishing Craftsman pays its Brave Points once.  The
        restart also shows the recorded shift is read back, not recomputed.
  B. A deploy between 00:00 and 08:00 UTC: the OLD server plays part of the day,
     then the new one takes the same save.
     B1 old server: the board is the old rotation (checks this test's model).
     B2 new server: same board, same progress, crafts add up; shift 1 recorded.
     B3 that day ends at 08:00 (8 hours long), then a fresh day.
     B4 a restart at 12:00 stays on the new day (the shift persisted); a won
        quest counts Quest Explorer through MissionEnd on that day (midnight UTC
        would still say D); a task row, a spin row, the calendar and a gift
        dated a later day are not rewound and grant nothing beyond what they
        already hold (a spin row whose spin is unused gives that one spin,
        stamped with its own later day).
  C. A deploy between 08:00 and 24:00 UTC.
     C1 old server, C2 new server as above; shift 0 recorded.
     C3 23:59 -> 00:00 does NOT roll (the day stretches to 08:00, 32 hours).
     C4 08:00 the next morning rolls.

Progress is planted straight into user_daily_tasks for the codes the test
does not play (it reads back only for the day the server thinks it is);
Craftsman (CM) is driven by real ItemMix requests and Quest Explorer (QE) by a
real MissionStart / MissionEnd, so both the read and the write side are
covered.  Days are chosen so those codes are on the board.  Each server's
first log line is checked against its fake clock, so a preload that did not
take fails loudly instead of testing the real date.

Reuses the fair-summon harness (tools/test_fair_summon.py in the main repo).

Usage (from the worktree root; get libfaketime once with
    docker run --rm -v "$DIR:/out" bf-server-build bash -c \\
      'cd /tmp && apt-get update -qq && apt-get download libfaketime && dpkg-deb -x libfaketime_*.deb /out'
and pass the folder holding libfaketimeMT.so.1):
  python3 -I scripts/test_daily_rollover_wire.py \\
      --new-bin out/build-linux-arm64/standalone_frontend/gimuserverw \\
      --old-bin ../dblock/out/build-linux-arm64/standalone_frontend/gimuserverw \\
      --faketime "$DIR/usr/lib/aarch64-linux-gnu/faketime"
Never point it at a save with real players: it makes its own under out/.
"""
import argparse
import datetime as dt
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def _harness() -> Path:
    for parent in Path(__file__).resolve().parents:
        if (parent / 'tools' / 'test_fair_summon.py').is_file():
            return parent / 'tools'
    raise SystemExit('tools/test_fair_summon.py not found above this script')


sys.path.insert(0, str(_harness()))
from test_fair_summon import Database, Player, fetch, gme_call, wire_error  # noqa: E402

DAILY_TASK_INFO = ('m7g0Ekb5', 'Hd8c3Y6')
UPDATE_INFO = ('RUV94Dqz', 'hy0P9xjsGJ6MAgb2')
ITEM_MIX = ('4P5GELTF', 'AFqKIJ8Z4mHPB9xg')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
MISSION = 10           # a first-area quest a fresh save can clear (test_daily_challenges_wire.py)
DAILY_LOGIN = ('4aClzokO', 'stI81haQ')     # the login spin
INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')  # what opens the wheel on Home
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')  # advances the login calendar, mints the day's gift
SIMULATED_SENDER = 'n9ZMPC0t'                # the simulated friend's daily gift

# kDailyTaskCodes, in order: the rotation shuffles this list.
CODES = ('AV', 'QE', 'VV', 'CM', 'UU', 'PU')
PER_DAY = 3
CM_TARGET = 5          # daily_task_mst: Craftsman, "Craft 5 Items/Spheres"
CM_POINTS = 20
DAY_START = 8 * 3600
SHIFT_KEY = 'daily_task_day_shift'
SPIN_KEY = 'daily_spin_day_shift'
GIFT_KEY = 'gift_day_shift'
RECIPE = 2005          # 5 x item 10400 + 500 Karma -> item 30000 (see test_synthesis_wire.py)
IMAGE = 'bf-server-build'
SAVE_PATH = '/srv/save/gme.sqlite'
LIB = 'libfaketimeMT.so.1'
MASK = (1 << 64) - 1

results: list[tuple[str, bool]] = []


def check(name: str, ok: bool, detail=None) -> None:
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or detail is None else f'  -> {detail}'), flush=True)


def game_day(t: int) -> int:
    return (t - DAY_START) // 86400


def utc_day(t: int) -> int:
    return t // 86400


def rotation(day: int) -> list[str]:
    """gme::dailyTaskRotation: an xorshift Fisher-Yates seeded on the day."""
    pool = list(CODES)
    seed = (day * 6364136223846793005 + 1442695040888963407) & MASK

    def step() -> int:
        nonlocal seed
        seed ^= (seed << 13) & MASK
        seed ^= seed >> 7
        seed ^= (seed << 17) & MASK
        return seed

    for i in range(len(pool), 1, -1):
        j = step() % i
        pool[i - 1], pool[j] = pool[j], pool[i - 1]
    return pool[:PER_DAY]


def at(day: int, hms: str) -> int:
    """UTC seconds of HH:MM:SS on the calendar day `day` (days since the epoch)."""
    h, m, s = (int(x) for x in hms.split(':'))
    return day * 86400 + h * 3600 + m * 60 + s


def stamp(t: int) -> str:
    return dt.datetime.fromtimestamp(t, dt.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')


def pick_day(first: int, ok) -> int:
    day = first
    while not ok(day):
        day += 1
    return day


def turns_over(day: int) -> bool:
    """CM on the board on `day` and the day after, and the boards differ."""
    return 'CM' in rotation(day) and 'CM' in rotation(day + 1) and rotation(day) != rotation(day + 1)


class Save:
    """A scratch deploy folder plus a Docker volume holding the save itself.

    ⚠ The save is NOT on the bind mount: on Docker Desktop a `docker exec`
    reader there kept seeing the save as it was before the server's writes
    (the server's own view was right), so every DB check read stale rows.
    A named volume lives inside the VM, where both see the same file."""

    def __init__(self, folder: Path, volume: str):
        self.folder, self.volume = folder, volume


class Server:
    """One server in Docker on a scratch save, its clock started by libfaketime."""

    def __init__(self, args, save: Save, binary: Path, clock: int, name: str):
        self.args, self.save, self.binary, self.clock = args, save, binary, clock
        self.name = f'bf-taskday-{name}-{secrets.token_hex(2)}'
        self.base = f'http://127.0.0.1:{args.port}'
        self.db = Database(save.folder / 'gme.sqlite', self.name, SAVE_PATH)

    def __enter__(self):
        subprocess.run(['docker', 'rm', '-f', self.name], capture_output=True)
        cmd = ['docker', 'run', '-d', '--name', self.name,
               '-p', f'127.0.0.1:{self.args.port}:9960',
               '-v', f'{self.binary.parent}:/bf/bin:ro',
               '-v', f'{self.save.folder}:/srv/deploy',
               '-v', f'{self.save.volume}:{Path(SAVE_PATH).parent}',
               '-v', f'{self.args.assets}:/srv/deploy/game_content:ro',
               '-v', f'{self.args.faketime}:/faketime:ro',
               '-e', f'LD_PRELOAD=/faketime/{LIB}',
               '-e', f'FAKETIME=@{stamp(self.clock)}',
               '-e', 'DONT_FAKE_MONOTONIC=1',
               '-e', 'TZ=UTC',
               IMAGE, f'/bf/bin/{self.binary.name}', '/srv/deploy/config.docker.json']
        started = subprocess.run(cmd, capture_output=True, text=True)
        if started.returncode != 0:
            subprocess.run(['docker', 'rm', '-f', self.name], capture_output=True)
            raise RuntimeError(f'docker run failed: {started.stderr.strip()}')
        began = time.monotonic()
        while True:
            try:
                fetch(f'{self.base}/')
                break
            except Exception:   # refused or reset until the listener is up
                if time.monotonic() - began > self.args.boot_timeout:
                    logs = subprocess.run(['docker', 'logs', '--tail', '30', self.name],
                                          capture_output=True, text=True)
                    raise RuntimeError(f'{self.name} did not come up:\n{logs.stdout}{logs.stderr}')
                time.sleep(0.5)
        # ⚠ An LD_PRELOAD that fails to load is only a warning, and the server
        # would run on the real clock.  Its log lines carry its clock.
        logs = subprocess.run(['docker', 'logs', self.name], capture_output=True, text=True)
        first = re.search(r'^(\d{8} \d{2}:\d{2}:\d{2})', logs.stdout + logs.stderr, re.M)
        seen = first and int(dt.datetime.strptime(first.group(1), '%Y%m%d %H:%M:%S')
                             .replace(tzinfo=dt.timezone.utc).timestamp())
        if not seen or not 0 <= seen - self.clock <= 60:
            self.__exit__()
            raise RuntimeError(f'{self.name}: libfaketime did not take: first log line {first and first.group(1)},'
                               f' expected about {stamp(self.clock)}')
        print(f'  [{self.binary.parent.parent.parent.parent.name}] up at fake {stamp(self.clock)}'
              f' after {time.monotonic() - began:.0f} s', flush=True)
        return self

    def __exit__(self, *exc):
        # A graceful stop lets SQLite fold its WAL back into the save.
        subprocess.run(['docker', 'stop', '-t', '20', self.name], capture_output=True)
        subprocess.run(['docker', 'rm', '-f', self.name], capture_output=True)

    def shift(self, name: str = SHIFT_KEY):
        """A recorded day shift; None when there is no server_state (the old server)."""
        try:
            row = self.db.one('SELECT value FROM server_state WHERE name = ?', name)
        except RuntimeError as e:
            if 'no such table: server_state' in str(e):
                return None
            raise
        return None if row is None else row[0]


class Tester:
    """A player and the ways this test pokes at its Daily Tasks."""

    def __init__(self, server: Server, player=None):
        self.server = server
        self.player = player or Player(server.base, server.db)
        self.player.db = server.db
        self.player.base = server.base
        self.uid = self.player.user_id

    @property
    def db(self) -> Database:
        return self.server.db

    def now(self) -> int:
        reply = gme_call(self.server.base, *UPDATE_INFO, {'IKqx1Cn9': self.player.login})
        if wire_error(reply):
            raise RuntimeError(f'UpdateInfo refused: {wire_error(reply)}')
        return int(reply['AhSmZF07']['QxWZoA04'])

    def wait_until(self, t: int) -> None:
        deadline = time.monotonic() + max(0, t - self.now()) + 60
        while self.now() < t:
            if time.monotonic() > deadline:
                raise RuntimeError(f'the server clock never reached {stamp(t)}')
            time.sleep(1)

    def still_before(self, boundary: int, label: str) -> None:
        """Checks made before a boundary only count if they finished before it."""
        now = self.now()
        check(f'{label}: the checks above ran before {stamp(boundary)[11:16]}', now < boundary, stamp(now))

    def board(self) -> list[tuple[str, int]]:
        reply = gme_call(self.server.base, *DAILY_TASK_INFO, {'IKqx1Cn9': self.player.login})
        if wire_error(reply):
            raise RuntimeError(f'DailyTaskUserInfo refused: {wire_error(reply)}')
        return [(row['O36Qv37k'], int(row['9cKyb15U'])) for row in reply.get('k23D7d43', [])]

    def plant(self, day: int, value: int = 1) -> None:
        """Progress on every code but CM, stamped `day`: it reads back only on that day."""
        for code in CODES:
            if code == 'CM':
                continue
            self.db.rows('INSERT INTO user_daily_tasks (user_id, task_key, times_completed, utc_day, rewarded)'
                         ' VALUES (?, ?, ?, ?, 0) ON CONFLICT(user_id, task_key) DO UPDATE SET'
                         ' times_completed = excluded.times_completed, utc_day = excluded.utc_day, rewarded = 0',
                         self.uid, code, value, day)

    def craft(self, count: int) -> None:
        db, uid = self.db, self.uid
        db.rows('INSERT INTO user_town_facilities(user_id,facility_id,lv) VALUES (?,1,1)'
                ' ON CONFLICT(user_id,facility_id) DO UPDATE SET lv=1', uid)
        db.rows("INSERT INTO user_campaign_missions(user_id,mission_id,state) VALUES (?,'21',2)"
                ' ON CONFLICT(user_id,mission_id) DO UPDATE SET state=2', uid)
        db.rows('DELETE FROM user_items WHERE user_id=? AND item_id IN (10400,30000)', uid)
        db.rows('INSERT INTO user_items(user_id,item_id,item_num) VALUES (?,10400,?)', uid, 5 * count)
        db.rows('UPDATE user_info SET karma=? WHERE id=?', 500 * count, uid)
        reply = gme_call(self.server.base, *ITEM_MIX,
                         {'IKqx1Cn9': self.player.login, 'JTf2jY5o': [{'4HqhTf3a': f'{RECIPE}:{count}'}]})
        if wire_error(reply):
            raise RuntimeError(f'ItemMix refused: {wire_error(reply)}')
        made = db.one('SELECT COALESCE(SUM(item_num),0) FROM user_items WHERE user_id=? AND item_id=30000', uid)[0]
        if made != count:
            raise RuntimeError(f'ItemMix made {made}, not {count}')

    def row(self, code: str):
        """(times_completed, utc_day, rewarded) of one task's row, or None."""
        found = self.db.one('SELECT times_completed, utc_day, rewarded FROM user_daily_tasks'
                            ' WHERE user_id = ? AND task_key = ?', self.uid, code)
        return tuple(found) if found else None

    def cm_row(self):
        return self.row('CM')

    def set_row(self, code: str, times: int, day: int, rewarded: int = 0) -> None:
        self.db.rows('INSERT INTO user_daily_tasks (user_id, task_key, times_completed, utc_day, rewarded)'
                     ' VALUES (?, ?, ?, ?, ?) ON CONFLICT(user_id, task_key) DO UPDATE SET'
                     ' times_completed = excluded.times_completed, utc_day = excluded.utc_day,'
                     ' rewarded = excluded.rewarded', self.uid, code, times, day, rewarded)

    def quest(self) -> None:
        """Wins MISSION through MissionStart / MissionEnd, as the client does."""
        self.player.set(tutorial_status=12, tutorial_end_flag=1, energy=500, energy_full_ts=0,
                        fight_point=50, max_fight_point=50, level=80)
        start = self.call(MISSION_START, {
            '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        serial = start['Kz7qfSs5'][0]['k9cxD7Ba']
        log = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT', 'U8uZLA34',
                                'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9', 'EcDR3f6M', 'xFH5i4o8', 'J3EBd90b', '2z8kTtBy',
                                '32edsbP0', 'CwN87TyL')}
        log.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': '', 'Fw2dr06x': ''})
        self.call(MISSION_END, {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
                                'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}],
                                'rXvA1E5y': [log]})

    def call(self, action, body: dict) -> dict:
        reply = gme_call(self.server.base, *action, dict({'IKqx1Cn9': self.player.login}, **body))
        if wire_error(reply) is not None:
            raise RuntimeError(f'{action[0]} refused: {wire_error(reply)}')
        return reply

    def spin_row(self):
        """(spin_day, spins_used, last_spin_utc_day) of the login spin, or None."""
        found = self.db.one('SELECT spin_day, spins_used, last_spin_utc_day FROM user_daily_spin'
                            ' WHERE user_id = ?', self.uid)
        return tuple(found) if found else None

    def spin(self) -> bool:
        """Asks for the day's spin; True when one was granted (the cycle day moved on)."""
        before = self.spin_row()
        self.call(DAILY_LOGIN, {})
        return self.spin_row()[0] == (before[0] if before else 1) + 1

    def wheel_spent(self) -> int:
        """Initialize's spins-used (Drudr2w5.35JXN4Ay): below 1 makes Home open the wheel."""
        info = self.call(INITIALIZE, {})['Drudr2w5']
        info = info[0] if isinstance(info, list) else info
        return int(info['35JXN4Ay'])

    def login(self) -> tuple[int, bool]:
        """UserInfo, as at launch: the login calendar's (cell, first_for_the_day)."""
        reply = self.call(USER_INFO, {'6FrKacq7': [{'Kn51uR4Y': 'rollover'}]})
        info = reply['3da6bd0a']
        info = info[0] if isinstance(info, list) else info
        return int(info['ad6i23pO']), bool(int(info['4tswNoV9']))

    def gift_days(self) -> list[str]:
        """The day keys of every gift the simulated friend has sent this player."""
        return [r[0] for r in self.db.rows('SELECT day FROM user_gifts WHERE user_id = ? AND from_user_id = ?'
                                           ' ORDER BY id', self.uid, SIMULATED_SENDER)]

    def campaign_day(self) -> str:
        return self.db.one('SELECT last_day FROM user_login_campaign WHERE user_id = ?', self.uid)[0]

    def qe_areas(self) -> str:
        reply = self.call(DAILY_TASK_INFO, {})
        return next((r.get('a3011F8b') for r in reply.get('k23D7d43', []) if r['O36Qv37k'] == 'QE'), None)

    def points(self) -> int:
        return self.db.one('SELECT avail_brave_points FROM user_info WHERE id = ?', self.uid)[0]


def key(day: int) -> str:
    """gme::dayKey: a day number as the "YYYY-MM-DD" date that numbers it."""
    return dt.datetime.fromtimestamp(day * 86400, dt.timezone.utc).strftime('%Y-%m-%d')


def expected(day: int, cm: int, planted: bool = True) -> list[tuple[str, int]]:
    return [(code, cm if code == 'CM' else int(planted)) for code in rotation(day)]


def fresh_save(args, name: str) -> Save:
    folder = args.workdir / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    deploy = args.root / 'deploy'
    for item in ('mst', 'archive', 'system'):
        subprocess.run(['cp', '-Rc', str(deploy / item), str(folder / item)], check=True)
    config = (deploy / 'config.docker.json').read_text()
    if config.count('"./gme.sqlite"') != 1:
        raise SystemExit('deploy/config.docker.json: expected one "./gme.sqlite" to point at the volume')
    (folder / 'config.docker.json').write_text(config.replace('"./gme.sqlite"', f'"{SAVE_PATH}"'))
    (folder / 'game_content').mkdir()
    (folder / 'log').mkdir()
    volume = f'bf-taskday-test-{name}'
    subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    subprocess.run(['docker', 'volume', 'create', volume], check=True, capture_output=True)
    args.volumes.append(volume)
    return Save(folder, volume)


def lead(args) -> int:
    """How long before a boundary to start a server: boot, setup, then a margin."""
    return args.boot + 60


def same(name: str, got, want) -> None:
    check(name, got == want, (got, want))


def scenario_a(args, first: int) -> None:
    print('\nA. fresh save, new server', flush=True)
    save = fresh_save(args, 'a')
    # Started before 08:00, so the migration records shift 1 and the label of
    # the morning is the calendar day: X before 08:00, X + 1 after.
    x = pick_day(first, turns_over)
    boundary = at(x, '08:00:00')
    with Server(args, save, args.new_bin, boundary - lead(args), 'a1') as server:
        t = Tester(server)
        same('A1: the migrations recorded shift 1 for tasks, spin and gifts (before 08:00 UTC)',
             (server.shift(), server.shift(SPIN_KEY), server.shift(GIFT_KEY)), (1, 1, 1))
        t.plant(x)
        t.craft(2)
        same("A1: before 08:00 the board is day X's, with its progress", t.board(), expected(x, 2))
        same('A1: the craft is stamped day X', t.cm_row(), (2, x, 0))
        same('A1: the first spin of day X is granted and stamped X', (t.spin(), t.spin_row()[2]), (True, x))
        same('A1: a second spin on day X is refused', t.spin(), False)
        same('A1: the login calendar opens cell 1 with its animation', t.login(), (1, True))
        same('A1: a second login on day X stays on cell 1, quietly', t.login(), (1, False))
        same("A1: the simulated friend's gift is keyed day X", t.gift_days(), [key(x)])
        t.still_before(boundary, 'A1')
        t.wait_until(boundary + 5)
        same("A1: after 08:00 the board is day X+1's, all fresh", t.board(), expected(x + 1, 0, planted=False))
        t.craft(3)
        same('A1: a craft after 08:00 starts a new count (3, not 5)', t.cm_row(), (3, x + 1, 0))
        same('A1: ...and shows on the board', dict(t.board()).get('CM'), 3)
        same('A1: after 08:00 Initialize offers the wheel again', t.wheel_spent(), 0)
        same('A1: after 08:00 a new spin is granted, stamped X+1', (t.spin(), t.spin_row()[2]), (True, x + 1))
        same('A1: ...and Initialize then reports it spent', t.wheel_spent(), 1)
        same('A1: after 08:00 the calendar moves to cell 2', t.login(), (2, True))
        same('A1: ...and a new gift arrives, keyed X+1', t.gift_days(), [key(x), key(x + 1)])
        player = t.player
        # Each clock keeps its own shift (they can move at different deploys):
        # give the spin 0 for A2 and check it follows that, not the others'.
        # A running server read its shifts at boot, so this only takes at A2.
        t.db.rows('UPDATE server_state SET value = 0 WHERE name = ?', SPIN_KEY)

    # 23:59 -> 00:00 a few days later.  Shift 1, so after 08:00 on Y the label
    # is Y + 1 and it stays Y + 1 past midnight.
    y = pick_day(x + 3, lambda d: 'CM' in rotation(d + 1))
    midnight = at(y + 1, '00:00:00')
    with Server(args, save, args.new_bin, midnight - lead(args), 'a2') as server:
        t = Tester(server, player)
        same('A2: the shifts are read back, not recomputed: tasks 1, spin 0 (set for this check), gifts 1',
             (server.shift(), server.shift(SPIN_KEY), server.shift(GIFT_KEY)), (1, 0, 1))
        t.plant(y + 1)
        t.craft(2)
        same('A2: before midnight the board is the day that began at 08:00', t.board(), expected(y + 1, 2))
        same('A2: before midnight the spin is granted on its own clock, stamped Y (shift 0)',
             (t.spin(), t.spin_row()[2]), (True, y))
        same('A2: before midnight the calendar moves to cell 3', t.login(), (3, True))
        gifts = t.gift_days()
        same('A2: ...with a gift keyed Y+1', gifts[-1:], [key(y + 1)])
        points = t.points()
        t.still_before(midnight, 'A2')
        t.wait_until(midnight + 5)
        same('A2: after midnight the board has NOT changed', t.board(), expected(y + 1, 2))
        same('A2: after midnight Initialize still reports the wheel spent', t.wheel_spent(), 1)
        same('A2: after midnight there is no second spin', t.spin(), False)
        same('A2: after midnight the calendar stays on cell 3, quietly', t.login(), (3, False))
        same('A2: after midnight no second gift', t.gift_days(), gifts)
        t.craft(3)
        same('A2: a craft after midnight adds to the same day (5)', t.cm_row(), (5, y + 1, 1))
        same(f'A2: finishing Craftsman paid {CM_POINTS} BP', t.points(), points + CM_POINTS)
        t.craft(1)
        same('A2: a sixth craft pays nothing more', (t.points(), t.cm_row()[0]), (points + CM_POINTS, 6))


def deploy_day(args, name: str, day: int, old_at: str, cut: str):
    """Old server, then the new one, on one save, on calendar day `day`."""
    save = fresh_save(args, name)
    tag = name.upper()
    with Server(args, save, args.old_bin, at(day, old_at), f'{name}1-old') as server:
        t = Tester(server)
        same(f'{tag}1: the old server keeps no shift', server.shift(), None)
        t.plant(day)
        t.craft(2)
        same(f"{tag}1: the old server's board is this test's model of day {cut}", t.board(), expected(day, 2))
        same(f'{tag}1: the old server stamped the craft day {cut}', t.cm_row(), (2, day, 0))
        same(f'{tag}1: the old server grants the day\'s spin, stamped {cut}', (t.spin(), t.spin_row()[2]), (True, day))
        same(f'{tag}1: the old server opens calendar cell 1', t.login(), (1, True))
        same(f"{tag}1: the old server keys the gift by the UTC date", t.gift_days(), [key(day)])
        player = t.player
    return save, player


def scenario_b(args, first: int) -> None:
    print('\nB. deploy between 00:00 and 08:00 UTC', flush=True)
    # B4 also clears a quest, so Quest Explorer has to be on day D+1's board.
    d = pick_day(first, lambda day: turns_over(day) and 'QE' in rotation(day + 1))
    save, player = deploy_day(args, 'b', d, '03:00:00', 'D')
    with Server(args, save, args.new_bin, at(d, '03:05:00'), 'b2-new') as server:
        t = Tester(server, player)
        same('B2: the migrations recorded shift 1 for tasks, spin and gifts',
             (server.shift(), server.shift(SPIN_KEY), server.shift(GIFT_KEY)), (1, 1, 1))
        same("B2: after the deploy the board is still day D's, progress kept", t.board(), expected(d, 2))
        same('B2: no second spin after the deploy (still day D)', t.spin(), False)
        same('B2: the calendar stays on cell 1, quietly', t.login(), (1, False))
        same('B2: no second gift (the key is still D)', t.gift_days(), [key(d)])
        t.craft(2)
        same('B2: crafts keep adding to day D (4)', t.cm_row(), (4, d, 0))
    boundary = at(d, '08:00:00')
    with Server(args, save, args.new_bin, boundary - lead(args), 'b3-new') as server:
        t = Tester(server, player)
        same('B3: before 08:00 it is still day D', t.board(), expected(d, 4))
        same('B3: before 08:00 still no second spin', t.spin(), False)
        t.still_before(boundary, 'B3')
        t.wait_until(boundary + 5)
        same('B3: at 08:00 day D ends and day D+1 starts fresh', t.board(), expected(d + 1, 0, planted=False))
        t.craft(1)
        same('B3: a craft after 08:00 starts a new count', t.cm_row(), (1, d + 1, 0))
        same('B3: at 08:00 a new spin, stamped D+1', (t.spin(), t.spin_row()[2]), (True, d + 1))
        same('B3: at 08:00 the calendar moves to cell 2', t.login(), (2, True))
        same('B3: ...and a new gift, keyed D+1', t.gift_days(), [key(d), key(d + 1)])
    # 12:00 on D: the task day is D+1 while midnight UTC still says D, so a
    # count stamped D here would mean something still used the old day.
    with Server(args, save, args.new_bin, at(d, '12:00:00'), 'b4-new') as server:
        t = Tester(server, player)
        same('B4: a restart at 12:00 keeps shift 1', server.shift(), 1)
        same('B4: ...and stays on day D+1', t.board(), expected(d + 1, 1, planted=False))
        t.set_row('QE', 1, d + 1)
        t.quest()
        check('B4: a won quest counts Quest Explorer on day D+1 through MissionEnd (2)',
              t.row('QE') == (2, d + 1, 0), (t.row('QE'), 'tile areas', t.qe_areas()))
        # A row stamped later than today (a rollback past the migration, or the
        # clock stepping back) is left alone rather than rewound and paid again.
        t.set_row('CM', CM_TARGET, d + 2, 1)
        points = t.points()
        t.craft(1)
        same('B4: a row stamped a later day is not rewound by a craft', t.cm_row(), (CM_TARGET, d + 2, 1))
        same('B4: ...and pays nothing', t.points(), points)
        same('B4: no second spin at 12:00 (still day D+1)', t.spin(), False)
        same('B4: the calendar stays on cell 2 at 12:00', t.login(), (2, False))
        # The same "never rewind" rule for the spin, the calendar and the gift.
        t.db.rows('UPDATE user_daily_spin SET last_spin_utc_day = ?, spins_used = 1 WHERE user_id = ?', d + 2, t.uid)
        same('B4: a spin row stamped a later day gives no spin', t.spin(), False)
        same('B4: ...and keeps its day', t.spin_row()[2], d + 2)
        t.db.rows('UPDATE user_daily_spin SET spins_used = 0 WHERE user_id = ?', t.uid)
        same('B4: a later-dated row with its spin unused gives that one spin', t.spin(), True)
        same('B4: ...stamped with the later day, not rewound to today', t.spin_row()[2], d + 2)
        same('B4: ...so no further spin today', t.spin(), False)
        t.db.rows('UPDATE user_login_campaign SET last_day = ? WHERE user_id = ?', key(d + 2), t.uid)
        same('B4: a calendar stored on a later day does not move', t.login(), (2, False))
        same('B4: ...and keeps its day', t.campaign_day(), key(d + 2))
        t.db.rows('DELETE FROM user_gifts WHERE user_id = ? AND from_user_id = ? AND day = ?',
                  t.uid, SIMULATED_SENDER, key(d + 1))
        t.db.rows('INSERT INTO user_gifts (user_id, from_user_id, gift_id, day, received) VALUES (?, ?, 1, ?, 1)',
                  t.uid, SIMULATED_SENDER, key(d + 2))
        t.login()
        same("B4: a gift dated a later day stops today's from being minted", t.gift_days(), [key(d), key(d + 2)])


def scenario_c(args, first: int) -> None:
    print('\nC. deploy between 08:00 and 24:00 UTC', flush=True)
    e = pick_day(first, turns_over)
    save, player = deploy_day(args, 'c', e, '14:00:00', 'E')
    with Server(args, save, args.new_bin, at(e, '14:05:00'), 'c2-new') as server:
        t = Tester(server, player)
        same('C2: the migrations recorded shift 0 for tasks, spin and gifts',
             (server.shift(), server.shift(SPIN_KEY), server.shift(GIFT_KEY)), (0, 0, 0))
        same("C2: after the deploy the board is still day E's, progress kept", t.board(), expected(e, 2))
        same('C2: no second spin after the deploy (still day E)', t.spin(), False)
        same('C2: the calendar stays on cell 1, quietly', t.login(), (1, False))
        same('C2: no second gift (the key is still E)', t.gift_days(), [key(e)])
        t.craft(2)
        same('C2: crafts keep adding to day E (4)', t.cm_row(), (4, e, 0))
    midnight = at(e + 1, '00:00:00')
    with Server(args, save, args.new_bin, midnight - lead(args), 'c3-new') as server:
        t = Tester(server, player)
        same('C3: before midnight it is day E', t.board(), expected(e, 4))
        points = t.points()
        t.still_before(midnight, 'C3')
        t.wait_until(midnight + 5)
        same('C3: midnight does not roll day E over (the old server would have)', t.board(), expected(e, 4))
        same('C3: Initialize does not offer the wheel at midnight', t.wheel_spent(), 1)
        same('C3: no new spin at midnight', t.spin(), False)
        same('C3: the calendar does not move at midnight', t.login(), (1, False))
        same('C3: no new gift at midnight', t.gift_days(), [key(e)])
        t.craft(1)
        same('C3: the next craft finishes Craftsman on day E (5) and pays once',
             (t.cm_row(), t.points()), ((5, e, 1), points + CM_POINTS))
    boundary = at(e + 1, '08:00:00')
    with Server(args, save, args.new_bin, boundary - lead(args), 'c4-new') as server:
        t = Tester(server, player)
        same('C4: until 08:00 the next morning it is still day E', t.board(), expected(e, 5))
        t.still_before(boundary, 'C4')
        t.wait_until(boundary + 5)
        same('C4: at 08:00 day E ends and day E+1 starts fresh', t.board(), expected(e + 1, 0, planted=False))
        t.craft(1)
        same('C4: a craft after 08:00 starts a new count', t.cm_row(), (1, e + 1, 0))
        same('C4: at 08:00 Initialize offers the wheel again', t.wheel_spent(), 0)
        same('C4: at 08:00 a new spin, stamped E+1', (t.spin(), t.spin_row()[2]), (True, e + 1))
        same('C4: at 08:00 the calendar moves to cell 2', t.login(), (2, True))
        same('C4: ...and a new gift, keyed E+1', t.gift_days(), [key(e), key(e + 1)])


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--new-bin', type=Path, default=root / 'out/build-linux-arm64/standalone_frontend/gimuserverw')
    ap.add_argument('--old-bin', type=Path, help='a server built before this change (scenarios B and C)')
    ap.add_argument('--faketime', type=Path, required=True, help=f'folder holding {LIB}')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    ap.add_argument('--port', type=int, default=0, help='host port for the servers (default: a free one)')
    ap.add_argument('--boot', type=int, default=10, help='seconds a server takes to start (sets the lead)')
    ap.add_argument('--boot-timeout', type=int, default=240)
    ap.add_argument('--only', default='abc', help='scenarios to run, e.g. "a" or "bc"')
    ap.add_argument('--first-day', default='2026-11-02', help='earliest calendar day the fake clocks use')
    args = ap.parse_args()
    args.root = root
    args.volumes = []
    if not args.port:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    args.workdir = root / 'out' / 'daily-rollover-test'
    args.new_bin, args.faketime = args.new_bin.resolve(), args.faketime.resolve()
    if not (args.faketime / LIB).is_file():
        raise SystemExit(f'{args.faketime / LIB} not found')
    if not args.new_bin.is_file():
        raise SystemExit(f'{args.new_bin} not found')
    if args.old_bin:
        args.old_bin = args.old_bin.resolve()
    elif set(args.only) & set('bc'):
        # The deploy scenarios are the ones a "+1 always" shortcut would fail.
        raise SystemExit('scenarios B and C need --old-bin (a server built before this change); --only a skips them')
    first = utc_day(int(dt.datetime.fromisoformat(args.first_day).replace(tzinfo=dt.timezone.utc).timestamp()))

    # The client's Today's Tasks said "Time till refresh: 21:26" at 10:33 UTC.
    t0 = at(first, '10:33:00')
    left = (game_day(t0) + 1) * 86400 + DAY_START - t0
    check(f'model: at 10:33 UTC the next rollover is {left // 3600}:{left % 3600 // 60:02d} away (client: 21:26)',
          left // 60 == 21 * 60 + 27)

    # Each scenario gets its own stretch of calendar so the days never overlap.
    if 'a' in args.only:
        scenario_a(args, first)
    if 'b' in args.only:
        scenario_b(args, first + 10)
    if 'c' in args.only:
        scenario_c(args, first + 20)

    failed = [name for name, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} passed')
    if not failed:
        for volume in args.volumes:
            subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
