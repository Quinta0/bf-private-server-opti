#!/usr/bin/env python3
"""Fairy Tail collab dungeon battles as balance-folder overlays.

Area 800131 "Fairy Tail Collaboration" sits under Seasons Past (area_mst), so
its five dungeons are open. Its battles are the archive's stand-ins with two
fixes:

  - Lacrima (801006-801011) drop 5 at a time instead of 1. They only drop
    here, and Mard Geer's Scroll of Black Arts (150 Dark Lacrima) and Mavis's
    Radiant Quartz (40 of each light-side Lacrima) took ~400 and ~70 clears.
  - Nothing that is not a Fairy Tail monster can be caught: the Tartaros boss
    is "Deemo and the Girl" (50564), a DEEMO gate unit, at 25%.

No random mimics either, so the chests keep their Lacrima rate.  A balance
file takes chest weights of 1-1000 only, and some archive bosses carry 2500 or
10000, so a monster whose weights run higher has them all scaled down by the
same factor (one chest pick keeps its odds).  The files go
to deploy/balance-overlays/fairy-tail/<id>.json; publishing one = copying it
to <balance_root>/server/missions/ (see scripts/gen_karma_dungeon.py).
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / 'deploy' / 'archive'
OUT = ROOT / 'deploy' / 'balance-overlays' / 'fairy-tail'

LACRIMA = {801006, 801007, 801008, 801009, 801010, 801011}
LACRIMA_AMOUNT = 5
NO_CATCH = {50564}   # Deemo and the Girl
NO_MIMICS = {'chance': 0, 'monsters': []}
MAX_WEIGHT = 1000   # MissionOverlay.cpp's per-row treasure weight limit
KEYS = ('id', 'name', 'zel', 'karma', 'exp', 'energy_cost', 'stages')
MISSIONS = (8301144, 8301145, 8301146, 8301147, 8301148, 8301149,
            8301310, 8301311, 8301312, 8301320, 8301321)


def build(record: dict) -> dict | None:
    """The overlay for one mission, or None when it needs no change."""
    changed = False
    stages = []
    for stage in record['stages']:
        monsters = []
        for m in stage['battle_monsters']:
            out = dict(m)
            if m['unit_drop_chance'] and m['unit_id'] in NO_CATCH:
                out['unit_drop_chance'] = 0
                out['authored_capture'] = True
                changed = True
            drops = []
            for t in m['treasure_drops']:
                if t['target_type'] == 4 and int(t['target_id']) in LACRIMA and t['amount'] != LACRIMA_AMOUNT:
                    t = {**t, 'amount': LACRIMA_AMOUNT}
                    changed = True
                drops.append(t)
            top = max((t['weight'] for t in drops), default=0)
            if top > MAX_WEIGHT:
                drops = [{**t, 'weight': max(1, round(t['weight'] * MAX_WEIGHT / top))} for t in drops]
            out['treasure_drops'] = drops
            monsters.append(out)
        stages.append({**stage, 'battle_monsters': monsters})
    if not changed:
        return None
    return {**{k: record[k] for k in KEYS}, 'stages': stages, 'random_mimics': NO_MIMICS}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=pathlib.Path, default=OUT)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = {r['id']: r for r in json.loads((ARCHIVE / 'mission.json').read_text())}
    stale = []
    for mission_id in MISSIONS:
        record = build(records[mission_id])
        path = args.out / f'{mission_id}.json'
        if record is None:
            print(f'{mission_id} {records[mission_id]["name"]}: unchanged, no overlay')
            if path.exists():
                stale.append(path.name)
            continue
        # Byte-identical to what Balance Forge publishes (battlemodel.file_bytes).
        text = json.dumps(record, indent=1, ensure_ascii=False) + '\n'
        print(f'{mission_id} {record["name"]}: {len(record["stages"])} waves')
        if args.check:
            if not path.exists() or path.read_text() != text:
                stale.append(path.name)
            continue
        args.out.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    if stale:
        print('out of date:', ', '.join(stale))
        return 1
    if args.check:
        print('up to date')
    return 0


if __name__ == '__main__':
    sys.exit(main())
