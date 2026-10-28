"""Add missions from captured live battles to deploy/archive/mission.json.

python scripts/gen_captured_missions.py --captures DIR --sgtext DIR [--check] [--report FILE]

SOURCE.  github.com/cheahjs/bravefrontier_data `missions_parsed/`: MissionStart
replies recorded from the live Global server in 2014, one file per run, named
"<mission name>_<y-d-m-h-m>.json".  Each holds the real waves: every enemy's
unit id, HP/ATK/DEF, act counts, AI id, drops and treasure chest.  This is
recovered data, not authored data -- the only battles of this kind we have.

WHAT IS FILLED.  Only missions the archive does not hold yet (a capture never
overwrites an existing record).  A capture is matched to a mission by its
English name (sgtext MST_DUNGEONS_MISSION_<id>_NAME) and accepted only when its
wave count equals MissionMst.battle_count; a name shared by several missing
missions is used only when every one of them has that wave count (the same
fight re-run under one name, e.g. a Vortex quest and its rerun).

WHAT IS NOT IN A CAPTURE, AND WHAT IS USED INSTEAD.
  position     the capture has none; the archive's most common layout for that
               many enemies (the same table every regenerated mission uses).
  monster id   the capture has the English name only; the id is the monster
               text entry with that name (sgtext MST_MONSTERS_<id>_NAME), so the
               client localises it, else a synthetic id >= 90,000,000 that has
               no text entry, which makes the client show the name we send.
  AI, skills   converted from the capture (AiBuilder): the AI rows go to
               ai.json as id 30,000,000 + captured id, the skills resolve by
               name (and hit pattern) in skill_mst.  An enemy whose skills do
               not all resolve keeps AI 1 ("Attack random").
  item drops   BattleMonster has no direct item drop; the capture's item drops
               are listed in the report only.  Its treasure chest IS converted.
  zel/karma/exp/energy  from MissionMst, like every other record.
Any enemy whose unit id is not in archive/unit.json makes the whole mission be
skipped (MissionArchiver refuses a record it cannot draw).
"""

import argparse
import base64
import collections
import json
import pathlib
import re
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / 'deploy' / 'archive'
MST = ROOT / 'deploy' / 'mst'
MISSIONS = ARCHIVE / 'mission.json'
SOURCES = ARCHIVE / 'mission_sources.json'
TUTORIAL = {1, 2}

SYNTHETIC_MONSTER_ID = 90_000_000
CAPTURE_DATE = re.compile(r'_(\d{4})-(\d{1,2})-(\d{1,2})-(\d{1,2})-(\d{1,2})\.json$')
DROP_TYPE = {'random': 5, 'lord': 1}
CJK = re.compile(r'[\u3040-\u30ff\u3400-\u9fff]')
BY_ID = re.compile(r'^(\d+)_')


def sgtext(folder: pathlib.Path, bundle: str) -> dict:
    """Newest sgtext_<bundle>_<ver>.zip as {key: text}.

    Each .csv inside is base64 text; after a header line every row is
    key^text^... (the format external tools read).
    """
    best = max(folder.glob(f'sgtext_{bundle}_*.zip'),
               key=lambda p: int(p.stem.rsplit('_', 1)[1]))
    out = {}
    with zipfile.ZipFile(best) as zf:
        for info in zf.infolist():
            if not info.filename.lower().endswith('.csv'):
                continue
            text = base64.b64decode(b''.join(zf.read(info).split())).decode('utf-8', errors='replace')
            for line in text.splitlines()[1:]:
                cols = line.split('^')
                if len(cols) >= 2 and cols[0]:
                    out[cols[0]] = cols[1].replace('\\n', '\n')
    return out


def capture_sort_key(path: pathlib.Path) -> tuple:
    m = CAPTURE_DATE.search(path.name)
    if not m:
        return (0,)
    y, d, mo, h, mi = map(int, m.groups())
    return (y, mo, d, h, mi)


def load_captures(folder: pathlib.Path) -> dict:
    """{lower-case mission name: [capture, ...] newest first}."""
    by_name = collections.defaultdict(list)
    for path in sorted(folder.glob('*.json'), key=capture_sort_key, reverse=True):
        for name, capture in json.loads(path.read_text(encoding='utf-8')).items():
            by_name[name.strip().lower()].append((path.name, capture))
    return by_name


def common_layouts(records: list) -> dict:
    counts = collections.defaultdict(collections.Counter)
    for record in records:
        for stage in record['stages']:
            layout = tuple(m['position'] for m in stage['battle_monsters'])
            counts[len(layout)][layout] += 1
    return {n: list(c.most_common(1)[0][0]) for n, c in counts.items()}


def treasure(capture_drop: dict) -> tuple[int, list]:
    chance = int(round(capture_drop.get('chest drop chance%', 0) or 0))
    drops = []
    for target, key in ((1, 'zel'), (2, 'karma'), (3, 'bc/hc')):
        weight = capture_drop.get(f'{key} chance%')
        if weight:
            drops.append({'target_type': target, 'weight': int(round(weight)),
                          'amount': int(round(capture_drop.get(f'{key} amount', 0) or 0)), 'target_id': ''})
    return (chance if drops else 0), drops


class AiBuilder:
    """Captured AI rows and skills -> ai.json records and BattleMonster.skills.

    The capture parser printed each row's conditions with the skill INDEX
    already resolved to the monster's skill NAME ("set skill: Eruption"), so
    one captured AI id can read differently per monster.  Turning names back
    into 1-based indexes into that monster's own skill list restores the
    original row; identical rows share one archive record, id
    CAPTURED_AI_BASE + 100 x captured id + variant (captured ids reach
    8,150,005, so the id stays below 900,000,000 and clear of the authored
    ids, which are all below CAPTURED_AI_BASE or exactly 60,000,001).

    Two things are left out because the parse is ambiguous about them, and a
    wrong guess can freeze an enemy: AI flags (`flg_on` rows and the number an
    action carries) and `turn_act`'s two-number window.  Those rows are dropped,
    so the enemy falls through to its other rows -- never to nothing, because a
    row-less turn is a plain attack.
    """

    CAPTURED_AI_BASE = 30_000_000
    UNSUPPORTED = {'flg_on', 'flg_off', 'turn_act'}

    def __init__(self):
        rows = json.loads((MST / 'skill_mst.json').read_text())
        rows = next(iter(rows.values())) if isinstance(rows, dict) else rows
        self.skills = collections.defaultdict(list)
        for r in rows:
            frames = [f.split(':') for f in (r.get('6Aou5M9r') or '').split(',') if f]
            dist = [int(f[1]) for f in frames if len(f) > 1 and f[1].lstrip('-').isdigit()]
            self.skills[(r.get('0nxpBDz2') or '').strip().lower()].append((int(r['nj9Lw7mV']), dist))
        # Any skill the server's skill table holds.  Monster-only skills (the
        # client files their F_SKILL_MST rows, ToZj4zyn "0", in a temporary
        # list it empties on every Home and after every result) used to
        # crash the battle load (GameScene::requestMonsterSummonFiles
        # @0x16433cc); MissionStart now re-sends those rows under 8aiBoHg5
        # (server 2bec1c1), so the captured skills are used as recorded.
        # MissionStart refuses a monster naming a skill skill_mst lacks.
        self.allowed = {int(r['nj9Lw7mV']) for r in rows}
        self.records = {}          # rows json -> archive record
        self.variants = collections.Counter()
        self.stats = collections.Counter()
        # Ids are content-addressed across runs: rows already in ai.json keep
        # their id, so gen_designed_missions and this script agree.
        self.taken = set()
        for r in json.loads((ARCHIVE / 'ai.json').read_text()):
            self.taken.add(r['id'])
            if r.get('name', '').startswith(('captured ', 'wiki ')):
                self.records[json.dumps(r['actions'], sort_keys=True)] = r
                captured, variant = divmod(r['id'] - self.CAPTURED_AI_BASE, 100)
                self.variants[captured] = max(self.variants[captured], variant + 1)

    def skill_ids(self, cap_skills: list) -> list:
        """One id per captured skill (None when no unit-owned skill has that
        name), preferring the one whose hit pattern matches."""
        out = []
        for s in cap_skills:
            cands = [c for c in self.skills.get(str(s.get('name', '')).strip().lower(), [])
                     if c[0] in self.allowed]
            if not cands:
                out.append(None)
                continue
            want = list(s.get('hit dmg% distribution') or [])
            exact = [sid for sid, dist in cands if dist == want]
            out.append(min(exact or [sid for sid, _ in cands]))
        return out

    def ai_id(self, cap: dict, kept: list) -> int:
        """`kept`: the captured skill names this monster keeps, in order; a
        row casting any other skill is dropped."""
        names = kept
        all_names = [str(s.get('name', '')) for s in cap.get('skills') or []]
        rows = []
        for action in cap['ai']['actions']:
            conds, drop = [], False
            for c in action['conditions/set parameters']:
                if c['type'] in self.UNSUPPORTED:
                    drop = True
                    break
                if c['type'] == 'set skill':
                    if c['parameters'] not in all_names:
                        self.stats['unknown skill reference'] += 1
                        return 1
                    if c['parameters'] not in names:
                        drop = True          # a monster-only skill: unsafe, see allowed
                        self.stats['rows dropped (monster-only skill)'] += 1
                        break
                    conds.append({'type': 'skill', 'parameter': names.index(c['parameters']) + 1})
                    continue
                try:
                    conds.append({'type': c['type'], 'parameter': int(c['parameters'])})
                except ValueError:
                    self.stats[f'non-numeric {c["type"]}'] += 1
                    return 1
            if drop:
                if not any(c['type'] == 'set skill' for c in action['conditions/set parameters']) or \
                        any(c['type'] in self.UNSUPPORTED for c in action['conditions/set parameters']):
                    self.stats['rows dropped (flags/turn_act)'] += 1
                continue
            if len(conds) > 5:
                self.stats['more than 5 conditions'] += 1
                return 1
            rows.append({
                'priority': len(rows) + 1, 'percent': float(action['percent']),
                'act_target': int(action['target type']), 'search_term': action['target conditions'],
                'self_conditions': conds, 'party_conditions': [],
                'action': {'type': action['action']['type'], 'flag_changes': [], 'unknown_bool': True,
                           'unknown_int_1': 0, 'unknown_int_2': 0}})
        if not rows:
            return 1
        key = json.dumps(rows, sort_keys=True)
        if key not in self.records:
            captured = int(cap['ai id'])
            while True:
                new_id = self.CAPTURED_AI_BASE + 100 * captured + self.variants[captured]
                self.variants[captured] += 1
                if new_id not in self.taken:
                    break
            if self.variants[captured] > 100:
                raise SystemExit(f'captured AI {captured} has more than 100 variants')
            self.taken.add(new_id)
            self.records[key] = {'id': new_id, 'name': f'captured {captured} {cap["ai"].get("name", "")}'.strip(),
                                 'actions': rows}
        return self.records[key]['id']

    def register(self, rows: list, name: str, base: int) -> int:
        """Content-addressed record for authored rows (ids from `base` up)."""
        key = json.dumps(rows, sort_keys=True)
        if key not in self.records:
            new_id = base
            while new_id in self.taken:
                new_id += 1
            self.taken.add(new_id)
            self.records[key] = {'id': new_id, 'name': name, 'actions': rows}
        return self.records[key]['id']

    def skill_id(self, name: str) -> int | None:
        cands = [sid for sid, _ in self.skills.get(str(name).strip().strip('"').lower(), []) if sid in self.allowed]
        return min(cands) if cands else None

    def apply(self, cap: dict, monster: dict) -> None:
        caps = cap.get('skills') or []
        ids = self.skill_ids(caps)
        kept = [str(s.get('name', '')) for s, i in zip(caps, ids) if i is not None]
        if len(kept) < len(caps):
            self.stats['monster-only skills left out'] += len(caps) - len(kept)
        monster['ai_id'] = self.ai_id(cap, kept)
        if kept and monster['ai_id'] != 1:
            monster['skills'] = [{'skill_id': i, 'show_name': True} for i in ids if i is not None]
        self.stats['monsters with captured AI' if monster['ai_id'] != 1 else 'monsters on AI 1'] += 1

    def merge_into(self, path: pathlib.Path) -> int:
        existing = json.loads(path.read_text())
        known = {r['id'] for r in existing}
        added = [r for r in self.records.values() if r['id'] not in known]
        path.write_text(json.dumps(sorted(existing + added, key=lambda r: r['id']), indent=4,
                                   ensure_ascii=False) + '\n', encoding='utf-8')
        return len(added)


class Converter:
    def __init__(self, sg: pathlib.Path):
        self.mission_names = {int(m[1]): v for k, v in sgtext(sg, 'Dungeons_M_NAME').items()
                              if (m := re.match(r'MST_DUNGEONS_MISSION_(\d+)_NAME$', k))}
        self.monster_ids = collections.defaultdict(list)
        for k, v in sgtext(sg, 'monsters').items():
            if m := re.match(r'MST_MONSTERS_(\d+)_NAME$', k):
                self.monster_ids[v.strip().lower()].append(int(m[1]))
        unit_rows = json.loads((ARCHIVE / 'unit.json').read_text())
        self.units = {u['id'] for u in unit_rows}
        self.unit_names = {u['id']: u['name'] for u in unit_rows}
        self.synthetic = {}
        self.ai = AiBuilder()

    def monster_id(self, name: str, unit_id: int) -> int:
        ids = self.monster_ids.get(name.strip().lower())
        if ids:
            return min(ids)
        key = (name, unit_id)
        if key not in self.synthetic:
            self.synthetic[key] = SYNTHETIC_MONSTER_ID + len(self.synthetic)
        return self.synthetic[key]

    def monster(self, cap: dict, position: str) -> dict:
        unit_id = int(cap['unit id'])
        lo, _, hi = str(cap.get('act counts') or '1~1').partition('~')
        drop = cap.get('unit drop') or {}
        drop_chance = int(drop.get('percent') or 0)
        chest, drops = treasure(cap.get('treasure drop') or {})
        name = cap['_name']
        if CJK.search(name):            # a JP capture: the unit's Global name
            name = self.unit_names.get(unit_id, name)
        out = {
            'id': self.monster_id(name, unit_id),
            'name': name,
            'unit_id': unit_id,
            'position': position,
            'hp': int(cap['stats']['hp']),
            'atk': int(cap['stats']['atk']),
            'def': int(cap['stats']['def']),
            'ai_id': 1,
            'act_min': int(lo or 1),
            'act_max': int(hi or lo or 1),
            'wait': 5,
            'unit_drop_id': unit_id if drop_chance else 0,
            'unit_drop_level': int(drop.get('level') or 1) if drop_chance else 0,
            'unit_drop_type': DROP_TYPE.get(drop.get('type'), 5) if drop_chance else 0,
            'unit_drop_chance': drop_chance,
            # The captured chance is the real one: keep MissionArchiver's 40%
            # floor (meant for the regenerated archive's junk 0/5) off it.
            'authored_capture': True,
            'zel_max_drop': int(cap.get('zel max drop') or 0),
            'zel_drop_count': int(cap.get('zel drop count') or 0),
            'karma_max_drop': int(cap.get('karma max drop') or 0),
            'karma_drop_count': int(cap.get('karma drop count') or 0),
            'treasure_chest_chance': chest,
            'treasure_drops': drops,
        }
        resists = cap.get('condition resists') or {}
        if any(resists.values()):
            out['ailment_resists'] = {
                'poison': int(resists.get('poison%', 0)), 'weak': int(resists.get('weaken%', 0)),
                'sick': int(resists.get('sick%', 0)), 'injury': int(resists.get('injury%', 0)),
                'curse': int(resists.get('curse%', 0)), 'paralysis': int(resists.get('paralysis%', 0))}
        self.ai.apply(cap, out)
        return out

    def record(self, mission_id: int, mst: dict, capture: dict, layouts: dict,
               special: dict | None = None) -> tuple[dict | None, str]:
        """`special`: name -> visuals of the record being replaced.  A capture's
        special boss (unit id 0: Juggernaut, Demon Abaddon, Cursed Estia) is drawn
        from monster art the capture does not name, so it borrows the art the
        replaced record already gives a monster of that name."""
        special = special or {}
        stages = []
        battles = capture['battles']
        for n, battle in enumerate(battles):
            caps = battle['monsters']
            if not caps:
                return None, f'wave {n + 1} is empty'
            missing = sorted({int(c['unit id']) for c in caps
                              if not (int(c['unit id']) == 0 and c['_name'] in special)} - self.units)
            if missing:
                return None, f'unit id(s) {missing} not in unit.json'
            layout = layouts.get(len(caps))
            if not layout:
                return None, f'no layout for {len(caps)} enemies'
            monsters = [self.monster(c, p) for c, p in zip(caps, layout)]
            for m in monsters:
                if m['unit_id'] == 0:
                    art = special[m['name']]
                    if 'unit_id' in art:
                        m['unit_id'] = art['unit_id']
                    else:
                        m['visuals'] = art
            stages.append({'is_boss': n == len(battles) - 1, 'first_attack_rate': 0,
                           'battle_monsters': monsters})
        return {
            'id': mission_id,
            'name': self.mission_names.get(mission_id) or mst.get('0iAIR2LP') or '',
            'zel': int(float(mst.get('Rs7bCE3t') or 0)),
            'karma': int(float(mst.get('HTVh8a65') or 0)),
            'exp': int(float(mst.get('d96tuT2E') or 0)),
            'energy_cost': int(float(mst.get('A8DEK5ob') or 0)),
            'stages': stages,
        }, ''


# Special bosses whose monster art has no animation file anywhere (monster_cgg_87600100
# is missing from the client and the CDN): drawn with this unit's art instead.
BOSS_UNIT_ART = {'cursed estia': 850015}     # Princess Estia


def regenerated(record: dict) -> bool:
    """A record the bulk regeneration produced rather than someone authoring it.

    Authored records carry what regeneration never emitted: an AI other than 1,
    monster skills or special-boss visuals, a mission script, or a deliberate
    capture flag (the event dungeons).  The tutorial is scripted, so it never
    counts as regenerated.
    """
    if record['id'] in TUTORIAL or record.get('script_monsters'):
        return False
    return all(m['ai_id'] == 1 and not m.get('skills') and not m.get('visuals')
               and not m.get('authored_capture')
               for stage in record['stages'] for m in stage['battle_monsters'])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', type=pathlib.Path, required=True, help='missions_parsed/ folder')
    ap.add_argument('--sgtext', type=pathlib.Path, required=True, help='content/sgtext/ folder')
    ap.add_argument('--check', action='store_true', help='report only; do not write mission.json')
    ap.add_argument('--report', type=pathlib.Path, help='write a JSON report of every decision')
    ap.add_argument('--replace-regenerated', action='store_true',
                    help='also replace records the bulk regeneration made (see regenerated()) with the capture')
    ap.add_argument('--replace-ids', type=int, nargs='+', default=[],
                    help='also replace these missions\' records, however they were made: stand-ins built before '
                         'the capture was matched (the Unholy Tower floors, Trials 02/03), checked by hand')
    args = ap.parse_args()

    raw = MISSIONS.read_bytes()
    records = json.loads(raw)
    by_id = {r['id']: r for r in records}
    mst = {int(m['j28VNcUW']): m for m in json.loads((MST / 'mission_mst.json').read_text())['oXeC1Ak9']}
    conv = Converter(args.sgtext)
    layouts = common_layouts(records)
    captures = load_captures(args.captures)

    missing_by_name = collections.defaultdict(list)
    replaceable = {mid for mid, r in by_id.items() if args.replace_regenerated and regenerated(r)}
    replaceable |= {mid for mid in args.replace_ids if mid in by_id and mid not in TUTORIAL}
    for mid in mst:
        if (mid not in by_id or mid in replaceable) and conv.mission_names.get(mid):
            missing_by_name[conv.mission_names[mid].strip().lower()].append(mid)

    added, skipped, ai_ids = [], [], collections.Counter()
    for name, runs in sorted(captures.items()):
        targets = missing_by_name.get(name, [])
        if not targets:
            continue
        counts = {mid: int(mst[mid].get('69vnphig') or 0) for mid in targets}
        run = next(((f, c) for f, c in runs if all(len(c['battles']) == n for n in counts.values())), None)
        if not run:
            skipped.append({'name': name, 'missions': targets, 'reason':
                            f'wave counts {sorted(set(counts.values()))} vs captured '
                            f'{sorted({len(c["battles"]) for _, c in runs})}'})
            continue
        source, capture = run
        for mid in targets:
            special = {}
            for stage in by_id.get(mid, {}).get('stages', []) if mid in replaceable else []:
                for m in stage['battle_monsters']:
                    v = m.get('visuals')
                    if v and v.get('img_a') and v.get('anm_cgg'):
                        special[m['name']] = v
                    elif v and m['name'].lower().startswith(tuple(BOSS_UNIT_ART)):
                        # Art without an animation file (MissionArchiver refuses it):
                        # draw the boss with the unit it is a form of.
                        special[m['name']] = {'unit_id': next(u for k, u in BOSS_UNIT_ART.items()
                                                              if m['name'].lower().startswith(k))}
            record, why = conv.record(mid, mst[mid], capture, layouts, special)
            if not record:
                skipped.append({'name': name, 'missions': [mid], 'reason': why})
                continue
            by_id[mid] = record
            added.append({'mission': mid, 'name': record['name'], 'source': source,
                          'waves': len(record['stages']), 'replaced': mid in replaceable})
            for battle in capture['battles']:
                ai_ids.update(int(m['ai id']) for m in battle['monsters'])

    # JP captures are filed by mission id (missions_parsed/jp/<id>_<date>.json).
    for path in sorted(args.captures.glob('jp/*.json'), key=capture_sort_key, reverse=True):
        m = BY_ID.match(path.name)
        mid = int(m[1]) if m else 0
        if mid not in mst or mid in by_id:
            continue
        capture = next(iter(json.loads(path.read_text(encoding='utf-8')).values()))
        if len(capture['battles']) != int(mst[mid].get('69vnphig') or 0):
            skipped.append({'name': path.name, 'missions': [mid], 'reason': 'wave count differs'})
            continue
        record, why = conv.record(mid, mst[mid], capture, layouts)
        if not record:
            skipped.append({'name': path.name, 'missions': [mid], 'reason': why})
            continue
        by_id[mid] = record
        added.append({'mission': mid, 'name': record['name'], 'source': f'jp/{path.name}',
                      'waves': len(record['stages'])})

    print(f'{sum(1 for a in added if a.get("replaced"))} of them replace a regenerated record')
    print(f'added {len(added)} mission(s), skipped {len(skipped)}; '
          f'{len(conv.synthetic)} synthetic monster id(s); {len(ai_ids)} captured AI id(s) seen')
    for s in skipped:
        print(f'  skip {s["missions"]} {s["name"]!r}: {s["reason"]}')
    if args.report:
        args.report.write_text(json.dumps({'added': added, 'skipped': skipped,
                                           'captured_ai_ids': sorted(ai_ids)}, indent=1, ensure_ascii=False))
    print('AI:', dict(conv.ai.stats), f'{len(conv.ai.records)} captured AI record(s)')
    if not args.check and added:
        conv.ai.merge_into(ARCHIVE / 'ai.json')
        sources = json.loads(SOURCES.read_text()) if SOURCES.exists() else {}
        for a in added:
            sources[str(a['mission'])] = {'method': 'captured', 'source': a['source'],
                                          **({'replaced': 'regenerated record'} if a.get('replaced') else {})}
        SOURCES.write_text(json.dumps(dict(sorted(sources.items(), key=lambda kv: int(kv[0]))),
                                      indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        MISSIONS.write_text(json.dumps([by_id[k] for k in sorted(by_id)], indent=1, ensure_ascii=False) + '\n',
                            encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
