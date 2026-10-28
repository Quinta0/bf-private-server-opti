"""Wire test for the Challenges badge after a battle (server branch
feat/challenge-badge, follow-up of ticket #18).

The Missions > Challenges count (RandallAchievementTopScene::setLayout,
getBadgeCount(2) + getBadgeCount(5)) is AchievementBadgeInfo, whose only
writer is parseBadgeData under h23iRjGN.dUujjBBK.  The client asks for
BadgeInfo only at login and on Home / Randall town entry, so the battle ends
that count daily Challenges now carry the complete badge set themselves.

Against a running LOCAL server and its save database:
   1. Every MissionEnd reply carries h23iRjGN.dUujjBBK, equal to what
      BadgeInfo answers at that moment (the complete set: parseBadgeData
      replaces it), dark while nothing is claimable.
   2. A MissionEnd that finishes a started quest challenge answers 2:0:1, and
      a replay of it (the no-reward path, a retry after a lost reply) too.
   3. An ArenaBattleEnd that finishes an Arena challenge answers 2:0:2.  A
      resend gets the stored reply plus the CURRENT badge; the stored copy
      (arena_battle.result_json) holds none.
   4. A RaidMissionEnd of a won run that finishes a raid challenge answers
      2:0:3; the retry likewise, with no badge in raid_mission_user.result_json.
   5. After Receive All: a replayed MissionEnd, a resent ArenaBattleEnd and a
      retried RaidMissionEnd all answer the badge dark -- computed now, never
      the stored moment's count.

Which challenges are open depends on the day; the test picks a quest-side and
an Arena challenge from the open window (every window has both) and drives
each to target - 1 in the save first.  Only Tuesday and the weekend offer a
raid challenge; on other days the test seeds Tuesday's (89008890) as a started
row of the open window, which the server counts like any other.
Reuses the fair-summon harness (tools/test_fair_summon.py in the main repo)
and scripts/test_daily_challenges_wire.py next to this file.

Usage: <venv with pycryptodome>/python -I scripts/test_challenge_badge_wire.py --url http://127.0.0.1:9987
Run it against a test server, never a save with real players.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_challenges_wire import (  # noqa: E402  (also puts tools/ on sys.path)
    ACCEPT, ACHIEVEMENT_INFO, ARENA_KINDS, BADGE_INFO, BATTLE_END, BATTLE_START, CHAPTER_DONE, ELEMENT_UNIT,
    MATCHING, MISSION, MISSION_END, MISSION_START, REWARD_RECEIVE, battle_log, condition, find_key, window)
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

BADGE = 'h23iRjGN'
BADGE_DATA = 'dUujjBBK'

RAID_AGREEMENT = ('Cu94xU8j', 'Eh5a3yr9')
RAID_QUICK_START = ('CWeTY8BQ', 'HsVofOJJ')
RAID_MISSION_START = ('2qDW9NBU', 'kY5y1zBj')
RAID_MISSION_INFO = ('BqF2x3EC', '6SyFs0Du')
RAID_BATTLE_START = ('8UiQj0uF', '1Jhavqb7')
RAID_BATTLE_END = ('5Ko2BjPA', 'gi6B0m2p')
RAID_UPDATE_PROGRESS = ('7geDL5Vb', '7DkVMG8Y')
RAID_MISSION_END = ('vgin9Mf1', '95ULkowE')
RAID_FALLBACK = 89008890      # Tuesday's "Clear 2 Raid Quests", seeded on days without one
RAID_MISSION = '2000'          # Edelma: boss 1 at point 4, one part 210001, 40000 HP
RAID_BOSS_PART = '210001'
UNIT_HP = 3000

# A battle log big enough to finish any quest-side challenge it touches.
BIG_LOG = dict(EcDR3f6M=999, xFH5i4o8=999, J3EBd90b=999, U8uZLA34=999, hoG2ieT5=9999,
               Fw2dr06x='1:99,2:99,3:99,4:99,5:99,6:99', **{'2z8kTtBy': 999, '32edsbP0': 999999})
QUIET_LOG = dict(Fw2dr06x='')


def badge_of(reply: dict):
    """The h23iRjGN.dUujjBBK string a reply carries, or None without the key."""
    block = reply.get(BADGE)
    if isinstance(block, list):
        block = block[0] if block else {}
    return None if block is None else (block or {}).get(BADGE_DATA)


def raid_party(uid: str, start: bool = False) -> list:
    rows = []
    for order in range(4):
        row = {'edy7fq3L': str(100 + order), 'pn16CNah': '10017', 'e7DK0FQT': str(UNIT_HP),
               'XuJL4pc5': str(order), 'gr48vsdJ': '0' if order == 0 else '1', '4A6LzBxr': '30',
               '2pAyFjmZ': '0', 'nBTx56W9': '1'}
        if start:
            row['PWXu25cg'] = '500'
        else:
            row.update({'h7eY3sAK': uid, 'gU2xtQ0V': '5', 'nIGZ1X9C': '0', '98WfKiyA': ''})
        rows.append(row)
    return rows


def first(reply: dict, group: str) -> dict:
    rows = reply.get(group) or [{}]
    return rows[0] if isinstance(rows, list) else rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9987')
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    ap.add_argument('--seed-raid', action='store_true',
                    help='seed the raid challenge even when the window has one (exercises the weekday path)')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail)[:500] if detail != '' else ''}")

    now = int(time.time())
    start_day, ends_at, ids = window(now)
    if ends_at - now < 900:
        print(f'the challenge day turns over in {ends_at - now} s; run again after 08:00 UTC')
        return 2
    mst = {int(r['M7SXoc31']): r for r in json.loads(
        (args.src / 'deploy' / 'mst' / 'achievement_subject_mst.json').read_text(encoding='utf-8'))['H9ATfJ38']}
    conds = {i: condition(mst[i]) for i in ids}
    quest_id = next((i for i in ids if conds[i]['kind'] not in ARENA_KINDS | {'raids'}), None)
    arena_id = next((i for i in ids if conds[i]['kind'] in ARENA_KINDS), None)
    raid_id = None if args.seed_raid else next((i for i in ids if conds[i]['kind'] == 'raids'), None)
    seeded_raid = raid_id is None
    if seeded_raid:
        raid_id = RAID_FALLBACK
        conds[raid_id] = condition(mst[raid_id])
    print(f'window {start_day}: quest {quest_id}, arena {arena_id}, raid {raid_id}{" (seeded)" if seeded_raid else ""}')

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

    def badge_info() -> str:
        return find_key(call(BADGE_INFO, {}), BADGE_DATA) or ''

    def near_target(sid: int) -> None:
        db.rows('UPDATE user_daily_challenges SET progress = ? WHERE user_id = ? AND window_day = ? AND subject_id = ?',
                conds[sid]['target'] - 1, uid, start_day, sid)

    def state(sid: int):
        row = db.one('SELECT state FROM user_daily_challenges WHERE user_id = ? AND window_day = ? AND subject_id = ?',
                     uid, start_day, sid)
        return row[0] if row else None

    def quest(log: dict, won: bool = True) -> tuple[dict, dict]:
        start = call(MISSION_START, {
            '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        serial = start['Kz7qfSs5'][0]['k9cxD7Ba']
        body = {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
                'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2' if won else '3'}],
                'rXvA1E5y': [battle_log(**log)]}
        return body, call(MISSION_END, body)

    def arena_battle(result: dict, won: bool = True):
        deck = {'fq2C03jh': [{'Z0Y4RoD7': '0', 'ciDqCjAF': '0', '59b1eE6N': ''}]}
        match = call(MATCHING, deck)
        enemy = (match.get('fQ3XImM2') or [{}])[0].get('h7eY3sAK')
        if not enemy:
            return None, None
        start = call(BATTLE_START, dict(deck, **{'9NX83YVe': [{'h7eY3sAK': enemy, 'Z7PXoxc2': '0'}],
                                                 'rj5fgJh0': [{'t5R47iwj': '0'}]}))
        if '9NX83YVe' not in start:
            return None, None
        entry = dict({'xL0p2MkY': start['9NX83YVe'][0]['xL0p2MkY'], '3pAQY5KW': '1' if won else '0',
                      'mFID53JZ': '5', 'hoG2ieT5': '0', '6PLsn8xo': '0', '5NRJQ1LU': '0', 'XP06YWdT': '0'}, **result)
        body = {'60subGk3': [{'EfbZ7mh9': '1'}], 'yTEvm85f': [entry]}
        return body, call(BATTLE_END, body)

    def raid_win() -> tuple[dict, dict, dict, str]:
        """A solo quick-start run of mission 2000, won on the boss, then MissionEnd twice."""
        call(RAID_AGREEMENT, {})
        room = first(call(RAID_QUICK_START, {'8iBuZa3n': [{'j28VNcUW': RAID_MISSION}]}), '8iBuZa3n').get('8VYd6xSX', '')
        room_node = {'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room}]}
        started = call(RAID_MISSION_START, {'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room, '8ZK3w0Np': uid}],
                                            'wc2DiMY1': raid_party(uid, start=True)})
        rm = next((r.get('FV6MU5nQ') for r in started.get('uwH62EZy', []) if r.get('h7eY3sAK') == uid), '')
        nodes = dict(room_node, g97TWISb=[{'ja39HMLq': rm}])
        for point in (2, 4):
            call(RAID_MISSION_INFO, dict(nodes, pw38qdui=[{'NLKce95n': '0'}], **{'81B4TvEK': [{'pjX27bKB': str(point)}]}))
        battle = call(RAID_BATTLE_START, dict(room_node, **{
            '9Q1Lq5FS': [{'h7eY3sAK': uid, 'j28VNcUW': RAID_MISSION}],
            '2cBk5WH8': [{'ja39HMLq': rm, 'SP29fLtH': '1', 'qbJcuC60': '4'}]}))
        serial = first(battle, 'yL3Yrf1M').get('1kisc6IF', '')
        hit = {'NH65Wj0f': '1', 'o96QPNZn': '1', 'xIB0C8PN': f'1:{RAID_BOSS_PART}:40000'}
        won = call(RAID_UPDATE_PROGRESS, dict(nodes, yL3Yrf1M=[{'1kisc6IF': serial, 'SP29fLtH': '1'}],
                                              QP4cwt7n=[hit], wc2DiMY1=raid_party(uid), pw38qdui=[{'NLKce95n': '0'}]))
        if first(won, 'g97TWISb').get('3tWuc7de') != '2':
            raise RuntimeError(f'raid run not won: {first(won, "g97TWISb")}')
        call(RAID_BATTLE_END, dict(room_node, yL3Yrf1M=[{'1kisc6IF': serial, 'SP29fLtH': '1', 'x69GYheC': '0'}],
                                   zS9s5bEI=[{'M4NAYyc6': '2', '6RxkwBv7': '', 'g5L3bKPu': '', '4T0Q2Bh5': ''}],
                                   QP4cwt7n=[], wc2DiMY1=raid_party(uid)))
        end_body = dict(nodes, GWfpjED4=[{'GWfpjED4': '1'}])
        return end_body, call(RAID_MISSION_END, end_body), call(RAID_MISSION_END, end_body), rm

    def stored_arena(battle_id: str) -> str:
        row = db.one('SELECT result_json FROM arena_battle WHERE battle_id = ?', int(battle_id))
        return row[0] if row else ''

    def stored_raid(rm: str) -> str:
        row = db.one('SELECT result_json FROM raid_mission_user WHERE room_mission_id = ? AND user_id = ?', int(rm), uid)
        return row[0] if row else ''

    # 1 ------------------------------------------------------------------
    chosen = [i for i in (quest_id, arena_id, raid_id) if i is not None]
    accepted = [i for i in chosen if not (seeded_raid and i == raid_id)]
    call(ACCEPT, {'1Q2pGAXE': [{'KT71m8Ae': '2', '3rhygS9K': '0',
                                'M7SXoc31': ','.join(map(str, accepted)), 'mnZ5K4Ii': '1'}]})
    if seeded_raid:
        db.rows('INSERT INTO user_daily_challenges (user_id, window_day, subject_id, state, progress)'
                ' VALUES (?, ?, ?, 1, 0)', uid, start_day, raid_id)
    check('0: the chosen challenges are started', all(state(i) == 1 for i in chosen), {i: state(i) for i in chosen})
    _, reply = quest(QUIET_LOG)
    sent = badge_of(reply)
    check('1: a MissionEnd that finishes nothing still carries h23iRjGN.dUujjBBK', sent is not None,
          sorted(reply)[:40])
    check('1: ...dark (no 2:0:) while nothing is claimable', sent is not None and '2:0:' not in sent, sent)
    check('1: ...and equal to what BadgeInfo answers (the complete set)', sent == badge_info(), (sent, badge_info()))

    # 2 ------------------------------------------------------------------
    expected = 0
    if quest_id is not None:
        near_target(quest_id)
        body, reply = quest(BIG_LOG)
        expected += 1
        check('2: the quest challenge is finished', state(quest_id) == 2, state(quest_id))
        check(f'2: that MissionEnd answers the Challenges badge 2:0:{expected}',
              f'2:0:{expected}' in (badge_of(reply) or ''), badge_of(reply))
        check('2: ...equal to BadgeInfo', badge_of(reply) == badge_info(), (badge_of(reply), badge_info()))
        replay = call(MISSION_END, body)
        check(f'2: a replay of it (no-reward path, a retry after a lost reply) answers 2:0:{expected} too',
              badge_of(replay) == badge_of(reply), (badge_of(replay), badge_of(reply)))
    else:
        check('2: a quest-side challenge in this window', False, ids)

    # 3 ------------------------------------------------------------------
    arena_body = None
    if arena_id is not None:
        near_target(arena_id)
        result = {'s92RgSwp': '1', 'XRoD9Hg7': '30', 'Wnc6A31E': '30',
                  'sQ2aMk8B': ','.join(ELEMENT_UNIT[e] for e in (1, 2, 3, 4))}
        body, reply = arena_battle(result)
        if reply is None:
            check('3: an Arena battle could be fought', False, 'no opponent')
        else:
            expected += 1
            check('3: the Arena challenge is finished', state(arena_id) == 2, state(arena_id))
            check(f'3: that ArenaBattleEnd answers 2:0:{expected}',
                  f'2:0:{expected}' in (badge_of(reply) or ''), badge_of(reply))
            check('3: ...equal to BadgeInfo', badge_of(reply) == badge_info(), (badge_of(reply), badge_info()))
            arena_body = body
            again = call(BATTLE_END, body)
            check('3: a resent BattleEnd gets the stored reply with the current badge',
                  again == reply, (sorted(set(again) ^ set(reply)), badge_of(again), badge_of(reply)))
            check('3: ...while the stored copy holds no badge',
                  stored_arena(body['yTEvm85f'][0]['xL0p2MkY']).startswith('{')
                  and BADGE not in stored_arena(body['yTEvm85f'][0]['xL0p2MkY']),
                  stored_arena(body['yTEvm85f'][0]['xL0p2MkY'])[-120:])
    else:
        check('3: an Arena challenge in this window', False, ids)

    # 4 ------------------------------------------------------------------
    near_target(raid_id)
    raid_body, end1, end2, rm = raid_win()
    expected += 1
    check('4: the raid challenge is finished', state(raid_id) == 2, state(raid_id))
    check(f'4: RaidMissionEnd answers the badge 2:0:{expected}',
          f'2:0:{expected}' in (badge_of(end1) or ''), badge_of(end1))
    check('4: ...equal to BadgeInfo', badge_of(end1) == badge_info(), (badge_of(end1), badge_info()))
    check('4: its retry gets the stored reply with the current badge',
          end2 == end1, (sorted(set(end1) ^ set(end2)), badge_of(end2)))
    check('4: ...while the stored copy holds no badge',
          stored_raid(rm).startswith('{') and BADGE not in stored_raid(rm), stored_raid(rm)[-120:])

    # 5 ------------------------------------------------------------------
    finished = [i for i in chosen if state(i) == 2]
    call(REWARD_RECEIVE, {'1Q2pGAXE': [{'KT71m8Ae': '2', '3rhygS9K': '0', 'M7SXoc31': ','.join(map(str, finished))}]})
    check('5: Receive All claimed them, BadgeInfo is dark', bool(finished) and '2:0:' not in badge_info(),
          (finished, badge_info()))
    dark = badge_info()
    body, reply = quest(QUIET_LOG)
    replay = call(MISSION_END, body)
    check('5: a replayed MissionEnd (no-reward path) still carries the badge, dark',
          badge_of(replay) is not None and '2:0:' not in badge_of(replay), badge_of(replay))
    check('5: ...equal to BadgeInfo', badge_of(replay) == badge_info(), (badge_of(replay), badge_info()))
    if arena_body is not None:
        again = call(BATTLE_END, arena_body)
        check('5: a resent ArenaBattleEnd now answers dark, not its old 2:0:',
              badge_of(again) == dark, (badge_of(again), dark))
    again = call(RAID_MISSION_END, raid_body)
    check('5: a retried RaidMissionEnd now answers dark, not its old 2:0:', badge_of(again) == dark,
          (badge_of(again), dark))

    # cost --------------------------------------------------------------
    t0 = time.perf_counter()
    for _ in range(20):
        badge_info()
    print(f'info: BadgeInfo round trip {1000 * (time.perf_counter() - t0) / 20:.1f} ms '
          f'(the badge a battle end now computes, plus one HTTP call)')

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
