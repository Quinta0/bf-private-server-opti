#!/usr/bin/env python3
"""Wire test: missions added from captured battles start, serve their waves and finish.

    python3 -I tools/test_captured_missions.py --src wt/vortex --url http://127.0.0.1:9974 \
        [--report re/work/vortex/captured_report.json] [--limit 40]

For every mission in the converter's report: MissionStart must issue a battle
serial and send one wave per archived stage with the archived enemy count and
HP, then MissionEnd (cleared) must be accepted.  Runs as the highest-level
account of the --src save (a TEST copy: its session token is rewritten).
"""
import argparse
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from test_vortex_tiles import borrow_test_account, call  # noqa: E402

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')


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


def mode_of(mission_id: int, record: dict) -> str:
    """Coarse mode bucket by id range, for spreading a sample."""
    for floor, mode in ((83_000_000, 'event'), (8_000_000, 'frontier_gate'), (5_000_000, 'grand_quest'),
                        (2_000_000, 'trials'), (1_000_000, 'frontier_hunter'), (800_000, 'event'),
                        (100_000, 'vortex')):
        if mission_id >= floor:
            return mode
    return 'story'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9974')
    ap.add_argument('--report', type=Path, default=Path('re/work/vortex/captured_report.json'))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--designed', type=int, default=0, metavar='N',
                    help='instead test N designed or wiki-rebuilt missions per mode (deploy/archive/mission_sources.json)')
    args = ap.parse_args()

    archive = {r['id']: r for r in json.loads((args.src / 'deploy/archive/mission.json').read_text())}
    if args.designed:
        sources = json.loads((args.src / 'deploy/archive/mission_sources.json').read_text())
        by_mode = {}
        for mid in sorted(int(k) for k, v in sources.items() if v.get('method', '').startswith(('designed', 'regenerated'))):
            by_mode.setdefault(mode_of(mid, archive[mid]), []).append(mid)
        # Spread the sample over each mode's id range: first, last and evenly between.
        missions = [ids[round(i * (len(ids) - 1) / max(args.designed - 1, 1))]
                    for ids in by_mode.values() for i in range(min(args.designed, len(ids)))]
        missions = sorted(set(missions))
    else:
        missions = [a['mission'] for a in json.loads(args.report.read_text())['added']]
    if args.limit:
        missions = missions[:args.limit]
    login, user, token = borrow_test_account(args.src / 'deploy')
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    # Story quests lock behind their prerequisites; this tests battle DATA, so
    # the TEST account gets every story mission marked cleared.
    with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:
        db.executemany('INSERT OR IGNORE INTO user_campaign_missions (user_id, mission_id, state, clear_count)'
                       ' VALUES (?, ?, 2, 1)', [(user, str(mid)) for mid in archive if 0 < mid < 100000])

    fails = 0
    for mid in missions:
        record = archive[mid]
        with sqlite3.connect(args.src / 'deploy/gme.sqlite', timeout=30) as db:  # a TEST save
            db.execute('UPDATE user_info SET energy=999 WHERE id=?', (user,))
        try:
            started = call(url, user, login, token, *MISSION_START, start_body(mid))
            serial = int(started['Kz7qfSs5'][0]['k9cxD7Ba'])
            text = json.dumps(started)
            hps = [str(m['hp']) for s in record['stages'] for m in s['battle_monsters']]
            missing_hp = [h for h in set(hps) if f'"{h}"' not in text]
            ended = call(url, user, login, token, *MISSION_END, end_body(serial))
            ok = not missing_hp and 'b5PH6mZa' not in ended
            detail = f'serial {serial}, {len(record["stages"])} wave(s), {len(hps)} enemies' + (
                f', HP not served: {missing_hp[:4]}' if missing_hp else '')
        except SystemExit as e:
            ok, detail = False, str(e)[:160]
        except Exception as e:  # noqa: BLE001 - report every mission, then fail
            ok, detail = False, f'{type(e).__name__}: {e}'[:160]
        fails += not ok
        print(f'{"PASS" if ok else "FAIL"} {mid} {record["name"]}: {detail}')
    print(f'{fails} failure(s) of {len(missions)}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
