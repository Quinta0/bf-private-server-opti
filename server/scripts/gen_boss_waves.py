"""Rebuild boss waves from the Global wiki and the game's own tables.

python3 -I scripts/gen_boss_waves.py --pages DIR [DIR ...] --hints COVERAGE.json [...] --sgtext DIR
                                     [--sprites DIR] [--monster-art DIR] [--check] [--report FILE]

Run after gen_boss_own_ai.py and gen_boss_wiki_skills.py.  Records that are 2014
captures, the tutorial or scripted stay as they are.  For the others:

0. A boss the server refuses to send (monster id 0, or boss art without its
   animation) is drawn on the unit other quests draw that boss on (Cursed Estia
   Lv1-9 are Princess Estia 850015), else on the drawable unit of that name
   (Xie Jing -> Demonic Witch Xie'Jing 860158).

1. THE WIKI'S BOSSES.  The quest's {{Quest}} / {{SQuest}} block (wiki_quest_blocks,
   matched by name, then battle count and energy) lists its bosses as unit ids or
   names, else in |bossname= as a display name over a unit's art ("Golden Razorhawk"
   over Holy Beast Belardo) or over monster art ("Goddess Mora" over Monster ills
   anime 0044), else as the bold headers of the notes' skill sections ("'''Blazing
   Mare'''").  A boss marked "(Wave 5)" fights in that wave, not in another boss wave;
   a "???" with neither a unit nor art (Goddess Mora's) joins later and is not in it.
   A boss slot that only attacks and holds another form of a
   wiki boss's evolution line (unit_evo_mst) becomes the wiki's form (Jaegar -> Ice
   King Jaegar).  When NO wiki boss is in the boss wave, the wave holds stand-ins (a
   regeneration that lost the real boss: pots, totems, another room's group); if
   every wiki boss is a drawable unit or monster art the client ships, and the single
   boss stage has a slot for each, the wave becomes the wiki's bosses alone, a lone
   boss where captured lone bosses stand.  More wiki bosses than stand-ins (Evie and
   her two familiars over one Keymaster) grow the wave where the captures' bosses of
   that count stand, the bosses after the first acting once and taking no co-boss
   HP, unless the notes say the bosses are picked at random ("one of the following"),
   that one of them is summoned ("Summons Pumburkiny(s)": it joins later, and its
   summoner's kit counts on it), or a lone stand-in fought with a notes kit that no
   wiki boss has a section of its own for.  The quests in UNMODELLED_KITS keep their
   stand-ins: their real kits hang on conditions the notes cannot be read for.  A boss with no
   drawable unit but its art (WST-1096, monster_anime_0042.png) is a special-art
   monster: unit 0 with visuals, its cgg and idle / atk / move cgs files, the
   movement and frame fields of an archive monster with that art, else of the
   archive's art bosses (Juggernaut's), their frames fitted into a shorter attack;
   its element is the wiki's {{UnitImage|element|X}} on its name, its notes header,
   that art elsewhere on the wiki, the zone, else the stand-in's.  A changed boss
   cannot be captured as itself (authored_capture, no unit drop: the server then
   gives its line's base-form drop, as for every boss).  The monster id is the text
   entry of the boss's name, as gen_captured_missions does.
2. STATS.  A boss on the bulk regeneration's placeholder block (1000 HP / 290 ATK /
   50 DEF), or a replaced one, in a mission whose regulars have real numbers, takes
   the 2014 captures' boss-to-regular ratios per boss count in the wave (a lone boss
   has about 17.6x the median regular's HP and 2.5x the strongest regular's), the
   larger of the two, times this mission's regulars (for a boss the art or grown-wave
   rules place, only regulars with real ATK and DEF; a grown wave's minions take the
   median such regular's numbers, else the captures' median regulars of the nearest
   energy, and never more than their boss's).  A mission with no real
   regulars (all placeholders) gives such a boss the captured bosses of the nearest
   energy instead (the median of the five closest with as many bosses in the wave,
   scaled to this energy as gen_designed_missions scales: HP^1.11, ATK^0.30,
   DEF^0.84); without an energy a replaced boss keeps the strongest stand-in's
   numbers, its ATK at least the regulars'.  The wiki's own HP for that boss
   (its bossNhp, "Has N HP" in its section, the sum of its phases, a
   "<center>''300,000 HP''</center>" under its art) wins, and a
   co-boss the wiki gives no HP takes the median of the wave's wiki HPs.  Metal,
   crystal and ghost regulars (DEF above HP) are no yardstick and are left out.
3. SKILLS.  A boss that only attacks takes its own wiki section first (as
   gen_boss_wiki_skills reads it: skills, triggers, "Next turn" chains; a lone boss
   whose notes group its skills under "'''SKILLS'''" / "'''Used at < 50% HP:'''"
   takes the groups, their titles as triggers; a boss the art or grown-wave rules
   place resolves its skill names with rare_anchors, so a common name such as "Empty
   Seal" takes the copy in its own run); else, when
   the slot is its unit (a "Raging Nick" drawn on another unit is not), the monster
   copies of its unit's own Brave Burst / Super / Ultimate: skill_mst rows with that
   name that are no unit's skill, the lowest id (the captures used the lowest copy
   in 148 of 149 cases: Head Bandit Zaza fought with Warrior's Execution 6690, not
   his unit BB 30062).  AI: UBB once below 25% HP, SBB once below 50% HP and then
   20% while below it, BB 35%, then a plain attack.
4. ONE ROW PER ID.  The client keeps the first MonsterMst row it is sent per monster
   id, so a boss whose id an earlier enemy or a variant of the mission sends with
   other numbers or behaviour gets an id of its own: another text id of its name,
   else a synthetic one (the server sends the name as the text's fallback).
5. SCRIPT TRIGGERS.  The client's F_MISSION_SCRIPT_MST swaps a monster by id when
   another drops below some HP (an event "3=<trigger>,<hp>" with a command
   "6=<target>,x,y" or "11=<target>", MonsterParty::changeMonster).  The game closes
   at that HP unless MissionStart describes the target, so a boss on a trigger
   whose target the record does not describe takes an id of its own, as in 4, and
   the script never fires on it (Goddess Mora 3150 -> "????" 36451 in Sparkling
   Rainbow Sky: the wiki's "???" summons have neither unit nor art).
Report: which rule changed which boss.
"""
import argparse
import copy
import json
import os
import pathlib
import re
import statistics
import sys
import unicodedata
from collections import Counter, defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_boss_wiki_skills import (WIKI_AI_BASE, SkillIndex, build_kit, finish_rows, needs_second_action,  # noqa: E402
                                  norm, row, sections)
import gen_captured_missions as gcm  # noqa: E402
from gen_designed_missions import EXPONENT, scale  # noqa: E402
import wiki_quest_blocks as wq  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'deploy' / 'archive'
MST = ROOT / 'deploy' / 'mst'
TUTORIAL = {1, 2}
UNIT_SKILL_KEYS = ('nj9Lw7mV', 'iEFZ6H19', 'cb0P4mp1')     # unit_mst: BB, SBB, UBB
STATS = ('hp', 'atk', 'def')
PLACEHOLDER = (1000, 290, 50)
SINGLE_BOSS_POSITION = '180:302'    # where every captured lone boss stands
RARITY = re.compile(r'\s*\((\d)\s*★\)\s*$')
UNIT_ART = re.compile(r'unit[ _]ills[ _](?:anime|thum|full|battle)[ _](\d+)', re.I)
MONSTER_ART = re.compile(r'monster[ _]ills', re.I)
MONSTER_ANIME = re.compile(r'monster[ _]ills[ _]anime[ _]([^.|\]]+?)\.(?:gif|png)', re.I)
ELEMENT = re.compile(r'\{\{\s*UnitImage\s*\|\s*element\s*\|\s*([A-Za-z]+)\s*\}\}', re.I)
HEADER_ELEMENT = re.compile(r"\{\{\s*UnitImage\s*\|\s*element\s*\|\s*([A-Za-z]+)\s*\}\}(?:\s*\{\{[^{}]*\}\})*\s*'''([^']+)'''",
                            re.I)
ELEMENT_IDS = {'fire': 1, 'water': 2, 'earth': 3, 'thunder': 4, 'light': 5, 'dark': 6}
WAVE = re.compile(r'\(\s*wave\s*(\d+)\s*\)', re.I)
CENTER = re.compile(r'<center>(.*?)</center>', re.I | re.S)
TAG = re.compile(r'<[^>]*>')
UNNAMED = re.compile(r'^\?+$')
SCRIPT_TRIGGER = re.compile(r'3=([0-9]+),([0-9]+)')     # a monster below <hp>% HP
SCRIPT_SWAP = re.compile(r'(?:6|11)=([0-9]+)(?:,.*)?')  # changeMonster(<target>, x, y)
SUMMONS = re.compile(r'\bsummon(?:s|ed|ing)?\b(?!\s+by\b)(.*)', re.I)
RANDOM_BOSSES = re.compile(r'\bboss(?:es)?\b[^.\n]*\b(?:at random|randomly|one of the|of the following)', re.I)
SKILL_GROUP = re.compile(r'^\W*(?:skills?|used|when|at|below|under|every|after|on)\b', re.I)
ART_PREFIXES = ('monster', 'unit')     # /monster/ also holds a few unit-named sets (Melfel's unit_anime_3002224)
ART_CGS = (('cgs_idle', ('{p}_idle_cgs_{s}.csv', '{p}_idle_{s}.csv', '{p}_cgs_idle_{s}.csv')),
           ('cgs_move', ('{p}_move_cgs_{s}.csv',)),
           ('cgs_atk', ('{p}_atk_cgs_{s}.csv', '{p}_atk_{s}.csv', '{p}_cgs_atk_{s}.csv')))
ART_FIELDS = ('effect_frame', 'damage_frame', 'drop_check_count', 'move_speed', 'attack_move_type',
              'back_move_type', 'after_image', 'skill_move_type', 'cursor_disp_pos', 'hp_disp_pos')
# Quests the art / grown-wave rules would give their real bosses, whose wiki kits hang on a
# condition the notes parser cannot read: built anyway, the boss would wipe the party on a
# timer or at random where the real one gave the player an answer.  They keep their stand-ins.
UNMODELLED_KITS = {
    10170: "Mora's Shadow: Scorn for the Weak (lethal) only when Goddess's Charm's ATK down stays uncleansed",
    10270: "Beiorg's Shadow: Non-Paralyzing World (fixed 100,000) only when its Paralysis fails",
    10470: "Mordlim's Shadow: the 300% Miasma World is the turn-50 one; the barrier drops to 60% when Sick",
    10870: "Kalon's Shadow: Empty Seal: Disjunction (KO all) only when nuked from above 50% to below 40%",
    8301301: "Florence: the 4,000,000 HP shield falls when the summoned Undead Tethers die",
    8301692: "Creation God Odin: Divine Creation (10,000%) is announced a turn ahead",
    8301711: "Illusionist Melfel: her turn-N nukes belong to five element phases",
    10766: "Supreme Goddess Mora: Despair / Extremity (9,999 to all) only after one of her four Sealed Spirits dies",
    22063: "Kagenui: Shadow Slash (70-80% of HP) only once Genba is defeated",
    25530004: "Harakhte: the Cry buffs (+400-1000% ATK) answer the Falcon Guardians' deaths",
    8301731: "Vash: the strongest .45 Long Gun copies (100% mitigation) only after the 4th reload",
    10734: "War Demon Shusui: the wiki's numbers are the older 4720-4725 kit, the names resolve to the later one",
    1577: "Lucius: God, Multifarious and Amorphous Lucius are forms fought one after another",
}
UNIT_INFO = re.compile(r'\{\{\s*UnitInfo\s*\|\s*([^|}]+)', re.I)
BR = re.compile(r'<br\s*/?>|\n', re.I)
LINK = re.compile(r'\[\[(?!(?:File|Image):)([^|\]]+)(?:\|[^\]]*)?\]\]', re.I)
LEVEL = re.compile(r'\s*lv\.?\s*\d+\s*$', re.I)
CAPTURE_FROM = re.compile(r"captur\w*\s+(?:\[\[)?([^\]\n.]+?)(?:\]\])?\s+(?:upon|after|when|by)\s+defeating\s+(?:\[\[)?([^\]\n.]+?)(?:\]\])?\s*(?:[.,;]|$)", re.I)
BOSS_DROP_CHANCE = json.loads((pathlib.Path(__file__).resolve().parents[1] / 'deploy' / 'archive' / 'boss_drops.json')
                              .read_text(encoding='utf-8'))['chance_by_line_rarity']
PREEMPT = re.compile(r'pre-?emptive(?:ly)?|attacks? first|strikes? first|first strike', re.I)
LINKED = re.compile(r'\[\[(?!(?:File|Image):)([^|\]]+)(?:\|[^\]]*)?\]\]', re.I)
ALL_WAVES = re.compile(r'\b(?:all|every)\s+(?:the\s+)?waves?\b', re.I)
NOT_ENGLISH = re.compile(r'[぀-ヿ㐀-鿿＀-￯]')   # unit.json keeps a few JP names


def stats_of(m: dict) -> tuple:
    return m['hp'], m['atk'], m['def']


FARM_NAME = re.compile(r'ghost|king|god|crystal|frog|arton|\bimps?\b|elgif|pegasus|metal|jewel|bulb|tone|pot|totem|idol', re.I)


def borrowed_block(m: dict) -> bool:
    """A real boss on a metal enemy's numbers (Lucifer at 10 HP / 100,000 DEF: its
    designed mission took them from a Metal Parade template) -- not a farm unit, whose
    10 HP is the point (ticket #11)."""
    return m['hp'] <= 1000 and m['def'] > m['hp'] and not FARM_NAME.search(m['name'])


def fodder(m: dict) -> bool:
    """A metal / crystal / ghost enemy (Light Ghost 10 HP / 999,999 DEF): no yardstick for a boss."""
    return m['def'] > m['hp']


def round_stat(v: float) -> int:
    step = 10 if v >= 100 else 1
    return max(1, int(round(v / step) * step))


def boss_ratios(records: list, sources: dict) -> dict:
    """Boss count in the wave (1, 2, 3+) -> (median ratios, max ratios): the captures'
    boss (hp, atk, def) over the median and over the strongest regular of its mission."""
    out = defaultdict(lambda: ([], []))
    for r in records:
        if (sources.get(str(r['id'])) or {}).get('method') != 'captured':
            continue
        regs = [m for s in r['stages'] if not s.get('is_boss') for m in s['battle_monsters'] if not fodder(m)]
        if not regs:
            continue
        med = [statistics.median(m[k] for m in regs) for k in STATS]
        top = [max(m[k] for m in regs) for k in STATS]
        for s in r['stages']:
            if s.get('is_boss') and s['battle_monsters']:
                k = min(len(s['battle_monsters']), 3)
                for m in s['battle_monsters']:
                    out[k][0].append([v / max(b, 1) for v, b in zip(stats_of(m), med)])
                    out[k][1].append([v / max(b, 1) for v, b in zip(stats_of(m), top)])
    return {k: tuple(tuple(statistics.median(x[i] for x in v) for i in range(3)) for v in pair)
            for k, pair in out.items()}


def captured_bosses(records: list, sources: dict, energy: dict) -> dict:
    """Boss count in the wave (1, 2, 3+) -> [(energy, (hp, atk, def))] of the captured bosses."""
    out = defaultdict(list)
    for r in records:
        e = float(energy.get(r['id']) or 0)
        if (sources.get(str(r['id'])) or {}).get('method') != 'captured' or e <= 0:
            continue
        for s in r['stages']:
            if s.get('is_boss') and s['battle_monsters']:
                out[min(len(s['battle_monsters']), 3)] += [(e, stats_of(m)) for m in s['battle_monsters']]
    return out


def median_stats(monsters: list) -> tuple:
    return tuple(round_stat(statistics.median(m[k] for m in monsters)) for k in STATS)


def captured_regulars(records: list, sources: dict, energy: dict) -> list:
    """[(energy, (hp, atk, def))] of each 2014 capture's median regular."""
    out = []
    for r in records:
        e = float(energy.get(r['id']) or 0)
        regs = [m for s in r['stages'] if not s.get('is_boss') for m in s['battle_monsters'] if not fodder(m)]
        if (sources.get(str(r['id'])) or {}).get('method') == 'captured' and e > 0 and regs:
            out.append((e, median_stats(regs)))
    return out


def by_energy(pool: list, e: float) -> tuple:
    near = sorted(pool, key=lambda p: abs(p[0] - e))[:5]
    return tuple(round_stat(statistics.median(scale(st[i], e / d, EXPONENT[k]) for d, st in near))
                 for i, k in enumerate(STATS))


def row_key(m: dict) -> str:
    return json.dumps([m.get(k) for k in ('unit_id', 'name', 'hp', 'atk', 'def', 'ai_id', 'skills', 'visuals',
                                          'act_min', 'act_max', 'wait')], sort_keys=True)


def evolution_lines(mst: dict) -> dict:
    """unit id -> the smallest unit id of its evolution line."""
    parent = {}
    def root(u):
        while parent.get(u, u) != u:
            u = parent[u]
        return u
    for r in mst[next(iter(mst))]:
        a, b = int(r['pn16CNah']), int(r['74VFwuTd'] or 0)
        if a and b:
            ra, rb = root(a), root(b)
            parent.setdefault(ra, ra)
            parent.setdefault(rb, rb)
            parent[max(ra, rb)] = min(ra, rb)
    return {u: root(u) for u in parent}


def tokens(s: str) -> set:
    return set(norm(s).split())


def same_name(a: str, b: str) -> bool:
    ta, tb = tokens(a), tokens(b)
    return bool(ta and tb) and (ta <= tb or tb <= ta)


def cgs_length(path: pathlib.Path) -> int:
    """An animation's length in frames: the sum of its cgs rows' durations (index, x, y, frames)."""
    total = 0
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        cols = line.split(',')
        if len(cols) > 3 and cols[3].strip().isdigit():
            total += int(cols[3])
    return total


def fit_frames(spec: str, ratio: float) -> str:
    """'24:124:1' -> the same effect at that share of a shorter attack."""
    return ','.join(':'.join([str(max(1, round(int(f[0]) * ratio)))] + f[1:])
                    for f in (part.split(':') for part in spec.split(',')))


def header_elements(notes: str) -> dict:
    """norm(boss) -> its element in the notes ("{{UnitImage|element|Fire}} '''War Demon Shusui'''")."""
    out = {}
    for el, name in HEADER_ELEMENT.findall(notes or ''):
        out.setdefault(norm(name), el.lower())
    return out


def script_swaps(rows: list) -> dict:
    """mission id -> {trigger: {targets}} of the client's F_MISSION_SCRIPT_MST rows."""
    out = defaultdict(lambda: defaultdict(set))
    for row in rows:
        mid = str(row.get('missionId') or '').strip()
        if not mid.isdigit():
            continue
        for event in str(row.get('missionScriptParams') or '').split('@'):
            parts = event.split(':', 3)
            trigger = SCRIPT_TRIGGER.fullmatch(parts[2].strip()) if len(parts) == 4 else None
            for command in parts[3].split('#') if trigger else ():
                if swap := SCRIPT_SWAP.fullmatch(command.strip()):
                    out[int(mid)][int(trigger[1])].add(int(swap[1]))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--pages', type=pathlib.Path, nargs='+', required=True, help='wiki page wikitext folders')
    ap.add_argument('--hints', type=pathlib.Path, nargs='+', required=True, help='coverage rows (notes fallback)')
    ap.add_argument('--sgtext', type=pathlib.Path, required=True)
    ap.add_argument('--sprites', type=pathlib.Path,
                    default=pathlib.Path.home() / 'bf-archive/assets/content/unit/img')
    ap.add_argument('--monster-art', type=pathlib.Path,
                    default=pathlib.Path.home() / 'bf-archive/assets/content/monster')
    ap.add_argument('--scripts', type=pathlib.Path,
                    default=pathlib.Path.home() / 'bf-archive/mst-decoded/3-mst-translated'
                    / 'F_MISSION_SCRIPT_MST_Ver288.json', help="the client's decoded F_MISSION_SCRIPT_MST")
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--report', type=pathlib.Path)
    args = ap.parse_args()

    records = json.loads((ARCHIVE / 'mission.json').read_text(encoding='utf-8'))
    sources = json.loads((ARCHIVE / 'mission_sources.json').read_text(encoding='utf-8'))
    variants = json.loads((ARCHIVE / 'mission_variants.json').read_text(encoding='utf-8'))
    mimic = json.loads((ARCHIVE / 'mimic.json').read_text(encoding='utf-8'))
    ai_text = (ARCHIVE / 'ai.json').read_text(encoding='utf-8')
    ais = json.loads(ai_text)
    swaps = script_swaps(json.loads(args.scripts.read_text(encoding='utf-8')))
    taken = {a['id'] for a in ais}
    by_rows = {json.dumps(a['actions'], sort_keys=True): a['id'] for a in ais if a['id'] >= WIKI_AI_BASE}
    sk = json.loads((MST / 'skill_mst.json').read_text(encoding='utf-8'))
    index = SkillIndex(sk[next(iter(sk))])
    skill_name = {int(r['nj9Lw7mV']): r['0nxpBDz2'] for r in sk[next(iter(sk))]}
    umst = json.loads((MST / 'unit_mst.json').read_text(encoding='utf-8'))
    umst = {int(u['pn16CNah']): u for u in umst[next(iter(umst))]}
    unit_skill_ids = {int(u[k]) for u in umst.values() for k in UNIT_SKILL_KEYS
                      if str(u.get(k) or '').isdigit() and int(u[k])}
    line_of = evolution_lines(json.loads((MST / 'unit_evo_mst.json').read_text(encoding='utf-8')))
    mmst = json.loads((MST / 'mission_mst.json').read_text(encoding='utf-8'))
    energy = {int(r['j28VNcUW']): str(int(float(r.get('A8DEK5ob') or 0))) for r in mmst[next(iter(mmst))]}
    units = {u['id']: u for u in json.loads((ARCHIVE / 'unit.json').read_text(encoding='utf-8'))}
    unit_by_name = defaultdict(list)
    for u in units.values():
        unit_by_name[norm(u['name'])].append(u['id'])
    sprites = {int(f[11:-4]) for f in os.listdir(args.sprites)
               if f.startswith('unit_anime_') and f.endswith('.png') and f[11:-4].isdigit()}
    art_files = {f.lower(): f for d in ('img', 'cgg', 'cgs') for f in os.listdir(args.monster_art / d)}
    art_stems = sorted((ART_PREFIXES.index(p), f[len(p) + 7:-4], p) for f in art_files for p in ART_PREFIXES
                       if f.startswith(f'{p}_anime_') and f.endswith('.png'))
    same_art, art_rows = {}, Counter()      # the archive's art bosses: Juggernaut, Demon Abaddon, ...
    for r in records:
        for s in r['stages']:
            for m in s['battle_monsters']:
                if v := m.get('visuals'):
                    same_art.setdefault(v['img_a'], copy.deepcopy(v))
                    if v['img_a'].startswith('monster_anime_'):
                        art_rows[json.dumps([v[k] for k in ART_FIELDS])] += 1
    art_template = next(v for v in same_art.values()
                        if json.dumps([v[k] for k in ART_FIELDS]) == art_rows.most_common(1)[0][0])
    art_template_length = cgs_length(args.monster_art / 'cgs' / art_template['cgs_atk'])
    conv = gcm.Converter(args.sgtext)
    by_quest = defaultdict(list)
    for b in wq.load(args.pages):
        by_quest[norm(b['quest'])].append(b)
    hints = {}
    for f in args.hints:
        for c in json.loads(f.read_text(encoding='utf-8')):
            hints.setdefault(int(c['mission_id']), c.get('quest') or {})
    ratios = boss_ratios(records, sources)
    by_count = captured_bosses(records, sources, energy)
    top_regulars = captured_regulars(records, sources, energy)
    layouts = defaultdict(Counter)          # boss count -> where the captures' bosses stand
    for r in records:
        if (sources.get(str(r['id'])) or {}).get('method') == 'captured':
            for s in r['stages']:
                if s.get('is_boss') and s['battle_monsters']:
                    layouts[len(s['battle_monsters'])][tuple(m['position'] for m in s['battle_monsters'])] += 1
    layouts = {n: c.most_common(1)[0][0] for n, c in layouts.items()}
    in_use = {m['id'] for r in records for s in r['stages'] for m in s['battle_monsters']}
    in_use |= {v['monster']['id'] for vs in variants.values() for v in vs}
    synthetic = [1 + max([gcm.SYNTHETIC_MONSTER_ID - 1] + [i for i in in_use if i >= gcm.SYNTHETIC_MONSTER_ID])]
    named_ids = {}

    def new_synthetic() -> int:
        synthetic[0] += 1
        return synthetic[0] - 1

    def monster_id(name: str, uid: int) -> int:
        ids = conv.monster_ids.get(name.strip().lower())
        if ids:
            return min(ids)
        if (name, uid) not in named_ids:
            named_ids[(name, uid)] = new_synthetic()
        return named_ids[(name, uid)]

    def register(rows: list, name: str) -> int:
        rows = finish_rows(rows)
        key = json.dumps(rows, sort_keys=True)
        if key not in by_rows:
            new_id = WIKI_AI_BASE
            while new_id in taken:
                new_id += 1
            taken.add(new_id)
            ais.append({'id': new_id, 'name': name, 'actions': rows})
            by_rows[key] = new_id
        return by_rows[key]

    def copy_kit(unit_id: int):
        u = umst.get(unit_id)
        if not u:
            return [], []
        slots = {}
        for slot, k in enumerate(UNIT_SKILL_KEYS):
            v = str(u.get(k) or '')
            if v.isdigit() and int(v) in skill_name:
                copies = [x for x in index.candidates(skill_name[int(v)]) if x not in unit_skill_ids]
                if copies and min(copies) not in slots.values():
                    slots[slot] = min(copies)
        skills = list(slots.values())
        at = {slot: skills.index(sid) + 1 for slot, sid in slots.items()}
        rows = []
        if 2 in at:
            rows.append(row([{'type': 'hp_pr_under', 'parameter': 25}, {'type': 'limited_act', 'parameter': 1}], 100, at[2]))
        if 1 in at:
            rows.append(row([{'type': 'hp_pr_under', 'parameter': 50}, {'type': 'limited_act', 'parameter': 1}], 100, at[1]))
            rows.append(row([{'type': 'hp_pr_under', 'parameter': 50}], 20, at[1]))
        if 0 in at:
            rows.append(row([], 35, at[0]))
        return skills, rows

    def block_for(r: dict) -> dict | None:
        cands = by_quest.get(norm(r['name']))
        if not cands:
            return None
        cands = [b for b in cands if b['battles'] == str(len(r['stages']))] or cands
        cands = [b for b in cands if b['energy'] == energy.get(r['id'])] or cands
        if len({json.dumps([b['bosses'], b['bossname']], sort_keys=True) for b in cands}) > 1:
            return None
        return cands[0]

    def clean(text: str) -> str:
        text = re.sub(r'\{\{[^{}]*\}\}', '', text)
        text = re.sub(r'\[\[(?:File|Image):[^\]]*\]\]', '', text, flags=re.I)
        return re.sub(r'\[\[(?:[^|\]]*\|)?([^\]]*)\]\]', r'\1', text).strip()

    def unit_ref(ref: str) -> tuple:
        """A boss field -> (unit id or None, display name)."""
        text = clean(ref)
        if text.isdigit():
            uid = int(text)
            return (uid, units[uid]['name']) if uid in units else (None, text)
        rarity = RARITY.search(text)
        name = RARITY.sub('', text).strip()
        ids = unit_by_name.get(norm(name), [])
        if rarity:
            ids = [i for i in ids if units[i].get('rarity') == int(rarity[1])]
        return (ids[0] if len(ids) == 1 else None), name

    def bossname_pairs(text: str) -> list:
        """|bossname= as [{unit id or None, display name, art key, element, hp, wave}]: 'Golden
        Razorhawk <br> [[File:Unit ills anime 60405.gif]]', '{{UnitInfo|Holy Beast
        Belardo|thumbnail}} Golden Razorhawk', '[[File:Unit_ills_thum_60063.png]]
        [[Lulujann|Priestess of Silence]]', '{{UnitImage|element|Light}} Goddess Mora <br>
        [[File:Monster ills anime 0044.gif]]' (art '0044', element 'light'), with a
        "<center>''300,000 HP''</center>" under its art or a "(Wave 5)" after its name."""
        out, pending = [], None             # pending: the boss named above its art

        def boss(seg: str) -> dict:
            name = TAG.sub('', clean(CENTER.sub('', seg))).replace("'''", '').replace("''", '')
            wave = WAVE.search(name)
            link, el = LINK.search(seg), ELEMENT.search(seg)
            hp = [h for c in CENTER.findall(seg)        # not the art in <center>[[File:...60122.gif]]</center>
                  if re.search(r'\bHP\b', c) and (h := wq.hp_value(clean(TAG.sub('', c))))]
            return {'name': WAVE.sub('', name).strip(), 'link': link[1] if link else None,
                    'element': el[1].lower() if el else None, 'hp': hp[0] if hp else None,
                    'wave': int(wave[1]) if wave else None}

        def named(b: dict) -> int | None:
            return (unit_ref(b['link'])[0] if b['link'] else None) or unit_ref(b['name'])[0]

        def done(b: dict, unit: int | None, name: str | None = None, art: str | None = None) -> None:
            out.append({'unit': unit, 'name': b['name'] if name is None else name, 'art_key': art,
                        'element': b['element'], 'hp': b['hp'], 'wave': b['wave']})

        for seg in BR.split(text or ''):
            b = boss(seg)
            art, monster = UNIT_ART.search(seg), MONSTER_ANIME.search(seg)
            if info := UNIT_INFO.search(seg):
                if pending:
                    done(pending, named(pending))
                done(b, unit_ref(info[1])[0], b['name'] or info[1].strip())
                pending = None
            elif art or MONSTER_ART.search(seg):
                uid = int(art[1]) if art and int(art[1]) in units else None
                if b['name']:
                    if pending:
                        done(pending, named(pending))
                    pending = b
                if pending:
                    done({**pending, 'element': pending['element'] or b['element'], 'hp': pending['hp'] or b['hp']},
                         uid or named(pending), art=monster[1] if monster and not art else None)
                elif uid:
                    done(b, uid, units[uid]['name'])
                pending = None
            elif b['name']:
                if pending:
                    done(pending, named(pending))
                pending = b
            elif b['hp'] and out and not out[-1]['hp']:
                out[-1]['hp'] = b['hp']     # the HP line under a boss's art
        if pending:
            done(pending, named(pending))
        return [w for w in out if w['name'] or w['unit']]

    def wiki_bosses(block: dict, secs: dict, waves: set) -> list:
        out = [dict(zip(('unit', 'name'), unit_ref(b['ref'])), hp=wq.hp_value(b['hp']), art=None)
               for b in block.get('bosses') or []]
        if not out:
            # a boss the wiki puts in another wave ("Heresy Demon Kalon (Wave 5)") is not this wave's
            out = [w for w in bossname_pairs(block.get('bossname')) if not w['wave'] or w['wave'] in waves]
            for w in out:
                sec = secs.get(norm(w['name']))
                w['hp'] = (sec or {}).get('hp') or w['hp']
                w['art'] = art_visuals(w['art_key'], w['name']) if w['art_key'] and w['unit'] not in sprites else None
            # "???" with neither a unit nor art: a boss summoned later (Goddess Mora's at < 50% HP)
            out = [w for w in out if w['unit'] or w['art'] or not UNNAMED.match(w['name'])]
        for w in out:
            # art the client does not ship ("Xie'Jing" over 'boss witch'): the one drawable unit
            # whose name ends in that name (Demonic Witch Xie'Jing 860158)
            if not w['unit'] and not w.get('art') and w.get('art_key') and len(tokens(w['name'])) >= 2:
                ends = [u for u in units.values() if u['id'] in sprites
                        and (' ' + norm(u['name'])).endswith(' ' + norm(w['name']))]
                if len(ends) == 1:
                    w['unit'] = ends[0]['id']
        if not any(w['unit'] or w['art'] for w in out):
            # a boss section is a bold header with skill bullets under it
            out = [{'unit': unit_ref(v['name'])[0], 'name': v['name'], 'hp': v['hp'], 'art': None}
                   for k, v in secs.items() if k and v['skills']]
        return out

    def art_visuals(key: str, name: str) -> dict | None:
        """The client's files for a wiki art key ('0044', 'rc7 p1'), None unless it ships the
        png, its cgg and an idle or attack cgs.  'Supreme Goddess Mora' over 0057 is
        monster_anime_0057_mora.png.  Movement and frames: an archive monster's with that art,
        else the archive's art bosses', their frames fitted into a shorter attack."""
        key = re.sub(r'\s+', '_', key.strip()).lower()
        squashed = re.sub(r'[^a-z0-9]', '', name.lower())
        stems = ([(p, s) for _, s, p in art_stems if s == key]
                 or [(p, s) for _, s, p in art_stems if s.startswith(key + '_') and s[len(key) + 1:] in squashed])
        if len({s for _, s in stems}) != 1:
            return None
        p, stem = stems[0]
        cgg = art_files.get(f'{p}_cgg_{stem}.csv')
        cgs = {slot: next((art_files[f] for n in names if (f := n.format(p=p, s=stem)) in art_files), '')
               for slot, names in ART_CGS}
        if not cgg or not (cgs['cgs_idle'] or cgs['cgs_atk']):
            return None
        img = art_files[f'{p}_anime_{stem}.png']
        if img in same_art:
            return copy.deepcopy(same_art[img])
        v = {'img_a': img, 'anm_cgg': cgg, 'cgs_idle': cgs['cgs_idle'], 'cgs_move': cgs['cgs_move'],
             'cgs_atk': cgs['cgs_atk'], 'cgs_skill': cgs['cgs_atk'], 'element': 0}
        v.update((k, art_template[k]) for k in ART_FIELDS)
        length = cgs_length(args.monster_art / 'cgs' / (cgs['cgs_atk'] or cgs['cgs_idle']))
        if 0 < length < art_template_length:
            for k in ('effect_frame', 'damage_frame'):
                v[k] = fit_frames(v[k], length / art_template_length)
        return v

    art_element = defaultdict(Counter)      # img_a -> the elements the wiki gives that art
    for blocks in by_quest.values():
        for b in blocks:
            for w in bossname_pairs(b['bossname']):
                if w['art_key'] and w['element'] and (v := art_visuals(w['art_key'], w['name'])):
                    art_element[v['img_a']][w['element']] += 1

    def element_of(w: dict, block: dict, stand_in: dict, alone: bool) -> tuple:
        """An art boss's element id and where it came from."""
        same = art_element.get(w['art']['img_a'])
        headers = header_elements(block.get('notes'))
        for source, el in (('bossname', w.get('element')),
                           ('notes header', headers.get(norm(w['name']))),
                           # the quest's one boss, its notes one section ("Demon Mare" over Blazing Mare's)
                           ('only notes header', next(iter(headers.values())) if alone and len(headers) == 1 else None),
                           ('same art on the wiki', same.most_common(1)[0][0] if same else None),
                           ('zone', block.get('zone_element'))):
            if (el or '').lower() in ELEMENT_IDS:
                return ELEMENT_IDS[el.lower()], source
        own = (stand_in.get('visuals') or {}).get('element') or (units.get(stand_in['unit_id']) or {}).get('element', 0)
        return own, 'stand-in'

    def english(uid: int, wiki_name: str) -> str:
        name = units[uid]['name']
        if NOT_ENGLISH.search(name):        # 'ＢＧ‐Ｅ７５０' -> 'BG-E750'
            name = unicodedata.normalize('NFKC', name).replace('‐', '-')
            if NOT_ENGLISH.search(name) and wiki_name and not NOT_ENGLISH.search(wiki_name):
                name = wiki_name
        return name

    def reunit(m: dict, uid: int, wiki_name: str) -> None:
        name = english(uid, wiki_name)
        m.pop('visuals', None)
        m.update(unit_id=uid, name=name, id=monster_id(name, uid),
                 unit_drop_id=0, unit_drop_chance=0, authored_capture=True)

    def reart(m: dict, visuals: dict, element: int, name: str) -> None:
        """A special-art monster: unit 0, which the client cannot resolve, so it reads /monster/."""
        m.update(unit_id=0, name=name, id=monster_id(name, 0), visuals={**visuals, 'element': element},
                 unit_drop_id=0, unit_drop_chance=0, authored_capture=True)

    drawn_on = defaultdict(Counter)       # (name without its level, levelled?) -> units it is drawn on
    for r in records:
        for s in r['stages']:
            for m in s['battle_monsters']:
                if not m.get('visuals') and m['unit_id'] in units:
                    drawn_on[norm(LEVEL.sub('', m['name'])), bool(LEVEL.search(m['name']))][m['unit_id']] += 1

    def refused(m: dict) -> bool:
        v = m.get('visuals')
        return m['id'] == 0 or (v is not None and not (v.get('img_a') and v.get('anm_cgg')))

    def drawable_unit(name: str) -> int | None:
        base = norm(LEVEL.sub('', name))
        for key in ((base, bool(LEVEL.search(name))), (base, not LEVEL.search(name))):
            if drawn_on.get(key):     # Lv10 is drawn like Lv1-9
                return drawn_on[key].most_common(1)[0][0]
        named = [u for u in units.values() if u['id'] in sprites and set(base.split()) <= tokens(u['name'])]
        return max(named, key=lambda u: (u.get('rarity') or 0, u['id']))['id'] if named else None

    def same_line(a: int, b: int) -> bool:
        return a != b and a in line_of and line_of.get(a) == line_of.get(b)

    report, why, unshipped = [], Counter(), []      # unshipped: art bosses the client has no files for
    for r in records:
        mid = r['id']
        if mid in TUTORIAL or r.get('script_monsters') or (sources.get(str(mid)) or {}).get('method') == 'captured':
            continue
        boss_stages = [s for s in r['stages'] if s.get('is_boss') and s['battle_monsters']]
        if not boss_stages:
            continue
        slots = [m for s in boss_stages for m in s['battle_monsters']]
        for m in slots:
            if refused(m) and (uid := drawable_unit(m['name'])):
                m.pop('visuals', None)
                m['unit_id'] = uid
                if m['id'] == 0:
                    m['id'] = monster_id(m['name'], uid)
                report.append({'mission': mid, 'rule': 'drawable again', 'boss': m['name'], 'unit': uid})
                why['drawable again'] += 1
        block = block_for(r)
        notes = (block or {}).get('notes') or (hints.get(mid) or {}).get('notes') or ''
        secs = sections(notes)
        wbs = wiki_bosses(block or {}, secs, {i + 1 for i, s in enumerate(r['stages']) if s.get('is_boss')})
        named_bosses = bossname_pairs((block or {}).get('bossname'))
        alias = {w['unit']: w['name'] for w in named_bosses if w['unit'] and w['name']}
        unshipped += [{'mission': mid, 'boss': w['name'], 'art': w['art_key']} for w in named_bosses
                      if w['art_key'] and w['unit'] not in sprites and not art_visuals(w['art_key'], w['name'])]
        # id(slot) -> wiki boss; replaced slots; a grown wave's minions; bosses the art / grown rules place
        paired, replaced, grown, placed = {}, set(), set(), set()

        # 1. the wiki's bosses: exact unit, then the same evolution line, then name
        for test in (lambda m, w: w['unit'] and m['unit_id'] == w['unit'],
                     lambda m, w: w['unit'] and same_line(m['unit_id'], w['unit']),
                     lambda m, w: same_name(w['name'], m['name'])):
            for w in wbs:
                if any(p is w for p in paired.values()):
                    continue
                m = next((m for m in slots if id(m) not in paired and test(m, w)), None)
                if m is not None:
                    paired[id(m)] = w
        for m in slots:
            w = paired.get(id(m))
            if (w and w['unit'] and same_line(m['unit_id'], w['unit']) and w['unit'] in sprites
                    and m['ai_id'] == 1 and not m.get('skills') and not m.get('visuals')):
                was = m['name']
                reunit(m, w['unit'], w['name'])
                report.append({'mission': mid, 'rule': 'wiki form', 'boss': m['name'], 'was': was})
                why['wiki form'] += 1
        stage_variants = {v['stage'] for v in variants.get(str(mid), [])}
        boss_index = [i for i, s in enumerate(r['stages']) if s.get('is_boss')]
        def skill_groups(name: str) -> dict | None:
            """'''SKILLS''' and '''Used at < 50% HP:''' group one boss's skills; they are no bosses."""
            if any(secs) and all(SKILL_GROUP.match(k) for k in secs if k):
                return {'name': name, 'hp': None,
                        'skills': [(n, f"{v['name']} {t}") for v in secs.values() for n, t in v['skills']]}
            return None

        def lone_section(name: str, groups: bool = False) -> dict | None:
            """The notes kit a quest's only boss slot fights with when no section is named after it."""
            return ((skill_groups(name) if groups else None) or secs.get('')
                    or (next(iter(secs.values())) if len([k for k in secs if k]) == 1 else None))

        # More wiki bosses than stand-ins: the wave grows, unless the notes say the bosses are
        # picked at random, say one of them is summoned (it joins later, and its summoner's kit
        # counts on it: Vohu's hounds, Cotton's defence systems), or a lone stand-in fought with
        # a notes kit no wiki boss is named after (skills under no header) and not every wiki
        # boss has a section of its own instead.
        called = [f' {norm(clean(s[1]))} ' for line in notes.splitlines() if (s := SUMMONS.search(line))]
        kit = lone_section('') if len(slots) == 1 else None
        summoned = {f' {norm(n)} ' for w in wbs for n in (w['name'], (units.get(w['unit']) or {}).get('name')) if n}
        grows = (len(wbs) in layouts and not RANDOM_BOSSES.search(notes)
                 and not any(n in c for n in summoned for c in called)
                 and (kit is None or any(kit is secs.get(norm(w['name'])) for w in wbs)
                      or all((secs.get(norm(w['name'])) or {}).get('skills') for w in wbs)))
        new_rule = len(wbs) > len(slots) or not all(w['unit'] in sprites for w in wbs)
        # a stand-in that drops the unit the wiki lists as the quest's capture is what players farm
        # there (Garden of Imps' 60% Almighty Imp Arton): not the new rules' to take
        farmed = {unit_ref(c)[0] for c in (block or {}).get('captures') or []} - {None}
        if new_rule and any(m.get('unit_drop_chance') and m.get('unit_drop_id') in farmed for m in slots):
            wbs = []
        if (wbs and not paired and len(boss_stages) == 1 and len(boss_index) == 1
                and boss_index[0] not in stage_variants and (len(wbs) <= len(slots) or grows)
                and all(w['unit'] in sprites or w['art'] for w in wbs) and new_rule and mid in UNMODELLED_KITS):
            report.append({'mission': mid, 'rule': 'stand-ins kept: kit not modelled', 'why': UNMODELLED_KITS[mid]})
            why['stand-ins kept: kit not modelled'] += 1
        elif (wbs and not paired and len(boss_stages) == 1 and len(boss_index) == 1
                and boss_index[0] not in stage_variants and (len(wbs) <= len(slots) or grows)
                and all(w['unit'] in sprites or w['art'] for w in wbs)):
            stage = boss_stages[0]
            was = [m['name'] for m in stage['battle_monsters']]
            chest = max(stage['battle_monsters'], key=lambda m: m['hp'])
            hits = max(m['atk'] for s in r['stages'] for m in s['battle_monsters'])
            # more wiki bosses than stand-ins: they stand where the captures' bosses of that count do
            spots = [m['position'] for m in stage['battle_monsters']] if len(wbs) <= len(slots) else layouts[len(wbs)]
            if len(wbs) > len(slots):
                report.append({'mission': mid, 'rule': 'wave grown', 'slots': [len(slots), len(wbs)]})
                why['wave grown'] += 1
            fresh = []
            for i, w in enumerate(wbs):
                # the strongest stand-in's numbers and chest, hitting at least as hard as
                # the quest's regulars (step 2 rescales them when it can)
                m = copy.deepcopy(chest)
                m.update(atk=hits, position=spots[i], ai_id=1, skills=[])
                if i and len(wbs) > len(slots):
                    m['act_min'], m['act_max'] = 1, 1      # the lone stand-in's actions are the main boss's
                    grown.add(id(m))
                drawn = {}
                if w['unit'] in sprites:
                    reunit(m, w['unit'], w['name'])
                else:
                    element, source = element_of(w, block or {}, chest, len(wbs) == 1)
                    reart(m, w['art'], element, w['name'])
                    drawn = {'art': m['visuals']['img_a'], 'element': [element, source]}
                    why['monster art'] += 1
                if len(wbs) == 1:
                    m['position'] = SINGLE_BOSS_POSITION
                fresh.append(m)
                paired[id(m)] = w
                replaced.add(id(m))
                if new_rule:
                    placed.add(id(m))
                report.append({'mission': mid, 'rule': 'stand-in replaced', 'boss': m['name'], 'was': was, **drawn})
                why['stand-in replaced'] += 1
            stage['battle_monsters'] = fresh
            slots = [m for s in boss_stages for m in s['battle_monsters']]

        def section_of(m: dict) -> dict | None:
            w = paired.get(id(m))
            shown = alias.get(m['unit_id'])
            sec = (secs.get(norm(m['name'])) or (secs.get(norm(w['name'])) if w and w['name'] else None)
                   or (secs.get(norm(shown)) if shown else None))
            # skill-group titles are read so for the bosses the art / grown rules place
            groups = id(m) in placed
            if sec is None and w is not None and len(wbs) == 1:
                # skills under no header (or under skill-group titles): the quest's one named boss
                sec = (skill_groups(m['name']) if groups else None) or secs.get('')
            if sec is None and len(slots) == 1:
                sec = lone_section(m['name'], groups)
            return sec

        # 2. stats
        regs = [m for s in r['stages'] if not s.get('is_boss') for m in s['battle_monsters']
                if stats_of(m) != PLACEHOLDER and not fodder(m)]
        # a boss placed by the art or grown-wave rules measures itself only against regulars with
        # real ATK and DEF: a 420,000 HP Imbued Tablet Elgif on placeholder 290 / 50 made bosses
        # of 900,000 to 4,000,000 HP on 15-50 energy quests
        real = [m for m in regs if (m['atk'], m['def']) != PLACEHOLDER[1:]]

        def yardstick(m: dict) -> tuple:
            pool = real if id(m) in placed else regs
            if not pool:
                return None, None
            return [statistics.median(x[k] for x in pool) for k in STATS], [max(x[k] for x in pool) for k in STATS]

        for s in boss_stages:
            k = min(len(s['battle_monsters']), 3)
            from_wiki, guessed = [], []
            for m in s['battle_monsters']:
                med, top = yardstick(m)
                if borrowed_block(m) and len(real if id(m) in placed else regs) < 3:
                    med = top = None        # one strong regular is no yardstick (Frontier Spire SP 7)
                if stats_of(m) == PLACEHOLDER or id(m) in replaced or borrowed_block(m):
                    guessed.append(m)
                    e = float(energy.get(mid) or 0)
                    if not med and e > 0 and (stats_of(m) == PLACEHOLDER or id(m) in replaced or borrowed_block(m)):
                        old = stats_of(m)
                        m['hp'], m['atk'], m['def'] = by_energy(by_count.get(k) or by_count[1], e)
                        report.append({'mission': mid, 'rule': 'captured boss by energy', 'boss': m['name'],
                                       'stats': [old, stats_of(m)]})
                        why['captured boss by energy'] += 1
                    elif med:
                        old = stats_of(m)
                        by_med, by_top = ratios[k]
                        m['hp'], m['atk'], m['def'] = (round_stat(max(a * x, b * y))
                                                       for a, x, b, y in zip(med, by_med, top, by_top))
                        report.append({'mission': mid, 'rule': 'captured boss ratio', 'boss': m['name'],
                                       'stats': [old, stats_of(m)]})
                        why['captured boss ratio'] += 1
                    if id(m) in grown and (real or e > 0):
                        # a grown wave's minion (Kagenui's Genin) is as strong as the quest's median
                        # regular, else as the captures' median regulars of the nearest energy (the
                        # strongest regular can be a mid-boss: 250,000 HP Thunderbirds by Harakhte)
                        old = stats_of(m)
                        m['hp'], m['atk'], m['def'] = median_stats(real) if real else by_energy(top_regulars, e)
                        # and no stronger than its boss, the wave's first (the captures' regulars of a
                        # 50-energy single battle out-hit its 3-boss waves' bosses)
                        m['hp'], m['atk'], m['def'] = map(min, stats_of(m), stats_of(s['battle_monsters'][0]))
                        report.append({'mission': mid, 'rule': 'minion as median regular', 'boss': m['name'],
                                       'stats': [old, stats_of(m)]})
                        why['minion as median regular'] += 1
                w = paired.get(id(m))
                hp = (w or {}).get('hp') or (section_of(m) or {}).get('hp')
                if hp:
                    from_wiki.append(m)
                    if hp != m['hp']:
                        report.append({'mission': mid, 'rule': 'wiki HP', 'boss': m['name'], 'hp': [m['hp'], hp]})
                        why['wiki HP'] += 1
                        m['hp'] = hp
            if from_wiki:
                hp = round_stat(statistics.median(m['hp'] for m in from_wiki))
                for m in guessed:
                    # a grown wave's minion is no co-boss: Leopold's 50,000,000 HP is not his Voidborns'
                    if all(m is not x for x in from_wiki) and m['hp'] != hp and id(m) not in grown:
                        report.append({'mission': mid, 'rule': 'co-boss wiki HP', 'boss': m['name'], 'hp': [m['hp'], hp]})
                        why['co-boss wiki HP'] += 1
                        m['hp'] = hp

        # 3. skills
        for m in slots:
            if m['ai_id'] != 1 or m.get('skills'):
                continue
            sec = section_of(m)
            skills, rows, rule = [], [], None
            if sec and sec['skills']:
                skills, rows, _ = build_kit(sec['skills'], index, rare_anchors=id(m) in placed)
                rule = 'wiki skills'
            if not rows and m['unit_id'] in units and not m.get('visuals') \
                    and same_name(m['name'], units[m['unit_id']]['name']):
                skills, rows = copy_kit(m['unit_id'])
                rule = 'own BB/SBB monster copies'
            if not rows:
                why['no source'] += 1
                continue
            m['skills'] = [{'skill_id': s, 'show_name': True} for s in skills]
            if needs_second_action(rows) and m['act_max'] < 2:
                m['act_min'], m['act_max'] = 2, 2
            m['ai_id'] = register(rows, f"wiki {m['name']}" if rule == 'wiki skills' else 'boss own bursts')
            report.append({'mission': mid, 'rule': rule, 'boss': m['name'], 'skills': [skill_name.get(s) for s in skills]})
            why[rule] += 1

        # 6. "Small chance to capture Fei upon defeating Xie'Jing": the boss drops it at the
        # server's boss-drop chance for that line, and nothing else in the quest does
        for line in notes.splitlines():
            if not (c := CAPTURE_FROM.search(line)):
                continue
            want, boss = tokens(c[1]), tokens(c[2])
            caught = [u for u in (unit_ref(x)[0] for x in (block or {}).get('captures') or []) if u and want <= tokens(units[u]['name'])]
            holder = next((m for m in slots if boss <= tokens(m['name']) or boss <= tokens((paired.get(id(m)) or {}).get('name') or '')), None)
            if not caught or holder is None:
                continue
            line_units = [u for u in units if line_of.get(u) is not None and line_of.get(u) == line_of.get(caught[0])] or [caught[0]]
            chance = BOSS_DROP_CHANCE[max(units[u].get('rarity') or 0 for u in line_units)]
            for s in r['stages']:
                for m in s['battle_monsters']:
                    if m is not holder and m.get('unit_drop_id') == caught[0]:
                        m.update(unit_drop_id=0, unit_drop_chance=0, authored_capture=True)
            holder.update(unit_drop_id=caught[0], unit_drop_level=1, unit_drop_type=0, unit_drop_chance=chance,
                          authored_capture=True)
            report.append({'mission': mid, 'rule': 'capture from boss', 'boss': holder['name'],
                           'unit': units[caught[0]]['name'], 'chance': chance})
            why['capture from boss'] += 1

        # 5. first strike: "Enemies in all waves featuring thieves ([[Thief Leon]], ...)
        # preemptively attack first" -> those waves (or every wave) strike first
        for line in notes.splitlines():
            if not PREEMPT.search(line):
                continue
            named = [tokens(x) for x in LINKED.findall(line) if tokens(x)]
            for i, s in enumerate(r['stages']):
                if s.get('first_attack_rate'):
                    continue
                if named and not any(n <= tokens(m['name']) for n in named for m in s['battle_monsters']):
                    continue
                if not named and not ALL_WAVES.search(line):
                    continue
                s['first_attack_rate'] = 100
                report.append({'mission': mid, 'rule': 'first strike', 'wave': i + 1})
                why['first strike'] += 1

        # 4. one MonsterMst row per id
        first = {}
        for s in r['stages']:
            if not s.get('is_boss'):
                for m in s['battle_monsters']:
                    first.setdefault(m['id'], row_key(m))
        for v in variants.get(str(mid), []):
            first.setdefault(v['monster']['id'], row_key(v['monster']))
        for m in mimic.get('monsters', []):
            first.setdefault(m['id'], row_key(m))
        for s in r['stages']:
            if not s.get('is_boss'):
                continue
            for m in s['battle_monsters']:
                key = row_key(m)
                if first.setdefault(m['id'], key) == key:
                    continue
                old = m['id']
                spare = [i for i in sorted(conv.monster_ids.get(m['name'].strip().lower(), [])) if i not in first]
                m['id'] = spare[0] if spare else new_synthetic()
                first[m['id']] = key
                report.append({'mission': mid, 'rule': 'own monster id', 'boss': m['name'], 'id': [old, m['id']]})
                why['own monster id'] += 1

        # 5. no boss on a script trigger whose swap target the record does not describe
        described = set(first) | {m['id'] for m in r.get('script_monsters') or []}
        unsafe = {t for t, targets in swaps.get(mid, {}).items() if targets - described}
        for s in r['stages']:
            for m in s['battle_monsters'] if s.get('is_boss') else ():
                if m['id'] not in unsafe:
                    continue
                old = m['id']
                spare = [i for i in sorted(conv.monster_ids.get(m['name'].strip().lower(), []))
                         if i not in first and i not in unsafe]
                m['id'] = spare[0] if spare else new_synthetic()
                first[m['id']] = row_key(m)
                report.append({'mission': mid, 'rule': 'script trigger', 'boss': m['name'], 'id': [old, m['id']]})
                why['script trigger'] += 1

    print(dict(why), f'| {len(ais) - len(json.loads(ai_text))} new AI(s) |',
          len({x['mission'] for x in report}), 'missions changed |',
          len(unshipped), 'art boss(es) the client ships no art for')
    if args.report:
        report += [{**x, 'rule': 'art not shipped (unchanged)'} for x in unshipped]
        args.report.write_text(json.dumps(report, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    if not args.check:
        (ARCHIVE / 'mission.json').write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        (ARCHIVE / 'ai.json').write_text(json.dumps(ais, indent=4, ensure_ascii=False)
                                         + ('\n' if ai_text.endswith('\n') else ''), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
