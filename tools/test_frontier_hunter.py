"""Wire test for Frontier Hunter scoring and rewards (server branch
feat/frontier-hunter-rewards, ticket #8).

Against a running LOCAL server and its save database:
  A. The Survey Office (ChallengeBase) carries the lobby phase (s7A3bGLe): stat 1,
     the running event, a period text, HR 1.
  B. A cleared run of quest 1009700 (Entrance, 0.1 % conversion) scores: the
     MissionEnd reply carries the result blocks (ZYSWU9V1 score, 21BqKn8b grade and
     Frontier Service gained, ZSC3t4Fv the point rewards reached, cv9rC2KN the best),
     the point rewards land in the present box once, and the week's service adds up.
     MissionStart tells the client the best so far (7B5Ue9kH).
  C. A lower second run adds service but no reward; a lost run adds nothing and
     shows 0; a score past 9,999,999 is capped; a retried result pays nothing.
  D. The ranking (ChallengeRanking) lists the player on the total board with their
     service, on the quest board 101 with their best, and on the sparks board.
  E. A point reward of type 10 (1009702 at 1.1M) raises Hunter Rank at once and pays
     that level's ladder reward.
  F. Claiming during the running week pays nothing.  A finished week (written into
     the save as last week's scores) turns the lobby to stat 3; ChallengeRankReward
     then pays it ONCE: one HR card per level (each with its ladder reward), the
     rank tier's items, the MVP sphere, all into the present box, no card of type
     10, the lobby back on stat 1 and the new HR in kN2i7qds.  A second claim and
     a burst of concurrent claims on another finished week pay exactly once.

Reuses the fair-summon harness (tools/test_fair_summon.py): throwaway guest
accounts, save edits made inside the server's container.

Usage: python3 -I tools/test_frontier_hunter.py --src wt/frohun --url http://127.0.0.1:9986
Run it against a test server, never a save with real players.
"""
import argparse
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, post, seal, wire_error  # noqa: E402

MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
CHALLENGE_BASE = ('nUAW2B0a', 'uE5Tsv6P')
CHALLENGE_RANKING = ('2Kxi7rIB', 'v1PzNE9f')
RANK_REWARD = ('m3fr7vxo', '4BuRdgr2')
CHAPTER_DONE = 12

EVENT = 97
ENTRANCE, MIDDLE, END = 1009700, 1009701, 1009702
WEEK = 7 * 24 * 3600
EPOCH_MONDAY = 4 * 24 * 3600


def this_week() -> int:
    return (int(time.time()) - EPOCH_MONDAY) // WEEK


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9986')
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    mst = args.src / 'deploy' / 'mst'
    point_rewards = [r for r in json.load(open(mst / 'chlng_mission_reward_mst.json'))['4C1Wt8sS']]
    ladder = {int(r['Sv80kL5r']): r for r in json.load(open(mst / 'challenge_hr_mst.json'))['h09mEvDR']}
    mvp_pairs = {(int(r['9w5e3ZJB']), r['j28VNcUW']) for r in json.load(open(mst / 'challenge_mvp_mst.json'))['nUmaEC41']
                 if r['c5yZnpB4'] == str(EVENT)}

    def rewards_upto(mission: int, score: int) -> set[int]:
        return {int(r['N1b6SUW4']) for r in point_rewards
                if int(r['j28VNcUW']) == mission and int(r['TPR79fyI']) <= score}

    db = Database(args.src / 'deploy' / 'gme.sqlite', container_on_port(args.url), args.container_db)
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, hunter_orbs=3, hunter_orb_rest_ts=0)
    uid = player.user_id
    sk = [{'Kn51uR4Y': 'frohun'}]

    def call(target, body: dict, who: Player | None = None) -> dict:
        who = who or player
        reply = gme_call(args.url, *target, dict({'IKqx1Cn9': who.login, '6FrKacq7': sk}, **body))
        if wire_error(reply) is not None:
            raise RuntimeError(f'{target[0]} refused: {wire_error(reply)}')
        return reply

    def presents() -> int:
        return db.one('SELECT COUNT(*) FROM user_presents WHERE user_id = ?', uid)[0]

    ends = {}   # the last MissionEnd body per player, for the retry check

    def run(mission: int, score: int, won: bool = True, note: str = '', who: Player | None = None) -> tuple[dict, dict]:
        who = who or player
        who.set(hunter_orbs=3, hunter_orb_rest_ts=0)
        start = call(MISSION_START, {
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}], 'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}, who)
        serial = start['Kz7qfSs5'][0]['k9cxD7Ba']
        log = {'47cJBUxz': '', 'Najhr8m6': '0', 'HTVh8a65': '0', '4T0Q2Bh5': '', '3MAT6quo': '',
               'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0', 'U8uZLA34': '0',
               'rZQJF5G9': '0', 'TW1Mrtp5': '0', 'e6BKoYy9': '0'}
        body = {
            'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2' if won else '3'}], 'rXvA1E5y': [log],
            'ZYSWU9V1': [{'TPR79fyI': str(score), 'pG2n1A28': '5', 'gUvjqu39': '1',
                          '7cev0fzQ': note or 'battle:5,spark:300,overkill:20,multikill:10,bbkill:30,oneturn:2'}],
            'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}
        ends[who.user_id] = body
        return start, call(MISSION_END, body, who)

    def week_service(cycle: int) -> int:
        row = db.one('SELECT COALESCE(SUM(service), 0) FROM user_frohun_scores WHERE user_id = ? AND cycle = ?',
                     uid, cycle)
        return row[0]

    def lobby() -> dict:
        reply = call(CHALLENGE_BASE, {})
        return {'stat': reply.get('s7A3bGLe', {}).get('j0Uszek2'), 'user': reply.get('s7A3bGLe', {}),
                'hr': reply.get('kN2i7qds', {}).get('Sv80kL5r'), 'event': reply.get('cvg8hzp9', {}).get('c5yZnpB4')}

    # A ---------------------------------------------------------------------
    first = lobby()
    check('A: the Survey Office reply carries the lobby phase: stat 1', first['stat'] == '1', first['user'])
    check('A: ...for the running event, with a period and HR 1',
          first['event'] == str(EVENT) and first['user'].get('s2gM3deu') and first['hr'] == '1', first)

    # B ---------------------------------------------------------------------
    week = this_week()
    before = presents()
    start, end = run(ENTRANCE, 120_000)
    check('B: MissionStart tells the client the best so far (7B5Ue9kH = 0)',
          start.get('7B5Ue9kH', {}).get('TPR79fyI') == '0', start.get('7B5Ue9kH'))
    check('B: the result echoes the score (ZYSWU9V1)', end.get('ZYSWU9V1', {}).get('TPR79fyI') == '120000',
          end.get('ZYSWU9V1'))
    result = end.get('21BqKn8b', {})
    check('B: grade S (5) and 120 Frontier Service (0.1 % of 120000)',
          result.get('Cdv07KEU') == '5' and result.get('TPR79fyI') == '120', result)
    paid = {int(r['empaR60j']) for r in end.get('ZSC3t4Fv', []) if r.get('IkmC8gG2') == '1'}
    expected = {int(r['TPR79fyI']) for r in point_rewards
                if int(r['j28VNcUW']) == ENTRANCE and int(r['TPR79fyI']) <= 120_000}
    check('B: the result lists the point rewards reached (50k, 100k)', paid == expected, (paid, expected))
    check('B: ...and they are in the present box', presents() - before == len(expected), presents() - before)
    stored = {r[0] for r in db.rows('SELECT reward_id FROM user_frohun_rewards WHERE user_id = ? AND cycle = ?',
                                    uid, week)}
    check('B: ...recorded as paid this week', stored == rewards_upto(ENTRANCE, 120_000), stored)
    best = {r['j28VNcUW']: r['TPR79fyI'] for r in end.get('cv9rC2KN', [])}
    check('B: the per-quest bests (cv9rC2KN) show 120000', best.get(str(ENTRANCE)) == '120000', best)
    check('B: the header total is the week\'s service', end.get('s7A3bGLe', {}).get('TPR79fyI') == '120',
          end.get('s7A3bGLe'))

    # C ---------------------------------------------------------------------
    before = presents()
    start, end = run(ENTRANCE, 60_000)
    check('C: the next start knows the best (120000)', start.get('7B5Ue9kH', {}).get('TPR79fyI') == '120000',
          start.get('7B5Ue9kH'))
    check('C: a lower run adds 60 service and no reward',
          week_service(week) == 180 and presents() == before
          and all(r.get('IkmC8gG2') != '1' for r in end.get('ZSC3t4Fv', [])), (week_service(week), end.get('ZSC3t4Fv')))
    _, lost = run(ENTRANCE, 500_000, won=False)
    check('C: a lost run shows score 0 (the client raises its cached best from it)',
          lost.get('ZYSWU9V1', {}).get('TPR79fyI') == '0'
          and {r['j28VNcUW']: r['TPR79fyI'] for r in lost.get('cv9rC2KN', [])}.get(str(ENTRANCE)) == '120000',
          (lost.get('ZYSWU9V1'), lost.get('cv9rC2KN')))
    runs = db.one('SELECT runs, clears FROM user_frohun_scores WHERE user_id = ? AND cycle = ? AND mission_id = ?',
                  uid, week, ENTRANCE)
    check('C: a lost run adds no service and counts as an entry', week_service(week) == 180 and list(runs) == [3, 2], runs)
    start, end = run(ENTRANCE, 99_999_999)
    cap = max(3 * max(int(r['TPR79fyI']) for r in point_rewards if int(r['j28VNcUW']) == ENTRANCE), 9_999_999)
    check(f'C: an inflated score is capped at {cap}', end.get('ZYSWU9V1', {}).get('TPR79fyI') == str(cap)
          and week_service(week) == 180 + round(cap * 0.1 / 100), (end.get('ZYSWU9V1'), week_service(week)))
    stored = {r[0] for r in db.rows('SELECT reward_id FROM user_frohun_rewards WHERE user_id = ? AND cycle = ?',
                                    uid, week)}
    check('C: ...and pays every Entrance reward once', stored == rewards_upto(ENTRANCE, cap), stored)
    before = presents()
    retry = call(MISSION_END, ends[uid])
    check('C: a retried result pays and scores nothing, and clears the reward lists',
          presents() == before and week_service(week) == 180 + round(cap * 0.1 / 100)
          and [r.get('IkmC8gG2') for r in retry.get('ZSC3t4Fv', [])] == ['0']
          and retry.get('21BqKn8b', {}).get('TPR79fyI') == '0', (retry.get('ZSC3t4Fv'), retry.get('21BqKn8b')))

    # D ---------------------------------------------------------------------
    boards = call(CHALLENGE_RANKING, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}).get('Hmiwj75u', [])
    mine = [r for r in boards if r['h7eY3sAK'] == str(uid)]
    total = [r for r in mine if r['NH65Wj0f'] == '1']
    check('D: the total board has the player with their service',
          total and total[0]['U4pMNjy0'] == str(week_service(week)) and int(total[0]['vV4P53m7']) >= 1, total)
    quest = [r for r in mine if r['NH65Wj0f'] == '2' and r['9w5e3ZJB'] == '101']
    check('D: the Entrance MVP board (101) has their best', quest and quest[0]['U4pMNjy0'] == str(cap)
          and quest[0]['j28VNcUW'] == str(ENTRANCE), quest)
    sparks = [r for r in mine if r['NH65Wj0f'] == '2' and r['9w5e3ZJB'] == '201']
    check('D: the sparks board (201) counts cleared runs only (3 x 300)',
          sparks and sparks[0]['U4pMNjy0'] == '900', sparks)
    check('D: no emulated rivals', not [r for r in boards if r['h7eY3sAK'].startswith('RIVAL')])
    check('D: every MVP row names a challenge_mvp_mst board',
          all((int(r['9w5e3ZJB']), r['j28VNcUW']) in mvp_pairs for r in boards if r['NH65Wj0f'] == '2'))

    # A player who only lost has no Frontier Service: no place on any board,
    # and no total row of their own (its RankOrder would become the header rank).
    loser = Player(args.url, db)
    loser.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1)
    run(ENTRANCE, 50_000, won=False, who=loser)
    seen = call(CHALLENGE_RANKING, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}, loser).get('Hmiwj75u', [])
    theirs = [r for r in seen if r['h7eY3sAK'] == str(loser.user_id)]
    check('D: a player with only losses has no total-board row',
          not [r for r in theirs if r['NH65Wj0f'] == '1'], theirs)
    check('D: ...and no place on the entries board (206)',
          all(r['U4pMNjy0'] == '0' for r in theirs if r['NH65Wj0f'] == '2'), theirs)
    others = call(CHALLENGE_RANKING, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}).get('Hmiwj75u', [])
    check('D: ...nor on anyone else\'s board', not [r for r in others if r['h7eY3sAK'] == str(loser.user_id)])

    # A tie goes to whoever reached the value first: a later run that adds
    # nothing must not move a player behind someone who got there after them.
    early, late = Player(args.url, db), Player(args.url, db)
    for who in (early, late):
        who.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1)
    run(ENTRANCE, 777_000, who=early)
    time.sleep(1.2)
    run(ENTRANCE, 777_000, who=late)
    time.sleep(1.2)
    run(ENTRANCE, 0, who=early, note='battle:1,spark:0')
    place = {}
    for who in (early, late):
        rows = call(CHALLENGE_RANKING, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]}, who).get('Hmiwj75u', [])
        place[who] = {r['9w5e3ZJB']: int(r['vV4P53m7']) for r in rows
                      if r['h7eY3sAK'] == str(who.user_id) and r['NH65Wj0f'] in ('1', '2')}
    check('D: equal service: the one who reached it first places higher (total board)',
          place[early].get('0', 0) and place[early]['0'] < place[late].get('0', 10**9), place)
    check('D: equal best on the Entrance: the earlier one places higher (board 101)',
          place[early].get('101', 0) and place[early]['101'] < place[late].get('101', 10**9), place)

    # E ---------------------------------------------------------------------
    run(MIDDLE, 100_000)
    hr_reward = [r for r in point_rewards if int(r['j28VNcUW']) == END and r['30Kw4WBa'] == '10']
    threshold = int(hr_reward[0]['TPR79fyI'])
    before = presents()
    _, end = run(END, threshold)
    hr_now = db.one('SELECT hunter_rank FROM user_info WHERE id = ?', uid)[0]
    check(f'E: the {threshold} point reward raises Hunter Rank 1 -> 2', hr_now == 2
          and end.get('kN2i7qds', {}).get('Sv80kL5r') == '2', (hr_now, end.get('kN2i7qds')))
    hr_row = db.one("SELECT present_type, target_id FROM user_presents WHERE user_id = ?"
                    " AND description LIKE '%Hunter Rank 2%'", uid)
    check('E: ...and pays the HR 2 ladder reward', hr_row is not None and str(hr_row[1]) == ladder[2]['TdDHf59J'],
          hr_row)

    # F ---------------------------------------------------------------------
    before = presents()
    early = call(RANK_REWARD, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]})
    check('F: a claim during the running week pays nothing',
          'p73SJiWv' not in early and presents() == before and early.get('s7A3bGLe', {}).get('j0Uszek2') == '1',
          early)

    # Last week, written into the save: far ahead of anyone, so place 1 on the
    # total board and on the Entrance board.
    last = week - 1
    db.rows('INSERT INTO user_frohun_scores (user_id, cycle, mission_id, event_id, best_score, service, runs, clears,'
            ' sparks, updated_at) VALUES (?, ?, ?, ?, ?, ?, 5, 5, 10, ?)',
            uid, last, ENTRANCE, EVENT, 2_000_000_000, 10**12, int(time.time()) - WEEK)
    pending = lobby()
    check('F: a finished week turns the lobby to stat 3 (the Reward button)', pending['stat'] == '3'
          and pending['user'].get('TPR79fyI') == str(10**12) and pending['user'].get('vV4P53m7') == '1', pending)

    hr_before = db.one('SELECT hunter_rank FROM user_info WHERE id = ?', uid)[0]
    before = presents()
    claim = call(RANK_REWARD, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]})
    cards = claim.get('p73SJiWv', [])
    hr_after = db.one('SELECT hunter_rank FROM user_info WHERE id = ?', uid)[0]
    # Place 1: the 1-100 tier (+8) and the top-10 bonus (+1).
    check(f'F: place 1 raises Hunter Rank by 9 ({hr_before} -> {hr_before + 9})', hr_after == hr_before + 9,
          hr_after)
    steps = [c for c in cards if c['NH65Wj0f'] == '1' and int(c['Sv80kL5r']) > 1]
    check('F: one HR card per level, each with its ladder reward',
          [int(c['Sv80kL5r']) for c in steps] == list(range(hr_before + 1, hr_after + 1))
          and all(c['30Kw4WBa'] == ladder[int(c['Sv80kL5r'])]['30Kw4WBa']
                  and c['TdDHf59J'] == ladder[int(c['Sv80kL5r'])]['TdDHf59J'] for c in steps),
          [(c['Sv80kL5r'], c['30Kw4WBa'], c['TdDHf59J']) for c in steps])
    items = {(c['30Kw4WBa'], c['TdDHf59J'], c['wJsB35iH']) for c in cards if c['NH65Wj0f'] == '1'
             and c['Sv80kL5r'] == '1'}
    check('F: the tier items: 40 Elementum Tomes and the God Hunter sphere',
          items == {('4', '800907', '40'), ('7', '36702', '1')}, items)
    mvp = {(c['9w5e3ZJB'], c['j28VNcUW'], c['TdDHf59J']) for c in cards if c['NH65Wj0f'] == '2'}
    check('F: the MVP cards: Entrance #1 (Heaven Sword) and sparks #1 (Spark Geyser)',
          ('101', str(ENTRANCE), '36700') in mvp and ('201', '0', '34812') in mvp, mvp)
    check('F: no card is of type 10, every MVP card names a board',
          all(c['30Kw4WBa'] != '10' for c in cards)
          and all((int(c['9w5e3ZJB']), c['j28VNcUW']) in mvp_pairs for c in cards if c['NH65Wj0f'] == '2'))
    check('F: everything went into the present box', presents() - before == len(cards), (presents() - before, len(cards)))
    check('F: the reply puts the lobby back on stat 1 with the new HR',
          claim.get('s7A3bGLe', {}).get('j0Uszek2') == '1' and claim.get('kN2i7qds', {}).get('Sv80kL5r') == str(hr_after),
          (claim.get('s7A3bGLe'), claim.get('kN2i7qds')))
    claimed = db.one('SELECT final_rank, hr_before, hr_after FROM user_frohun_claims WHERE user_id = ? AND cycle = ?',
                     uid, last)
    check('F: the claim is recorded', claimed is not None and list(claimed) == [1, hr_before, hr_after], claimed)

    before = presents()
    again = call(RANK_REWARD, {'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]})
    check('F: a second claim pays nothing', 'p73SJiWv' not in again and presents() == before
          and db.one('SELECT hunter_rank FROM user_info WHERE id = ?', uid)[0] == hr_after, again)
    back = lobby()
    check('F: the lobby is back on stat 1 with the new HR', back['stat'] == '1' and back['hr'] == str(hr_after), back)

    # A burst of claims on a second finished week pays it once.
    older = week - 2
    db.rows('INSERT INTO user_frohun_scores (user_id, cycle, mission_id, event_id, best_score, service, runs, clears,'
            ' updated_at) VALUES (?, ?, ?, ?, 1, 1, 1, 1, ?)', uid, older, ENTRANCE, EVENT, int(time.time()) - 2 * WEEK)
    before = presents()
    envelope = seal(*RANK_REWARD, {'IKqx1Cn9': player.login, '6FrKacq7': sk, 'cvg8hzp9': [{'c5yZnpB4': str(EVENT)}]})
    replies = []
    racers = [threading.Thread(target=lambda: replies.append(post(args.url, RANK_REWARD[1], envelope)))
              for _ in range(6)]
    for t in racers:
        t.start()
    for t in racers:
        t.join()
    paid_claims = [r for r in replies if 'p73SJiWv' in r]
    rows = db.one('SELECT COUNT(*) FROM user_frohun_claims WHERE user_id = ? AND cycle = ?', uid, older)[0]
    check('F: six concurrent claims of one week: one pays, one claim row',
          len(paid_claims) == 1 and rows == 1 and presents() - before == len(paid_claims[0]['p73SJiWv']),
          (len(paid_claims), rows, presents() - before))

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
