#!/usr/bin/env python3
"""Wire test: the Forge difficulty log (forge-clears.jsonl, GET /bf/gme/forgeClears.php).

    PYTHONPATH=<repo>/tools <repo>/.venv/bin/python \
        scripts/test_forge_clears_wire.py [--src wt/forge] [--port 9983] [--assets DIR] [--keep]

Boots a throwaway server (container bf-diff-clears, save in volume bf-diff-clears-data).
A fresh server reports no runs.  Then, for a throwaway guest on two story quests A and B:
  clear     start A, wait 2 s, win in turns "2,1,3": A has 1 run, 1 clear, >= 2 s, 6 turns,
            the squad's summed ATK/HP and top rarity;
  loss      start A, end it lost (status 3): A has 1 fail;
  abandon   start A, then start B: A has 1 abandon; B is won with an own-unit helper,
            whose stats are summed into B's squad;
  replay    the settled MissionEnd sent again adds nothing;
  privacy   neither the file nor the endpoint holds the player's id;
  reboot    after a container restart the aggregates are the same (rebuilt from the file).
Removes its container and volume (unless --keep).
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

from forge_wire_common import MISSION_END, MISSION_START, Server, end_body, fetch, gme_call, run, start_body, table, wire_error
from test_fair_summon import Database, Player

CONTAINER, VOLUME = 'bf-diff-clears', 'bf-diff-clears-data'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9983)
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

    work = Path(tempfile.mkdtemp(prefix='forge-clears-'))
    server = Server(src, work, args.port, args.assets, CONTAINER, VOLUME)
    try:
        _, missions = table(src, 'mission_mst.json')
        _, dungeons = table(src, 'dungeon_mst.json')
        _, units = table(src, 'unit_mst.json')
        rarity = {int(r['pn16CNah']): int(r['7ofj5xa1']) for r in units}
        dtype = {r['MHx05sXt']: r['3hPeI1RV'] for r in dungeons}
        archived = {int(r['id']) for r in json.loads((src / 'deploy' / 'archive' / 'mission.json').read_text(encoding='utf-8'))}
        story = sorted(int(r['j28VNcUW']) for r in missions
                       if 11 <= int(r['j28VNcUW']) < 900 and dtype.get(r['MHx05sXt']) == '0'
                       and int(r['j28VNcUW']) in archived and ':' not in r.get('1e6jxQzf', ''))
        qa, qb = story[3], story[4]

        server.start()

        def clears() -> dict:
            return json.loads(fetch(f'{server.url}/bf/gme/forgeClears.php')[1])

        first = clears()
        check('a fresh server: no runs, no quests', first.get('runs') == 0 and first.get('missions') == {}
              and first.get('since') is None, first)

        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')
        player = Player(server.url, db)
        player.set(tutorial_status=12, tutorial_end_flag=1, energy=9999, energy_full_ts=0)
        uid = player.user_id
        for m in sorted(int(r['j28VNcUW']) for r in missions if int(r['j28VNcUW']) <= qb):
            db.rows('INSERT OR IGNORE INTO user_campaign_missions (user_id, mission_id, state, attain_percent,'
                    ' clear_count, last_cleared_at) VALUES (?, ?, 2, 100, 1, 0)', uid, str(m))

        squad = db.rows('SELECT u.unit_id, u.base_hp + u.ext_hp, u.base_atk + u.ext_atk FROM user_decks d'
                        ' JOIN user_units u ON u.user_id = d.user_id AND u.user_unit_id = d.user_unit_id'
                        ' WHERE d.user_id = ? AND d.deck_type = 1 AND d.deck_num = 0 AND d.user_unit_id > 0', uid)
        check('the guest has a squad 0 to sum', len(squad) > 0, squad)
        hp, atk = sum(int(r[1]) for r in squad), sum(int(r[2]) for r in squad)
        top = max(rarity.get(int(r[0]), 0) for r in squad)
        # A copy of the guest's first unit, outside the squad: the own-unit helper.
        cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)') if r[1] != 'user_unit_id']
        db.rows(f'INSERT INTO user_units ({", ".join(cols)}) SELECT {", ".join(cols)} FROM user_units'
                ' WHERE user_id = ? ORDER BY user_unit_id LIMIT 1', uid)
        spare = db.one('SELECT user_unit_id, unit_id, base_hp + ext_hp, base_atk + ext_atk FROM user_units'
                       ' WHERE user_id = ? ORDER BY user_unit_id DESC LIMIT 1', uid)

        def start(mission: int, helper: str = '0') -> str:
            reply = gme_call(server.url, *MISSION_START, start_body(player.login, mission, 0, helper))
            if wire_error(reply) is not None or 'Kz7qfSs5' not in reply:
                raise RuntimeError(f'MissionStart {mission} refused: {wire_error(reply)}')
            return reply['Kz7qfSs5'][0].get('k9cxD7Ba', '0')

        def end(serial: str, won: bool = True, turns: str = '') -> dict:
            body = end_body(player.login, serial, won)
            if turns:
                body['rXvA1E5y'][0]['Lu0pAk1R'] = turns
            reply = gme_call(server.url, *MISSION_END, body)
            if wire_error(reply) is not None:
                raise RuntimeError(f'MissionEnd {serial} refused: {wire_error(reply)}')
            return body

        # ---- a clear -----------------------------------------------------------------
        serial = start(qa)
        time.sleep(2.2)
        settled = end(serial, True, '2,1,3')
        a = clears()['missions'].get(str(qa), {})
        check('clear: 1 run, 1 clear, no fails or abandons',
              (a.get('runs'), a.get('clears'), a.get('fails'), a.get('abandons')) == (1, 1, 0, 0), a)
        check('clear: real seconds from start to end (>= 2), p90 the same',
              isinstance(a.get('clear_seconds_median'), int) and 2 <= a['clear_seconds_median'] <= 30
              and a.get('clear_seconds_p90') == a.get('clear_seconds_median'), a)
        check('clear: turns 2+1+3 = 6', a.get('clear_turns_median') == 6, a)
        check(f'clear: squad ATK {atk} / HP {hp} summed, top rarity {top}',
              a.get('squad_atk_median') == atk and a.get('squad_hp_median') == hp and a.get('top_rarity') == {str(top): 1}, a)

        # ---- the same result again -----------------------------------------------------
        reply = gme_call(server.url, *MISSION_END, settled)
        a = clears()['missions'].get(str(qa), {})
        check('replay: the settled MissionEnd again adds no run', wire_error(reply) is None and a.get('runs') == 1, a)

        # ---- a loss ------------------------------------------------------------------
        end(start(qa), False, '1,4')
        a = clears()['missions'].get(str(qa), {})
        check('loss: a fail, the clear time and turns still the clear\'s',
              (a.get('runs'), a.get('clears'), a.get('fails')) == (2, 1, 1) and a.get('clear_turns_median') == 6, a)

        # ---- an abandon, then a clear with an own helper --------------------------------
        start(qa)
        helper = f'MYUNIT{spare[0]}'
        serial_b = start(qb, helper)
        a = clears()['missions'].get(str(qa), {})
        check('abandon: starting another quest abandons the open one',
              (a.get('runs'), a.get('abandons')) == (3, 1), a)
        end(serial_b, True, '3')
        b = clears()['missions'].get(str(qb), {})
        h_atk, h_hp = atk + int(spare[3]), hp + int(spare[2])
        top_b = max(top, rarity.get(int(spare[1]), 0))
        check(f'own helper: its stats summed in (ATK {h_atk}, HP {h_hp})',
              (b.get('runs'), b.get('clears')) == (1, 1) and b.get('squad_atk_median') == h_atk
              and b.get('squad_hp_median') == h_hp and b.get('top_rarity') == {str(top_b): 1}, b)

        # ---- privacy -------------------------------------------------------------------
        lines = run(['docker', 'exec', CONTAINER, 'cat', '/data/forge-clears.jsonl']).stdout
        records = [json.loads(line) for line in lines.splitlines() if line.strip()]
        check('the file: 4 lines (clear, fail, abandon, clear) in order',
              [r.get('r') for r in records] == ['clear', 'fail', 'abandon', 'clear'], records)
        check('the file: a fail keeps its status, an abandon has no seconds',
              len(records) == 4 and records[1].get('st') == 3 and 's' not in records[2], records)
        body = fetch(f'{server.url}/bf/gme/forgeClears.php')[1].decode()
        check('no user id in the file or the endpoint', uid not in lines and uid not in body
              and str(player.login) not in body, uid)

        # ---- reboot ---------------------------------------------------------------------
        before = clears()
        booted = server.status().get('booted_at')
        run(['docker', 'restart', CONTAINER])
        deadline = time.time() + 600
        while time.time() < deadline:
            try:
                if server.status().get('booted_at') not in (None, booted):
                    break
            except Exception:  # noqa: BLE001 - not listening yet
                pass
            time.sleep(2)
        after = clears()
        check('reboot: the aggregates are rebuilt from the file, unchanged',
              after.get('missions') == before.get('missions') and after.get('runs') == before.get('runs') == 4
              and after.get('since') == before.get('since'), (before, after))
    except Exception as e:  # noqa: BLE001 - report it, then clean up
        check(f'{type(e).__name__}: {e}', False)
    finally:
        if not args.keep:
            server.remove()
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f'kept: container {CONTAINER}, volume {VOLUME}')
    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
