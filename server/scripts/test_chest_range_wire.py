"""Wire test: ranged chest prizes, amount .. amount_max (the balancers' "1-3 Rose Bolt").

python3 scripts/test_capture_wire.py prepare SAVE           # once, while the server is stopped
python3 scripts/test_chest_range_wire.py --token FILE --balance DIR [--url URL]

Writes DIR/server/missions/540.json: quest 540 with every enemy's chest at 100% holding item
10903 at amount 1, amount_max 3.  Over 10 starts every chest in MissionStart's drop_info
(LE6JkUp7, "idx/1/4:weight:item:count") must hold 1..3, not always the same count, and the
MissionEnds that report them ("4T0Q2Bh5" item:count, as the client does) must raise the item
by exactly their sum, read from each MissionEnd reply's warehouse stack (wgV86x1q).  Needs
pycryptodome (.venv/bin/python) and PYTHONPATH=<repo>/tools.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_boss_drops_wire import archive, find_drop_info, walk  # noqa: E402
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402

QUEST, ITEM, LOW, HIGH, RUNS = 540, '10903', 1, 3, 10


def held(reply: dict) -> int:
    """The item's stack in a MissionEnd reply's warehouse."""
    return next((int(n['wgV86x1q']) for n in walk(reply) if isinstance(n, dict)
                 and n.get('kixHbe54') == ITEM and 'wgV86x1q' in n), 0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--token', type=Path, required=True)
    ap.add_argument('--balance', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:19982')
    args = ap.parse_args()
    login, user, token, _ = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'

    record = archive()[QUEST]
    record.pop('mimic_chests', None)
    record.pop('random_mimics', None)
    for stage in record['stages']:
        for m in stage['battle_monsters']:
            m.update(treasure_chest_chance=100, treasure_drops=[
                {'target_type': 4, 'weight': 100, 'target_id': ITEM, 'amount': LOW, 'amount_max': HIGH}])
    overlay = args.balance / 'server' / 'missions' / f'{QUEST}.json'
    overlay.parent.mkdir(parents=True, exist_ok=True)
    overlay.write_text(json.dumps(record))
    def run(report: bool) -> tuple[list, int]:
        started = call(url, user, login, token, *MISSION_START, start_body(QUEST))
        if 'b5PH6mZa' in started:
            raise SystemExit(f'MissionStart {QUEST} refused: {started["b5PH6mZa"]}')
        # Item chests only ("idx/1/4:weight:item:count"); mimics are encoded differently.
        got = [int(e.split(':')[3]) for wave in find_drop_info(started).split('@') if wave.count('|') >= 2
               for e in wave.split('|')[2].strip().split('-') if '/1/4:' in e and e.split(':')[2] == ITEM]
        body = end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba']))
        body['rXvA1E5y'][0]['4T0Q2Bh5'] = f'{ITEM}:{sum(got)}' if report and got else ''
        ended = call(url, user, login, token, *MISSION_END, body)
        if 'b5PH6mZa' in ended:
            raise SystemExit(f'MissionEnd {QUEST} refused: {ended["b5PH6mZa"]}')
        return got, held(ended)

    try:
        time.sleep(3)   # the overlay folder is polled every 2 s
        _, before = run(False)
        counts, reported, after = [], 0, before
        for _ in range(RUNS):
            got, after = run(True)
            counts += got
            reported += sum(got)
    finally:
        overlay.unlink(missing_ok=True)
    fails = 0
    for label, ok, detail in (
            ('every chest holds 1..3', bool(counts) and all(LOW <= c <= HIGH for c in counts), sorted(set(counts))),
            ('the count is rolled, not fixed', len(set(counts)) > 1, f'{len(counts)} chests'),
            ('MissionEnd grants exactly the rolled total', after - before == reported, f'{before} -> {after}, rolled {reported}')):
        print(f'{"PASS" if ok else "FAIL"} {label}: {detail}')
        fails += not ok
    print(f'\n{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
