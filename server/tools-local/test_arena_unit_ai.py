"""Wire test: arena battles carry the unit AI rows (ticket #45).

In Arena every unit on both sides acts on its own AI: the client looks up
UnitMst.ai_id in its AI list and only casts a BB/SBB when a row says so.  The
list holds whatever the last battle reply sent (MissionStart sends monster AI),
so ArenaBattleStart and ArenaBattleRestart must send the unit AI rows
(89ausgc4) or every unit only attacks.

Checks: both replies carry rows for every AI id in the unit master and for
every unit on both sides (a player opponent and, with --db, a bot); every id
has a `skill:0` (gauge full) skill row; the rows of id 100 are the original
F_AI_MST rows byte for byte; the action strings count as the unit's action.

Usage: python -I test_arena_unit_ai.py --url http://127.0.0.1:9962 [--db /save/gme.sqlite [--exec-in CONTAINER]]
--exec-in runs the one database write inside the server's container (docker
exec), for a save in a Docker volume: never write a running save from the host.
"""
import argparse
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, '/tools')
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_arena import (BATTLE_START, MATCHING, RESTART, Player, check,  # noqa: E402
                        db_exec)
import test_arena  # noqa: E402

UNIT_MST = Path(__file__).resolve().parent.parent / 'deploy/mst/unit_mst.json'
DECK = {'fq2C03jh': [{'Z0Y4RoD7': '0', 'ciDqCjAF': '0', '59b1eE6N': ''}]}
NO_PARTY = '0:non:non:non@' * 4
PADDING = 'non:0@'
NO_FLAGS = '-1,' * 12
# Original Global F_AI_MST id 100 (無策攻撃), wire order.
AI_100 = [
    ('1', NO_PARTY + '#skill:0@' + PADDING * 4, '2', 'hp_50pr_over', 'skill@' + NO_FLAGS + '@1@0@0', 60.0),
    ('2', NO_PARTY + '#skill:0@' + PADDING * 4, '2', 'random', 'skill@' + NO_FLAGS + '@1@0@0', 20.0),
    ('3', NO_PARTY + '#' + PADDING * 5, '2', 'random', 'attack@' + NO_FLAGS + '@1@0@0', 100.0),
]


WRITE_SNIPPET = ('import sqlite3, sys; c = sqlite3.connect(sys.argv[1], timeout=10); '
                 'c.execute(sys.argv[2], sys.argv[3:]); c.commit()')


def write_db(args, sql: str, *params) -> None:
    if args.exec_in:
        subprocess.run(['docker', 'exec', args.exec_in, 'python3', '-c', WRITE_SNIPPET, args.db, sql,
                        *map(str, params)], check=True)
    else:
        db_exec(args.db, sql, *params)


def unit_ai_ids() -> dict[str, str]:
    units = json.loads(UNIT_MST.read_text(encoding='utf-8'))['2r9cNSdt']
    return {u['pn16CNah']: u['i74vGUFa'] for u in units}


def rows_by_ai(reply: dict) -> dict[str, list]:
    by_ai = defaultdict(list)
    for r in reply.get('89ausgc4', []):
        by_ai[r['4eEVw5hL']].append(r)
    return by_ai


def bb_row(rows: list) -> bool:
    return any(r['Hhgi79M1'].startswith('skill@') and '#skill:0@' in r['q7Nit8JW'] for r in rows)


def check_side(label: str, units_json: str, by_ai: dict, ai_of: dict) -> None:
    units = json.loads(units_json)
    missing = [(u['P_UNIT_ID'], ai_of.get(u['P_UNIT_ID'])) for u in units
               if not bb_row(by_ai.get(ai_of.get(u['P_UNIT_ID'], ''), []))]
    check(f'{label}: every unit ({len(units)}) has a BB row for its AI id', units and not missing, missing)
    no_bb = [u['P_UNIT_ID'] for u in units if u.get('P_SKILL_ID') in (None, '', '0')]
    check(f'{label}: every unit carries a BB id', not no_bb, no_bb)


def check_reply(label: str, reply: dict, ai_of: dict) -> dict:
    by_ai = rows_by_ai(reply)
    wanted = {a for a in ai_of.values() if a not in ('', '0')}
    check(f'{label}: 89ausgc4 sent', bool(reply.get('89ausgc4')), sorted(reply)[:12])
    check(f'{label}: rows for every unit AI id', wanted <= set(by_ai), sorted(wanted - set(by_ai)))
    check(f'{label}: every unit AI id has a gauge-full skill row',
          all(bb_row(by_ai[a]) for a in wanted if a in by_ai),
          [a for a in wanted if a in by_ai and not bb_row(by_ai[a])])
    got_100 = [(r['yu18xScw'], r['q7Nit8JW'], str(r['4xctV8gF']), r['VBj9u0ot'], r['Hhgi79M1'],
                float(r['ug9xV4Fz'])) for r in by_ai.get('100', [])]
    check(f'{label}: AI 100 rows are the original ones', got_100 == AI_100, got_100)
    every_counts = all(r['Hhgi79M1'].endswith('@1@0@0') for r in reply.get('89ausgc4', []))
    check(f'{label}: every row counts as the unit\'s action', every_counts)
    check_side(f'{label} enemy', reply['9NX83YVe'][0]['U42YcFNu'], by_ai, ai_of)
    return by_ai


def start_battle(p: Player) -> tuple[dict, dict]:
    enemy = p.call(MATCHING, DECK)['fQ3XImM2'][0]
    start = p.call(BATTLE_START, {'9NX83YVe': [{'h7eY3sAK': enemy['h7eY3sAK'], 'Z7PXoxc2': '0'}],
                                  **DECK, 'rj5fgJh0': [{'t5R47iwj': '0'}]})
    return enemy, start


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9962')
    ap.add_argument('--db')
    ap.add_argument('--exec-in')
    args = ap.parse_args()
    ai_of = unit_ai_ids()

    a, b = Player(args.url, 'ArenaAiA'), Player(args.url, 'ArenaAiB')
    a.identify()
    b.identify()

    enemy, start = start_battle(a)
    check('player opponent offered', not enemy['h7eY3sAK'].startswith('ARENABOT'), enemy['h7eY3sAK'])
    check('BattleStart succeeded', '9NX83YVe' in start, str(start)[:160])
    rows = check_reply('BattleStart', start, ai_of)

    restart = a.call(RESTART, {'60subGk3': [{'EfbZ7mh9': '1'}]})
    check('Restart succeeded', '9NX83YVe' in restart, str(restart)[:160])
    check('Restart rows equal BattleStart rows', restart.get('89ausgc4') == start.get('89ausgc4'))
    check_side('Restart own party', restart['5K5wsBoL'][0]['U42YcFNu'], rows, ai_of)

    if args.db:
        # Out of everyone's points band: Matching falls back to a bot.
        write_db(args, 'UPDATE user_arena SET points = 50000000 WHERE user_id = ?', a.user_id)
        enemy, start = start_battle(a)
        check('bot opponent offered', enemy['h7eY3sAK'].startswith('ARENABOT'), enemy['h7eY3sAK'])
        check('bot BattleStart succeeded', '9NX83YVe' in start, str(start)[:160])
        check_reply('bot BattleStart', start, ai_of)
        write_db(args, 'UPDATE user_arena SET points = 0 WHERE user_id = ?', a.user_id)
    else:
        print('SKIP  bot opponent (no --db)')

    print(f'{test_arena.failures} failure(s)')
    return 1 if test_arena.failures else 0


if __name__ == '__main__':
    sys.exit(main())
