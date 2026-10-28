"""Put the ORIGINAL enemies into designed missions, from the Global wiki.

python scripts/gen_wiki_missions.py --hints FILE --captures DIR --sgtext DIR [--check]

SOURCE.  bravefrontierglobal.fandom.com (CC BY-SA), parsed into one hint row
per mission (`--hints`, the coverage_by_mission.json the source survey wrote):
  quest  the {{Quest}} block: boss unit ids, boss HP (46 quests), and the units
         the quest lets you capture -- i.e. its regular enemies.
  fh     a Frontier Hunter round list: per round, the alternative enemy groups
         (unit ids or names) that round could roll.
The wiki gives no ATK/DEF and no AI.  So the ENEMIES come from the wiki and the
NUMBERS from the closest recorded battle: when the unit itself fought in a
2014 capture, its captured profile (stats, AI, skills, drops) is used, scaled
to this mission's difficulty like gen_designed_missions does; otherwise the
designed record's own enemy in that slot keeps its numbers and only changes
unit.  A wiki boss HP replaces the scaled HP outright.  A slot's unit drop
moves to its new unit here; gen_capture_rules.py, run last, keeps it only
when the original game let you capture that unit.

Only missions gen_designed_missions wrote, and old records the bulk
regeneration made (gen_captured_missions.regenerated), are touched; each is
listed in mission_sources.json with method "designed+wiki" or
"regenerated+wiki" and the fields it used.
A Frontier Hunter round rolled one of its groups at random on the live server;
an archive record is fixed, so each mission takes the group its id selects
(id mod group count) -- different missions of a season show different groups.
"""

import argparse
import collections
import copy
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import (ARCHIVE, MISSIONS, MST, Converter, common_layouts,  # noqa: E402
                                   load_captures, regenerated)
from gen_designed_missions import EXPONENT, SOURCES, Library, category, difficulty, scale  # noqa: E402

MAX_WAVE = 6
WIKI_AI_BASE = 900_000_000
MAX_AI_ROWS = 24
MAX_BOSS_SKILLS = 20        # the most any authored boss carries is 17 (Grahdens)
REGULAR_HP_PERCENTILE = 0.98  # regulars skip captures stronger than this share of captured regulars
HP_UNDER = re.compile(r'(?:at\s*)?(?:<|below|under|less than)\s*(\d+)\s*%\s*hp|hp\s*(?:<|below|under)\s*(\d+)\s*%', re.I)
TURN_N = re.compile(r'^\s*turn\s+(\d+)\b', re.I)
EVERY_N = re.compile(r'every\s+(\d+)\s+turns?', re.I)


def ai_row(conds: list, percent: float, skill: int | None, target: int = 2) -> dict:
    if skill is not None:
        conds = [{'type': 'skill', 'parameter': skill}] + conds
    return {'priority': 0, 'percent': float(percent), 'act_target': target, 'search_term': 'random',
            'self_conditions': conds[:5], 'party_conditions': [],
            'action': {'type': 'skill' if skill is not None else 'attack', 'flag_changes': [],
                       'unknown_bool': True, 'unknown_int_1': 0, 'unknown_int_2': 0}}


def trigger_rows(trigger: str, skill: int, heals: bool) -> list:
    """One wiki trigger as AI rows, in the client's own vocabulary
    (ai.kdl): once at turn N, every Nth turn, once below X% HP, and -- for
    "unspecified" or prose the vocabulary cannot state -- a 30% chance."""
    t = str(trigger or '').strip()
    target = 1 if heals else 2
    head = t.split('>')[0]
    if (m := TURN_N.match(head)):
        return [ai_row([{'type': 'act', 'parameter': int(m[1])}, {'type': 'limited_act', 'parameter': 1}], 100, skill, target)]
    if (m := EVERY_N.search(head)):
        return [ai_row([{'type': 'actbetween', 'parameter': int(m[1])}], 100, skill, target)]
    if (m := HP_UNDER.search(head)):
        threshold = int(m[1] or m[2])
        if '>' in t:        # a phase: below X% the boss keeps using it
            return [ai_row([{'type': 'hp_pr_under', 'parameter': threshold}], 30, skill, target)]
        return [ai_row([{'type': 'hp_pr_under', 'parameter': threshold}, {'type': 'limited_act', 'parameter': 1}],
                    100, skill, target)]
    return [ai_row([], 30, skill, target)]


def regular_hp(monster: dict, diff: float) -> float:
    """A captured enemy's HP scaled to energy 30, for comparing captures."""
    return monster['hp'] * (30 / diff) ** EXPONENT['hp']


class Profiles:
    """unit id -> captured enemy profiles [(monster, difficulty)]."""

    def __init__(self, library: Library):
        self.by_unit = collections.defaultdict(list)
        for item in library.items:
            for stage in item['record']['stages']:
                for m in stage['battle_monsters']:
                    self.by_unit[m['unit_id']].append((m, item['difficulty'], stage['is_boss']))
        # Elite captures (the hero quests: Vargas, Eze and Lance at 150k HP for
        # 20 energy) would put mini-bosses into every regular wave, so a regular
        # only takes a profile whose HP, at equal difficulty, is not above
        # REGULAR_HP_PERCENTILE of all captured regulars.
        hps = sorted(regular_hp(m, d) for cands in self.by_unit.values() for m, d, boss in cands if not boss)
        self.regular_cap = hps[int(REGULAR_HP_PERCENTILE * (len(hps) - 1))] if hps else float('inf')

    def get(self, unit_id: int, diff: float, boss: bool) -> dict | None:
        cands = self.by_unit.get(unit_id)
        if not cands:
            return None
        same_role = [c for c in cands if c[2] == boss]
        if not boss:
            same_role = [c for c in same_role if regular_hp(c[0], c[1]) <= self.regular_cap]
        if not same_role and not boss:
            # Never give a regular a boss's numbers: Vargas, captured only as a
            # 235k-HP boss, filled four regular waves of Flame of Virtue Lv.3.
            # The caller keeps the slot's own numbers instead.
            return None
        same_role = same_role or cands
        m, d, _ = min(same_role, key=lambda c: abs(c[1] - diff))
        out = copy.deepcopy(m)
        for stat, exponent in EXPONENT.items():
            out[stat] = scale(out[stat], diff / d, exponent)
        return out


def reunit(monster: dict, unit_id: int, name: str) -> dict:
    """The slot's numbers and behaviour on another (ordinary, drawable) unit."""
    out = copy.deepcopy(monster)
    out.pop('visuals', None)        # a unit-backed enemy must not carry boss art
    out['unit_id'] = unit_id
    out['name'] = name
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--hints', type=pathlib.Path, nargs='+', required=True,
                    help='coverage rows: the missing missions\' and coverage_regenerated.json')
    ap.add_argument('--page-hints', type=pathlib.Path,
                    help='page_hints.json: per-wave enemies, HP and boss forms from wiki pages')
    ap.add_argument('--captures', type=pathlib.Path, required=True)
    ap.add_argument('--sgtext', type=pathlib.Path, required=True)
    ap.add_argument('--sprites', type=pathlib.Path,
                    default=pathlib.Path.home() / 'bf-archive/assets/content/unit/img',
                    help='unit/img/ of the asset archive: a unit with no unit_anime_<id>.png there cannot be drawn')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    by_id = {r['id']: r for r in records}
    sources = json.loads(SOURCES.read_text())
    mst = {int(m['j28VNcUW']): m for m in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    conv = Converter(args.sgtext)
    layouts = common_layouts(records)
    dnames = {}
    library = Library(conv, load_captures(args.captures), mst, layouts, dnames)
    profiles = Profiles(library)
    # Only units the CDN can draw: the client downloads unit_anime_<id>.png for
    # every enemy, and a missing sheet stops the battle with a DLC error.
    sprites = {p.name for p in args.sprites.glob('unit_anime_*.png')}
    if not sprites:
        raise SystemExit(f'no unit sprites under {args.sprites}')
    units = {u['id']: u['name'] for u in json.loads((ARCHIVE / 'unit.json').read_text())
             if f"unit_anime_{u['id']}.png" in sprites}
    by_name = {}
    for uid, name in sorted(units.items(), reverse=True):
        by_name[name.strip().lower()] = uid

    def unit_of(token: str) -> int | None:
        token = str(token).strip()
        if token.isdigit():
            return int(token) if int(token) in units else None
        return by_name.get(token.lower())

    def monster_for(uid: int, slot: dict, diff: float, boss: bool) -> dict:
        prof = profiles.get(uid, diff, boss)
        stats['enemies from a captured profile' if prof else 'enemies on the slot\'s numbers'] += 1
        out = prof if prof else reunit(slot, uid, units[uid])
        out['name'] = units[uid]
        out['id'] = conv.monster_id(units[uid], uid)
        if out.get('unit_drop_id'):
            out['unit_drop_id'] = uid
        return out

    def place(wave: list) -> list:
        wave = wave[:max(layouts)]
        layout = layouts[len(wave)]
        for m, pos in zip(wave, layout):
            m['position'] = pos
        return wave

    def unit_for_enemy(e: dict) -> int | None:
        if e.get('unit_id') in units:
            return e['unit_id']
        m = re.match(r'unit ills anime (\d+)', str(e.get('image') or '').lower())
        return int(m[1]) if m and int(m[1]) in units else None

    def wiki_boss_ai(m: dict, boss_skills: list) -> None:
        """The boss's skills and triggers from the page as its AI; skills the
        skill table does not know by name are left out."""
        names, rows = [], []
        for sk in boss_skills:
            sid = conv.ai.skill_id(sk.get('name', ''))
            if sid is None:
                stats['wiki boss skills not in skill_mst'] += 1
                continue
            if sid not in names:
                names.append(sid)
            heals = bool(re.search(r'\b(heal|recover|restore)', str(sk.get('effect') or ''), re.I))
            rows += trigger_rows(sk.get('trigger', ''), names.index(sid) + 1, heals)
        if not rows:
            return
        # Once-only and turn triggers first, then periodic, then chances; a
        # plain attack closes the list.  The client walks rows in order.
        def rank(r):
            types = {c['type'] for c in r['self_conditions']}
            return 0 if 'limited_act' in types else 1 if 'actbetween' in types else 2 if 'hp_pr_under' in types else 3
        names = names[:MAX_BOSS_SKILLS]
        rows = [r for r in rows if all(c['type'] != 'skill' or c['parameter'] <= len(names)
                                       for c in r['self_conditions'])]
        rows.sort(key=rank)
        rows = rows[:MAX_AI_ROWS - 1]     # the most any authored AI has is 23
        rows.append(ai_row([], 100, None))
        for i, r in enumerate(rows, 1):
            r['priority'] = i
        m['skills'] = [{'skill_id': sid, 'show_name': True} for sid in names]
        m['ai_id'] = conv.ai.register(rows, f'wiki {m["name"]}', WIKI_AI_BASE)
        stats['bosses with wiki skills'] += 1

    def apply_page(mid: int, ph: dict, stages: list, diff: float) -> bool:
        """Per-wave enemies from a wiki page.  Alternatives a wave could roll
        (spawn options) and alternate wave sets (variants) are picked by
        mission id; the page's waves fill the record's LAST waves."""
        variants = sorted({str(w['variant']) for w in ph.get('waves') or [] if w.get('variant') is not None})
        chosen = variants[mid % len(variants)] if variants else None
        by_no = {}
        for w in ph.get('waves') or []:
            if w.get('variant') is not None and str(w['variant']) != chosen:
                continue
            by_no.setdefault(int(w['wave']) if str(w['wave']).isdigit() else 0, []).append(w)
        waves = [opts[mid % len(opts)]['enemies'] for _, opts in sorted(by_no.items())]
        waves = [w for w in waves if w]
        if not waves:
            return False
        boss_hp = {}
        for b in ph.get('boss') or []:
            for form in b.get('forms') or []:
                if form.get('hp'):
                    boss_hp.setdefault(str(b.get('name', '')).lower(), int(form['hp']))
        skills_by_boss = collections.defaultdict(list)
        for sk in ph.get('skills') or []:
            skills_by_boss[str(sk.get('boss') or '').strip().lower()].append(sk)
        k = min(len(waves), len(stages))
        for stage, enemies in zip(stages[len(stages) - k:], waves[len(waves) - k:]):
            wave = []
            for j, e in enumerate(enemies[:MAX_WAVE]):
                slot = stage['battle_monsters'][j % len(stage['battle_monsters'])]
                uid = unit_for_enemy(e)
                if uid:
                    m = monster_for(uid, slot, diff, stage['is_boss'])
                else:
                    # A special boss with its own art.  Its visuals wait on the
                    # Juggernaut investigation (same mechanism); until then it
                    # fights on the slot's drawable unit under its real name.
                    m = copy.deepcopy(slot)
                    m['name'] = str(e.get('name') or m['name'])
                    m['id'] = conv.monster_id(m['name'], m['unit_id'])
                    stats['special bosses on a stand-in unit'] += 1
                hp = e.get('hp') or boss_hp.get(str(e.get('name', '')).lower())
                if hp:
                    m['hp'] = int(hp)
                    stats['enemies with wiki HP'] += 1
                for stat in ('atk', 'def'):
                    if e.get(stat):
                        m[stat] = int(e[stat])
                boss_skills = skills_by_boss.get(str(e.get('name', '')).strip().lower())
                if boss_skills:
                    wiki_boss_ai(m, boss_skills)
                wave.append(m)
            stage['battle_monsters'] = place(wave)
        return True

    stats = collections.Counter()
    modes = {'vortex', 'event', 'frontier_hunter', 'trials', 'frontier_gate', 'grand_quest', 'story'}
    hints = [h for path in args.hints for h in json.loads(path.read_text())]
    if args.page_hints:
        hints += [{'mission_id': ph['mission_id'], 'pagehint': ph} for ph in json.loads(args.page_hints.read_text())]
    for hint in hints:
        mid = hint['mission_id']
        src = sources.get(str(mid))
        if mid not in by_id:
            continue
        if not src and regenerated(by_id[mid]):
            # An old regenerated record: the wiki names its real enemies too.
            src = sources[str(mid)] = {'method': 'regenerated'}
        if not src or src.get('method') not in ('designed', 'regenerated'):   # once per mission
            continue
        record, row = by_id[mid], mst[mid]
        cat = category(mid, int(row['VjCY7rX4']), int(row['MHx05sXt']))
        diff = difficulty(row, record['name'], cat)
        stages = record['stages']
        used = []

        # A template from the same dungeon or quest series already IS this
        # quest's lineup (a captured sister level); the wiki adds only HP.
        sister = src.get('picked_by') in ('same dungeon', 'same series')

        if 'quest' in hint:
            q = hint['quest']
            bosses = [u for u in dict.fromkeys(unit_of(t) for t in q.get('boss_unit_ids') or []) if u][:MAX_WAVE]
            # "Captures" lists what the quest lets you catch: its regular
            # enemies in most quests, but just the featured hero in character
            # quests -- so it only counts with two or more non-boss units.
            regulars = [u for u in dict.fromkeys(unit_of(t) for t in q.get('captures') or [])
                        if u and u not in bosses]
            if len(regulars) < 2:
                regulars = []
            if sister:
                regulars = []       # the sister level's waves stay; its boss may differ
            if bosses and sister:
                # Same fight, other form of the boss (Lv. 1 Traveler Alma, Lv. 3
                # Hero Alma): swap the unit, keep the sister boss's numbers and AI.
                slots = stages[-1]['battle_monsters']
                wave = [reunit(slots[i % len(slots)], u, units[u]) for i, u in enumerate(bosses)]
                for m in wave:
                    m['id'] = conv.monster_id(m['name'], m['unit_id'])
                    if m.get('unit_drop_id'):
                        m['unit_drop_id'] = m['unit_id']
                hps = [int(h) for h in q.get('boss_hp') or [] if str(h).isdigit()]
                for m, hp in zip(wave, hps):
                    m['hp'] = hp
                stages[-1]['battle_monsters'] = place(wave)
                used.append('boss' + ('+hp' if hps else ''))
                bosses = []
            if bosses:
                slot = stages[-1]['battle_monsters'][0]
                wave = [monster_for(u, slot, diff, True) for u in bosses]
                hps = [int(h) for h in q.get('boss_hp') or [] if str(h).isdigit()]
                for m, hp in zip(wave, hps):
                    m['hp'] = hp
                stages[-1]['battle_monsters'] = place(wave)
                used.append('boss' + ('+hp' if hps else ''))
            if regulars:
                k = 0
                for stage in stages[:-1]:
                    wave = []
                    for slot in stage['battle_monsters']:
                        wave.append(monster_for(regulars[k % len(regulars)], slot, diff, False))
                        k += 1
                    stage['battle_monsters'] = place(wave)
                used.append('enemies')
        elif 'pagehint' in hint:
            if apply_page(mid, hint['pagehint'], stages, diff):
                used.append('page waves')
        elif 'fh' in hint:
            rounds = hint['fh'].get('rounds') or []
            for i, stage in enumerate(stages):
                if not rounds:
                    break
                rnd = rounds[min(i * len(rounds) // len(stages), len(rounds) - 1)]
                groups = [g for g in rnd.get('groups') or [] if g]
                if not groups:
                    continue
                group = [u for u in (unit_of(t) for t in groups[mid % len(groups)]) if u][:MAX_WAVE]
                if not group:
                    continue
                slots = stage['battle_monsters']
                stage['battle_monsters'] = place([monster_for(u, slots[j % len(slots)], diff, stage['is_boss'])
                                                  for j, u in enumerate(group)])
            used.append('rounds')

        if used:
            src['method'] = src['method'] + '+wiki'
            src['wiki'] = {'used': used, 'page': (hint.get('quest') or hint.get('fh') or hint.get('pagehint') or {}).get('page')}
            stats[cat] += 1

    print('upgraded from the wiki:', dict(stats), 'missions', sum(v for k, v in stats.items() if k in modes))
    if not args.check and stats:
        conv.ai.merge_into(ARCHIVE / 'ai.json')
        MISSIONS.write_text(json.dumps([by_id[k] for k in sorted(by_id)], indent=1, ensure_ascii=False) + '\n',
                            encoding='utf-8')
        SOURCES.write_text(json.dumps(sources, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
