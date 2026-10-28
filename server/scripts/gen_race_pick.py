#!/usr/bin/env python3
"""Writes deploy/archive/race_pick.json: the units the level 999 race's unit pick offers.

The first ten accounts to reach level 999 each get ONE free pick from every 7* and Omni unit
(owner, 2026-10-22; gme/common/UnitPick.cpp splits the pool into Omni and 7* pickers per
element).  Every 7* and Omni stage of a line is in it, with the same filters as the summon
gates: guest-only lines (no leader skill at the summon stage, gen_event_gates.guest_only) and
the lines whose battle effect files are missing (the LEFT_OUT lists of gen_collab_gacha,
gen_house_gates, gen_event_gates and gen_global_gate) are left out, and so is any unit without
a deploy/archive/unit.json record.

    python3 scripts/gen_race_pick.py           # rewrite the file
    python3 scripts/gen_race_pick.py --check   # exit 1 if it is out of date
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_collab_gacha as collab  # noqa: E402
import gen_event_gates as event    # noqa: E402
import gen_global_gate as global_gate  # noqa: E402
import gen_house_gates as house    # noqa: E402

OUT = collab.ROOT / 'deploy' / 'archive' / 'race_pick.json'
RARITIES = {'7': '7*', '8': 'Omni'}


def pool() -> list[int]:
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    archived = {str(u['id']) for u in collab.load(collab.UNIT_ARCHIVE)}
    line = collab.evolution_lines()
    broken = {line(str(k))[0] for k in (*collab.LEFT_OUT, *house.LEFT_OUT, *event.LEFT_OUT, *global_gate.LEFT_OUT)}
    out = []
    for uid, unit in units.items():
        if unit.get('7ofj5xa1') not in RARITIES or uid not in archived:
            continue
        base = line(uid)[0]
        if base in broken or base not in units or event.guest_only(units[base]):
            continue
        out.append(int(uid))
    return sorted(out)


def render(ids: list[int]) -> str:
    return json.dumps({
        'about': 'Units the level 999 race unit pick offers (scripts/gen_race_pick.py; gme/common/UnitPick.cpp).',
        'units': ids,
    }, indent=1) + '\n'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    text = render(pool())
    if args.check:
        same = OUT.is_file() and OUT.read_text(encoding='utf-8') == text
        print('race_pick.json is ' + ('up to date' if same else 'OUT OF DATE'))
        return 0 if same else 1
    OUT.write_text(text, encoding='utf-8')
    units = json.loads(text)['units']
    print(f'wrote {OUT} ({len(units)} units)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
