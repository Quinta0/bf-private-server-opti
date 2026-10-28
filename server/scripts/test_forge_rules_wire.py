#!/usr/bin/env python3
"""Wire test: Balance Forge rules.json (squad rules, unlock rules, schedule windows).

    PYTHONPATH=<repo>/tools <repo>/.venv/bin/python \
        scripts/test_forge_rules_wire.py [--src wt/forge] [--port 9986] [--assets DIR] [--keep]

Boots one throwaway server (container bf-forge-rules, save in volume bf-forge-rules-data,
a scratch balance root) and never restarts it: every step rewrites server/rules.json and
waits until forgeStatus.php reports the new bytes (the server re-reads the file at most
every 2 s).  A throwaway guest with every story quest below the "need" quest cleared:

  squads    one rule per quest: elements, one element, max cost, max units, banned
            (squad and helper), required (met by another form of the line), helper
            none / own_units / friends_only, helper elements.  Each refused start
            shows the rule's message and changes neither energy nor
            open_mission_serial; the fixed squad or helper starts.
  trials    a Trial Zone quest is checked against user_trial_decks, not the normal
            squad Z0Y4RoD7 names; an own helper is refused when it is in a trial
            squad and allowed when it is only in the normal squad.
  unlocks   a dungeon needing an uncleared quest: start refused, the tile gone from
            UserInfo's permit lists; the need cleared -> tile back, start allowed.
  schedules a quest that opens in 1 h (refused "opens again", tile hidden), one open
            now, a weekly span covering now and one that does not; a banner closed
            now: GachaList and UserInfo rails lack it, GachaAction is refused.
  status    sha256 of the bytes written, counts, a rule with an unknown quest
            dropped alone and listed; a broken file keeps the previous rules live
            and sets rules.error.

Removes its container and volume (unless --keep).
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

from forge_wire_common import (GACHA_ACTION, GACHA_LIST, MISSION_START, USER_INFO, Server, gme_call,
                               permitted, sha, start_body, table, wire_error, write_atomic)
from test_fair_summon import Database, Player

CONTAINER, VOLUME = 'bf-forge-rules', 'bf-forge-rules-data'
FIRE_A, FIRE_B, FIRE_A_EVO, WATER = 10011, 10021, 10012, 20011   # Fencer Vargas, Beast Zegar, Burning Vargas, Selena
GATE = 15850                                                     # its own rail tile, not in the weekly rotation
BAD_MISSION = 99999999
DAYS = ('mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun')


def week_time(t: datetime) -> str:
    return f'{DAYS[t.weekday()]} {t:%H:%M}'


def utc(t: datetime) -> str:
    return f'{t:%Y-%m-%d %H:%M:%S}'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9986)
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

    work = Path(tempfile.mkdtemp(prefix='forge-rules-'))
    server = Server(src, work, args.port, args.assets, CONTAINER, VOLUME)
    rules_path = work / 'balance' / 'server' / 'rules.json'
    try:
        _, missions = table(src, 'mission_mst.json')
        _, dungeons = table(src, 'dungeon_mst.json')
        _, units = table(src, 'unit_mst.json')
        unit = {int(r['pn16CNah']): r for r in units}
        dtype = {r['MHx05sXt']: r['3hPeI1RV'] for r in dungeons}
        name = {int(r['j28VNcUW']): r['0iAIR2LP'] for r in missions}
        dungeon_of = {int(r['j28VNcUW']): int(r['MHx05sXt']) for r in missions}
        archived = {int(r['id']) for r in json.loads((src / 'deploy' / 'archive' / 'mission.json').read_text(encoding='utf-8'))}
        # Story quests with an authored battle (the map lists only those).
        story = sorted(int(r['j28VNcUW']) for r in missions
                       if 11 <= int(r['j28VNcUW']) < 900 and dtype.get(r['MHx05sXt']) == '0'
                       and int(r['j28VNcUW']) in archived)
        need = story[-1]                                  # left uncleared: the unlock rule's need
        trial = min(int(r['j28VNcUW']) for r in missions if dtype.get(r['MHx05sXt']) == '2')
        squad_q = story[:10]
        used_dungeons = {dungeon_of[m] for m in squad_q}
        ul_mission = next(m for m in story[10:] if dungeon_of[m] not in used_dungeons)
        ul_dungeon = dungeon_of[ul_mission]
        rest = [m for m in story[10:-1] if dungeon_of[m] != ul_dungeon]
        sc_later, sc_now, sc_week_in, sc_week_out = rest[:4]

        server.start()
        check('the server boots with no rules.json', True)
        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')
        player = Player(server.url, db)
        other = Player(server.url, db)
        player.set(tutorial_status=12, tutorial_end_flag=1, energy=9999, energy_full_ts=0, gems=5000)
        uid = player.user_id
        db.rows('DELETE FROM user_campaign_missions WHERE user_id = ?', uid)
        for m in sorted(int(r['j28VNcUW']) for r in missions if int(r['j28VNcUW']) < need):
            db.rows('INSERT OR IGNORE INTO user_campaign_missions (user_id, mission_id, state, attain_percent,'
                    ' clear_count, last_cleared_at) VALUES (?, ?, 2, 100, 1, 0)', uid, str(m))

        cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)') if r[1] != 'user_unit_id']
        template = db.one('SELECT user_unit_id FROM user_units WHERE user_id = ? ORDER BY user_unit_id LIMIT 1', uid)[0]

        def give(unit_id: int) -> int:
            db.rows(f'INSERT INTO user_units ({", ".join(cols)}) SELECT {", ".join(cols)} FROM user_units'
                    ' WHERE user_unit_id = ?', template)
            new = db.one('SELECT MAX(user_unit_id) FROM user_units WHERE user_id = ?', uid)[0]
            row = unit[unit_id]
            db.rows('UPDATE user_units SET unit_id = ?, bb_id = ?, sbb_id = ? WHERE user_unit_id = ?',
                    unit_id, row['nj9Lw7mV'], row['iEFZ6H19'], new)
            return new

        fa, fb, fe, wa = give(FIRE_A), give(FIRE_B), give(FIRE_A_EVO), give(WATER)
        hf, hw = give(FIRE_B), give(WATER)               # helper cards: MYUNIT<hf>, MYUNIT<hw>

        def squad(members: list, deck: int = 0) -> None:
            db.rows('DELETE FROM user_decks WHERE user_id = ? AND deck_type = 1 AND deck_num = ?', uid, deck)
            for order, u in enumerate(members):
                db.rows('INSERT INTO user_decks (user_id, user_unit_id, deck_type, deck_num, member_type, disp_order)'
                        ' VALUES (?, ?, 1, ?, ?, ?)', uid, u, deck, 0 if order == 0 else 1, order)

        def trial_squads(decks: dict) -> None:
            db.rows('DELETE FROM user_trial_decks WHERE user_id = ?', uid)
            for deck, members in decks.items():
                for order, u in enumerate(members):
                    db.rows('INSERT INTO user_trial_decks (user_id, deck_num, disp_order, member_type, user_unit_id)'
                            ' VALUES (?, ?, ?, ?, ?)', uid, deck, order, 0 if order == 0 else 1, u)

        def state() -> tuple:
            return tuple(db.one('SELECT energy, open_mission_serial FROM user_info WHERE id = ?', uid))

        def start(mission: int, helper: str = '0', deck: int = 0) -> dict:
            return gme_call(server.url, *MISSION_START, start_body(player.login, mission, deck, helper))

        def refused(label: str, mission: int, text: str, helper: str = '0', deck: int = 0) -> None:
            before = state()
            reply = start(mission, helper, deck)
            message = wire_error(reply) or ''
            check(f'{label}: refused with "{text}"', text in message, message or list(reply)[:4])
            check(f'{label}: ...nothing spent, no battle opened', state() == before, (before, state()))

        def allowed(label: str, mission: int, helper: str = '0', deck: int = 0) -> None:
            reply = start(mission, helper, deck)
            check(f'{label}: starts', wire_error(reply) is None and 'Kz7qfSs5' in reply, wire_error(reply) or list(reply)[:4])

        def publish(payload, label: str) -> bytes:
            data = payload if isinstance(payload, bytes) else json.dumps(payload, indent=1).encode()
            before = server.status().get('rules', {})
            write_atomic(rules_path, data)
            deadline = time.time() + 15
            rules = before
            while time.time() < deadline:
                time.sleep(0.5)
                rules = server.status().get('rules', {})
                if rules.get('sha256') == sha(data) or (rules.get('error') and rules.get('error') != before.get('error')):
                    break
            check(f'{label}: forgeStatus picks the file up without a restart',
                  rules.get('sha256') == sha(data) or rules.get('error'), rules)
            return data

        def user_info() -> dict:
            return gme_call(server.url, *USER_INFO, {'6FrKacq7': [{'Kn51uR4Y': 'forge'}], 'IKqx1Cn9': player.login})

        # ---- squad rules ---------------------------------------------------------------
        q = dict(zip(('el', 'one', 'cost', 'units', 'ban', 'req', 'none', 'own', 'friend', 'hel'), squad_q))
        cost_a, cost_b = int(unit[FIRE_A]['gvT2ds0Q']), int(unit[FIRE_B]['gvT2ds0Q'])
        squads = [
            {'id': 'sq-el', 'missions': [q['el']], 'elements': [1]},
            {'id': 'sq-one', 'missions': [q['one']], 'one_element': True},
            {'id': 'sq-cost', 'missions': [q['cost']], 'max_cost': cost_a},
            {'id': 'sq-units', 'missions': [q['units']], 'max_units': 1},
            {'id': 'sq-ban', 'missions': [q['ban']], 'banned_units': [FIRE_B]},
            {'id': 'sq-req', 'missions': [q['req']], 'required_units': [FIRE_A]},
            {'id': 'sq-none', 'missions': [q['none']], 'helper': 'none'},
            {'id': 'sq-own', 'missions': [q['own']], 'helper': 'own_units'},
            {'id': 'sq-friend', 'missions': [q['friend']], 'helper': 'friends_only'},
            {'id': 'sq-hel', 'missions': [q['hel']], 'helper_elements': [1]},
            {'id': 'sq-trial', 'missions': [trial], 'elements': [1]},
            {'id': 'sq-unknown', 'missions': [BAD_MISSION], 'max_units': 2},
        ]
        written = publish({'format': 1, 'squads': squads}, 'squad rules')
        rules = server.status().get('rules', {})
        check('status: sha256 of the bytes, 11 squad rules, the unknown quest\'s rule dropped and named',
              rules.get('sha256') == sha(written) and rules.get('counts') == {'squads': 11, 'unlocks': 0, 'schedules': 0}
              and rules.get('errors') == [{'rule': 'sq-unknown', 'message': f'Quest {BAD_MISSION} is not in the game.'}]
              and rules.get('error') is None and isinstance(rules.get('loaded_at'), int) and rules.get('present') is True,
              rules)

        squad([wa])
        refused('elements (a Water unit, Fire only)', q['el'], 'Only Fire units can fight in this quest.')
        squad([fa, fb])
        allowed('elements (two Fire units)', q['el'])
        squad([fa, wa])
        refused('one element (Fire + Water)', q['one'], 'All units in your squad must share one element')
        squad([fa, fb])
        allowed('one element (Fire + Fire)', q['one'])
        refused('max cost', q['cost'], f'Your squad costs {cost_a + cost_b}; this quest allows at most {cost_a}.')
        squad([fa])
        allowed('max cost (one unit)', q['cost'])
        squad([fa, fb])
        refused('max units', q['units'], 'This quest allows at most 1 unit in your squad, not counting the helper.')
        squad([fa])
        allowed('max units (one unit)', q['units'])
        squad([fa, fb])
        refused('banned unit in the squad', q['ban'], f"{unit[FIRE_B]['utP1c0CD']} can't be used in this quest.")
        squad([fa])
        refused('banned unit as the helper', q['ban'], f"{unit[FIRE_B]['utP1c0CD']} can't be used in this quest.",
                helper=f'MYUNIT{hf}')
        allowed('banned unit (squad and helper without it)', q['ban'], helper=f'MYUNIT{hw}')
        squad([wa])
        refused('required unit missing', q['req'], f"This quest needs {unit[FIRE_A]['utP1c0CD']} (any form) in your squad.")
        squad([fe])
        allowed('required unit met by another form of its line', q['req'])
        squad([fa])
        refused('helper none', q['none'], 'This quest must be done without a helper.', helper=f'MYUNIT{hw}')
        allowed('helper none (solo)', q['none'])
        refused('helper own_units (a stranger)', q['own'], 'the helper must be one of your own units',
                helper=str(other.user_id))
        allowed('helper own_units (an own unit)', q['own'], helper=f'MYUNIT{hw}')
        refused('helper friends_only (an own unit)', q['friend'], 'the helper must be one of your friends',
                helper=f'MYUNIT{hw}')
        db.rows('INSERT OR REPLACE INTO user_relations (user_id, other_id, state, favorite, created_at)'
                ' VALUES (?, ?, 1, 0, 0)', uid, str(other.user_id))
        allowed('helper friends_only (a friend)', q['friend'], helper=str(other.user_id))
        refused('helper elements (a Water helper)', q['hel'], 'the helper must be a Fire unit', helper=f'MYUNIT{hw}')
        allowed('helper elements (a Fire helper)', q['hel'], helper=f'MYUNIT{hf}')

        # ---- trials --------------------------------------------------------------------
        squad([fa, fb])                                  # the normal squad Z0Y4RoD7 names would pass
        trial_squads({0: [fa], 1: [wa]})
        refused('trial: a Water unit in trial squad 2 although squad 0 is all Fire', trial,
                'Every trial squad must follow this.')
        trial_squads({0: [fa], 1: [fe]})
        allowed('trial: every trial squad all Fire', trial)
        refused('trial: an own helper that is in a trial squad', trial, 'already in the squad', helper=f'MYUNIT{fa}')
        allowed('trial: an own helper only in the normal squad (not fighting)', trial, helper=f'MYUNIT{fb}')

        # ---- unlocks and schedules -----------------------------------------------------
        now = datetime.now(timezone.utc).replace(microsecond=0)
        hour = timedelta(hours=1)
        squad([fa])
        info = user_info()
        check('before the unlock rule: its dungeon is on the map', permitted(info, 'MHx05sXt', ul_dungeon),
              ul_dungeon)
        v2 = {'format': 1, 'squads': squads[:1],
              'unlocks': [{'id': 'ul-dungeon', 'dungeons': [ul_dungeon], 'needs': [need]}],
              'schedules': [
                  {'id': 'sc-later', 'missions': [sc_later], 'open': utc(now + hour)},
                  {'id': 'sc-now', 'missions': [sc_now], 'open': utc(now - hour), 'close': utc(now + hour)},
                  {'id': 'sc-week-in', 'missions': [sc_week_in],
                   'weekly': [{'from': week_time(now - hour), 'to': week_time(now + hour)}]},
                  {'id': 'sc-week-out', 'missions': [sc_week_out],
                   'weekly': [{'from': week_time(now + hour), 'to': week_time(now + 2 * hour)}]},
                  {'id': 'sc-gate', 'gates': [GATE], 'open': utc(now + hour)},
                  {'id': 'sc-bad-span', 'missions': [sc_now], 'weekly': [{'from': 'sat 08:00', 'to': 'sat 08:00'}]},
              ]}
        written = publish(v2, 'unlock and schedule rules')
        rules = server.status().get('rules', {})
        check('status: 1 squad, 1 unlock, 5 schedules; the empty weekly span dropped with a plain reason',
              rules.get('sha256') == sha(written) and rules.get('counts') == {'squads': 1, 'unlocks': 1, 'schedules': 5}
              and rules.get('errors') == [{'rule': 'sc-bad-span', 'message': 'Weekly span from sat 08:00 to sat 08:00 is empty.'}],
              rules)
        refused('unlock: the locked dungeon\'s quest', ul_mission, f'This quest is locked. Clear "{name[need]}" first.')
        info = user_info()
        check('unlock: the locked dungeon and its quest are gone from UserInfo\'s permit lists',
              not permitted(info, 'MHx05sXt', ul_dungeon) and not permitted(info, 'j28VNcUW', ul_mission), ul_dungeon)
        refused('schedule: a quest that opens in 1 h', sc_later, 'This quest is closed right now. It opens again on')
        check('schedule: the closed quest is gone from the permit lists', not permitted(info, 'j28VNcUW', sc_later), sc_later)
        check('schedule: the open quests are still listed',
              all(permitted(info, 'j28VNcUW', m) for m in (sc_now, sc_week_in)), (sc_now, sc_week_in))
        allowed('schedule: a quest open now (open..close)', sc_now)
        allowed('schedule: a weekly span covering now', sc_week_in)
        refused('schedule: a weekly span that starts in 1 h', sc_week_out, 'It opens again on')

        db.rows('INSERT OR IGNORE INTO user_campaign_missions (user_id, mission_id, state, attain_percent,'
                ' clear_count, last_cleared_at) VALUES (?, ?, 2, 100, 1, 0)', uid, str(need))
        info = user_info()
        check('unlock: the need cleared -> the dungeon is back on the map', permitted(info, 'MHx05sXt', ul_dungeon), ul_dungeon)
        allowed('unlock: the need cleared -> the quest starts', ul_mission)

        def rail(reply: dict) -> set:
            return {int(x) for c in reply.get('IBs49NiH', []) for x in str(c.get('3rCmq58M', '')).split(',') if x}

        listing = gme_call(server.url, *GACHA_LIST, {'6FrKacq7': [{'Kn51uR4Y': 'forge'}], 'IKqx1Cn9': player.login})
        check('schedule: GachaList\'s rail lacks the closed banner (its info row stays)',
              GATE not in rail(listing) and any(g.get('7Ffmi96v') == str(GATE) for g in listing.get('1IR86sAv', [])),
              sorted(rail(listing))[:20])
        check('schedule: UserInfo\'s rail lacks it too', GATE not in rail(info) and len(rail(info)) > 0, sorted(rail(info))[:20])
        gems = player.balance()['gems']
        pull = gme_call(server.url, *GACHA_ACTION, {'6FrKacq7': [{'Kn51uR4Y': 'forge'}], 'IKqx1Cn9': player.login,
                                                    '1IR86sAv': [{'7Ffmi96v': str(GATE), 'a329kbl8': '1', '324b023k': '0'}]})
        check('schedule: GachaAction on the closed banner is refused, nothing spent',
              'This banner is closed right now. It opens again on' in (wire_error(pull) or '')
              and player.balance()['gems'] == gems, wire_error(pull))

        # ---- a broken file keeps the last good rules -----------------------------------
        publish(b'{"format": 1, "squads": [', 'a broken rules.json')
        rules = server.status().get('rules', {})
        check('broken file: rules.error says why, sha256 and counts stay the live ones',
              rules.get('error') and 'does not read' in rules['error'] and rules.get('sha256') == sha(written)
              and rules.get('counts') == {'squads': 1, 'unlocks': 1, 'schedules': 5}, rules)
        refused('broken file: the previous rules are still enforced', sc_later, 'It opens again on')
        typo = json.loads(json.dumps(v2))
        typo['schedules'][0]['opne'] = typo['schedules'][0].pop('open')
        publish(json.dumps(typo).encode(), 'a rules.json with a misspelt key')
        rules = server.status().get('rules', {})
        check('misspelt key: the whole file refused (a typo never drops a restriction silently)',
              'opne' in (rules.get('error') or '') and rules.get('sha256') == sha(written), rules)
    except Exception as e:  # noqa: BLE001 - report it, then clean up
        check(f'{type(e).__name__}: {e}', False)
    finally:
        if not args.keep:
            server.remove()
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f'kept: container {CONTAINER}, volume {VOLUME}, balance root {work / "balance"}')
    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
