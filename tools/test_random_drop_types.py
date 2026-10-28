"""Wire test for random unit types on quest drops (server branch
fix/random-drop-types, ticket #21).

Against a running LOCAL server: mission 10's monsters all carry unit_drop_type 5
(Oracle) in the archive.  Over 30 MissionStarts the drops the server announces
(idx:0:unit:level:type, which the client shows and echoes at MissionEnd) must
roll types like a summon: several of Lord/Anima/Breaker/Guardian/Oracle (1-5),
never 0 and never Rex (6).  --baseline expects the old behaviour: only 5.

Usage: python3 -I tools/test_random_drop_types.py --src wt/dblock --url http://127.0.0.1:9979
Run it against a test server, never a save with real players.
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION = 10
DROP_UNITS = {10030, 20030, 30030, 40030, 60030}
DROP = re.compile(r'(?<![\d:])\d+:0:(\d{5}):(\d+):(\d)(?![\d])')
CHAPTER_DONE = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9979')
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    ap.add_argument('--baseline', action='store_true')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    db = Database(args.src / 'deploy' / 'gme.sqlite', container_on_port(args.url), args.container_db)
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, energy=500, energy_full_ts=0)

    types = Counter()
    for _ in range(30):
        reply = gme_call(args.url, *MISSION_START, {
            'IKqx1Cn9': player.login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        if wire_error(reply) is not None:
            raise RuntimeError(f'MissionStart refused: {wire_error(reply)}')
        for unit, _level, kind in DROP.findall(json.dumps(reply)):
            if int(unit) in DROP_UNITS:
                types[int(kind)] += 1

    check('the starts announced unit drops', sum(types.values()) >= 20, dict(types))
    if args.baseline:
        check('baseline: every drop is type 5 (Oracle)', set(types) == {5}, dict(types))
    else:
        check('drops roll several types (at least 3 of 1-5)', len(set(types) & {1, 2, 3, 4, 5}) >= 3, dict(types))
        check('never type 0 or Rex (6)', not (set(types) & {0, 6}), dict(types))
        check('Oracle is no longer the only type', types[5] < sum(types.values()), dict(types))
    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
