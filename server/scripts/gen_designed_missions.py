"""Design battles for missions no capture covers, from the closest real one.

python scripts/gen_designed_missions.py --captures DIR --sgtext DIR [--check] [--only CATEGORY ...]

WHY.  The archive holds battles for about 1,400 of MissionMst's 3,433 missions;
nothing else survives of the rest (the client never stored waves, the live
server sent them at MissionStart, and the 2014 captures cover ~200 names).  A
mission without a record is either hidden (PermitPlace) or served mission 10's
waves relabelled.  The owner's call (2026-10-16): design fights for those from
the partial data we do have, and label them as designed.

HOW, per missing mission (only missions with no archive record are touched):
  1. Template: a REAL captured battle (gen_captured_missions' library: every
     2014 capture whose wave count matches its mission), chosen in order of
     preference -- same dungeon, then the same quest series ("X Lv. 3" for
     "X Lv. 1"), then the same element theme (keywords in the dungeon and
     mission names), then any; ties go to the closest difficulty, then the
     closest wave count.
  2. Waves: MissionMst.battle_count.  Extra template waves are dropped from the
     middle (the boss wave is always last); missing ones repeat the template's
     non-boss waves in order.
  3. Difficulty: the mission's energy cost, the best predictor in the
     captures (log-log fit over 181 captured missions: HP ~ energy^1.11,
     r=0.73; ATK ~ energy^0.30; DEF ~ energy^0.84).  Modes with no energy cost
     use their "Lv. N"/floor/expedition number instead (see difficulty()).
     Stats scale by (target / template)^exponent, clamped to x0.25..x6.
  4. Rewards: zel/karma/exp/energy from MissionMst; drops, capture chances and
     treasure chests as the template had them.  gen_capture_rules.py, run last,
     then removes every unit drop the original game never offered.
Not filled: missions whose dungeon or area the MSTs do not define, those
behind the 999999999 "never" prerequisite, and JP-only (untranslated) ones --
none of them can be reached.
Every record written is listed in deploy/archive/mission_sources.json with its
method and template, so a later capture or wiki source can replace it.
"""

import argparse
import collections
import copy
import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import (ARCHIVE, MISSIONS, MST, SOURCES, Converter, common_layouts,  # noqa: E402
                                   load_captures)
EXPONENT = {'hp': 1.11, 'atk': 0.30, 'def': 0.84}
CLAMP = (0.25, 6.0)
ELEMENT_WORDS = {
    'fire': ('fire', 'flame', 'blaze', 'burn', 'ember', 'inferno', 'volcan', 'scorch', 'magma', 'sun'),
    'water': ('water', 'ice', 'frost', 'snow', 'zero', 'sea', 'ocean', 'aqua', 'tide', 'freez', 'winter'),
    'earth': ('earth', 'forest', 'tree', 'wood', 'leaf', 'flower', 'garden', 'wind', 'sky', 'nature', 'green'),
    'thunder': ('thunder', 'lightning', 'spark', 'volt', 'storm', 'electric', 'iron'),
    'light': ('light', 'holy', 'heaven', 'angel', 'shining', 'divine', 'god', 'saint', 'star'),
    'dark': ('dark', 'shadow', 'moon', 'demon', 'night', 'abyss', 'devil', 'hell', 'death', 'black'),
}
SERIES = re.compile(r'\s*(lv\.?\s*\d+|floor\s*\d+|stage\s*\d+|battle\s*\d+|\d+)\s*$', re.I)
NUMBER = re.compile(r'(\d+)\s*$')
CJK = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')


def category(mission_id: int, area_id: int, dungeon_id: int) -> str:
    if area_id == 3000001:
        return 'frontier_gate'
    if area_id == 1000010:
        return 'frontier_hunter'
    if mission_id < 100000:
        return 'story'
    if area_id < 200000:
        return 'vortex'
    if 2000000 <= dungeon_id < 2200000:
        return 'trials'
    if 5000000 <= area_id < 6000000:
        return 'grand_quest'
    return 'event'


def theme(*names: str) -> str | None:
    text = ' '.join(names).lower()
    scores = {e: sum(text.count(w) for w in words) for e, words in ELEMENT_WORDS.items()}
    best = max(scores, key=scores.get)
    return best if scores[best] else None


def difficulty(mst: dict, name: str, cat: str) -> float:
    """Energy-equivalent difficulty.  Energy-free modes get a number-based one
    anchored on the captured Uncharted Frontier 6 (energy 0, played like an
    energy-30 quest)."""
    energy = float(mst.get('A8DEK5ob') or 0)
    if energy > 0:
        return energy
    n = NUMBER.search(name)
    level = int(n[1]) if n else 1
    if cat == 'frontier_hunter':
        return 30 * (1 + 0.04 * (level - 6))
    if cat == 'frontier_gate':
        return 10 + 2 * level
    return 20


class Library:
    """Real captured battles as archive records, with what selection needs."""

    def __init__(self, conv: Converter, captures: dict, mst: dict, layouts: dict, dungeons: dict):
        self.items = []
        for mid, name in conv.mission_names.items():
            runs = captures.get(name.strip().lower())
            row = mst.get(mid)
            if not runs or not row:
                continue
            waves = int(row.get('69vnphig') or 0)
            run = next(((f, c) for f, c in runs if len(c['battles']) == waves), None)
            if not run:
                continue
            record, _ = conv.record(mid, row, run[1], layouts)
            if not record:
                continue
            elements = collections.Counter(m['element'] for b in run[1]['battles'] for m in b['monsters'])
            dungeon = int(row['MHx05sXt'])
            dname = dungeons.get(dungeon, '')
            cat = category(mid, int(row['VjCY7rX4']), dungeon)
            self.items.append({
                'mission': mid, 'record': record, 'dungeon': dungeon, 'source': run[0],
                'series': SERIES.sub('', name).strip().lower(),
                'element': elements.most_common(1)[0][0], 'cat': cat,
                'difficulty': difficulty(row, name, cat),
            })

    def pick(self, dungeon: int, series: str, element: str | None, diff: float, waves: int, cat: str):
        def closeness(t):
            return (abs(math.log(diff / t['difficulty'])), abs(len(t['record']['stages']) - waves))
        for how, pool in (
                ('same dungeon', [t for t in self.items if t['dungeon'] == dungeon]),
                ('same series', [t for t in self.items if series and t['series'] == series]),
                ('same mode and element', [t for t in self.items if t['cat'] == cat and element
                                           and t['element'] == element]),
                ('same element', [t for t in self.items if element and t['element'] == element]),
                ('same mode', [t for t in self.items if t['cat'] == cat]),
                ('closest difficulty', self.items)):
            if pool:
                return how, min(pool, key=closeness)
        return None, None


def fit_waves(stages: list, waves: int) -> list:
    boss, body = stages[-1], stages[:-1]
    if waves <= 1:
        return [copy.deepcopy(boss)]
    if len(body) >= waves - 1:
        keep = [round(i * (len(body) - 1) / max(waves - 2, 1)) for i in range(waves - 1)] if waves > 2 else [0]
        out = [copy.deepcopy(body[i]) for i in keep]
    else:
        out = [copy.deepcopy(body[i % len(body)]) for i in range(waves - 1)] if body else \
              [copy.deepcopy(boss) for _ in range(waves - 1)]
    for stage in out:
        stage['is_boss'] = False
    final = copy.deepcopy(boss)
    final['is_boss'] = True
    return out + [final]


def scale(value: int, ratio: float, exponent: float) -> int:
    factor = min(max(ratio ** exponent, CLAMP[0]), CLAMP[1])
    scaled = value * factor
    step = 10 if scaled >= 100 else 1
    return max(1, int(round(scaled / step) * step))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True)
    ap.add_argument('--sgtext', type=pathlib.Path, required=True)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--only', nargs='*', help='categories to fill (default: all)')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    by_id = {r['id']: r for r in records}
    mst = {int(m['j28VNcUW']): m for m in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    conv = Converter(args.sgtext)
    dungeon_names = {int(k.split('_')[3]): v for k, v in
                     __import__('gen_captured_missions').sgtext(args.sgtext, 'Dungeons_D').items()
                     if re.match(r'MST_DUNGEONS_DUNGEON_\d+_NAME$', k)}
    library = Library(conv, load_captures(args.captures), mst, common_layouts(records), dungeon_names)
    sources = json.loads(SOURCES.read_text()) if SOURCES.exists() else {}
    dungeon_rows = {int(d['MHx05sXt']) for d in json.loads((MST / 'dungeon_mst.json').read_text())['01pzrTCm']}
    area_ids = {int(a['VjCY7rX4']) for a in json.loads((MST / 'area_mst.json').read_text())['3SG2wX0R']}

    made = collections.Counter()
    hows = collections.Counter()
    for mid, row in sorted(mst.items()):
        if mid <= 0 or mid in by_id:   # MissionMst row 0 is a placeholder
            continue
        name = conv.mission_names.get(mid) or row.get('0iAIR2LP') or ''
        dungeon, area = int(row['MHx05sXt']), int(row['VjCY7rX4'])
        # Unreachable rows stay unfilled: a dungeon or area the MSTs do not
        # define, the 999999999 "never" prerequisite, and JP-only text.
        if (dungeon not in dungeon_rows or area not in area_ids
                or '999999999' in str(row.get('HSRhkf70') or '') or CJK.search(name)):
            continue
        cat = category(mid, area, dungeon)
        if args.only and cat not in args.only:
            continue
        waves = max(1, int(row.get('69vnphig') or 1))
        diff = difficulty(row, name, cat)
        element = theme(dungeon_names.get(dungeon, ''), name)
        how, tpl = library.pick(dungeon, SERIES.sub('', name).strip().lower(), element, diff, waves, cat)
        if not tpl:
            continue
        ratio = diff / tpl['difficulty']
        stages = fit_waves(tpl['record']['stages'], waves)
        for stage in stages:
            for m in stage['battle_monsters']:
                for stat, exponent in EXPONENT.items():
                    m[stat] = scale(m[stat], ratio, exponent)
        by_id[mid] = {
            'id': mid, 'name': name,
            'zel': int(float(row.get('Rs7bCE3t') or 0)), 'karma': int(float(row.get('HTVh8a65') or 0)),
            'exp': int(float(row.get('d96tuT2E') or 0)), 'energy_cost': int(float(row.get('A8DEK5ob') or 0)),
            'stages': stages,
        }
        sources[str(mid)] = {'method': 'designed', 'template': tpl['mission'], 'template_source': tpl['source'],
                             'picked_by': how, 'difficulty': round(diff, 1),
                             'scale': round(ratio, 3), 'element_theme': element}
        made[cat] += 1
        hows[how] += 1

    print(f'library: {len(library.items)} captured battle(s)')
    print('designed:', dict(made), 'total', sum(made.values()))
    print('templates picked by:', dict(hows))
    if not args.check and made:
        conv.ai.merge_into(ARCHIVE / 'ai.json')   # template enemies carry captured AI
        MISSIONS.write_text(json.dumps([by_id[k] for k in sorted(by_id)], indent=1, ensure_ascii=False) + '\n',
                            encoding='utf-8')
        SOURCES.write_text(json.dumps(dict(sorted(sources.items(), key=lambda kv: int(kv[0]))),
                                      indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
