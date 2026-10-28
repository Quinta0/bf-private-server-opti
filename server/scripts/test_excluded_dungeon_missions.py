"""Souls Training Ground (Soul Sanctuary Lv. 1-4, missions 100050-100053) keeps the x3 speed button.

The client hides the battle-speed button for every mission in the UserInfo
reply's ExcludedDungonMissionMstResponse (key 3aDk1xk7, mission id j28VNcUW);
MissionBattleManager::checkBattleSpeedButton is the table's only reader.  The
server sends deploy/mst/excluded_dungeon_mission_mst.json as it is, so this
checks that file.

usage: python3 scripts/test_excluded_dungeon_missions.py
"""
import json
import sys
from pathlib import Path

TABLE_KEY, MISSION_ID = '3aDk1xk7', 'j28VNcUW'
SOULS_TRAINING_GROUND = {'100050', '100051', '100052', '100053'}
# The other 22 stay hidden: nobody asked for their speed button.
STILL_EXCLUDED = {
    '100500', '100501', '100502', '100600', '100601', '100602', '100603', '100604',
    '100605', '100606', '100607', '100608', '100609', '100610', '100611', '100612',
    '100613', '100614', '100650', '100651', '100652', '102920',
}


def main() -> int:
    path = Path(__file__).resolve().parent.parent / 'deploy' / 'mst' / 'excluded_dungeon_mission_mst.json'
    data = json.loads(path.read_text(encoding='utf-8'))
    ids = [row[MISSION_ID] for row in data[TABLE_KEY]]
    failures = []
    if len(ids) != len(set(ids)):
        failures.append('a mission id is listed twice')
    clash = SOULS_TRAINING_GROUND & set(ids)
    if clash:
        failures.append(f'Souls Training Ground still excluded: {sorted(clash)}')
    if set(ids) != STILL_EXCLUDED:
        failures.append(f'table drifted: extra {sorted(set(ids) - STILL_EXCLUDED)}, '
                        f'missing {sorted(STILL_EXCLUDED - set(ids))}')
    for failure in failures:
        print('FAIL', failure)
    print(f'{len(ids)} excluded missions' + ('' if failures else ', Souls Training Ground has its speed button'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
