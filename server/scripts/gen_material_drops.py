"""Material drops: put the original per-enemy item drops into the chests.

python scripts/gen_material_drops.py --captures DIR --sgtext DIR --wiki-quests FILE --wiki-hints FILE... [--check]

WHY.  The 2014 captures list item drops per enemy (Green Grass 3%, Hades Claw
0.5%, ...), but no generator converted them ("listed in the report only"), so
the archive had 1,360 item chest entries in all and players saw almost no
materials.  The server rolls ONE chest per enemy at MissionStart
(MissionArchiver::encodeTreasureDrop: treasure_chest_chance, then one pick by
weight; target_type 4 is an item) and MissionEnd pays what the client reports.

ITEMS, per enemy, from the closest captured battle:
  reference  the mission's own capture; else its designed template; else the
             capture gen_designed_missions would pick (same dungeon, series,
             mode and element, then closest difficulty).
  enemy      the same unit in the reference's matching wave, else anywhere in
             the reference, else in the capture of closest difficulty that has
             it (its own drops, even none), else the reference enemy in the
             same slot.  Waves map like fit_waves: the boss wave to the boss.
  wiki       items a matched wiki quest lists under `drops` (no rate there) are
             added at the captures' median item rate.
Only story, Vortex, event and Frontier Hunter missions, or captured ones; not
the tutorial, trials or scripted fights.

CHEST.  Each item keeps its captured chance: the chest chance grows by the
items' total and every outcome keeps its odds (weights in 0.01%):
  chance' = ceil(chance + sum(item %)) (max 100); old entries share chance*100,
  item i weighs round(p_i * 100).
An enemy whose chest already holds an item is left alone, so a second run
changes nothing.  Only treasure_chest_chance and treasure_drops change.
"""

import argparse
import collections
import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import (MISSIONS, MST, SOURCES, TUTORIAL, Converter, common_layouts,  # noqa: E402
                                   load_captures, sgtext)
from gen_designed_missions import SERIES, Library, category, difficulty, theme  # noqa: E402

ITEM_TYPE = 4
MODES = {'story', 'vortex', 'event', 'frontier_hunter'}
WIKI_LINK = re.compile(r'\[\[([^\]|]+)')


def wave_index(k: int, waves: int, ref_waves: int) -> int:
    if k == waves - 1 or ref_waves == 1:
        return ref_waves - 1
    return min(ref_waves - 2, round(k * (ref_waves - 2) / max(waves - 2, 1)))


def merge(monster: dict, items: list) -> bool:
    """Fold [(item id, percent)] into the enemy's chest with every outcome's odds kept."""
    drops = monster.get('treasure_drops') or []
    chance = int(monster.get('treasure_chest_chance') or 0) if drops else 0
    if not items or any(d['target_type'] == ITEM_TYPE for d in drops) or (drops and not chance):
        return False                  # (a chest list that never rolls is left as authored)
    new_chance = min(100, math.ceil(chance + sum(p for _, p in items)))
    old_total = sum(d['weight'] for d in drops) or 1
    out = [{**d, 'weight': max(1, round(d['weight'] * chance * 100 / old_total))} for d in drops] if chance else []
    out += [{'target_type': ITEM_TYPE, 'target_id': str(i), 'weight': max(1, round(p * 100)), 'amount': 1}
            for i, p in items]
    monster['treasure_chest_chance'] = new_chance
    monster['treasure_drops'] = out
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True)
    ap.add_argument('--sgtext', type=pathlib.Path, required=True)
    ap.add_argument('--wiki-quests', type=pathlib.Path, required=True)
    ap.add_argument('--wiki-hints', type=pathlib.Path, nargs='*', default=[])
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    sources = json.loads(SOURCES.read_text())
    mst = {int(m['j28VNcUW']): m for m in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    rows = json.loads((MST / 'item_mst.json').read_text())
    item_id = {r['c7Z6xDB2'].strip().lower(): int(r['kixHbe54'])
               for r in (next(iter(rows.values())) if isinstance(rows, dict) else rows)}
    conv = Converter(args.sgtext)
    dungeon_names = {int(k.split('_')[3]): v for k, v in sgtext(args.sgtext, 'Dungeons_D').items()
                     if re.match(r'MST_DUNGEONS_DUNGEON_\d+_NAME$', k)}
    captures = load_captures(args.captures)
    library = Library(conv, captures, mst, common_layouts(records), dungeon_names)

    stats = collections.Counter()

    def items_of(cap_monster: dict) -> list:
        out = []
        for i in cap_monster.get('item drop') or []:
            iid = item_id.get(i['_name'].strip().lower())
            if iid and i.get('percent'):
                out.append((iid, float(i['percent'])))
            elif not iid:
                stats['unknown item names'] += 1
        return out

    refs, by_unit = {}, collections.defaultdict(list)     # mission -> (item, battles); unit -> [(diff, items)]
    for item in library.items:
        run = next(c for f, c in captures[conv.mission_names[item['mission']].strip().lower()] if f == item['source'])
        battles = [[(int(m['unit id']), items_of(m)) for m in b['monsters']] for b in run['battles']]
        refs[item['mission']] = (item, battles)
        for b in battles:
            for unit, its in b:
                by_unit[unit].append((item['difficulty'], its))
    rates = sorted(p for _, b in refs.values() for w in b for _, its in w for _, p in its)
    median = rates[len(rates) // 2]

    quests = json.loads(args.wiki_quests.read_text())
    wiki_items = {}
    for path in args.wiki_hints:
        for hint in json.loads(path.read_text()):
            q = hint.get('quest')
            row = q and next((r for r in quests.get(q['quest'], []) if r.get('page') == q['page']), None)
            names = WIKI_LINK.findall((row or {}).get('drops') or '')
            ids = [item_id[n.strip().lower()] for n in names if n.strip().lower() in item_id]
            if ids:
                wiki_items[hint['mission_id']] = ids

    for record in records:
        mid = record['id']
        row = mst.get(mid)
        if mid in TUTORIAL or record.get('script_monsters') or not row:
            continue
        cat = category(mid, int(row['VjCY7rX4']), int(row['MHx05sXt']))
        if cat not in MODES and mid not in refs:
            continue
        name = record['name']
        diff = difficulty(row, name, cat)
        template = (sources.get(str(mid)) or {}).get('template')
        if mid in refs:
            ref = refs[mid]
        elif template in refs:
            ref = refs[template]
        else:
            _, tpl = library.pick(int(row['MHx05sXt']), SERIES.sub('', name).strip().lower(),
                                  theme(dungeon_names.get(int(row['MHx05sXt']), ''), name), diff,
                                  len(record['stages']), cat)
            ref = refs.get(tpl['mission']) if tpl else None
        if not ref:
            continue
        battles = ref[1]
        everywhere = {u: its for b in battles for u, its in b}
        touched = False
        for k, stage in enumerate(record['stages']):
            wave = battles[wave_index(k, len(record['stages']), len(battles))]
            in_wave = {u: its for u, its in wave}
            for j, m in enumerate(stage['battle_monsters']):
                unit = m['unit_id']
                if unit in in_wave:
                    its = in_wave[unit]
                elif unit in everywhere:
                    its = everywhere[unit]
                elif by_unit.get(unit):
                    its = min(by_unit[unit], key=lambda c: abs(math.log(diff / c[0])))[1]
                else:
                    its = wave[j % len(wave)][1]
                have = {i for i, _ in its}
                its = its + [(i, median) for i in wiki_items.get(mid, []) if i not in have]
                if merge(m, its):
                    touched = True
                    stats['enemies given items'] += 1
        stats['missions'] += touched

    print(f'median captured item rate {median}%;', dict(stats))
    if not args.check and stats['enemies given items']:
        MISSIONS.write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
