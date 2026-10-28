"""Wire test: the Forge's global capture and boss-drop numbers in server/tuning.json (ticket #164).

python3 scripts/test_global_tuning_wire.py prepare SAVE            # while the server is stopped
python3 scripts/test_global_tuning_wire.py run --token FILE --balance DIR --container NAME [--url URL]

`prepare` is test_boss_drops_wire's.  `run` talks to a server built from this branch whose balance
root is DIR on this machine; it writes DIR/server/tuning.json between starts (the server re-reads
it on the next MissionStart) and removes it at the end.  Every start ends as a clear.

Quest 540 Mischief and Malice, from MissionStart's drop_info (LE6JkUp7): R = Rantoul (20081), a
regular enemy with no capture of its own (the floor gives it one); Mimir (50040) the base form its
boss line drops (Great Sage Mimir, a 3-star line: test_boss_drops_wire).
  default   no file: R drops (the 40% floor), Mimir sometimes;
  floor     capture_floor_pct 100: R on every start; 0: never;
  boss      boss_drop_pct_r3 100: Mimir on every start; 0: never;
  rejected  capture_floor_pct 101 is refused whole: the last good values stay;
  gone      the file removed: R drops again.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, 'tools')
from test_boss_drops_wire import MIMIR, drops, find_drop_info, prepare  # noqa: E402
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402

QUEST, RANTOUL, RUNS = 540, 20081, 30


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('save', type=Path)
    r = sub.add_parser('run')
    r.add_argument('--token', type=Path, required=True)
    r.add_argument('--balance', type=Path, required=True)
    r.add_argument('--container', required=True)
    r.add_argument('--url', default='http://127.0.0.1:19982')
    args = ap.parse_args()
    if args.cmd == 'prepare':
        return prepare(args.save)

    login, user, token, _ = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    tuning = args.balance / 'server' / 'tuning.json'
    tuning.parent.mkdir(parents=True, exist_ok=True)
    fails = []

    def check(label, ok, detail=''):
        print(f'{"PASS" if ok else "FAIL"} {label}{": " + str(detail) if detail != "" else ""}')
        if not ok:
            fails.append(label)

    def tune(body):
        if body is None:
            tuning.unlink(missing_ok=True)
        else:
            tuning.write_text(body if isinstance(body, str) else json.dumps(body))
        time.sleep(2)            # Docker Desktop's file sharing shows a changed file a moment late

    def every(unit, n=RUNS):
        seen = 0
        for _ in range(n):
            started = call(url, user, login, token, *MISSION_START, start_body(QUEST))
            if 'b5PH6mZa' in started:
                raise SystemExit(f'MissionStart {QUEST} refused: {started["b5PH6mZa"]}')
            seen += unit in [u for _, _, u in drops(find_drop_info(started))]
            ended = call(url, user, login, token, *MISSION_END, end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba'])))
            if 'b5PH6mZa' in ended:
                raise SystemExit(f'MissionEnd {QUEST} refused: {ended["b5PH6mZa"]}')
        return seen

    try:
        tune(None)
        n = every(RANTOUL)
        check('default: R drops at the 40% floor', 0 < n < RUNS, n)
        tune({'capture_floor_pct': 100})
        check('floor 100: R on every start', every(RANTOUL) == RUNS)
        tune({'capture_floor_pct': 0})
        check('floor 0: R never', every(RANTOUL) == 0)
        tune({'boss_drop_pct_r3': 100})
        check('boss r3 100: Mimir on every start', every(MIMIR) == RUNS)
        tune({'boss_drop_pct_r3': 0})
        check('boss r3 0: Mimir never', every(MIMIR) == 0)
        tune({'capture_floor_pct': 100})
        every(RANTOUL, 2)            # the server takes a file at the next MissionStart
        tune({'capture_floor_pct': 101})
        check('rejected: the last good floor stays', every(RANTOUL, 10) == 10)
        log = subprocess.run(['docker', 'logs', args.container], capture_output=True, text=True)
        check('rejected: the log says why', 'capture_floor_pct must be a whole number' in log.stdout + log.stderr)
        tune(None)
        n = every(RANTOUL)
        check('gone: R drops at the floor again', 0 < n < RUNS, n)
    finally:
        tune(None)
    print(f'\n{len(fails)} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
