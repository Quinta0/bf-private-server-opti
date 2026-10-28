"""Wire test: a daily quest can be won once per game day (ticket #100).

Daily dungeons (DungeonMst type 100) and the featured events' "daily" payout
missions (Daily Tan) refuse MissionStart after today's win, with a message and
Home, nothing spent; the next game day (08:00 UTC) opens them again.  The
server runs in Docker with a libfaketime clock, as in test_event_rotation_wire.py.

  python3 -I scripts/test_daily_once_wire.py --faketime "$DIR/usr/lib/aarch64-linux-gnu/faketime"
"""
import argparse
import datetime as dt
import socket
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_rollover_wire import LIB, Server, at, check, fresh_save, results  # noqa: E402
from test_event_rotation_wire import MISSION_START, Tester  # noqa: E402
from test_fair_summon import wire_error  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DAILY_TAN = 20001010            # its dungeon has no type: daily through the event rotation
AGAINST_THE_DARKNESS = 20000909  # Defend the Gates, a type-100 dungeon
GATE_SENTRY = 20000906           # a farm mission: no limit
REFUSED = 'already cleared this daily quest today'


def start_refused(t: Tester, mission: int) -> str:
    reply = t.call(MISSION_START, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}],
        '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0',
                      'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}]})
    return wire_error(reply) or ''


def energy(t: Tester) -> int:
    return int(t.server.db.one('SELECT energy FROM user_info WHERE id = ?', t.player.user_id)[0])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bin', type=Path, default=ROOT / 'out/build-linux-arm64/standalone_frontend/gimuserverw')
    ap.add_argument('--faketime', type=Path, required=True, help=f'folder holding {LIB}')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--boot-timeout', type=int, default=240)
    ap.add_argument('--day', default='2026-10-27', help='a game day to test (its 08:00 UTC is crossed)')
    args = ap.parse_args()
    args.root, args.volumes, args.workdir = ROOT, [], ROOT / 'out' / 'daily-once-test'
    args.bin, args.faketime = args.bin.resolve(), args.faketime.resolve()
    if not args.port:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    day = int(dt.datetime.fromisoformat(args.day).replace(tzinfo=dt.timezone.utc).timestamp()) // 86400
    turn = at(day, '08:00:00')
    save = fresh_save(args, 'daily')

    with Server(args, save, args.bin, turn - 150, 'once') as server:
        t = Tester(server)
        t.player.set(tutorial_status=12, tutorial_end_flag=1, energy=9999)
        for mission, label in ((DAILY_TAN, 'Daily Tan'), (AGAINST_THE_DARKNESS, 'Against the Darkness')):
            t.clear(mission)
            before = energy(t)
            error = start_refused(t, mission)
            check(f'{label}: a second start the same day is refused with the reason, nothing spent',
                  REFUSED in error and energy(t) == before, (error, before, energy(t)))
        t.clear(GATE_SENTRY)
        t.clear(GATE_SENTRY)
        check('Gate Sentry: a farm mission replays freely', True)
        check('the checks above ran before 08:00', t.now() < turn, t.now() - turn)

        t.wait_until(turn + 2)
        for mission, label in ((DAILY_TAN, 'Daily Tan'), (AGAINST_THE_DARKNESS, 'Against the Darkness')):
            error = start_refused(t, mission)
            check(f'{label}: after 08:00 UTC it starts again', not error, error)

    failed = [name for name, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} passed')
    if not failed:
        for volume in args.volumes:
            subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
