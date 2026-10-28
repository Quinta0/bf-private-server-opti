"""Wire test for the daily Challenges (server branch feat/daily-challenges,
ticket #18): the Challenges tab of the Randall Merit hall lists the
open window's challenges, counts real battles for the ones a player started,
pays each finished one once, and turns over at 08:00 UTC.

Against a running LOCAL server and its save database:
   1. The list (GetAchievementInfo mode 1 / category 2) is the open window's
      set, every row "Start" (state 0) with its target and the seconds left.
   2. Start All (AchievementAccept, comma list, flag 1) starts as many as the
      cap of 5 allows; nothing else is stored.
   3. Give Up (flag 2) drops an attempt; a finished, unclaimed challenge of an
      earlier window still shows, still counts against the cap, and is
      claimable once.
   4. A cleared quest moves the started quest challenges by exactly what its
      battle log says, and nothing that was not started.
   5. A second quest adds to running totals and leaves "in one Quest"
      high-water marks alone when it did less.
   6. An Arena win moves the started arena challenges (wins, battles,
      flawless, BB kills, overkills, enemies by element).
   7. Finishing challenges makes them claimable and lights the badge (2:0:N);
      Receive All pays their Merit Points once, a second claim pays nothing.
   8. A challenge started in an earlier window and never finished comes back
      ONCE as an expired notice (state 3), then never again.
   9. The challenge week rides in the H9ATfJ38 table (35 category-2 rows).

Raid clears ("Clear N Raid Quests") are not driven here; they are counted in
RaidMissionEnd on a won run.  Which challenges are open depends on the day,
so the checks follow whatever window the server is in.

Reuses the fair-summon harness (tools/test_fair_summon.py in the main repo).

Usage: python3 -I scripts/test_daily_challenges_wire.py --url http://127.0.0.1:9985
Run it against a test server, never a save with real players.
"""
import argparse
import json
import sys
import time
from pathlib import Path


def _harness() -> Path:
    for parent in Path(__file__).resolve().parents:
        if (parent / 'tools' / 'test_fair_summon.py').is_file():
            return parent / 'tools'
    raise SystemExit('tools/test_fair_summon.py not found above this script')


sys.path.insert(0, str(_harness()))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

ACHIEVEMENT_INFO = ('YPBU7MD8', 'AKjzyZ81')
REWARD_RECEIVE = ('uq69mTtR', 'cbE74zBZ')
ACCEPT = ('dx5qvm7L', 'g9N1y7bc')
BADGE_INFO = ('nJ3A7qFp', 'bGxX67KB')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
MATCHING = ('82ztj6Iy', 'Q53tUIBE')
BATTLE_START = ('7f1rg92L', 'zaMR9o0d')
BATTLE_END = ('oQNtKU51', 'jkgWq1d0')

MISSION = 10
CHAPTER_DONE = 12
CAP = 5
DAY_OFFSET = 8 * 3600
# The canonical week by challenge weekday, Monday first (DailyChallenges.hpp).
WEEK = [(89008881, 5), (89008886, 5), (89008891, 5), (89008896, 5), (89008901, 5),
        (89008906, 10), (89008906, 10)]
ALL_IDS = set(range(89008881, 89008916))

# disp_order -> (kind, high-water)
KINDS = {8500: ('quests', False), 9900: ('raids', False), 7600: ('arena_battles', False),
         7700: ('arena_wins', False), 8100: ('arena_flawless', False), 8200: ('arena_bb', False),
         8300: ('arena_overkills', False), 8000: ('arena_elem', False), 6606: ('quest_elem', False),
         6500: ('enemies', False), 5701: ('overkills', False), 7201: ('multikills', False),
         7301: ('ailments', False), 7401: ('criticals', False), 7001: ('stronger_damage', False),
         6201: ('best_sparks', True), 6301: ('best_bc', True)}
ARENA_KINDS = {'arena_battles', 'arena_wins', 'arena_flawless', 'arena_bb', 'arena_overkills', 'arena_elem'}
# Starter units by element, from deploy/mst/unit_mst.json (iNy0ZU5M): Vargas
# Fire, Selena Water, Lance Earth, Eze Thunder.
ELEMENT_UNIT = {1: '10011', 2: '20011', 3: '30011', 4: '40011'}


def window(now: int) -> tuple[int, int, list[int]]:
    day = (now - DAY_OFFSET) // 86400
    weekday = (day + 3) % 7
    start = day - 1 if weekday == 6 else day
    length = 2 if weekday >= 5 else 1
    first, count = WEEK[weekday]
    return start, (start + length) * 86400 + DAY_OFFSET, list(range(first, first + count))


def numbers(packed: str) -> list[int]:
    out, cur = [], ''
    for c in packed + ' ':
        if c.isdigit():
            cur += c
        elif cur:
            out.append(int(cur))
            cur = ''
    return out


def condition(row: dict) -> dict:
    nums = numbers(row['t5R47iwj'])
    kind, high = KINDS[int(row['8zaB7d6W'])]
    element = nums[0] if kind in ('arena_elem', 'quest_elem') else 0
    return {'kind': kind, 'element': element, 'target': nums[-1], 'high': high, 'point': int(row['9hH0neGa'])}


def quest_amounts(log: dict, won: bool) -> dict:
    elems = {}
    for pair in filter(None, log.get('Fw2dr06x', '').split(',')):
        e, n = pair.split(':')
        elems[int(e)] = elems.get(int(e), 0) + int(n)
    return {'quests': int(won), 'overkills': int(log['EcDR3f6M']), 'multikills': int(log['xFH5i4o8']),
            'ailments': int(log['J3EBd90b']), 'criticals': int(log['2z8kTtBy']),
            'stronger_damage': int(log['32edsbP0']), 'best_sparks': int(log['U8uZLA34']),
            'best_bc': int(log['hoG2ieT5']), 'enemies': sum(elems.values()), 'quest_elem': elems}


def arena_amounts(result: dict, won: bool) -> dict:
    elems = {}
    for unit in filter(None, result.get('sQ2aMk8B', '').split(',')):
        e = next(k for k, v in ELEMENT_UNIT.items() if v == unit)
        elems[e] = elems.get(e, 0) + 1
    return {'arena_battles': 1, 'arena_wins': int(won), 'arena_flawless': int(won and result['s92RgSwp'] == '1'),
            'arena_bb': int(result['XRoD9Hg7']), 'arena_overkills': int(result['Wnc6A31E']), 'arena_elem': elems}


def amount_for(cond: dict, amounts: dict) -> int:
    value = amounts.get(cond['kind'], 0)
    return value.get(cond['element'], 0) if isinstance(value, dict) else value


def battle_log(**values) -> dict:
    log = {'47cJBUxz': '', 'Najhr8m6': '0', 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
           'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
           'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0', 'EcDR3f6M': '0', 'xFH5i4o8': '0',
           'J3EBd90b': '0', '2z8kTtBy': '0', '32edsbP0': '0', 'CwN87TyL': '0', 'Fw2dr06x': ''}
    log.update({k: str(v) for k, v in values.items()})
    return log


def find_key(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        node = list(node.values())
    if isinstance(node, list):
        for item in node:
            found = find_key(item, key)
            if found is not None:
                return found
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9985')
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    now = int(time.time())
    start_day, ends_at, ids = window(now)
    if ends_at - now < 600:
        print(f'the challenge day turns over in {ends_at - now} s; run again after 08:00 UTC')
        return 2
    mst = {int(r['M7SXoc31']): r for r in json.loads(
        (args.src / 'deploy' / 'mst' / 'achievement_subject_mst.json').read_text(encoding='utf-8'))['H9ATfJ38']}
    conds = {i: condition(mst[i]) for i in ids}
    print(f'window opened on challenge day {start_day}, closes in {ends_at - now} s: {ids}')

    db = Database(args.src / 'deploy' / 'gme.sqlite', container_on_port(args.url), args.container_db)
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, energy=500, energy_full_ts=0,
               fight_point=50, max_fight_point=50, level=80)
    uid = player.user_id

    def call(action, body: dict) -> dict:
        reply = gme_call(args.url, *action, dict({'IKqx1Cn9': player.login}, **body))
        if wire_error(reply) is not None:
            raise RuntimeError(f'{action[0]} refused: {wire_error(reply)}')
        return reply

    def rows_of(reply: dict) -> dict:
        return {int(r['M7SXoc31']): {'progress': int(r['pG2n1A28']), 'need': int(r['9g4hdDJn']),
                                     'state': int(r['0B5bQgZh']), 'left': int(r['VDKB0Y5h']),
                                     'reward': int(r['rPk8gtY5'])}
                for r in reply.get('YTRJLG65') or []}

    def board() -> tuple[dict, dict]:
        reply = call(ACHIEVEMENT_INFO, {'1Q2pGAXE': [{'YQG2AE5h': '1', 'KT71m8Ae': '2', '3rhygS9K': '0'}]})
        return rows_of(reply), reply

    def accept(sids, flag: int) -> dict:
        return rows_of(call(ACCEPT, {'1Q2pGAXE': [{'KT71m8Ae': '2', '3rhygS9K': '0',
                                                   'M7SXoc31': ','.join(map(str, sids)), 'mnZ5K4Ii': str(flag)}]}))

    def claim(sids) -> dict:
        return call(REWARD_RECEIVE, {'1Q2pGAXE': [{'KT71m8Ae': '2', '3rhygS9K': '0',
                                                   'M7SXoc31': ','.join(map(str, sids))}]})

    def stored() -> dict:
        return {int(s): {'window': w, 'state': st, 'progress': p, 'reward': r}
                for w, s, st, p, r in db.rows(
                    'SELECT window_day, subject_id, state, progress, reward_received FROM user_daily_challenges'
                    ' WHERE user_id = ? AND window_day = ?', uid, start_day)}

    def merit() -> int:
        return int(db.one('SELECT achieve_point FROM user_info WHERE id = ?', uid)[0])

    def badge() -> str:
        return find_key(call(BADGE_INFO, {}), 'dUujjBBK') or ''

    def quest(log: dict, won: bool = True) -> None:
        start = call(MISSION_START, {
            '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        serial = start['Kz7qfSs5'][0]['k9cxD7Ba']
        call(MISSION_END, {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
                           'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2' if won else '3'}],
                           'rXvA1E5y': [log]})

    def arena(result: dict, won: bool = True) -> bool:
        deck = {'fq2C03jh': [{'Z0Y4RoD7': '0', 'ciDqCjAF': '0', '59b1eE6N': ''}]}
        match = call(MATCHING, deck)
        enemy = (match.get('fQ3XImM2') or [{}])[0].get('h7eY3sAK')
        if not enemy:
            return False
        start = call(BATTLE_START, dict(deck, **{'9NX83YVe': [{'h7eY3sAK': enemy, 'Z7PXoxc2': '0'}],
                                                 'rj5fgJh0': [{'t5R47iwj': '0'}]}))
        if '9NX83YVe' not in start:
            return False
        body = dict({'xL0p2MkY': start['9NX83YVe'][0]['xL0p2MkY'], '3pAQY5KW': '1' if won else '0',
                     'mFID53JZ': '5', 'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0'}, **result)
        call(BATTLE_END, {'60subGk3': [{'EfbZ7mh9': '1'}], 'yTEvm85f': [body]})
        return True

    # 1 ------------------------------------------------------------------
    rows, reply = board()
    check('1: the list is exactly the open window', sorted(rows) == ids, sorted(rows))
    check('1: every row is Start (state 0), nothing to claim',
          all(r['state'] == 0 and r['reward'] == 0 and r['progress'] == 0 for r in rows.values()), rows)
    check('1: each row carries its MST target', all(rows[i]['need'] == conds[i]['target'] for i in rows),
          {i: (rows[i]['need'], conds[i]['target']) for i in rows if i in conds})
    check('1: and the seconds left on the window',
          all(abs(r['left'] - (ends_at - now)) < 120 for r in rows.values()),
          {i: r['left'] for i, r in rows.items()})
    sent = [r for r in reply.get('H9ATfJ38') or [] if r.get('KT71m8Ae') == '2']
    check('9: the challenge week rides in H9ATfJ38', {int(r['M7SXoc31']) for r in sent} == ALL_IDS, len(sent))
    check('7: nothing claimable, no Challenges badge', '2:0:' not in badge(), badge())

    # 2 ------------------------------------------------------------------
    rows = accept(ids, 1)
    started = ids[:CAP]
    check(f'2: Start All starts the first {len(started)} (cap {CAP})',
          [i for i in ids if rows.get(i, {}).get('state') == 1] == started, rows)
    check('2: the rest stay Start', all(rows[i]['state'] == 0 for i in ids[CAP:]), rows)
    check('2: one attempt per started challenge is stored', sorted(stored()) == started, stored())

    # 3 ------------------------------------------------------------------
    victim = started[0]
    rows = accept([victim], 2)
    check('3: Give Up returns the challenge to Start', rows[victim]['state'] == 0, rows[victim])
    check('3: ...and drops its attempt', victim not in stored(), stored())
    other = sorted(ALL_IDS - set(ids))
    leftover = other[0]
    db.rows('INSERT INTO user_daily_challenges (user_id, window_day, subject_id, state, progress)'
            ' VALUES (?, ?, ?, 2, ?)', uid, start_day - 3, leftover, int(numbers(mst[leftover]['t5R47iwj'])[-1]))
    rows = accept([victim], 1)
    check('3: an unclaimed finished challenge of an earlier window still shows, claimable',
          rows.get(leftover, {}).get('state') == 2 and rows[leftover]['reward'] == 1, rows.get(leftover))
    check('3: ...and holds a slot, so re-starting the 5th is refused at the cap',
          rows[victim]['state'] == 0 and victim not in stored(), rows[victim])
    before = merit()
    claim([leftover])
    check('3: claiming it pays its Merit Points', merit() - before == int(mst[leftover]['9hH0neGa']),
          (before, merit()))
    claim([leftover])
    check('3: a second claim pays nothing', merit() - before == int(mst[leftover]['9hH0neGa']), merit())
    rows = accept([victim], 1)
    check('3: with the slot free, the 5th starts', rows[victim]['state'] == 1 and victim in stored(), rows[victim])
    check('3: the paid leftover is gone from the list', leftover not in rows, sorted(rows))

    # 4 ------------------------------------------------------------------
    first = battle_log(EcDR3f6M=7, xFH5i4o8=4, J3EBd90b=3, **{'2z8kTtBy': 6, '32edsbP0': 5000, 'CwN87TyL': 9000},
                       U8uZLA34=40, hoG2ieT5=300, Fw2dr06x='1:4,2:5,3:6,4:7,5:1')
    expect = {i: 0 for i in started}
    amounts = quest_amounts(first, True)
    quest(first)
    for i in started:
        expect[i] = min(conds[i]['target'], amount_for(conds[i], amounts))
    got = {i: s['progress'] for i, s in stored().items()}
    check('4: a cleared quest moves exactly the started quest challenges', got == expect, (got, expect))
    check('4: nothing is stored for a challenge not started', sorted(stored()) == started, sorted(stored()))
    rows, _ = board()
    check('4: the list reports the same progress',
          all(rows[i]['progress'] == expect[i] for i in started), {i: rows[i]['progress'] for i in started})

    # 5 ------------------------------------------------------------------
    second = battle_log(EcDR3f6M=2, xFH5i4o8=2, J3EBd90b=1, **{'2z8kTtBy': 1, '32edsbP0': 1000},
                        U8uZLA34=10, hoG2ieT5=50, Fw2dr06x='3:2')
    amounts = quest_amounts(second, True)
    quest(second)
    for i in started:
        add = amount_for(conds[i], amounts)
        expect[i] = min(conds[i]['target'], max(expect[i], add) if conds[i]['high'] else expect[i] + add)
    got = {i: s['progress'] for i, s in stored().items()}
    check('5: running totals add, one-quest marks keep the better battle', got == expect, (got, expect))

    # 6 ------------------------------------------------------------------
    result = {'s92RgSwp': '1', 'XRoD9Hg7': '3', 'GRCVD1i8': '3', 'Wnc6A31E': '4',
              'sQ2aMk8B': ','.join([ELEMENT_UNIT[1], ELEMENT_UNIT[3], ELEMENT_UNIT[3], ELEMENT_UNIT[4]])}
    if arena(result):
        amounts = arena_amounts(result, True)
        for i in started:
            expect[i] = min(conds[i]['target'], expect[i] + amount_for(conds[i], amounts))
        got = {i: s['progress'] for i, s in stored().items()}
        check('6: an Arena win moves the started arena challenges', got == expect, (got, expect))
    else:
        arena_started = [i for i in started if conds[i]['kind'] in ARENA_KINDS]
        check('6: an Arena battle could be fought', not arena_started, f'arena challenges started: {arena_started}')

    # 7 ------------------------------------------------------------------
    drivable = [i for i in started if conds[i]['kind'] != 'raids']
    for i in drivable:
        db.rows('UPDATE user_daily_challenges SET progress = ? WHERE user_id = ? AND window_day = ? AND subject_id = ?',
                conds[i]['target'] - 1, uid, start_day, i)
    big = battle_log(EcDR3f6M=999, xFH5i4o8=999, J3EBd90b=999, **{'2z8kTtBy': 999, '32edsbP0': 999999},
                     U8uZLA34=999, hoG2ieT5=9999, Fw2dr06x='1:99,2:99,3:99,4:99')
    quest(big)
    arena({'s92RgSwp': '1', 'XRoD9Hg7': '30', 'Wnc6A31E': '30',
           'sQ2aMk8B': ','.join(ELEMENT_UNIT[e] for e in (1, 2, 3, 4))})
    done = stored()
    finished = [i for i in drivable if done[i]['state'] == 2]
    check('7: one more battle finishes every drivable started challenge', finished == drivable,
          {i: done[i] for i in drivable})
    rows, _ = board()
    check('7: finished rows read Completed / claimable at their target',
          all(rows[i]['state'] == 2 and rows[i]['reward'] == 1 and rows[i]['progress'] == conds[i]['target']
              and rows[i]['left'] < 0 for i in finished), {i: rows[i] for i in finished})
    check(f'7: the badge counts them (2:0:{len(finished)})', f'2:0:{len(finished)}' in badge(), badge())
    before = merit()
    reply = claim(finished)
    paid = sum(conds[i]['point'] for i in finished)
    check(f'7: Receive All pays their {paid} Merit Points', merit() - before == paid, (before, merit()))
    claimed = rows_of(reply)
    check('7: ...and the reply re-sends the bucket with them claimed',
          all(claimed[i]['reward'] == 2 for i in finished) and set(ids) <= set(claimed), claimed)
    claim(finished)
    check('7: claiming again pays nothing', merit() - before == paid, merit())
    check('7: the badge is dark again', '2:0:' not in badge(), badge())

    # 8 ------------------------------------------------------------------
    expired = other[1]
    db.rows('INSERT INTO user_daily_challenges (user_id, window_day, subject_id, state, progress)'
            ' VALUES (?, ?, ?, 1, 1)', uid, start_day - 2, expired)
    rows, _ = board()
    check('8: a started challenge of an earlier window comes back as expired (state 3)',
          rows.get(expired, {}).get('state') == 3 and rows[expired]['left'] == 0, rows.get(expired))
    rows, _ = board()
    check('8: ...exactly once', expired not in rows, sorted(rows))
    check('8: the open window is untouched by it', set(ids) <= set(rows), sorted(rows))
    repeat = ids[-1]
    db.rows('INSERT INTO user_daily_challenges (user_id, window_day, subject_id, state, progress)'
            ' VALUES (?, ?, ?, 1, 1)', uid, start_day - 7, repeat)
    _, reply = board()
    states = sorted(int(r['0B5bQgZh']) for r in reply.get('YTRJLG65') or [] if int(r['M7SXoc31']) == repeat)
    check("8: last week's attempt at an id offered again is still reported, beside this week's row",
          len(states) == 2 and states[-1] == 3, states)

    # 10 -----------------------------------------------------------------
    # The lifetime records (category 1) still claim through their own latch.
    records = rows_of(call(ACHIEVEMENT_INFO, {'1Q2pGAXE': [{'YQG2AE5h': '1', 'KT71m8Ae': '1', '3rhygS9K': '-1'}]}))
    ready = [i for i, r in records.items() if r['reward'] == 1 and int(mst[i]['9hH0neGa']) > 0]
    if ready:
        record = ready[0]
        before = merit()
        reply = call(REWARD_RECEIVE, {'1Q2pGAXE': [{'KT71m8Ae': '1', '3rhygS9K': mst[record]['3rhygS9K'],
                                                    'M7SXoc31': str(record)}]})
        check('10: a finished lifetime record still pays its Merit Points',
              merit() - before == int(mst[record]['9hH0neGa']), (record, before, merit()))
        check('10: ...and reads claimed', rows_of(reply).get(record, {}).get('reward') == 2, rows_of(reply).get(record))
        call(REWARD_RECEIVE, {'1Q2pGAXE': [{'KT71m8Ae': '1', '3rhygS9K': mst[record]['3rhygS9K'],
                                            'M7SXoc31': str(record)}]})
        check('10: once', merit() - before == int(mst[record]['9hH0neGa']), merit())
    else:
        check('10: a finished lifetime record to claim', False, 'none claimable')

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
