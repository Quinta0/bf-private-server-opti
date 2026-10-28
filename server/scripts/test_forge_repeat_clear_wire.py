#!/usr/bin/env python3
"""Wire test: Balance Forge repeat-clear rewards (mission_mst S2ChOKo6).

    PYTHONPATH=<repo>/tools <repo>/.venv/bin/python \
        scripts/test_forge_repeat_clear_wire.py [--src wt/forge] [--port 9985] [--assets DIR] [--keep]

Boots a throwaway server (container bf-forge-repeat, save in volume bf-forge-repeat-data)
with a mission_mst overlay: Cave of Dancing Flames (20) first clear "8:0:1:0:0", repeat
clear "daily:2,3:0:5000:0:0", and the first Research Lab trial with a repeat reward.
Then, for a throwaway guest: clear 1 pays the first-clear gem only; clears 2 and 3 pay
5,000 Zel each ("Repeat clear reward"); clear 4 pays nothing; a lost battle pays nothing;
moving the ledger row to yesterday's game day pays again; a lab replay pays nothing.
Removes its container and volume (unless --keep).
"""
from __future__ import annotations

import argparse
import copy
import json
import shutil
import sys
import tempfile
from pathlib import Path

from forge_wire_common import MISSION_END, MISSION_START, Server, end_body, gme_call, sha, start_body, table, wire_error
from test_fair_summon import Database, Player

CONTAINER, VOLUME = 'bf-forge-repeat', 'bf-forge-repeat-data'
MISSION, FIRST, REPEAT = 20, '8:0:1:0:0', 'daily:2,3:0:5000:0:0'
LAB_REPEAT = '3:0:777:0:0'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9985)
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive' / 'assets')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    src = args.src.resolve()
    failures = 0

    def check(label: str, ok, detail='') -> bool:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  -- ' + str(detail)[:400]}", flush=True)
        return bool(ok)

    work = Path(tempfile.mkdtemp(prefix='forge-repeat-'))
    server = Server(src, work, args.port, args.assets, CONTAINER, VOLUME)
    try:
        _, missions = table(src, 'mission_mst.json')
        _, dungeons = table(src, 'dungeon_mst.json')
        trial_dungeons = {r['MHx05sXt'] for r in dungeons if r['3hPeI1RV'] == '2'}
        lab = min(int(r['j28VNcUW']) for r in missions if r['MHx05sXt'] in trial_dungeons)
        cave = copy.deepcopy(next(r for r in missions if r['j28VNcUW'] == str(MISSION)))
        cave.update({'SiYs27Cj': FIRST, 'S2ChOKo6': REPEAT})
        trial = copy.deepcopy(next(r for r in missions if r['j28VNcUW'] == str(lab)))
        trial['S2ChOKo6'] = LAB_REPEAT
        overlay = json.dumps({'format': 1, 'table': 'mission_mst.json', 'key': ['j28VNcUW'],
                              'rows': [cave, trial]}, indent=1).encode()
        path = work / 'balance' / 'server' / 'mst' / 'mission_mst.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(overlay)

        server.start()
        entry = server.status().get('mst', {}).get('mission_mst.json', {})
        check('status: the mission_mst overlay with S2ChOKo6 is applied',
              entry.get('applied') is True and entry.get('sha256') == sha(overlay) and entry.get('rows_replaced') == 2, entry)

        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')
        player = Player(server.url, db)
        player.set(tutorial_status=12, tutorial_end_flag=1, energy=9999, energy_full_ts=0)
        uid = player.user_id
        for m in sorted(int(r['j28VNcUW']) for r in missions if int(r['j28VNcUW']) < MISSION):
            db.rows('INSERT OR IGNORE INTO user_campaign_missions (user_id, mission_id, state, attain_percent,'
                    ' clear_count, last_cleared_at) VALUES (?, ?, 2, 100, 1, 0)', uid, str(m))

        def presents(description: str) -> list:
            return sorted((int(r[0]), int(r[1])) for r in db.rows(
                'SELECT present_type, target_cnt FROM user_presents WHERE user_id = ? AND description = ?',
                uid, description))

        def battle(mission: int, won: bool = True) -> None:
            started = gme_call(server.url, *MISSION_START, start_body(player.login, mission))
            if wire_error(started) is not None or 'Kz7qfSs5' not in started:
                raise RuntimeError(f'MissionStart {mission} refused: {wire_error(started)}')
            serial = started['Kz7qfSs5'][0].get('k9cxD7Ba', '0')
            ended = gme_call(server.url, *MISSION_END, end_body(player.login, serial, won))
            if wire_error(ended) is not None:
                raise RuntimeError(f'MissionEnd {mission} refused: {wire_error(ended)}')

        battle(MISSION)
        check('clear 1 (first): the first-clear gem, no repeat reward',
              presents('First clear reward') == [(8, 1)] and presents('Repeat clear reward') == [],
              (presents('First clear reward'), presents('Repeat clear reward')))
        battle(MISSION)
        battle(MISSION)
        check('clears 2 and 3: 5,000 Zel each', presents('Repeat clear reward') == [(3, 5000), (3, 5000)],
              presents('Repeat clear reward'))
        battle(MISSION)
        check('clear 4: the daily cap of 2 is reached, nothing more', len(presents('Repeat clear reward')) == 2,
              presents('Repeat clear reward'))
        battle(MISSION, won=False)
        check('a lost battle pays nothing', len(presents('Repeat clear reward')) == 2 and presents('First clear reward') == [(8, 1)],
              presents('Repeat clear reward'))
        ledger = db.rows("SELECT offer_id, count FROM user_exchange_purchases WHERE user_id = ? AND shop = 'repeat-clear'", uid)
        check('the ledger holds one row "20@d<game day>" with count 2',
              len(ledger) == 1 and str(ledger[0][0]).startswith(f'{MISSION}@d') and int(ledger[0][1]) == 2, ledger)
        if ledger:
            day = int(str(ledger[0][0]).split('@d')[1])
            db.rows("UPDATE user_exchange_purchases SET offer_id = ? WHERE user_id = ? AND shop = 'repeat-clear'",
                    f'{MISSION}@d{day - 1}', uid)
        battle(MISSION)
        check('a new game day pays again', len(presents('Repeat clear reward')) == 3, presents('Repeat clear reward'))

        battle(lab)
        battle(lab)
        check('a Research Lab replay with a repeat reward pays nothing',
              (3, 777) not in presents('Repeat clear reward'), presents('Repeat clear reward'))
    except Exception as e:  # noqa: BLE001 - report it, then clean up
        check(f'{type(e).__name__}: {e}', False)
    finally:
        if not args.keep:
            server.remove()
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f'kept: container {CONTAINER}, volume {VOLUME}, balance root {work / "balance"}')
    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
