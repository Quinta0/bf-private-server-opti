"""Material dungeons whose boss IS the unit you farm there: let you catch it.

python scripts/gen_material_boss_captures.py [--check]

WHY (ticket #117).  Celestial Garden's boss is a Sphere Frog, the
reason to run it, yet every archive version since its records were built
(2026-10-16) gave that boss no capture (authored_capture, chance 0), so no
player could catch one there.  Its sister Garden of Imps lets you catch its
boss, Almighty Imp Arton (60% in 102920).  gen_capture_rules.py only keeps or
removes drops, so it never added this one, and MissionArchiver's boss drops
(#82) skip farm units.  No original rate is recorded (the wiki lists only the
Burst Frogs), so the boss gets the archive's default capture chance.

RULE: in every mission named in BOSS_CAPTURES, a boss-wave enemy drawn as
that unit with no drop gets unit_drop_id = its unit, level 1, type 5 (the
type is rolled at MissionStart anyway), the default chance, and
authored_capture true.  Enemies that already drop something are left alone,
so a second run changes nothing.  Run it after gen_capture_rules.py when
rebuilding the archive.
"""

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import MISSIONS  # noqa: E402
from gen_capture_rules import DEFAULT_CHANCE, RANDOM_DROP  # noqa: E402

BOSS_CAPTURES = {'Celestial Garden': 20302}     # mission name -> boss unit (Sphere Frog)


def apply(record: dict) -> int:
    unit = BOSS_CAPTURES.get(record['name'])
    given = 0
    for stage in record['stages'] if unit else []:
        for m in stage['battle_monsters'] if stage['is_boss'] else []:
            if m['unit_id'] == unit and not m.get('visuals') and not m.get('unit_drop_chance'):
                m.update(unit_drop_id=unit, unit_drop_level=1, unit_drop_type=RANDOM_DROP,
                         unit_drop_chance=DEFAULT_CHANCE, authored_capture=True)
                given += 1
    return given


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='report only; do not write mission.json')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    given = {r['id']: n for r in records if (n := apply(r))}
    print(f'{sum(given.values())} boss(es) in {len(given)} mission(s) can now be caught: {sorted(given)}')
    if given and not args.check:
        MISSIONS.write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
