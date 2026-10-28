"""Real numbers for regular enemies still on the bulk regeneration's placeholder block.

python3 -I scripts/gen_placeholder_regulars.py --captures DIR --sgtext DIR [--check] [--report FILE]

Run after gen_boss_waves.py (the last step of the boss-wave chain).

WHY.  The bulk regeneration wrote hp 1000 / atk 290 / def 50 for every enemy it
had no numbers for.  gen_boss_waves gave those quests' bosses real stats, but in
a quest where EVERY regular (every monster of a non-boss wave) is still on that
block, the waves before the boss stay trivial: about 300 story, Vortex and event
quests.  Captures are left alone, and so are the tutorial and scripted fights.

THE YARDSTICK is the 2014 captures' ORDINARY regulars: gen_wiki_missions.Profiles'
material without what it mixes in.  Left out are
  - mid-quest boss waves: Converter.record marks only the last wave a boss wave,
    but the captures name the others' groups ("中boss1", "Mboss2", "LD M3 MidB
    Grp1"), and those hold 30-40k HP Zahhaks and Liza in 12-energy story quests;
  - elite captures, whose ordinary regulars sit over ELITE times their mode's
    (the 50-energy hero trials at 90-235k HP scaled to 30 energy, 12-30x the
    Vortex's 7.7k; Trial 01);
  - unnamed mini-bosses: a slot over STRONG times its capture's median regular
    (Affectionate Flames Lv. 3's lone 60,000-HP Firedrake among 7-8k regulars);
  - the tutorial (scripted 300-HP Mossies that difficulty() rates like a
    20-energy quest);
  - a monster whose name is not its unit's or the slot's (Moody Gloomy's 99,999
    DEF and Eerie Mandragora's 90,000 HP are not Gloomy's or Mandragora's).

HOW, once per monster id of such a quest (the client keeps the first MonsterMst
row it is sent per id, so every enemy with that id -- in the waves and in the
quest's mission_variants.json alternatives -- gets the same numbers; an
alternative that already has numbers gives them to its id):
  1. A farm unit keeps its captured block unscaled: the element families (two or
     more names that differ only in an element word: Pot, Idol, Totem, Spirit,
     Nymph, Ghost, Golem, ...) and metal / crystal fodder (DEF above HP).  Every
     captured family keeps one block per unit at every energy and mode (every Pot
     32,000/1,000/250 from 12 to 35 energy), so a unit without its own capture
     takes its family's most common block.  Not below the lowest energy the
     captures show that family at (Pots from 12, Totems only at 50).
  2. Else its unit's own capture as an ordinary regular in this quest's mode
     (story, Vortex, event: at equal energy story regulars have ~2.8x the HP of
     the other two), at the nearest difficulty -- the most common block there --
     scaled as gen_designed_missions scales (HP^1.11, ATK^0.30, DEF^0.84), and
     only when that stays inside its x0.25..x6 clamp.
  3. Else a typical regular of the nearest captured battle, picked as
     gen_designed_missions picks its templates (same dungeon, then quest series,
     mode and element theme, element theme, mode, any; ties to the closest
     difficulty): the median-HP ordinary regular of that capture, farm units
     left out, scaled likewise.  Only captures with an energy cost serve as
     templates (Frontier Hunter's levels are calibrated; the Metal Parades not).
Only hp, atk and def change; unit ids, monster ids, drops, AI, skills and
positions stay, and every boss (every monster of a boss wave) stays as it is.
A committed overlay file of a changed quest (deploy/balance-overlays/
*/<id>.json, served ahead of the archive once published) whose regulars are on
the placeholder block takes the same numbers per monster id.
The run is idempotent: once a quest's regulars carry real numbers it no longer
qualifies.
"""

import argparse
import collections
import copy
import json
import math
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import (ARCHIVE, MISSIONS, MST, ROOT, SOURCES, TUTORIAL, Converter,  # noqa: E402
                                   common_layouts, load_captures, sgtext)
from gen_designed_missions import (CLAMP, EXPONENT, SERIES, Library, category, difficulty, scale,  # noqa: E402
                                   theme)
from gen_wiki_missions import regular_hp  # noqa: E402

VARIANTS = ARCHIVE / 'mission_variants.json'
OVERLAYS = ROOT / 'deploy' / 'balance-overlays'
PLACEHOLDER = (1000, 290, 50)
STATS = ('hp', 'atk', 'def')
ELEMENT_WORD = re.compile(r'\b(fire|water|earth|thunder|light|dark)\b', re.I)
BOSS_GROUP = re.compile(r'boss|midb', re.I)
ELITE = 5
STRONG = 2.5


def stats_of(m: dict) -> tuple:
    return m['hp'], m['atk'], m['def']


def norm(name: str) -> str:
    return ' '.join(str(name or '').lower().split())


def family_key(name: str, rarity) -> tuple | None:
    """"Earth Spirit" (2*) -> ('spirit', 2); None for a name without an element word."""
    if not ELEMENT_WORD.search(name):
        return None
    return norm(ELEMENT_WORD.sub(' ', name)), rarity


def most_common(blocks: list) -> tuple:
    """The block seen most often; a tie goes to the median of the tied blocks."""
    counts = collections.Counter(blocks)
    top = max(counts.values())
    tied = sorted(b for b, n in counts.items() if n == top)
    return tied[(len(tied) - 1) // 2]


class Regulars:
    """The captures' ordinary regulars (see THE YARDSTICK), by unit."""

    def __init__(self, library: Library, captures: dict, units: dict):
        raw = {f: c for runs in captures.values() for f, c in runs}
        self.units = units
        members = collections.defaultdict(set)
        for u in units.values():
            if k := family_key(u['name'], u.get('rarity')):
                members[k].add(u['id'])
        self.family = {uid: k for k, us in members.items()
                       if len({norm(units[u]['name']) for u in us}) > 1 for uid in us}
        slots = []                                  # (capture, monster)
        for item in library.items:
            for battle, stage in zip(raw[item['source']]['battles'], item['record']['stages']):
                if stage['is_boss'] or any(BOSS_GROUP.search(c.get('group name') or '') for c in battle['monsters']):
                    continue
                slots += [(item, m) for m in stage['battle_monsters']
                          if norm(m['name']) == norm(units.get(m['unit_id'], {}).get('name'))]
        by_capture = collections.defaultdict(list)
        for item, m in slots:
            if self.ordinary(m):
                by_capture[item['mission']].append(regular_hp(m, item['difficulty']))
        level = {item['mission']: statistics.median(by_capture[item['mission']])
                 for item in library.items if by_capture.get(item['mission'])}
        by_mode = collections.defaultdict(list)
        for item in library.items:
            if item['mission'] in level:
                by_mode[item['cat']].append(level[item['mission']])
        self.elite = {item['mission'] for item in library.items if item['mission'] in level
                      and level[item['mission']] > ELITE * statistics.median(by_mode[item['cat']])}
        median = {}
        for item, m in slots:
            if self.ordinary(m):
                median.setdefault(item['mission'], []).append(m['hp'])
        median = {k: statistics.median(v) for k, v in median.items()}
        self.by_unit = collections.defaultdict(list)
        for item, m in slots:
            if item['mission'] in self.elite or (self.ordinary(m) and m['hp'] > STRONG * median[item['mission']]):
                continue
            self.by_unit[m['unit_id']].append((item, m))

    def ordinary(self, m: dict) -> bool:
        """Not fodder: neither DEF above HP nor an element-family farm unit."""
        return m['def'] <= m['hp'] and m['unit_id'] not in self.family

    def typical(self, item: dict) -> dict | None:
        """The median-HP ordinary regular of one capture."""
        regs = sorted((m for i, m in (x for xs in self.by_unit.values() for x in xs)
                       if i is item and self.ordinary(m)),
                      key=lambda m: (m['hp'], m['atk'], m['def'], m['unit_id']))
        return regs[(len(regs) - 1) // 2] if regs else None

    def fixed_block(self, uid: int, diff: float) -> tuple:
        """Step 1: (block, source unit) for a farm unit, else (None, None)."""
        own = [stats_of(m) for _, m in self.by_unit.get(uid, [])]
        fam = self.family.get(uid)
        if fam is None:
            if own and all(b[2] > b[0] for b in own):          # metal / crystal fodder
                return most_common(own), uid
            return None, None
        kin = [(u, item, m) for u in sorted(self.by_unit) if self.family.get(u) == fam
               for item, m in self.by_unit[u]]
        if not kin:
            return None, None
        block = most_common(own) if own else most_common([stats_of(m) for _, _, m in kin])
        if block[2] <= block[0] and diff < min(item['difficulty'] for _, item, _ in kin):
            return None, None
        source = uid if own else min(u for u, _, m in kin if stats_of(m) == block)
        return block, source

    def own_profile(self, uid: int, diff: float, cat: str) -> tuple:
        """Step 2: (numbers, capture mission, its difficulty), else (None, None, None).
        Never for a farm unit: its block does not scale (a 150,000-HP Totem below
        the energy its family was captured at takes step 3)."""
        cands = [(item, m) for item, m in self.by_unit.get(uid, []) if item['cat'] == cat]
        if uid in self.family or any(m['def'] > m['hp'] for _, m in self.by_unit.get(uid, [])) or not cands:
            return None, None, None
        gap = min(abs(math.log(diff / item['difficulty'])) for item, _ in cands)
        near = [(item, m) for item, m in cands if abs(math.log(diff / item['difficulty'])) == gap]
        block = most_common([stats_of(m) for _, m in near])
        item = next(item for item, m in near if stats_of(m) == block)
        ratio = diff / item['difficulty']
        if not CLAMP[0] <= ratio ** EXPONENT['hp'] <= CLAMP[1]:
            return None, None, None
        return (tuple(scale(v, ratio, EXPONENT[k]) for v, k in zip(block, STATS)),
                item['mission'], item['difficulty'])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True, help='missions_parsed/ folder')
    ap.add_argument('--sgtext', type=pathlib.Path, required=True, help='content/sgtext/ folder')
    ap.add_argument('--check', action='store_true', help='report only; write nothing')
    ap.add_argument('--report', type=pathlib.Path, help='write every decision as JSON')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_text(encoding='utf-8'))
    sources = json.loads(SOURCES.read_text(encoding='utf-8'))
    variants = json.loads(VARIANTS.read_text(encoding='utf-8'))
    mst = {int(m['j28VNcUW']): m for m in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    units = {u['id']: u for u in json.loads((ARCHIVE / 'unit.json').read_text(encoding='utf-8'))}
    conv = Converter(args.sgtext)
    dungeon_names = {int(k.split('_')[3]): v for k, v in sgtext(args.sgtext, 'Dungeons_D').items()
                     if re.match(r'MST_DUNGEONS_DUNGEON_\d+_NAME$', k)}
    captures = load_captures(args.captures)
    library = Library(conv, captures, mst, common_layouts(records), dungeon_names)
    library.items = [t for t in library.items if t['mission'] not in TUTORIAL]
    regulars = Regulars(library, captures, units)
    typical = {t['mission']: regulars.typical(t) for t in library.items}
    templates = copy.copy(library)
    templates.items = [t for t in library.items if t['mission'] not in regulars.elite and typical[t['mission']]
                       and (float(mst[t['mission']].get('A8DEK5ob') or 0) > 0 or t['cat'] == 'frontier_hunter')]
    overlays = {int(p.stem): p for p in sorted(OVERLAYS.glob('*/*.json')) if p.stem.isdigit()}

    report, why, overlaid = [], collections.Counter(), []
    for r in records:
        mid = r['id']
        if mid in TUTORIAL or r.get('script_monsters') or (sources.get(str(mid)) or {}).get('method') == 'captured':
            continue
        regular_stages = {i for i, s in enumerate(r['stages']) if not s.get('is_boss')}
        regs = [m for i in sorted(regular_stages) for m in r['stages'][i]['battle_monsters']]
        if not regs or any(stats_of(m) != PLACEHOLDER for m in regs):
            continue
        alternatives = [v['monster'] for v in variants.get(str(mid), []) if v['stage'] in regular_stages]
        bosses = [m for s in r['stages'] if s.get('is_boss') for m in s['battle_monsters']]
        boss_ids = {m['id'] for m in bosses}
        row = mst[mid]
        dungeon = int(row['MHx05sXt'])
        cat = category(mid, int(row['VjCY7rX4']), dungeon)
        diff = difficulty(row, r['name'], cat)
        template = None

        same_id = collections.defaultdict(list)
        for m in regs + alternatives:
            same_id[m['id']].append(m)
        decided = {}                                # monster id -> (numbers, how, source)
        for m in regs + alternatives:
            if m['id'] in decided:
                continue
            if m['id'] in boss_ids:
                raise SystemExit(f'mission {mid}: regular id {m["id"]} is also a boss id')
            if len({x['unit_id'] for x in same_id[m['id']]}) > 1:
                raise SystemExit(f'mission {mid}: monster id {m["id"]} stands for several units')
            real = sorted({stats_of(x) for x in same_id[m['id']]} - {PLACEHOLDER})
            if len(real) > 1:
                raise SystemExit(f'mission {mid}: monster id {m["id"]} already has two stat blocks')
            if real:                                # an alternative already has numbers: its id keeps them
                decided[m['id']] = (real[0], 'kept', None)
                continue
            uid = m['unit_id']
            block, unit = regulars.fixed_block(uid, diff)
            if block:
                decided[m['id']] = (block, 'fixed block', {'unit': unit})
                continue
            numbers, source, at = regulars.own_profile(uid, diff, cat)
            if numbers:
                decided[m['id']] = (numbers, 'own profile', {'mission': source, 'difficulty': at})
                continue
            if template is None:
                how, tpl = templates.pick(dungeon, SERIES.sub('', r['name']).strip().lower(),
                                          theme(dungeon_names.get(dungeon, ''), r['name']), diff,
                                          len(r['stages']), cat)
                if tpl is None:
                    raise SystemExit(f'mission {mid}: no captured battle to take a regular from')
                ratio = diff / tpl['difficulty']
                pick = typical[tpl['mission']]
                template = (tuple(scale(v, ratio, EXPONENT[k]) for v, k in zip(stats_of(pick), STATS)),
                            {'mission': tpl['mission'], 'difficulty': tpl['difficulty'], 'picked_by': how,
                             'regular': pick['name']})
            decided[m['id']] = (template[0], 'nearest capture', template[1])

        for numbers, how, _ in decided.values():
            if numbers == PLACEHOLDER:
                raise SystemExit(f'mission {mid}: a regular would keep the placeholder block ({how})')
        for m in regs + alternatives:
            m['hp'], m['atk'], m['def'] = decided[m['id']][0]
        if mid in overlays:
            overlay = json.loads(overlays[mid].read_text(encoding='utf-8'))
            hits = [m for s in overlay['stages'] if not s.get('is_boss') for m in s['battle_monsters']
                    if stats_of(m) == PLACEHOLDER and m['id'] in decided]
            for m in hits:
                m['hp'], m['atk'], m['def'] = decided[m['id']][0]
            if hits:
                overlaid.append((overlays[mid], overlay))
        for _, how, _ in decided.values():
            why[how] += 1
        # A co-boss on a regular's captured profile (gen_wiki_missions) can match a
        # regular of that unit or family: same capture, not a boss's numbers.  Reported.
        boss_blocks = {stats_of(b) for b in bosses}
        report.append({
            'mission': mid, 'name': r['name'], 'mode': cat, 'difficulty': round(diff, 1),
            'regulars': {str(i): {'numbers': n, 'how': h, 'from': s} for i, (n, h, s) in decided.items()},
            'median_regular_hp': statistics.median(m['hp'] for m in regs),
            'weakest_boss_hp': min((b['hp'] for b in bosses), default=0),
            'regulars_matching_a_boss': sum(1 for m in regs if stats_of(m) in boss_blocks),
            'overlay': str(overlays[mid].relative_to(ROOT)) if mid in overlays else None,
        })

    print(len(report), 'quests,', sum(why.values()), 'monster ids:', dict(why),
          f'| {len(regulars.elite)} elite captures left out | {len(overlaid)} overlay(s) updated')
    if args.report:
        args.report.write_text(json.dumps(report, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    if not args.check and report:
        MISSIONS.write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        VARIANTS.write_text(json.dumps(variants, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        for path, overlay in overlaid:
            path.write_text(json.dumps(overlay, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
