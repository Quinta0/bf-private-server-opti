"""Wire test for two round-2 core gaps (RAIDS_ROUND2_CORE.md 8, items 1 and 4).

1. Kill-count clear conditions: mission 2003 ("Underground Loiterer") is won
   by RaidMisCondClearMst "2:213101:5", not by a boss.  No battle data names
   monster 213101 (it is not a RaidBossPartsMst monster), so the server counts
   every defeated enemy toward it.  BattleEnd's zS9s5bEI.6RxkwBv7
   ("monsterId:count,...") is added to the run's tally once per closed
   battle; the run is won when the tally meets every target.
2. Battle item use: a client event of type 2 ("0:itemId:useNum") takes the
   items off the raid item set (raid_equip_items kind 0) once, and counts
   RaidItemUse (1VbdgX76).

Usage: python -I raid_kills_test.py --url http://127.0.0.1:9965 --db /deploy/gme.sqlite
Creates a throwaway account and writes its warehouse directly: back up
deploy/gme.sqlite first.
"""
import argparse
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from raid_entry_test import RAID_AGREEMENT, RAID_ITEM_EDIT, Player  # noqa: E402
from raid_field_test import RAID_BATTLE_END, Run, first, mine, party, refused  # noqa: E402

MISSION, NEEDED = '2003', 5
ANY = '*'   # the tally key that counts every defeated enemy
CURE = '20000'
WON = '2'
ITEM_USE_STAT = '1VbdgX76'


def route_from(mst_dir: Path, mission: str, point: int) -> int:
    """A point one RaidUserRouteMst step from `point` (not itself)."""
    data = json.loads((mst_dir / 'raid_user_route_mst.json').read_text())
    rows = data if isinstance(data, list) else next(iter(data.values()))
    for row in rows:
        if row['j28VNcUW'] == mission and int(row['4vSjr6zw']) == point and int(row['Qf67k1ut']) != point:
            return int(row['Qf67k1ut'])
    raise RuntimeError(f'no route from point {point} in mission {mission}')


def battle_end(run: Run, defeated: str, events: list) -> dict:
    return run.p.call(RAID_BATTLE_END, **run.room_node(),
                      yL3Yrf1M=[{'1kisc6IF': run.serial, 'SP29fLtH': '', 'x69GYheC': '0'}],
                      zS9s5bEI=[{'M4NAYyc6': '2', '6RxkwBv7': defeated, 'g5L3bKPu': '', '4T0Q2Bh5': ''}],
                      QP4cwt7n=events, wc2DiMY1=party(run.p.user_id))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9965')
    ap.add_argument('--db', default='/deploy/gme.sqlite')
    args = ap.parse_args()
    db = sqlite3.connect(args.db, timeout=15, isolation_level=None)
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail)[:600] if detail and not ok else ''}")

    def value(sql: str, *params, default=None):
        row = db.execute(sql, params).fetchone()
        return row[0] if row else default

    a = Player(args.url, 'K')
    a.call(RAID_AGREEMENT)
    # A raid item set of 3 Cure, carried out of the warehouse (CAMP_ITEMS 4.5).
    db.execute('INSERT INTO user_items(user_id, item_id, item_num) VALUES (?, ?, 5)'
               ' ON CONFLICT(user_id, item_id) DO UPDATE SET item_num = 5', (a.user_id, int(CURE)))
    a.call(RAID_ITEM_EDIT, Fihk73ZR=[{'kixHbe54': CURE, 'XuJL4pc5': '0', 'wgV86x1q': '3'}], GvV1w4yM=[])
    check('0 raid set holds 3 Cure', value('SELECT item_num FROM raid_equip_items WHERE user_id = ? AND kind = 0',
                                           a.user_id) == 3)

    run = Run(a, MISSION)
    r = run.start()
    start = int(mine(r, '81B4TvEK', a.user_id).get('pjX27bKB') or 0)
    point = route_from(Path(args.db).parent / 'mst', MISSION, start)
    run.info(point)

    # Battle 1: three enemies of two kinds, and 2 Cure used.
    r = run.battle_start(point)
    check('1a route battle opened', not refused(r) and run.serial not in ('', '0'), r)
    r = battle_end(run, '100100:2,100200:1', [run.new_event(2, f'0:{CURE}:2')])
    result = first(r, 'g97TWISb').get('3tWuc7de')
    check('1b 3 of 5 enemies defeated: run still running', not refused(r) and result == '1', first(r, 'g97TWISb'))
    check('1c kill tally: 3 in all, per monster too',
          value('SELECT num FROM raid_mission_kill WHERE room_mission_id = ? AND monster_id = ?', run.rm, ANY) == 3
          and value('SELECT num FROM raid_mission_kill WHERE room_mission_id = ? AND monster_id = ?',
                    run.rm, '100100') == 2)
    check('1d battle item event took 2 Cure off the set',
          value('SELECT item_num FROM raid_equip_items WHERE user_id = ? AND kind = 0', a.user_id, default=0) == 1)
    check('1e RaidItemUse counted 2', value('SELECT value FROM raid_user_stat WHERE user_id = ? AND stat = ?',
                                            a.user_id, ITEM_USE_STAT) == 2)

    # A replayed BattleEnd (same serial, same event id) adds nothing.
    replay = [{'NH65Wj0f': '2', 'o96QPNZn': str(run.event - 1), 'xIB0C8PN': f'0:{CURE}:2'}]
    battle_end(run, '100100:3', replay)
    check('2 replayed BattleEnd: tally and set unchanged',
          value('SELECT num FROM raid_mission_kill WHERE room_mission_id = ? AND monster_id = ?', run.rm, ANY) == 3
          and value('SELECT item_num FROM raid_equip_items WHERE user_id = ? AND kind = 0', a.user_id, default=0) == 1)

    # Battle 2: the last two, which meets the target.
    r = run.battle_start(point)
    r = battle_end(run, '100300:2', [])
    check('3a five enemies defeated: run won', first(r, 'g97TWISb').get('3tWuc7de') == WON, first(r, 'g97TWISb'))
    r = run.end()
    check('3b MissionEnd collects the win', not refused(r), r)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
