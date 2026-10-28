"""Wire test for the battle-item bar refresh (server branch fix/item-bar-refresh,
ticket #9): MissionEnd must send the bar after spending the battle's uses,
so the client cannot offer items the server has already taken.

Against a running LOCAL server and its save database:
  1. A player with Cure x5 (slot 0) and High Cure x3 (slot 1) plays mission 10 and
     uses 2 Cures: the server's bar is now 3 / 3, and the MissionEnd reply carries
     the whole bar (71U5wzhI) with those counts.
  2. A second battle reporting 4 Cures (more than the 3 held) takes the bar to
     0 and the reply says 0, so the client's bar empties too.
  3. A battle that uses nothing still re-sends the bar unchanged.

Reuses the fair-summon harness (tools/test_fair_summon.py): a throwaway guest
account, save edits made inside the server's container.

Usage: python3 -I tools/test_item_bar.py --src wt/dblock --url http://127.0.0.1:9979
Run it against a test server, never a save with real players.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
MISSION = 10                       # Start of Adventure, 3 energy
CURE, HIGH_CURE = 20000, 20001
BAR = '71U5wzhI'
CHAPTER_DONE = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9979')
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    db = Database(args.src / 'deploy' / 'gme.sqlite', container_on_port(args.url), args.container_db)
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, energy=200, energy_full_ts=0)
    db.rows('DELETE FROM user_equip_items WHERE user_id = ?', player.user_id)
    db.rows('INSERT INTO user_equip_items (user_id, disp_order, item_id, item_num) VALUES (?, 0, ?, 5), (?, 1, ?, 3)',
            player.user_id, CURE, player.user_id, HIGH_CURE)

    def stored() -> dict:
        return {int(i): int(n) for i, n in db.rows(
            'SELECT item_id, item_num FROM user_equip_items WHERE user_id = ? ORDER BY disp_order', player.user_id)}

    def battle(used: str) -> dict:
        start = gme_call(args.url, *MISSION_START, {
            'IKqx1Cn9': player.login,
            '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        if wire_error(start) is not None:
            raise RuntimeError(f'MissionStart refused: {wire_error(start)}')
        serial = start['Kz7qfSs5'][0]['k9cxD7Ba']
        log = {'47cJBUxz': used, 'Najhr8m6': '0', 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
               'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
               'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0'}
        end = gme_call(args.url, *MISSION_END, {'IKqx1Cn9': player.login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
                                                'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}],
                                                'rXvA1E5y': [log]})
        if wire_error(end) is not None:
            raise RuntimeError(f'MissionEnd refused: {wire_error(end)}')
        return end

    def sent_bar(reply: dict) -> dict | None:
        if BAR not in reply:
            return None
        return {int(r['kixHbe54']): int(r['wgV86x1q']) for r in reply[BAR]}

    reply = battle(f'{CURE}:2')
    check('1: the server spent 2 Cures', stored() == {CURE: 3, HIGH_CURE: 3}, stored())
    check('1: MissionEnd sends the whole bar with those counts', sent_bar(reply) == {CURE: 3, HIGH_CURE: 3},
          sent_bar(reply))

    reply = battle(f'{CURE}:4')
    check('2: reporting 4 Cures when 3 are held empties the slot', stored() == {CURE: 0, HIGH_CURE: 3}, stored())
    check('2: ...and the reply says 0, so the client bar empties too', sent_bar(reply) == {CURE: 0, HIGH_CURE: 3},
          sent_bar(reply))

    reply = battle('')
    check('3: a battle with no item use re-sends the bar unchanged', sent_bar(reply) == {CURE: 0, HIGH_CURE: 3},
          sent_bar(reply))

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
