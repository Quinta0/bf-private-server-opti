"""Keep unit drops only on enemies the original game let you capture.

python scripts/gen_capture_rules.py --captures DIR --wiki-quests FILE [--wiki-hints FILE ...] [--check] [--report FILE]

WHY (ticket #47).  MissionArchiver::applyCaptureFloor gives every
enemy without `authored_capture` at least a 40% chance to capture its
unit_drop_id, or its own unit when that is 0.  So the bosses of the old
regenerated records, the wiki bosses and units that took a template slot's
drop (gen_designed_missions / gen_wiki_missions), and the event bosses that
drop their base form all became catchable: Lava, God Knight Will, Captain
Mega, Creator Maxwell and hundreds of rare-summon units.

EVIDENCE that the original game let you capture a unit:
  2014     a capture (missions_parsed/, jp/ too) shows the unit with a unit
           drop.  An enemy whose name is not its unit's (Thief Henchman on
           Thief Leon) needs a capture showing that very name dropping it.
  wiki     a {{Quest}} block lists it under captureN (quests.json, every
           quest).
  boss     for a boss: a 2014 drop in a boss wave, or a wiki quest listing
           the unit as both boss and capture (Bulb Haven, Box of Desires,
           Guardians of Lore).
  farm     a farm unit (unit_mst kind other than 0: EXP fodder like Ghosts,
           Frogs and Imps, evolution material like Idols, Totems and the Mecha
           Gods, Jewels to sell), boss or not: that is what they are for.
  quest    a mission matched to a wiki quest (--wiki-hints) lets you catch
           what that quest's own captureN list names, boss or not; an enemy
           there with no drop gets one at the wiki's captureNrate, else the
           unit's 2014 chance, else 40% (Paula in Gaia's Voice Lv. 3 at 10%).  A boss is a boss-wave enemy with at least
           half the wave's top HP, or a script_monsters form; the weaker ones
           are minions and need regular evidence only.

RULE, for every enemy of every record but the tutorial's scripted 1 and 2:
  kept     the unit it drops (unit_drop_id, else its own unit) has the
           evidence; nothing changes, so the floor may still raise it to 40%.
  removed  otherwise -- a stand-in ("Phantom ...") or a boss dropping its base
           form included -- and always for special bosses (`visuals`):
           unit_drop_* go to 0 and authored_capture to true, so the floor
           leaves it at 0.  Drops are only added from a quest's own list.
Copies of one monster id share the client's MonsterMst row in a mission, so
they share the verdict: one removed copy removes them all.
Only the drop fields and authored_capture change, and a second run changes
nothing.  Run it last when rebuilding the archive (re/work/vortex/sources/README.md).
"""

import argparse
import collections
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_captured_missions import ARCHIVE, MISSIONS, MST, TUTORIAL  # noqa: E402

ANIME = re.compile(r'anime (\d+)', re.I)
CAPTURE = re.compile(r'capture(\d+)')
DEFAULT_CHANCE = 40             # MissionArchiver's floor
RANDOM_DROP = 5                 # unit_drop_type of a captured drop (gen_captured_missions.DROP_TYPE)
DROP_FIELDS = ('unit_drop_id', 'unit_drop_level', 'unit_drop_type', 'unit_drop_chance')


class Evidence:
    """Units the original game let you capture: `regular` anywhere, `boss` in
    a boss wave; `named` the (enemy name, unit) pairs the 2014 captures show
    dropping, for enemies whose name is not their unit's."""

    def __init__(self, captures: pathlib.Path, wiki_quests: pathlib.Path, unit_names: dict,
                 hints: list = ()):
        self.units = unit_names
        self.regular, self.boss, self.named = set(), set(), set()
        rows = json.loads((MST / 'unit_mst.json').read_text())
        self.farm = {int(r['pn16CNah']) for r in (next(iter(rows.values())) if isinstance(rows, dict) else rows)
                     if str(r.get('xP9oi5Z2') or '0') != '0'}     # unit_mst.kind
        chances = collections.defaultdict(list)
        for path in [*captures.glob('*.json'), *captures.glob('jp/*.json')]:
            for capture in json.loads(path.read_text(encoding='utf-8')).values():
                battles = capture['battles']
                for n, battle in enumerate(battles):
                    for m in battle['monsters']:
                        if float((m.get('unit drop') or {}).get('percent') or 0) > 0:
                            unit = int(m['unit id'])
                            self.regular.add(unit)
                            chances[unit].append(int(float(m['unit drop']['percent'])))
                            self.named.add((m['_name'].strip().lower(), unit))
                            if n == len(battles) - 1:
                                self.boss.add(unit)
        by_name = {name.strip().lower(): uid for uid, name in sorted(unit_names.items(), reverse=True)}

        def unit_of(token) -> int | None:
            token = str(token or '').strip()
            uid = int(token) if token.isdigit() else by_name.get(token.lower())
            return uid if uid in unit_names else None

        self.chance = {u: sorted(c)[len(c) // 2] for u, c in chances.items()}   # median
        self.wiki = set()
        quests = json.loads(wiki_quests.read_text(encoding='utf-8'))
        for rows in quests.values():
            for row in rows:
                caught = {unit_of(v) for k, v in row.items()
                          if k.startswith('capture') and not k.endswith('rate') and k != 'capture#'} - {None}
                bosses = {unit_of(v) for k, v in row.items() if re.fullmatch(r'boss\d+', k)}
                bosses |= {unit_of(u) for u in ANIME.findall(row.get('bossname') or '')}
                self.wiki |= caught
                self.boss |= caught & bosses     # a quest whose boss wave you could catch from

        self.quest = {}                 # mission id -> {unit: wiki capture rate or None}
        for path in hints:
            for hint in json.loads(pathlib.Path(path).read_text(encoding='utf-8')):
                q = hint.get('quest')
                rows = q and quests.get(q['quest'], [])
                row = rows and next((r for r in rows if r.get('page') == q['page']), rows[0])
                if row:
                    self.quest[hint['mission_id']] = {
                        unit_of(v): int(r) if str(r := row.get(f'capture{m[1]}rate') or '').isdigit() else None
                        for k, v in row.items() if (m := CAPTURE.fullmatch(k)) and unit_of(v)}

    def own(self, monster: dict) -> bool:
        """Drawn as its own unit under that unit's name, and dropping nothing else."""
        unit = monster['unit_id']
        return (not monster.get('visuals') and monster.get('unit_drop_id') in (0, None, unit)
                and monster['name'].strip().lower() == self.units.get(unit, '').strip().lower())

    def allows(self, monster: dict, boss: bool, quest: dict) -> bool:
        if self.own(monster) and (monster['unit_id'] in quest or monster['unit_id'] in self.farm):
            return True
        if monster.get('visuals'):
            return False
        unit = monster.get('unit_drop_id') or monster['unit_id']
        name = monster['name'].strip().lower()
        if name != self.units.get(unit, '').strip().lower() and (name, unit) not in self.named:
            return False        # a stand-in, or a boss dropping another form
        return unit in self.boss if boss else unit in self.regular or unit in self.wiki


def apply(record: dict, evidence: Evidence, stats: collections.Counter, removed: collections.Counter) -> None:
    enemies = []
    for stage in record['stages']:
        top = max((m['hp'] for m in stage['battle_monsters']), default=0)
        enemies += [(m, stage['is_boss'] and m['hp'] * 2 >= top) for m in stage['battle_monsters']]
    enemies += [(m, True) for m in record.get('script_monsters') or []]
    quest = evidence.quest.get(record['id'], {})
    blocked = {m['id'] for m, boss in enemies if not evidence.allows(m, boss, quest)}
    missing = {m['id'] for m, _ in enemies if evidence.own(m) and m['unit_id'] in quest
               and m.get('authored_capture') and not m.get('unit_drop_chance')}
    for m, _ in enemies:
        if m['id'] in missing - blocked:
            unit = m['unit_id']
            rate = quest[unit] or evidence.chance.get(unit) or DEFAULT_CHANCE
            m.update(unit_drop_id=unit, unit_drop_level=1, unit_drop_type=RANDOM_DROP, unit_drop_chance=rate)
            stats['added from the quest'] += 1
            continue
        if m['id'] not in blocked:
            stats['kept'] += 1
            continue
        if m.get('authored_capture') and not any(m.get(f) for f in DROP_FIELDS):
            stats['already none'] += 1
            continue
        stats['removed'] += 1
        removed[m.get('unit_drop_id') or m['unit_id']] += 1
        for field in DROP_FIELDS:
            m[field] = 0
        m['authored_capture'] = True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True, help='missions_parsed/ folder')
    ap.add_argument('--wiki-quests', type=pathlib.Path, required=True, help='wiki/quests.json')
    ap.add_argument('--wiki-hints', type=pathlib.Path, nargs='*', default=[],
                    help='coverage_by_mission.json coverage_regenerated.json: mission -> wiki quest')
    ap.add_argument('--check', action='store_true', help='report only; do not write mission.json')
    ap.add_argument('--report', type=pathlib.Path, help='write {unit id: enemies whose drop was removed}')
    args = ap.parse_args()

    records = json.loads(MISSIONS.read_bytes())
    unit_names = {u['id']: u['name'] for u in json.loads((ARCHIVE / 'unit.json').read_text())}
    evidence = Evidence(args.captures, args.wiki_quests, unit_names, args.wiki_hints)
    print(f'catchable: {len(evidence.regular | evidence.wiki)} unit(s) '
          f'({len(evidence.regular)} from 2014 drops, {len(evidence.wiki)} from wiki lists); '
          f'{len(evidence.boss)} in a boss wave; {len(evidence.farm)} farm unit(s); '
          f'{len(evidence.quest)} mission(s) with their wiki quest list')
    stats, removed = collections.Counter(), collections.Counter()
    for record in records:
        if record['id'] not in TUTORIAL:
            apply(record, evidence, stats, removed)
    print(dict(stats), f'{len(removed)} unit(s) no longer drop')
    if args.report:
        args.report.write_text(json.dumps({f'{uid} {unit_names.get(uid, "?")}': n for uid, n in removed.most_common()},
                                          indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    if not args.check and stats['removed']:
        MISSIONS.write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
