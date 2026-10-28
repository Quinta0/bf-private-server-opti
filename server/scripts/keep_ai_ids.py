"""Keep generated AI ids stable across reruns of the boss-wave chain.

python3 -I scripts/keep_ai_ids.py --previous OLD_AI_JSON

Run last.  The chain numbers its AIs (ids from 900,000,000) in the order it builds
them, so one new kit early on shifts every later id, and a live Forge overlay
(/opt/bf/prod/balance/server/missions) that names such an id would silently fight
with another boss's rows (Acnologia 900000228, Ptah 900000248).  Here every
generated AI whose rows the previous ai.json already had takes that AI's id; a
changed or new one gets a fresh id above the previous ones; a previous AI nothing
uses any more is kept, since an overlay may still name it.  ai_id references in
mission.json, mission_variants.json and deploy/balance-overlays are rewritten.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'deploy' / 'archive'
BASE = 900_000_000


def rows_key(ai: dict) -> str:
    return json.dumps(ai['actions'], sort_keys=True)


def monsters(doc):
    """Every monster dict in a mission record, variants file or overlay."""
    if isinstance(doc, dict):
        if 'ai_id' in doc and 'hp' in doc:
            yield doc
        for v in doc.values():
            yield from monsters(v)
    elif isinstance(doc, list):
        for v in doc:
            yield from monsters(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--previous', type=pathlib.Path, required=True, help='the ai.json prod serves now')
    args = ap.parse_args()
    previous = json.loads(args.previous.read_text(encoding='utf-8'))
    ai_text = (ARCHIVE / 'ai.json').read_text(encoding='utf-8')
    current = json.loads(ai_text)
    prev_gen = {a['id']: a for a in previous if a['id'] >= BASE}
    by_rows = {}
    for i, a in sorted(prev_gen.items()):
        by_rows.setdefault(rows_key(a), i)
    next_id = max([BASE - 1, *prev_gen]) + 1
    remap, used = {}, set()
    for a in sorted((a for a in current if a['id'] >= BASE), key=lambda a: a['id']):
        old = by_rows.get(rows_key(a))
        if old is not None and old not in used:
            remap[a['id']] = old
        else:
            remap[a['id']], next_id = next_id, next_id + 1
        used.add(remap[a['id']])
    out = [a for a in current if a['id'] < BASE]
    out += [dict(a, id=remap[a['id']]) for a in current if a['id'] >= BASE]
    out += [a for i, a in prev_gen.items() if i not in used]          # an overlay may name it
    out.sort(key=lambda a: a['id'])
    files = [ARCHIVE / 'mission.json', ARCHIVE / 'mission_variants.json',
             *sorted((ROOT / 'deploy' / 'balance-overlays').glob('*/*.json'))]
    touched = 0
    for f in files:
        text = f.read_text(encoding='utf-8')
        doc = json.loads(text)
        n = 0
        for m in monsters(doc):
            if m['ai_id'] in remap and remap[m['ai_id']] != m['ai_id']:
                m['ai_id'] = remap[m['ai_id']]
                n += 1
        if n:
            f.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + ('\n' if text.endswith('\n') else ''), encoding='utf-8')
            touched += n
    (ARCHIVE / 'ai.json').write_text(json.dumps(out, indent=4, ensure_ascii=False)
                                     + ('\n' if ai_text.endswith('\n') else ''), encoding='utf-8')
    kept = sum(1 for k, v in remap.items() if v in prev_gen)
    print(f'{kept} AIs keep their previous id, {len(remap) - kept} new, '
          f'{len(prev_gen) - len(used & set(prev_gen))} previous kept for overlays, {touched} references rewritten')
    return 0


if __name__ == '__main__':
    sys.exit(main())
