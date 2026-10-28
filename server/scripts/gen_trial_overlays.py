#!/usr/bin/env python3
"""Research Lab trials the archive got wrong, as balance-folder overlays.

The boss chain (gen_boss_waves.py) starts from c738033's records, so an edit
to mission.json would be lost on its next run; an overlay wins over the
archive record whatever the chain does.  Each boss keeps the chain's numbers,
AI and skills.  An overlay gets no boss drop (MissionArchiver::applyBossDrops);
the trial's own unit is its first-clear reward in mission_mst (SiYs27Cj).

  2000003 Trial No. 004  two fights (wiki "Trial No. 004", ==Battle==): Mad God
          Zebra, then Cardes the Malevolent.  The archive put both in one boss
          wave, because gen_wiki_missions read the page's two boss headers as
          one wave.  Each stands where a lone boss stands.  Cardes's summons
          (Defiant God Luther, Tyrant Goddess Phee) are not modelled.
  8360041 Trial No. X3   Temptress Xie'Jing was drawn on Brave Knight Karl
          (20233), the template's stand-in, so players fought Karl.  Her own
          unit 860156 ships only as .sam parts, which a boss cannot be drawn
          from, so she is drawn on Demonic Witch Xie'Jing (860158) as Battle
          No. X1 draws her.  Her summons (the three Fei, Bozdell) are not
          modelled.

The files go to deploy/balance-overlays/trials/<id>.json; publishing one =
copying it to <balance_root>/server/missions/.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / 'deploy' / 'archive'
OUT = ROOT / 'deploy' / 'balance-overlays' / 'trials'

SINGLE_BOSS_POSITION = '180:302'    # where every captured lone boss stands
KEYS = ('id', 'name', 'zel', 'karma', 'exp', 'energy_cost')


def one_wave(record: dict) -> dict:
    if len(record['stages']) != 1:
        raise SystemExit(f'{record["id"]}: expected one wave, found {len(record["stages"])}')
    return record['stages'][0]


def trial_004(record: dict) -> list:
    """Mad God Zebra, then Cardes the Malevolent, from the archive's one shared boss wave."""
    order = (60234, 60425)
    wave = one_wave(record)
    by_unit = {m['unit_id']: m for m in wave['battle_monsters']}
    if sorted(by_unit) != sorted(order):
        raise SystemExit(f'{record["id"]}: wave holds {sorted(by_unit)}, expected {sorted(order)}')
    return [{**wave, 'is_boss': i == len(order) - 1,
             'battle_monsters': [{**by_unit[u], 'position': SINGLE_BOSS_POSITION}]}
            for i, u in enumerate(order)]


def trial_x3(record: dict) -> list:
    """Temptress Xie'Jing drawn on Demonic Witch Xie'Jing instead of Brave Knight Karl."""
    stand_in, drawn_on = 20233, 860158
    wave = one_wave(record)
    bosses = wave['battle_monsters']
    if [(m['name'], m['unit_id']) for m in bosses] != [("Temptress Xie'Jing", stand_in)]:
        raise SystemExit(f'{record["id"]}: wave holds {[(m["name"], m["unit_id"]) for m in bosses]}')
    return [{**wave, 'battle_monsters': [{**bosses[0], 'unit_id': drawn_on}]}]


TRIALS = {2000003: trial_004, 8360041: trial_x3}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=pathlib.Path, default=OUT)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    records = {r['id']: r for r in json.loads((ARCHIVE / 'mission.json').read_text())}
    stale = []
    for mission_id, build in TRIALS.items():
        record = records[mission_id]
        overlay = {**{k: record[k] for k in KEYS}, 'stages': build(record)}
        path = args.out / f'{mission_id}.json'
        # Byte-identical to what Balance Forge publishes (battlemodel.file_bytes).
        text = json.dumps(overlay, indent=1, ensure_ascii=False) + '\n'
        print(f'{mission_id} {overlay["name"]}: {len(overlay["stages"])} waves')
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
