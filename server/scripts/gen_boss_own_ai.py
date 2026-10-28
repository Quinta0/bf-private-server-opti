"""Give every boss its own AI and skills where the archive already knows them.

python3 -I scripts/gen_boss_own_ai.py [--check] [--report FILE]

Many records were built from templates (gen_designed_missions, the bulk
regeneration), so a boss often fights with "Attack random" (AI 1, no skills) or
with the AI and skills of the template's boss, while the archive holds that very
boss's own behaviour in another mission.  This copies it over.

A DONOR is a boss-wave monster whose behaviour is its own:
  - its AI is named after it (the original Trial and Creation God AIs, the
    wiki AIs gen_wiki_missions built from the boss's skill list), or
  - it comes from a 2014 live capture (mission_sources method "captured").
A TARGET is a boss-wave monster of the same name (case and punctuation aside),
in a record that is not a capture, that either just attacks (AI 1, no skills)
or runs ANOTHER boss's named AI (a template's wiki / Trial / Creation God AI).
A capture's generic AI ("captured N ...") that a template carried along with
its own recorded monster is that monster's behaviour, and is left alone.  It takes the
donor's ai_id, skills, and act_rate / ailment_resists when the donor has them;
HP, ATK, DEF, drops and position stay.  Several donors: the one whose HP is
closest to the target's (the same fight's difficulty).
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'deploy' / 'archive'
TUTORIAL = {1, 2}
COPY = ('ai_id', 'skills', 'act_rate', 'ailment_resists')


def norm(name: str) -> str:
    return re.sub(r'[^a-z0-9]+', ' ', (name or '').lower()).strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='report only; write nothing')
    ap.add_argument('--report', type=Path)
    args = ap.parse_args()

    text = (ARCHIVE / 'mission.json').read_text(encoding='utf-8')
    records = json.loads(text)
    sources = json.loads((ARCHIVE / 'mission_sources.json').read_text(encoding='utf-8'))
    ai_names = {a['id']: norm(a.get('name')) for a in json.loads((ARCHIVE / 'ai.json').read_text(encoding='utf-8'))}

    def own(m: dict) -> bool:
        return bool(norm(m['name'])) and norm(m['name']) in ai_names.get(m['ai_id'], '')

    NAMED = ('wiki ', 'trial no', 'the creation god')

    def wrong(m: dict) -> bool:
        if m['ai_id'] == 1 and not m.get('skills'):
            return True
        name = ai_names.get(m['ai_id'], '')
        return name.startswith(NAMED) and not own(m)

    def captured(r: dict) -> bool:
        return (sources.get(str(r['id'])) or {}).get('method') == 'captured'

    donors = defaultdict(list)
    for r in records:
        for s in r['stages']:
            if not s.get('is_boss'):
                continue
            for m in s['battle_monsters']:
                if m['ai_id'] != 1 and m.get('skills') and (own(m) or captured(r)):
                    donors[norm(m['name'])].append(m)

    changed = []
    for r in records:
        if r['id'] in TUTORIAL or captured(r):
            continue
        for s in r['stages']:
            if not s.get('is_boss'):
                continue
            for m in s['battle_monsters']:
                pool = donors.get(norm(m['name']))
                if not pool or not wrong(m):
                    continue
                d = min(pool, key=lambda x: abs(x['hp'] - m['hp']))
                before = (m['ai_id'], len(m.get('skills') or []))
                for k in COPY:
                    if k in d:
                        m[k] = json.loads(json.dumps(d[k]))
                changed.append({'mission': r['id'], 'boss': m['name'], 'ai': [before[0], m['ai_id']],
                                'skills': [before[1], len(m.get('skills') or [])]})

    missions = sorted({c['mission'] for c in changed})
    print(f'{len(donors)} bosses with their own behaviour; {len(changed)} boss slots in {len(missions)} missions take it')
    if args.report:
        args.report.write_text(json.dumps(changed, indent=1) + '\n', encoding='utf-8')
    if not args.check:
        (ARCHIVE / 'mission.json').write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
