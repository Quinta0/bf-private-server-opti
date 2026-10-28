"""Wire test: rolling slots give different enemies and catches per MissionStart.

python3 -I scripts/test_mission_rolls_wire.py --token FILE [--url URL] [--runs N] MISSION...

FILE holds [gumi id, user id, token] (tools/test_vortex_tiles.borrow_test_account, written
before the server started).  Each start is ended as a clear, so the next one is allowed.
Prints, per mission, how many distinct lineups and caught units the runs served and fails
when a mission with alternatives in deploy/archive/mission_variants.json served one lineup.
"""
import argparse
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, 'tools')
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('missions', type=int, nargs='+')
    ap.add_argument('--token', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9986')
    ap.add_argument('--runs', type=int, default=25)
    args = ap.parse_args()
    login, user, token = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    rolling = set(json.loads(Path('deploy/archive/mission_variants.json').read_text()))
    fails = 0
    for mid in args.missions:
        lineups, catches, names = collections.Counter(), collections.Counter(), collections.Counter()
        for _ in range(args.runs):
            started = call(url, user, login, token, *MISSION_START, start_body(mid))
            named = {r['o49dYfpH']: r['3evIn0zZ'] for r in started.get('U0v5IeJo') or []}
            placed = started.get('75t0sx9z') or []
            lineups[tuple((r['Qzhp8B40'], r['hZtF1s8B'], r['o49dYfpH']) for r in placed)] += 1
            names.update(named[r['o49dYfpH']] for r in placed if r['o49dYfpH'] in named)
            catches.update(r['hw3L0uVj'].split(':')[1] for r in placed if r['hw3L0uVj'].split(':')[1] != '0')
            ended = call(url, user, login, token, *MISSION_END, end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba'])))
            if 'b5PH6mZa' in ended:
                fails += 1
                print(f'FAIL {mid}: {ended["b5PH6mZa"]}')
        ok = len(lineups) > 1 or str(mid) not in rolling
        fails += not ok
        print(f'{"PASS" if ok else "FAIL"} {mid}: {len(lineups)} lineups in {args.runs} runs; '
              f'enemies {dict(names.most_common(8))}; caught units served {dict(catches.most_common(8))}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
