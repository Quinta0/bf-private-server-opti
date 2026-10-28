"""Per-run enemy rolls for material and EXP fights: deploy/archive/mission_variants.json.

python scripts/gen_mission_variants.py --captures DIR --sgtext DIR --wiki-quests FILE --wiki-hints FILE... [--check]

WHY.  The original game rolled these fights' enemies every run: a Metal Parade
slot was a Metal Ghost, King or God, an Idol or Totem fight rolled elements.
An archive record is fixed, so every run showed and dropped the same units.
The server now rolls one enemy per listed slot at MissionStart
(MissionArchiver::rollVariants); this script lists the alternatives.

SOURCES, per slot of an archive record (never the tutorial, scripted fights or
missions an overlay file replaces, which the server skips anyway):
  captures  a captured mission whose quest was recorded more than once rolls,
            in each slot, among the enemies seen there across the runs with
            its wave count, weighted by how often each was seen (Dark Metal
            Parade: Metal Ghost 21, King 21, God 4, Dark Crystal 1).
  wiki      a slot holding a farm unit, in a mission whose wiki quest lists two
            or more units of that family (Totem, Crystal, Idol, ...) among its
            captures, rolls among them; captureNrate is the weight when given,
            else each star of rarity halves it (Almighty Imp Arton 1, the
            common Imps 2).  Regular waves only: the quest's boss stays.
ENEMIES.  Each alternative is a whole enemy.  One the record already has is
copied from it (the client keeps one MonsterMst row per id); a new one is
built from its capture (stats, captured AI when ai.json has it, else AI 1)
or, for a wiki swap, is the slot's enemy on the other unit.  Then the same
last rebuild steps as the record: gen_capture_rules (catchable or not, the
unit's own drop) and gen_material_drops (items in the chest).
"""

import argparse
import collections
import copy
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import ARCHIVE, MISSIONS, SOURCES, TUTORIAL, Converter, load_captures  # noqa: E402
from gen_capture_rules import Evidence  # noqa: E402
from gen_material_drops import ITEM_TYPE, merge  # noqa: E402

VARIANTS = ARCHIVE / 'mission_variants.json'
FAMILIES = ('Idol', 'Spirit', 'Totem', 'Nymph', 'Pot', 'Ghost', 'King', 'God', 'Crystal', 'Bulb', 'Tone', 'Frog',
            'Imp', 'Golem', 'Mecha')


def family_of(name: str) -> str:
    """Totem for "Fire Totem", Imp for "Power Imp Pakpak": the farm kind a name belongs to."""
    words = name.split()
    return next((w for w in FAMILIES if w in words), words[-1] if words else '')
CAPTURE = re.compile(r'capture(\d+)')
SYNTHETIC_ID = 95_000_000       # a swap whose usual monster id is taken (no text entry: the client shows our name)


def key(monster: dict) -> tuple:
    return monster['name'].strip().lower(), monster['unit_id']


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True)
    ap.add_argument('--sgtext', type=pathlib.Path, required=True)
    ap.add_argument('--wiki-quests', type=pathlib.Path, required=True)
    ap.add_argument('--wiki-hints', type=pathlib.Path, nargs='*', default=[])
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = {r['id']: r for r in json.loads(MISSIONS.read_bytes())}
    sources = json.loads(SOURCES.read_text())
    unit_rows = json.loads((ARCHIVE / 'unit.json').read_text())
    units = {u['id']: u['name'] for u in unit_rows}
    rarity = {u['id']: int(u.get('rarity') or 1) for u in unit_rows}
    conv = Converter(args.sgtext)
    ai_ids = {r['id'] for r in json.loads((ARCHIVE / 'ai.json').read_text())}
    evidence = Evidence(args.captures, args.wiki_quests, units, args.wiki_hints)
    captures = load_captures(args.captures)
    rows = json.loads((ARCHIVE.parent / 'mst' / 'item_mst.json').read_text())
    item_id = {r['c7Z6xDB2'].strip().lower(): int(r['kixHbe54'])
               for r in (next(iter(rows.values())) if isinstance(rows, dict) else rows)}
    stats = collections.Counter()

    def finish(m: dict, boss: bool, quest: dict, items: list | None) -> dict:
        """The record's own last steps for one enemy: capture rule, then materials."""
        if not (evidence.own(m) and (m['unit_id'] in quest or m['unit_id'] in evidence.farm)) \
                and not evidence.allows(m, boss, quest):
            m.update(unit_drop_id=0, unit_drop_level=0, unit_drop_type=0, unit_drop_chance=0, authored_capture=True)
        if items is not None:
            merge(m, items)
        return m

    def from_capture(cap: dict, position: str, boss: bool, quest: dict) -> dict:
        m = conv.monster(cap, position)
        if m['ai_id'] not in ai_ids:          # a captured AI ai.json lacks: plain attacks
            m['ai_id'] = 1
            m.pop('skills', None)
            stats['new enemies on AI 1'] += 1
        items = [(item_id[i['_name'].strip().lower()], float(i['percent'])) for i in cap.get('item drop') or []
                 if i['_name'].strip().lower() in item_id and i.get('percent')]
        return finish(m, boss, quest, items)

    out, synthetic = {}, {}
    for mid, record in sorted(records.items()):
        if mid in TUTORIAL or record.get('script_monsters'):
            continue
        quest = evidence.quest.get(mid, {})
        known = {}                            # monster id -> the definition every copy must share
        for stage in record['stages']:
            for m in stage['battle_monsters']:
                known.setdefault(m['id'], m)
        slots = {}

        # 1. Captured quests recorded more than once.
        src = sources.get(str(mid)) or {}
        runs = []
        if src.get('method') == 'captured':
            runs = [c for _, c in captures.get(record['name'].strip().lower(), [])
                    if len(c['battles']) == len(record['stages'])]
        if len(runs) > 1:
            for i, stage in enumerate(record['stages']):
                for j, base in enumerate(stage['battle_monsters']):
                    seen = collections.Counter()
                    sample = {}
                    for run in runs:
                        wave = run['battles'][i]['monsters']
                        if j < len(wave):
                            name = wave[j]['_name']
                            k = (name.strip().lower(), int(wave[j]['unit id']))
                            seen[k] += 1
                            sample.setdefault(k, wave[j])
                    if len(seen) < 2:
                        continue
                    options = []
                    for k, n in seen.most_common():
                        cap = dict(sample[k], _name=sample[k]['_name'])
                        mon_id = conv.monster_id(cap['_name'], int(cap['unit id']))
                        if mon_id in known:
                            m = copy.deepcopy(known[mon_id])
                        else:
                            m = from_capture(cap, base['position'], stage['is_boss'], quest)
                            known[m['id']] = m
                            m = copy.deepcopy(m)
                        if m['unit_id'] not in units:
                            continue
                        m['position'] = base['position']
                        options.append((n, m))
                    if len(options) > 1:
                        slots[(i, j)] = options
                        stats['slots from repeated captures'] += 1

        # 2. Wiki capture lists: a family of farm units (all Totems, all Crystals).
        if quest:
            family = collections.defaultdict(list)
            for u in quest:
                if u in evidence.farm and u in units:
                    family[family_of(units[u])].append(u)
            for i, stage in enumerate(record['stages']):
                if stage['is_boss']:
                    continue
                for j, base in enumerate(stage['battle_monsters']):
                    members = family.get(family_of(units[base['unit_id']])) if base['unit_id'] in units else None
                    if (i, j) in slots or base['unit_id'] not in (members or []) or len(members) < 2:
                        continue
                    options = []
                    for u in members:
                        # The slot's own numbers on each unit of the family; the
                        # unit's usual id only when no other definition holds it.
                        m = copy.deepcopy(base)
                        m.update(unit_id=u, name=units[u])
                        m.pop('visuals', None)
                        if base.get('unit_drop_chance') or not base.get('authored_capture'):
                            m['unit_drop_id'] = u
                        m = finish(m, False, quest, None)
                        m['id'] = conv.monster_id(units[u], u)
                        same = lambda a, b: {**a, 'position': ''} == {**b, 'position': ''}
                        while m['id'] in known and not same(known[m['id']], m):
                            m['id'] = synthetic[mid] = synthetic.get(mid, SYNTHETIC_ID) + 1
                        known.setdefault(m['id'], copy.deepcopy(m))
                        m['position'] = base['position']
                        top = max(rarity[x] for x in members)
                        options.append((quest.get(u) or 2 ** (top - rarity[u]), m))
                    slots[(i, j)] = options
                    stats['slots from wiki families'] += 1

        if slots:
            out[str(mid)] = [{'stage': i, 'slot': j, 'weight': w, 'monster': m}
                             for (i, j), options in sorted(slots.items()) for w, m in options]
            stats['missions'] += 1

    stats['alternatives'] = sum(len(v) for v in out.values())
    stats['with item chests'] = sum(1 for v in out.values() for o in v
                                    if any(d['target_type'] == ITEM_TYPE for d in o['monster'].get('treasure_drops') or []))
    print(dict(stats))
    if not args.check:
        VARIANTS.write_text(json.dumps(out, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
