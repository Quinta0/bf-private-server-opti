"""Rebuild the Monday Karma dungeons as balance-folder battles (ticket #43).

python scripts/gen_karma_dungeon.py [--out DIR] [--check]

WHY.  Souls Training Ground (Soul Sanctuary Lv. 1-4, missions 100050-100053)
came out of gen_wiki_missions.py, which read the wiki's capture list (the four
stat Imps) as the regular enemies: every wave was 1000-HP Imps at 35% capture,
a Lv. 4 clear paid ~100 Karma, and its boss was a 1000-HP Hellborn Dilias
that could be caught.  Sacred Land of the Souls Lv. 2 (100001) came out of
gen_designed_missions.py with the Shining Stage collab template, so its boss
was a catchable Diva Hatsune Miku.  Its Lv. 1 and Lv. 3 (100000, 100002),
open the same Monday, were Box of Desires clones with 100%-catchable Mimic and
Dragon Mimic bosses.

WHAT THE REAL DUNGEON WAS (re/work/vortex/sources/wiki/pages/Souls Training
Ground.txt).  A Karma dungeon: Skeletons (Lv. 1), Skeleton Kings (Lv. 2) or
Skull Lords (Lv. 3-4) that drop Karma as they are hit; Archangels ("Blessed
Angels" from Lv. 2) that drop no Karma but leave a chest of Karma; one room
(room 4) with Azrael and two escorts, Azrael dropping a lot of Karma plus a
chest; and a Death God boss: Memetes, Hell King Hades, Death God Lodaga and,
on Lv. 4, Hellborn Dilias (240,000 HP).  The stat Imps were a rare
alternative to room 4; an archive record is fixed, so they are left out.
Sacred Land of the Souls is the older name of the same dungeon (Gathering of
Souls); its Lv. 1-3 have the Lv. 1-3 rosters and bosses.

HOW.  Every enemy starts from a real 2014 capture (SOURCES), scaled to the
level's difficulty with gen_designed_missions.scale().  The difficulty is the
wiki-era energy (15/20/25/30): this data version halved the energy (MST
8/10/13/15) but kept the battles.  The Karma figures are the wiki's, per
level.  Nothing can be caught (authored_capture with chance 0, so the 40%
capture floor leaves it alone), and the chests opt out of the shared random
Mimic.  energy_cost, exp, zel and karma are the mission_mst values, as Balance
Forge requires.

The files go to deploy/balance-overlays/karma-dungeon/<id>.json; publishing
one = copying it to <balance_root>/server/missions/ (docs/features/
BATTLE_BUILDER.md).  --check exits 1 when a committed file differs from what
this script would write.
"""

import argparse
import copy
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_designed_missions import EXPONENT, scale  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / 'deploy' / 'archive'
MST = ROOT / 'deploy' / 'mst'
OUT = ROOT / 'deploy' / 'balance-overlays' / 'karma-dungeon'

# Captured 2014 enemies: name -> (mission id, monster id, that mission's energy).
SOURCES = {
    # The Box of Desires is the Wednesday dungeon of the same Enhancement area.
    'skeleton': (100200, 60550, 10),        # The Box of Desires Lv. 1
    'skeleton_king': (100201, 60600, 20),   # The Box of Desires Lv. 2
    'archangel': (842, 50600, 15),          # Heavenly Intent
    'azrael': (842, 53800, 15),             # Heavenly Intent
    'memetes': (100802, 61050, 30),         # Dweller in the Darkness Lv. 3
    'hades': (900000, 61100, 20),           # Out the Window Lv. 2
    'lodaga': (553, 61150, 14),             # Wicked Hostility (story boss)
}

# Layouts used by captured waves of the same size.
POSITIONS = {
    1: ['180:302'],
    3: ['180:302', '120:248', '96:352'],
    4: ['160:240', '215:300', '160:360', '105:300'],
}

# Wave plans: R regular, A Archangel (Karma chest), Z Azrael, B boss.  Room 4
# is Azrael's: on the wiki its escorts drop a little more Karma than the other
# regulars, but the client keeps ONE MonsterMst row per monster id (the first
# it is sent), so every copy of an id in a mission must carry the same numbers
# and the escorts are plain regulars.
# The wiki's per-clear Karma (Lv. 1 at least 10,000, Lv. 2 16,000-22,000,
# Lv. 3 32,000-40,000) depends on how many Angels appear: 5 on Lv. 1 and 6 on
# the 8-battle levels reach it.
PLAN_6 = ['RRA', 'RAR', 'ARA', 'ZRR', 'ARR', 'B']
PLAN_8 = ['RRA', 'RAR', 'RAR', 'ZRR', 'RRA', 'ARR', 'ARR', 'B']
PLAN_8_TWIN = ['RRA', 'RAR', 'RAR', 'ZZRR', 'RRA', 'ARR', 'ARR', 'B']

# Per level, from the wiki page.  karma: what each enemy can drop while hit
# (karma_max_drop); chest: the Karma in its chest.
LEVELS = {
    1: {'difficulty': 15, 'plan': PLAN_6, 'regular': 'skeleton', 'boss': 'memetes', 'azrael_acts': 2,
        'karma': {'R': 50, 'Z': 2000, 'B': 3000}, 'chest': {'A': 700, 'Z': 1500}},
    2: {'difficulty': 20, 'plan': PLAN_8, 'regular': 'skeleton_king', 'boss': 'hades', 'azrael_acts': 3,
        'karma': {'R': 115, 'Z': 3000, 'B': 5000}, 'chest': {'A': 1000, 'Z': 2000}},
    3: {'difficulty': 25, 'plan': PLAN_8, 'regular': 'skull_lord', 'boss': 'lodaga', 'azrael_acts': 4,
        'karma': {'R': 210, 'Z': 5000, 'B': 10000}, 'chest': {'A': 2000, 'Z': 4000}},
    4: {'difficulty': 30, 'plan': PLAN_8_TWIN, 'regular': 'skull_lord', 'boss': 'dilias', 'azrael_acts': 4,
        'karma': {'R': 700, 'Z': 10000, 'B': 55000}, 'chest': {'A': 2500, 'Z': 5000}},
}

# mission id -> level.  100000-100002 are Sacred Land of the Souls Lv. 1-3
# (Gathering of Souls, open the same Monday): stand-ins too, from Box of
# Desires and Shining Stage templates, with catchable Mimic, Diva Hatsune Miku
# and Dragon Mimic bosses.
MISSIONS = {100050: 1, 100051: 2, 100052: 3, 100053: 4, 100000: 1, 100001: 2, 100002: 3}

NO_MIMICS = {'chance': 0, 'monsters': []}


def find_monster(records: dict, mission_id: int, monster_id: int) -> dict:
    for stage in records[mission_id]['stages']:
        for monster in stage['battle_monsters']:
            if monster['id'] == monster_id:
                return copy.deepcopy(monster)
    raise SystemExit(f'monster {monster_id} not in mission {mission_id}')


def scaled(monster: dict, ratio: float) -> dict:
    out = dict(monster)
    for stat, exponent in EXPONENT.items():
        out[stat] = scale(monster[stat], ratio, exponent)
    return out


def profile(records: dict, name: str, difficulty: float) -> dict:
    """A captured enemy scaled to `difficulty`, or a derived one."""
    if name == 'skull_lord':
        # No capture fought a Skull Lord: the Skeleton King one level up.
        # The wiki: it attacks several times per round and hits hard.
        base = profile(records, 'skeleton_king', difficulty)
        return {**base, 'id': 63700, 'name': 'Skull Lord', 'unit_id': 60052, 'ai_id': 30002000,
                'act_min': 1, 'act_max': 2, 'atk': scale(base['atk'], 1.25, 1)}
    if name == 'lodaga':
        # The only capture is a story boss at energy 14.  Its ATK/DEF are
        # kept; its HP is the Vortex scale (Dweller in the Darkness Lv. 3,
        # energy 30: Shadow Oboro 145,000 HP), which is ~120,000 at 25.
        return {**find_monster(records, *SOURCES['lodaga'][:2]), 'hp': 120000}
    if name == 'dilias':
        # No capture.  Lodaga's profile one level up; HP from the wiki.  It
        # can act 3 times a round and uses its BB (Doorway to Hades, single
        # target) and SBB (Soul Eater, all) through the AI of the captured
        # two-skill boss Shadow Oboro: skill 2 every 4th action, skill 1 at 25%.
        base = scaled(find_monster(records, *SOURCES['lodaga'][:2]), difficulty / 25)
        return {**base, 'id': 63200, 'name': 'Hellborn Dilias', 'unit_id': 60105, 'hp': 240000,
                'ai_id': 31030000, 'act_min': 3, 'act_max': 3,
                'skills': [{'skill_id': 3460, 'show_name': True}, {'skill_id': 3461, 'show_name': True}],
                'ailment_resists': {'poison': 100, 'weak': 100, 'sick': 100, 'injury': 50,
                                    'curse': 100, 'paralysis': 100}}
    mission_id, monster_id, energy = SOURCES[name]
    return scaled(find_monster(records, mission_id, monster_id), difficulty / energy)


def drop_count(karma: int, role: str) -> int:
    """How many pieces an enemy's Karma drops in.  The client pays karma_max_drop
    as karma_drop_count pieces of max/count, floored, one per lucky hit (10%
    a hit before Karma leader skills and spheres; MonsterParty::entryMonsters
    @0x10c1eec, DamageData::update @0x103cec0), so the total must divide
    evenly.  Captured regulars drop in 5 pieces, captured bosses in 10-20."""
    if not karma:
        return 0
    # Azrael healed every turn in the original, so it could be hit for its
    # Karma for a long time; this one cannot heal, so fewer, bigger pieces.
    count = 5 if karma < 1000 or role == 'Z' else 10 if karma <= 20000 else 25
    if karma % count:
        raise SystemExit(f'{karma} Karma does not split into {count} equal drops')
    return count


def enemy(base: dict, role: str, level: dict, position: str, boss_wave: bool) -> dict:
    out = dict(base)
    out['position'] = position
    out['unit_drop_id'] = out['unit_drop_level'] = out['unit_drop_type'] = out['unit_drop_chance'] = 0
    out['authored_capture'] = True
    karma = level['karma'].get(role, 0)
    out['karma_max_drop'] = karma
    out['karma_drop_count'] = drop_count(karma, role)
    chest = level['chest'].get(role)
    out['treasure_chest_chance'] = 100 if chest else 0
    out['treasure_drops'] = [{'target_type': 2, 'weight': 100, 'amount': chest, 'target_id': ''}] if chest else []
    if role == 'Z':
        out['act_min'] = 2
        out['act_max'] = level['azrael_acts']
    if not boss_wave:
        out.pop('skills', None)
        out.pop('ailment_resists', None)
    out.pop('act_rate', None)
    return out


def build(records: dict, mst_row: dict, mission_id: int, level_no: int) -> dict:
    level = LEVELS[level_no]
    diff = level['difficulty']
    bases = {'R': profile(records, level['regular'], diff),
             'A': profile(records, 'archangel', diff), 'Z': profile(records, 'azrael', diff),
             'B': profile(records, level['boss'], diff)}
    stages = []
    for wave in level['plan']:
        positions = POSITIONS[len(wave)]
        boss = wave == 'B'
        stages.append({'is_boss': boss, 'first_attack_rate': 0,
                       'battle_monsters': [enemy(bases[role], role, level, positions[i], boss)
                                           for i, role in enumerate(wave)]})
    if len(stages) != int(mst_row['69vnphig']):
        raise SystemExit(f'{mission_id}: {len(stages)} waves, mission_mst says {mst_row["69vnphig"]}')
    return {'id': mission_id, 'name': mst_row['0iAIR2LP'],
            'zel': int(float(mst_row.get('Rs7bCE3t') or 0)), 'karma': int(float(mst_row.get('HTVh8a65') or 0)),
            'exp': int(float(mst_row.get('d96tuT2E') or 0)), 'energy_cost': int(float(mst_row['A8DEK5ob'])),
            'stages': stages, 'random_mimics': NO_MIMICS}


def karma_ceiling(record: dict) -> int:
    """All the Karma one clear can give: every drop and every chest."""
    return sum(m['karma_max_drop'] + sum(t['amount'] for t in m['treasure_drops'] if t['target_type'] == 2)
               for s in record['stages'] for m in s['battle_monsters'])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=pathlib.Path, default=OUT)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = {r['id']: r for r in json.loads((ARCHIVE / 'mission.json').read_text())}
    mst = {int(r['j28VNcUW']): r for r in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    stale = []
    for mission_id, level_no in MISSIONS.items():
        record = build(records, mst[mission_id], mission_id, level_no)
        # Byte-identical to what Balance Forge publishes (battlemodel.file_bytes).
        text = json.dumps(record, indent=1, ensure_ascii=False) + '\n'
        path = args.out / f'{mission_id}.json'
        print(f'{mission_id} {record["name"]}: {len(record["stages"])} waves, '
              f'boss {record["stages"][-1]["battle_monsters"][0]["name"]} '
              f'{record["stages"][-1]["battle_monsters"][0]["hp"]:,} HP, '
              f'up to {karma_ceiling(record):,} Karma, {record["energy_cost"]} energy')
        if args.check:
            if not path.exists() or path.read_text() != text:
                stale.append(path.name)
            continue
        args.out.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    if stale:
        print('out of date:', ', '.join(stale))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
