#!/usr/bin/env python3
"""Wire test: Sandbox mode (ticket #137, gimuserver/gme/common/Sandbox.hpp).

    PYTHONPATH=<repo>/scripts:<repo>/tools <repo>/.venv/bin/python \
        scripts/test_sandbox_wire.py [--src wt/sandbox] [--port 9991] [--assets DIR] [--keep]

Boots one throwaway server (container bf-sandbox137-wire, save in volume bf-sandbox137-wire-data) with
four fresh accounts past the tutorial: A and N (normal), S and T (switched to Sandbox on the wire).

  serial    the Serial Code screen (CampaignReceipt 4MCxgS5p/pCIRMw04): an unknown code is refused
            plainly; SANDBOX explains and arms the confirmation; a confirmation older than 10 minutes
            only explains again; SANDBOX again confirms; once on, SANDBOX says so and nothing (a
            Grand Mission receipt included) turns it off.  Every answer is a Retry refusal (cmd 2).
  unlock    with no quest cleared, S's map holds the last story quest and more dungeons than A's
            (every Vortex day), MissionStart lets S start that quest and refuses A; S's rail holds the
            whole weekly rotation pool and S can pull a pool banner that is not featured; A cannot.
  pools     helpers (FriendGet: an A-S friendship stays on the Social list but lends no helper),
            friend requests, Arena matching and ranking, Colosseum matching and ranking, raid room
            search and RaidEnter, Frontier Hunter boards: Sandbox and normal accounts never see each
            other.
  race      reaching level 999 enters A in level_race, not S.

Removes its container and volume (unless --keep).
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

from forge_wire_common import (GACHA_LIST, MISSION_END, MISSION_START, USER_INFO, Server, end_body, gme_call,
                               permit_rows, permitted, start_body, table, wire_error)
from test_fair_summon import Database, Player

CONTAINER, VOLUME = 'bf-sandbox137-wire', 'bf-sandbox137-wire-data'
CAMPAIGN_RECEIPT = ('5Imq3wC0', '4DAgP80B')
FRIEND_GET = ('2o4axPIC', 'EoYuZ2nbImhCU1c0')
FRIEND_APPLY = ('WUNi08YL', 'gbpdAEZuv8IP40UF')
ARENA_MATCHING = ('82ztj6Iy', 'Q53tUIBE')
ARENA_FRIEND_LIST = ('xkd9H50w', 'L3TDhrn2')
COLO_BATTLE_START = ('ysP6qYcy', 'iFIHAT5V')
COLO_FRIEND_LIST = ('AoBvG2YN', 'Wj5fH1HV')
RAID_ROOM_LIST = ('YF9UQpn6', 'c0Qf94Ir')
RAID_ROOM_EDIT = ('52H0dkjv', '9KdeuMX8')
RAID_ENTER = ('yZReG75i', 'N0Azw6ob')
CHALLENGE_RANKING = ('2Kxi7rIB', 'v1PzNE9f')
RACE_MISSION = 10
RETRY = 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9991)
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive' / 'assets')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    src = args.src.resolve()
    failures = 0

    def check(label: str, ok, detail='') -> bool:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  -- ' + str(detail)[:400]}", flush=True)
        return bool(ok)

    server = Server(src, Path(tempfile.mkdtemp(prefix='sandbox-wire-')), args.port, args.assets, CONTAINER, VOLUME)
    try:
        _, missions = table(src, 'mission_mst.json')
        _, dungeons = table(src, 'dungeon_mst.json')
        dtype = {r['MHx05sXt']: r['3hPeI1RV'] for r in dungeons}
        archived = {int(r['id']) for r in json.loads((src / 'deploy/archive/mission.json').read_text(encoding='utf-8'))}
        story = sorted(int(r['j28VNcUW']) for r in missions
                       if 11 <= int(r['j28VNcUW']) < 900 and dtype.get(r['MHx05sXt']) == '0'
                       and int(r['j28VNcUW']) in archived)
        last_story = story[-1]
        pool = [g['gate'] for g in json.loads((src / 'deploy/archive/summon_rotation.json').read_text())['pool']]
        levels = json.loads((src / 'deploy/mst/user_level_mst.json').read_text())['YDv9bJ3s']
        exp_999 = next(int(r['d96tuT2E']) for r in levels if int(r['D9wXQI2V']) == 999)

        server.start()
        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')
        players = {}
        for name in 'ANST':
            p = Player(server.url, db)
            p.set(tutorial_status=12, tutorial_end_flag=1, energy=9999, energy_full_ts=0, gems=100000,
                  colosseum_tickets=50)
            players[name] = p
        A, N, S, T = (players[k] for k in 'ANST')
        ids = {k: str(p.user_id) for k, p in players.items()}

        def call(who: Player, api, body=None) -> dict:
            return gme_call(server.url, *api, {'IKqx1Cn9': who.login, **(body or {})})

        def code(who: Player, typed: str) -> dict:
            return call(who, CAMPAIGN_RECEIPT, {'4MCxgS5p': [{'pCIRMw04': typed}]})

        def cmd(reply: dict):
            return ((reply.get('_unencrypted') or {}).get('b5PH6mZa') or {}).get('iPD12YCr')

        def flags(who: Player):
            return db.one('SELECT sandbox_at, sandbox_asked_at FROM user_info WHERE id = ?', who.user_id)

        def mentions(reply, *names) -> set:
            text = json.dumps(reply)
            return {n for n in names if ids[n] in text}

        # -- serial ----------------------------------------------------------------------------
        r = code(S, 'NOTACODE')
        check('serial: an unknown code is refused plainly, Retry', wire_error(r) == 'This serial code is not valid.'
              and str(cmd(r)) == str(RETRY), r)
        r = code(S, ' sandbox ')
        on, asked = flags(S)
        check('serial: SANDBOX (any case, spaces) explains and arms the confirmation, Retry',
              'Enter SANDBOX again within 10 minutes' in (wire_error(r) or '') and str(cmd(r)) == str(RETRY)
              and on == 0 and asked > 0, (r, on, asked))
        db.rows('UPDATE user_info SET sandbox_asked_at = ? WHERE id = ?', int(time.time()) - 601, S.user_id)
        r = code(S, 'SANDBOX')
        on, asked = flags(S)
        check('serial: a confirmation older than 10 minutes only explains again',
              'Enter SANDBOX again' in (wire_error(r) or '') and on == 0 and asked > int(time.time()) - 60, (r, on))
        r = code(S, 'SANDBOX')
        on, asked = flags(S)
        check('serial: SANDBOX again within 10 minutes switches it on',
              (wire_error(r) or '').startswith('Sandbox mode is on') and on > 0 and asked == 0, (r, on))
        r = code(S, 'SANDBOX')
        check('serial: once on, SANDBOX says so and changes nothing',
              wire_error(r) == 'Sandbox mode is already on for this account.' and flags(S)[0] == on, r)
        r = call(S, CAMPAIGN_RECEIPT, {'j28VNcUW': '1'})
        check('serial: a Grand Mission receipt leaves the flag on (one way)', flags(S)[0] == on, r)
        code(T, 'SANDBOX')
        code(T, 'SANDBOX')
        check('serial: T is in Sandbox mode too', flags(T)[0] > 0, flags(T))
        check('serial: A and N stay normal', flags(A)[0] == 0 and flags(N)[0] == 0)

        # -- unlock ----------------------------------------------------------------------------
        info_s, info_a = call(S, USER_INFO), call(A, USER_INFO)
        check(f'unlock: the last story quest {last_story} is on S\'s map, not A\'s',
              permitted(info_s, 'j28VNcUW', last_story) and not permitted(info_a, 'j28VNcUW', last_story))
        dun = lambda reply: {row['MHx05sXt'] for row in permit_rows(reply) if 'MHx05sXt' in row}
        check('unlock: S\'s map holds every dungeon A\'s does, and more (every Vortex day)',
              dun(info_a) < dun(info_s), (len(dun(info_a)), len(dun(info_s))))
        r = gme_call(server.url, *MISSION_START, start_body(S.login, last_story))
        check('unlock: MissionStart lets S start the last story quest', wire_error(r) is None, wire_error(r))
        r = gme_call(server.url, *MISSION_START, start_body(A.login, last_story))
        check('unlock: ...and refuses A', 'still locked' in (wire_error(r) or ''), wire_error(r))
        rail = lambda reply: {int(g) for c in reply.get('IBs49NiH') or [] for g in str(c.get('3rCmq58M', '')).split(',')
                              if g.isdigit()}
        rail_s, rail_a = rail(S.gacha_list()), rail(A.gacha_list())
        featured = [g for g in pool if g in rail_a]
        check('unlock: S\'s rail holds the whole rotation pool, A\'s only this week\'s',
              set(pool) <= rail_s and len(featured) == 3, (sorted(set(pool) - rail_s), featured))
        other = next(g for g in pool if g not in featured)
        pull_s, pull_a = S.pull(other), A.pull(other)
        check(f'unlock: S pulls pool banner {other} that is not featured', wire_error(pull_s.reply) is None,
              wire_error(pull_s.reply))
        check('unlock: ...A is told it rotated out', 'rotated out' in (wire_error(pull_a.reply) or ''),
              wire_error(pull_a.reply))

        # -- pools -----------------------------------------------------------------------------
        now = int(time.time())
        db.rows('INSERT INTO user_relations (user_id, other_id, state, favorite, created_at) VALUES (?, ?, 1, 0, ?),'
                ' (?, ?, 1, 0, ?)', A.user_id, S.user_id, now, S.user_id, A.user_id, now)
        fs, fa = call(S, FRIEND_GET), call(A, FRIEND_GET)
        check('helpers: S\'s helper picker has no normal account', not mentions(fs.get('xZH6EIQ7'), 'A', 'N'),
              mentions(fs.get('xZH6EIQ7'), 'A', 'N'))
        check('helpers: ...but the A-S friendship stays on S\'s Social list', 'A' in mentions(fs.get('tojMy68W'), 'A'))
        check('helpers: A\'s helper picker has no Sandbox account', not mentions(fa.get('xZH6EIQ7'), 'S', 'T'),
              mentions(fa.get('xZH6EIQ7'), 'S', 'T'))
        print(f'info  helpers: S sees {sorted(mentions(fs.get("xZH6EIQ7"), "T"))}, A sees '
              f'{sorted(mentions(fa.get("xZH6EIQ7"), "N"))} as strangers')
        call(A, FRIEND_APPLY, {'BPz1e5tU': [{'h7eY3sAK': ids['T']}]})
        check('friends: A cannot send T a friend request',
              db.one('SELECT COUNT(*) FROM user_relations WHERE user_id = ? AND other_id = ?', A.user_id, T.user_id)[0] == 0)

        offered = set()
        for _ in range(4):
            offered |= mentions(call(S, ARENA_MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]}), 'A', 'N', 'T')
        check('arena: S is never offered a normal account', not offered & {'A', 'N'}, offered)
        db.rows('INSERT OR IGNORE INTO user_arena (user_id) VALUES (?), (?), (?), (?)', *(p.user_id for p in (A, N, S, T)))
        ranks = call(S, ARENA_FRIEND_LIST)
        check('arena: S\'s ranking lists T and no normal account',
              mentions(ranks, 'A', 'N', 'T') == {'T'}, mentions(ranks, 'A', 'N', 'T'))
        ranks = call(A, ARENA_FRIEND_LIST)
        check('arena: A\'s ranking lists N and no Sandbox account',
              mentions(ranks, 'N', 'S', 'T') == {'N'}, mentions(ranks, 'N', 'S', 'T'))

        cls = [{'3mMAn6L5': '1'}]
        for k, p in enumerate((A, N, S, T)):
            call(p, COLO_FRIEND_LIST, {'KtSHFZY7': cls})
            unit = db.one('SELECT user_unit_id FROM user_units WHERE user_id = ? ORDER BY user_unit_id LIMIT 1', p.user_id)[0]
            party = json.dumps([{'P_USER_UNIT_ID': str(unit), 'P_DISPORDER': '0', 'P_MEMBER_TYPE': '0'}])
            db.rows('UPDATE user_colosseum_class SET season_battle_cnt = 1, battle_cnt = 1, points = ?,'
                    ' atk_party = ?, def_party = ? WHERE user_id = ? AND class_id = 1', 100 * (k + 1), party, party,
                    p.user_id)
        board = call(S, COLO_FRIEND_LIST, {'KtSHFZY7': cls})
        check('colosseum: S\'s ranking lists T and no normal account',
              mentions(board, 'A', 'N', 'T') == {'T'}, mentions(board, 'A', 'N', 'T'))
        board = call(N, COLO_FRIEND_LIST, {'KtSHFZY7': cls})
        check('colosseum: N\'s ranking lists A and no Sandbox account',
              mentions(board, 'A', 'S', 'T') == {'A'}, mentions(board, 'A', 'S', 'T'))
        met = set()
        for _ in range(3):
            met |= mentions(call(S, COLO_BATTLE_START, {'KtSHFZY7': cls, 'fq2C03jh': [{'69vnphig': '1'}]}),
                            'A', 'N', 'T')
        check('colosseum: S is matched with T, never a normal account', met == {'T'}, met)

        def room(who: Player) -> str:
            node = {'mnZ5K4Ii': '1', '43hMuY2I': '1', 'NQR1Vd3t': '1', 'wYmg52xp': '1', 'nXCf2FE9': '1',
                    '0TfXnP3m': '1', 'j28VNcUW': '2000', 'j3sGA1zt': '1', '1Txpza2u': '1', 'KQA1oWj7': '1',
                    'TdDHf59J': ''}
            reply = call(who, RAID_ROOM_EDIT, {'8iBuZa3n': [node]})
            rows = reply.get('8iBuZa3n') or []
            rows = rows if isinstance(rows, list) else [rows]
            return str(rows[0].get('8VYd6xSX', '')) if rows else ''

        room_a, room_t = room(A), room(T)
        check('raids: A and T each opened a room', room_a and room_t, (room_a, room_t))
        listed = lambda who: {str(r.get('8VYd6xSX')) for r in (call(who, RAID_ROOM_LIST, {'8iBuZa3n': [
            {'ouU5H7qm': '1', 'j3sGA1zt': '-1', '1Txpza2u': '-1', '0TfXnP3m': '1', '43hMuY2I': '1'}]}).get('8iBuZa3n') or [])}
        found_s, found_n = listed(S), listed(N)
        check('raids: S\'s room search finds T\'s room, not A\'s', room_t in found_s and room_a not in found_s, found_s)
        check('raids: N\'s room search finds A\'s room, not T\'s', room_a in found_n and room_t not in found_n, found_n)
        r = call(S, RAID_ENTER, {'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_a}]})
        check('raids: RaidEnter refuses S A\'s room', wire_error(r) == 'This Room is no longer available.', wire_error(r))
        r = call(S, RAID_ENTER, {'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_t}]})
        check('raids: ...and lets S into T\'s', wire_error(r) is None, wire_error(r))

        cycle = (int(time.time()) - 4 * 86400) // (7 * 86400)
        for k, name in enumerate('ANT'):
            db.rows('INSERT INTO user_frohun_scores (user_id, cycle, mission_id, service, runs, service_at)'
                    ' VALUES (?, ?, 1, ?, 1, ?)', ids[name], cycle, 10 * (k + 1), now)
        board = call(S, CHALLENGE_RANKING)
        check('frontier hunter: S\'s boards list T and no normal account',
              mentions(board, 'A', 'N', 'T') == {'T'}, mentions(board, 'A', 'N', 'T'))
        board = call(A, CHALLENGE_RANKING)
        check('frontier hunter: A\'s boards list N and no Sandbox account',
              mentions(board, 'N', 'S', 'T') == {'N'}, mentions(board, 'N', 'S', 'T'))

        # -- race ------------------------------------------------------------------------------
        for p in (A, S):
            p.set(level=998, exp=exp_999 - 1)
            started = gme_call(server.url, *MISSION_START, start_body(p.login, RACE_MISSION))
            gme_call(server.url, *MISSION_END, end_body(p.login, started['Kz7qfSs5'][0]['k9cxD7Ba']))
        levels_now = {k: db.one('SELECT level FROM user_info WHERE id = ?', players[k].user_id)[0] for k in 'AS'}
        race = {str(r[0]) for r in db.rows('SELECT user_id FROM level_race')}
        check('race: A and S both reached level 999', levels_now == {'A': 999, 'S': 999}, levels_now)
        check('race: level_race holds A, not S', ids['A'] in race and ids['S'] not in race, race)
    finally:
        if not args.keep:
            server.remove()
    print(f'\n{"ALL PASS" if failures == 0 else f"{failures} FAILURE(S)"}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
