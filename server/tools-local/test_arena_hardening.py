"""Regression test for the Arena review fixes (stage 4).

1. Forged BattleEnd counters are capped per battle, and the running totals
   saturate, so one cheater cannot push a total past INT_MAX and break login,
   rankings and matching for everyone who is shown them.  A row that is
   already past INT_MAX (written before the fix) must still load.
2. A gem refill of the Arena Orbs stops the regeneration clock, so the next
   spend does not refund an orb immediately.
3. settleBattle is one transaction: a failure part-way leaves the battle open
   and unpaid, and a resent BattleEnd then settles it exactly once.

Needs --db (the worktree's deploy/ mounted at /deploy) for the state the wire
cannot produce: a pre-fix bad row, a stale orb clock, and a failing statement
(a temporary SQLite trigger).  Run against a test server only.

Usage: python -I test_arena_hardening.py --url http://host.docker.internal:9962 --db /deploy/gme.sqlite
"""
import argparse
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, '/tools')
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_arena import (MATCHING, BATTLE_START, BATTLE_END, USER_INFO, ARENA_INFO,  # noqa: E402
                        FRIEND_LIST, Player, check, fight)
import test_arena  # noqa: E402

SHOP_USE = ('xe8tiSf4', 'qthMXTQSkz3KfH9R')
INT_MAX = 2147483647
HUGE = '1000000000'
TRIGGER = 'arena_test_fail_karma'


def db(path: str, sql: str, *args):
    con = sqlite3.connect(path, timeout=10)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def archive(p: Player) -> dict:
    return p.call(USER_INFO)['PQ56vbkI'][0]


def orbs(p: Player) -> str:
    return str(p.call(USER_INFO)['fEi17cnx'][0]['YS2JG9no'])


def refill_orbs(path: str, p: Player):
    db(path, 'UPDATE user_info SET fight_point = max_fight_point WHERE id = ?', p.user_id)


def test_counter_caps(path: str, a: Player, b: Player):
    refill_orbs(path, a)
    cheat = {'hoG2ieT5': HUGE, '6PLsn8xo': HUGE, 'mFID53JZ': HUGE}
    for _ in range(3):
        fight(a, win=True, extra=cheat)
    arc = archive(a)
    check('forged crystals capped per battle (3 x 9999)',
          (arc['8bAdgn37'], arc['I5Uin24C']) == ('29997', '29997'), arc)
    check('forged defeated units capped per battle (3 x 6)', arc['mFID53JZ'] == '18', arc['mFID53JZ'])

    # A row past INT_MAX, as the pre-fix server could write it.
    db(path, 'UPDATE user_arena SET sum_b_crystal = 3000000000, sum_h_crystal = 9000000000,'
             ' defeat_unit_cnt = 5000000000 WHERE user_id = ?', a.user_id)
    ui = a.call(USER_INFO)
    check('UserInfo survives a counter past INT_MAX', 'PQ56vbkI' in ui, str(ui)[:160])
    if 'PQ56vbkI' in ui:
        check('archive counter saturates at INT_MAX', ui['PQ56vbkI'][0]['8bAdgn37'] == str(INT_MAX),
              ui['PQ56vbkI'][0]['8bAdgn37'])
    ranking = b.call(FRIEND_LIST)
    check("another player's rankings survive the bad row",
          'BYaF62TE' in ranking and a.user_id in {r['h7eY3sAK'] for r in ranking['BYaF62TE']},
          str(ranking)[:160])
    match = b.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]})
    check('Matching still answers', 'fQ3XImM2' in match, str(match)[:160])

    refill_orbs(path, a)
    _, _, end = fight(a, win=True, extra={'hoG2ieT5': '5'})
    check('a further battle settles on the bad row', end is not None and 'yTEvm85f' in end, str(end)[:160])
    stored = db(path, 'SELECT sum_b_crystal FROM user_arena WHERE user_id = ?', a.user_id)[0][0]
    check('stored total saturated by MIN(), not grown', stored == INT_MAX, stored)


def test_refill_stops_clock(path: str, a: Player):
    now = int(time.time())
    db(path, 'UPDATE user_info SET fight_point = 0, gems = 10 WHERE id = ?', a.user_id)
    db(path, 'UPDATE user_arena SET orb_ts = ? WHERE user_id = ?', now - 1800, a.user_id)
    reply = a.call(SHOP_USE, {'32ibWjFG': [{'60IsqxDt': '4', '03UGMHxF': '1'}]})
    check('ShopUse refills the orbs', str(reply.get('fEi17cnx', [{}])[0].get('YS2JG9no')) == '3', str(reply)[:160])
    ts = db(path, 'SELECT orb_ts FROM user_arena WHERE user_id = ?', a.user_id)[0][0]
    check('gem refill clears the orb clock', ts == 0, ts)

    # A stale clock left on a full bar (pre-fix rows): the next sync clears it.
    db(path, 'UPDATE user_arena SET orb_ts = ? WHERE user_id = ?', now - 7200, a.user_id)
    a.call(ARENA_INFO)
    ts = db(path, 'SELECT orb_ts FROM user_arena WHERE user_id = ?', a.user_id)[0][0]
    check('sync on a full bar clears a stale clock', ts == 0, ts)

    fight(a, win=False)
    check('spending after a refill is not refunded', orbs(a) == '2', orbs(a))


def test_atomic_settle(path: str, a: Player):
    db(path, 'UPDATE user_info SET fight_point = 3, gems = 0 WHERE id = ?', a.user_id)
    db(path, 'UPDATE user_arena SET points = 480, rank_id = 1, max_rank_id = 1 WHERE user_id = ?', a.user_id)
    match = a.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]})
    start = a.call(BATTLE_START, {'9NX83YVe': [{'h7eY3sAK': match['fQ3XImM2'][0]['h7eY3sAK']}]})
    battle_id = start['9NX83YVe'][0]['xL0p2MkY']
    end_body = {'yTEvm85f': [{'xL0p2MkY': battle_id, '3pAQY5KW': '1'}]}

    # The karma UPDATE runs after the rank-up gem: fail it to land mid-settle.
    db(path, f'CREATE TRIGGER {TRIGGER} BEFORE UPDATE OF karma ON user_info'
             f" WHEN NEW.id = '{a.user_id}' BEGIN SELECT RAISE(ABORT, 'arena test'); END;")
    try:
        failed = a.call(BATTLE_END, end_body)
    finally:
        db(path, f'DROP TRIGGER IF EXISTS {TRIGGER};')
    check('settle failing part-way is refused', 'yTEvm85f' not in failed, str(failed)[:160])
    status, result = db(path, 'SELECT status, result_json FROM arena_battle WHERE battle_id = ?', int(battle_id))[0]
    check('battle rolled back to open', (status, result) == (0, ''), (status, result[:40]))
    gems = db(path, 'SELECT gems FROM user_info WHERE id = ?', a.user_id)[0][0]
    max_rank = db(path, 'SELECT max_rank_id FROM user_arena WHERE user_id = ?', a.user_id)[0][0]
    check('rank-up gem rolled back', (gems, max_rank) == (0, 1), (gems, max_rank))

    end = a.call(BATTLE_END, end_body)
    ok = 'yTEvm85f' in end
    check('resent BattleEnd settles the battle', ok, str(end)[:160])
    if ok:
        res = end['yTEvm85f'][0]
        check('rank-up 1 -> 2 paid once', (res['s07yWuZR'], res['C1w4H62i']) == ('1', '2')
              and str(end['fEi17cnx'][0]['03UGMHxF']) == '1', (res, end['fEi17cnx'][0].get('03UGMHxF')))
        check('second resend is identical', a.call(BATTLE_END, end_body) == end)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9962')
    ap.add_argument('--db', required=True)
    args = ap.parse_args()

    a, b = Player(args.url, 'HardenA'), Player(args.url, 'HardenB')
    a.identify()
    b.identify()
    # Keep a and b in each other's matching band.
    db(args.db, 'INSERT OR IGNORE INTO user_arena (user_id, updated_at) VALUES (?, 0)', b.user_id)

    test_counter_caps(args.db, a, b)
    test_refill_stops_clock(args.db, a)
    test_atomic_settle(args.db, a)

    print(f'{test_arena.failures} failure(s)')
    return 1 if test_arena.failures else 0


if __name__ == '__main__':
    sys.exit(main())
