"""Mega Jewel Parade's last room is led by a Golden God you can catch.

python scripts/gen_mega_jewel_parade.py [--check]

WHY (ticket #130).  Golden God (50134), the top of the Jewel line,
came from Global's Mega Jewel Parade: the wiki's quest record (Parade:MJP,
re/work/vortex/sources/wiki/quests.json) lists him as its capture and as the
richest Jewel unit (10,000 Zel max).  Our 100652 was built like the Super
Jewel Parade, led by a Jewel God, so no Golden God ever appeared and players
had no way to get one.

RULE: in the mission named Mega Jewel Parade, the boss room's Jewel God
becomes a Golden God with its own monster id (54850 is the Uncharted
Frontier stand-in, and the client keeps the first monster row per id), three
times the Jewel God's HP (his unit HP is 3.4x a Jewel God's), the line's 60%
capture and 10,000 Zel.  A second run finds no Jewel God there and changes
nothing.  Run it after gen_material_boss_captures.py when rebuilding the
archive.
"""

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import MISSIONS  # noqa: E402

MISSION = 'Mega Jewel Parade'
JEWEL_GOD, GOLDEN_GOD, MONSTER_ID = 50133, 50134, 54851


def apply(record: dict) -> int:
    if record['name'] != MISSION:
        return 0
    changed = 0
    for stage in record['stages']:
        for m in stage['battle_monsters'] if stage['is_boss'] else []:
            if m['unit_id'] == JEWEL_GOD:
                m.update(id=MONSTER_ID, name='Golden God', unit_id=GOLDEN_GOD, hp=m['hp'] * 3,
                         unit_drop_id=GOLDEN_GOD, unit_drop_chance=60, zel_max_drop=10000)
                changed += 1
    return changed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='report only; do not write mission.json')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    taken = {m['id'] for r in records if r['name'] != MISSION for s in r['stages'] for m in s['battle_monsters']}
    if MONSTER_ID in taken:
        sys.exit(f'monster id {MONSTER_ID} is used by another mission')
    changed = {r['id']: n for r in records if (n := apply(r))}
    print(f'{sum(changed.values())} Golden God(s) in {sorted(changed)}')
    if changed and not args.check:
        MISSIONS.write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
