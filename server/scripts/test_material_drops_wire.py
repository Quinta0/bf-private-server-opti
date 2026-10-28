"""Wire test: material drops are served at MissionStart and paid at MissionEnd.

python3 -I scripts/test_material_drops_wire.py --src WORKTREE --url http://127.0.0.1:PORT --token FILE [--runs N]

FILE holds [gumi id, user id, token] from tools/test_vortex_tiles.borrow_test_account, written
BEFORE the server started (a host write to a running save is not seen).  For each mission it
starts N battles, counts the item chests in drop_info (LE6JkUp7: "monster/1/4:weight:item:amount"),
and ends each one reporting exactly those items, the way the client does (4T0Q2Bh5).  Stop the
server, then rerun with --check-items to compare user_items with what was reported.
"""
import argparse
import collections
import json
import re
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools'))
sys.path.insert(0, 'tools')
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402

ITEM = re.compile(r'\d+/1/4:\d+:(\d+):(\d+)')
MISSIONS = (10, 85, 100310, 1101)


def find(obj, key):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == key:
                yield v
            yield from find(v, key)
    elif isinstance(obj, list):
        for v in obj:
            yield from find(v, key)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9984')
    ap.add_argument('--token', type=Path, required=True)
    ap.add_argument('--runs', type=int, default=15)
    ap.add_argument('--check-items', type=Path, help='the reported.json of a finished run: compare with user_items')
    args = ap.parse_args()
    login, user, token = json.loads(args.token.read_text())
    report = args.token.with_name('reported.json')
    if args.check_items:
        want = {int(k): v for k, v in json.loads(args.check_items.read_text())['items'].items()}
        before = {int(k): v for k, v in json.loads(args.check_items.read_text())['before'].items()}
        db = sqlite3.connect(args.src / 'deploy/gme.sqlite')
        have = dict(db.execute('SELECT item_id, item_num FROM user_items WHERE user_id = ?', (user,)).fetchall())
        bad = {i: (before.get(i, 0), n, have.get(i, 0)) for i, n in want.items() if have.get(i, 0) != before.get(i, 0) + n}
        print(f'{len(want)} item kind(s) reported; mismatched: {bad or "none"}')
        return 1 if bad else 0

    db = sqlite3.connect(args.src / 'deploy/gme.sqlite')
    before = dict(db.execute('SELECT item_id, item_num FROM user_items WHERE user_id = ?', (user,)).fetchall())
    db.close()
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    reported, fails = collections.Counter(), 0
    for mid in MISSIONS:
        chests = 0
        for _ in range(args.runs):
            started = call(url, user, login, token, *MISSION_START, start_body(mid))
            drop_info = ''.join(str(v) for v in find(started, 'LE6JkUp7'))
            got = [(int(i), int(n)) for i, n in ITEM.findall(drop_info)]
            chests += len(got)
            body = end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba']))
            body['rXvA1E5y'][0]['4T0Q2Bh5'] = ','.join(f'{i}:{n}' for i, n in got)
            ended = call(url, user, login, token, *MISSION_END, body)
            if 'b5PH6mZa' in ended:
                fails += 1
                print(f'FAIL {mid}: {ended["b5PH6mZa"]}')
            for i, n in got:
                reported[i] += n
        print(f'{mid}: {chests} item chest(s) in {args.runs} run(s)')
    report.write_text(json.dumps({'items': reported, 'before': before}))
    print(f'reported {sum(reported.values())} item(s) of {len(reported)} kind(s); {fails} failure(s); now stop the server and run --check-items {report}')
    return 1 if fails or not reported else 0


if __name__ == '__main__':
    sys.exit(main())
