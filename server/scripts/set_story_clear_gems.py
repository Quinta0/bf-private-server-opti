"""Story first-clear Gems: 3 for each quest area, 8 for a zone's final boss.

python scripts/set_story_clear_gems.py [--check]

The owner's call (2026-10-19): "quests areas give 3 gems on completion with
the final boss of the zone giving 8".  MissionEnd queues F_MISSION_MST
clear_rewards (SiYs27Cj) as "First clear reward" presents on a first clear
only, and in the story the Gem entry (type 8, 1 Gem) sits on the last quest of
each quest area (dungeon).  This rewrites only that entry's amount in
deploy/mst/mission_mst.json:
  8  on the zone's final boss: its quest that unlocks the next story area
     (AreaMst.need_mission_id; the nearest later area when it unlocks two,
     e.g. Morgan's 265 -> Ishgria 300 over 234 -> the side area 20000).  A zone
     that unlocks no area (the last ones) takes its highest quest id.
  3  on every other story quest that pays a Gem.
Story = mission ids 1-99,999.  Other entries and other modes are untouched.
Clears already made are not paid again: the present is first-clear only.
"""

import argparse
import collections
import json
import pathlib

MST = pathlib.Path(__file__).resolve().parent.parent / 'deploy' / 'mst'
MISSIONS = MST / 'mission_mst.json'
STORY_END = 100_000
AREA_GEMS, FINAL_GEMS = 3, 8


def ints(text) -> list:
    return [int(x) for x in str(text or '').split(',') if x.strip().isdigit() and int(x)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='report only; do not write mission_mst.json')
    args = ap.parse_args()

    data = json.loads(MISSIONS.read_bytes())
    rows = {int(r['j28VNcUW']): r for r in data['oXeC1Ak9']}
    gem = {m: r for m, r in rows.items() if 0 < m < STORY_END
           and any(e.split(':')[0] == '8' for e in (r.get('SiYs27Cj') or '').split(','))}
    zone = {m: int(r['VjCY7rX4']) for m, r in rows.items()}
    zones = collections.defaultdict(list)
    for m in sorted(gem):
        zones[zone[m]].append(m)

    unlocks = collections.defaultdict(list)      # zone -> [(area it unlocks, quest)]
    for area in json.loads((MST / 'area_mst.json').read_text())['3SG2wX0R']:
        for need in ints(area.get('HSRhkf70')):
            if need in gem and int(area['VjCY7rX4']) in zones:
                unlocks[zone[need]].append((int(area['VjCY7rX4']), need))
    finals = {}
    for z, quests in zones.items():
        if unlocks[z]:
            finals[z] = min(unlocks[z], key=lambda u: (u[0] <= z, u[0]))[1]
        else:
            finals[z] = quests[-1]

    changed = 0
    for m, r in gem.items():
        amount = FINAL_GEMS if finals[zone[m]] == m else AREA_GEMS
        entries = []
        for e in r['SiYs27Cj'].split(','):
            parts = e.split(':')
            if parts[0] == '8':
                parts[2] = str(amount)
            entries.append(':'.join(parts))
        value = ','.join(entries)
        changed += value != r['SiYs27Cj']
        r['SiYs27Cj'] = value
    print(f'{len(gem)} story quest(s) pay a first-clear Gem in {len(zones)} zone(s); '
          f'{len(finals)} final boss(es) at {FINAL_GEMS}, the rest at {AREA_GEMS}; {changed} changed')
    print('finals:', sorted(finals.values()))
    if changed and not args.check:
        MISSIONS.write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
