"""Unit AI records for Arena (ticket #45) -> deploy/archive/ai.json.

In Arena both sides fight on their own: every unit runs the AI record named by
its UnitMst.ai_id (i74vGUFa), and the client only casts a BB or SBB when a row
of that record says so.  The arena battle reply sends these records
(MissionArchiver::unitAiRows); without them every unit only attacks.

The rows are the original Global F_AI_MST rows for ids 100-170, from the public
datamine github.com/cheahjs/bravefrontier_data ai.json (last Global update
2017-05-17; the JP dump agrees row for row).  Its fields map one to one:
"conditions/set parameters" -> self_conditions, "target conditions" ->
search_term, "target type" -> act_target, action "parameters" (all -1) -> no
flag changes.  The dump has no action count or wait columns; every row counts
as the unit's action, as in every other record here, because a player unit
that does not count its action acts again.

The unit master also uses ids 180-230 (122 later units), which postdate that
dump.  Their rows are lost, so they get the rows of 100, the plain "no
strategy" attacker: BB when the gauge is full, else attack.

A `skill:0` self term passes when the unit's BB or SBB gauge is full
(PlayerUnit/RivalUnit::isSkillFree is false), and a `skill` action casts the
SBB if it is ready, otherwise the BB; the skill picks its own side, so
act_target only matters for attacks.

Usage: python3 scripts/gen_arena_unit_ai.py [--check]
Merges by id like the mission generators: other records are left untouched.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
AIS = ROOT / 'deploy/archive/ai.json'
UNITS = ROOT / 'deploy/mst/unit_mst.json'

ENEMY, OWN_SIDE = 2, 1
BB_READY = [('skill', 0)]


def row(priority, percent, act_target, search_term, kind, conditions=()):
    return {
        'priority': priority,
        'percent': float(percent),
        'act_target': act_target,
        'search_term': search_term,
        'self_conditions': [{'type': t, 'parameter': p} for t, p in conditions],
        'party_conditions': [],
        'action': {'type': kind, 'flag_changes': [], 'unknown_bool': True,
                   'unknown_int_1': 0, 'unknown_int_2': 0},
    }


# id: (English name, original name, rows) -- datamine order is wire order.
ORIGINAL = {
    100: ('No strategy, attack', '無策攻撃', [
        row(1, 60, ENEMY, 'hp_50pr_over', 'skill', BB_READY),
        row(2, 20, ENEMY, 'random', 'skill', BB_READY),
        row(3, 100, ENEMY, 'random', 'attack'),
    ]),
    110: ('Bullet (finish the wounded)', '鉄砲玉', [
        row(1, 60, ENEMY, 'hp_50pr_under', 'skill', BB_READY),
        row(2, 30, ENEMY, 'random', 'skill', BB_READY),
        row(3, 70, ENEMY, 'hp_max', 'attack', [('actbetween', 3)]),
        row(4, 50, ENEMY, 'hp_min', 'attack'),
        row(5, 100, ENEMY, 'random', 'attack'),
    ]),
    120: ('Kill first', 'キル優先', [
        row(1, 60, ENEMY, 'random', 'skill', BB_READY),
        row(2, 20, ENEMY, 'atk_max', 'skill', BB_READY),
        row(3, 30, ENEMY, 'hp_min', 'attack'),
        row(4, 100, ENEMY, 'random', 'attack'),
    ]),
    130: ('Buffer', 'バフ付与', [
        row(1, 60, OWN_SIDE, 'random', 'skill', BB_READY),
        row(2, 30, ENEMY, 'atk_max', 'attack'),
        row(3, 100, ENEMY, 'random', 'attack'),
    ]),
    140: ('No strategy, heal', '無策回復', [
        row(1, 80, OWN_SIDE, 'hp_50pr_under', 'skill', BB_READY),
        row(2, 20, OWN_SIDE, 'hp_min', 'skill', BB_READY),
        row(3, 100, ENEMY, 'random', 'attack'),
    ]),
    150: ('Heal early', '高回復', [
        row(1, 100, OWN_SIDE, 'hp_75pr_under', 'skill', BB_READY),
        row(2, 50, ENEMY, 'hp_min', 'attack'),
        row(3, 100, ENEMY, 'random', 'attack'),
    ]),
    160: ('Heal late', '低回復', [
        row(1, 100, OWN_SIDE, 'hp_25pr_under', 'skill', BB_READY),
        row(2, 50, ENEMY, 'atk_max', 'attack'),
        row(3, 100, ENEMY, 'random', 'attack'),
    ]),
}
STAND_IN = 100


def records(unit_ai_ids):
    out = []
    for ai_id in sorted(unit_ai_ids):
        if ai_id in ORIGINAL:
            name, original, rows = ORIGINAL[ai_id]
            out.append({'id': ai_id, 'name': f'Arena unit AI: {name} ({original})', 'actions': rows})
        else:
            name, _, rows = ORIGINAL[STAND_IN]
            out.append({'id': ai_id, 'name': f'Arena unit AI {ai_id}: stand-in, rows of {STAND_IN} ({name})',
                        'actions': rows})
    return out


def unit_ai_ids():
    units = json.loads(UNITS.read_text(encoding='utf-8'))['2r9cNSdt']
    return {int(u['i74vGUFa']) for u in units if int(u.get('i74vGUFa') or 0) > 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='fail if ai.json is not up to date')
    args = ap.parse_args()

    raw = AIS.read_bytes()
    by_id = {r['id']: r for r in json.loads(raw)}
    wanted = records(unit_ai_ids())
    clashes = [r['id'] for r in wanted if r['id'] in by_id and not by_id[r['id']]['name'].startswith('Arena unit AI')]
    if clashes:
        sys.exit(f'ai.json ids {clashes} already hold other records; refusing to overwrite them')
    for r in wanted:
        by_id[r['id']] = r
    new = (json.dumps([by_id[k] for k in sorted(by_id)], indent=4, ensure_ascii=False) + '\n').encode('utf-8')

    if args.check:
        if new != raw:
            sys.exit('deploy/archive/ai.json is stale: run scripts/gen_arena_unit_ai.py')
        print(f'ai.json up to date ({len(wanted)} unit AI records)')
        return
    AIS.write_bytes(new)
    print(f'wrote {len(wanted)} unit AI records: {[r["id"] for r in wanted]}')


if __name__ == '__main__':
    main()
