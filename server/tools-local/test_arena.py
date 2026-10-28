"""Wire test for Classic Arena (gme/handlers/Arena.cpp).

Three fake devices create accounts (CreateUser gives each a starter deck), then
player A runs the whole loop against the others: standing, matching, battle
start, settle, idempotent resend, orb exhaustion, rank-up rewards, retire,
rankings, and the arena deck surviving DeckEdit.

Two steps need state the wire cannot cheaply produce (a refill without gems,
and enough points for a rank-up), so they write the SQLite file directly when
--db is given (mount the worktree's deploy/ dir).  Run against a test server.

Usage: python -I test_arena.py --url http://host.docker.internal:9962 [--db /deploy/gme.sqlite]
"""
import argparse
import json
import secrets
import sqlite3
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, '/tools')
sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke_test import fetch, gme_call  # noqa: E402

CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
DECK_EDIT = ('m2Ve9PkJ', 'd7UuQsq8')
ARENA_INFO = ('oim9TU1D', 'oqQxdFfa')
ARENA_START = ('zu6ifpy0', 'bGjK4UH1')
MATCHING = ('82ztj6Iy', 'Q53tUIBE')
BATTLE_START = ('7f1rg92L', 'zaMR9o0d')
RESTART = ('NPyXDgV9', 'sWofKGMl')
BATTLE_END = ('oQNtKU51', 'jkgWq1d0')
RETIRE = ('eyg8sA32', '93i8EbyM')
FRIEND_LIST = ('xkd9H50w', 'L3TDhrn2')

failures = 0


def check(label: str, ok: bool, detail: str = '') -> bool:
    global failures
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail else ''}")
    return ok


class Player:
    def __init__(self, base: str, name: str):
        self.base = base
        query = urllib.parse.urlencode({
            'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
            'dp': 'Android', 'dn': 'arena-test', 'v': '16'})
        status, body = fetch(f'{base}/accounts/guest/login/?{query}')
        assert status == 200, status
        login = json.loads(body)
        self.login = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'B5JQyV8j': name}
        reply = self.call(CREATE_USER, {'9u45RGcV': [{'Kn51uR4Y': str(1 + secrets.randbelow(4))}]})
        self.user_id = None
        self.create_reply = reply

    def identify(self) -> dict:
        """UserInfo, then send the user id in the login block like the client
        does once it has one (getUserIdentity refuses requests without it)."""
        reply = self.call(USER_INFO)
        self.user_id = reply['8jBJ7uKR'][0]['h7eY3sAK']
        self.login['h7eY3sAK'] = self.user_id
        return reply

    def call(self, action, body=None) -> dict:
        payload = {'IKqx1Cn9': [self.login]}
        payload.update(body or {})
        return gme_call(self.base, *action, payload)


def refused(reply: dict) -> bool:
    return '_unencrypted' in reply or 'error' in json.dumps(reply).lower()[:200]


def standing(p: Player) -> dict:
    return p.call(ARENA_INFO)['8jBJ7uKR'][0]


def fight(p: Player, win: bool, extra: dict | None = None):
    match = p.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0', 'ciDqCjAF': '0', '59b1eE6N': ''}]})
    enemy = match['fQ3XImM2'][0]
    start = p.call(BATTLE_START, {
        '9NX83YVe': [{'h7eY3sAK': enemy['h7eY3sAK'], 'Z7PXoxc2': '0'}],
        'fq2C03jh': [{'Z0Y4RoD7': '0', 'ciDqCjAF': '0', '59b1eE6N': ''}],
        'rj5fgJh0': [{'t5R47iwj': '0'}]})
    if '9NX83YVe' not in start:
        return enemy, start, None
    battle_id = start['9NX83YVe'][0]['xL0p2MkY']
    body = {'xL0p2MkY': battle_id, '3pAQY5KW': '1' if win else '0', 'mFID53JZ': '5',
            'hoG2ieT5': '3', '6PLsn8xo': '2', '5NRJQ1LU': '12345', 'XP06YWdT': '17'}
    body.update(extra or {})
    end = p.call(BATTLE_END, {'60subGk3': [{'EfbZ7mh9': '1'}], 'yTEvm85f': [body]})
    return enemy, start, end


def db_exec(path: str | None, sql: str, *args) -> bool:
    if not path:
        return False
    con = sqlite3.connect(path, timeout=10)
    con.execute(sql, args)
    con.commit()
    con.close()
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9962')
    ap.add_argument('--db')
    args = ap.parse_args()

    a, b, c = (Player(args.url, n) for n in ('ArenaA', 'ArenaB', 'ArenaC'))
    ui = a.identify()
    b.identify()
    c.identify()
    check('three accounts', len({a.user_id, b.user_id, c.user_id}) == 3, (a.user_id, b.user_id, c.user_id))

    arena = ui['8jBJ7uKR']
    check('UserInfo: one arena row, rank "1"', len(arena) == 1 and arena[0]['JmFn3g9t'] == '1', arena)
    check('UserInfo: one archive row', len(ui['PQ56vbkI']) == 1)
    team = ui['fEi17cnx'][0]
    check('UserInfo: 3 arena orbs', str(team['YS2JG9no']) == '3', team['YS2JG9no'])
    gates = [g.get('e63D1BV0') for g in ui.get('2375D38i', [])]
    check('Standard Arena gate (5) unlocked, 15 still locked', '5' not in gates and '15' in gates, gates)
    check('ArenaInfo standing', standing(a)['U4pMNjy0'] == '0')
    check('ArenaStart standing', a.call(ARENA_START)['8jBJ7uKR'][0]['JmFn3g9t'] == '1')

    # Arena deck persists through DeckEdit and comes back on UserInfo.
    a.call(DECK_EDIT, {'Ti62XfZK': [{'Z0Y4RoD7': '0', 'gKNfIZiA': '2'}]})
    check('DeckEdit saves arena deck', a.call(USER_INFO)['fEi17cnx'][0]['gKNfIZiA'] in (2, '2'))
    a.call(DECK_EDIT, {'Ti62XfZK': [{'Z0Y4RoD7': '0', 'gKNfIZiA': '0'}]})

    # Wrong opponent id is refused without spending an orb.
    a.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]})
    bad = a.call(BATTLE_START, {'9NX83YVe': [{'h7eY3sAK': 'NOTOFFERED', 'Z7PXoxc2': '0'}]})
    check('BattleStart refuses an id that was not offered', '9NX83YVe' not in bad, str(bad)[:160])

    enemy, start, end = fight(a, win=True)
    # Any player with a deck may be offered (earlier runs leave accounts behind).
    check('Matching offers a real player', not enemy['h7eY3sAK'].startswith('ARENABOT'), enemy)
    check('Matching leader unit present', enemy['pn16CNah'] not in ('', '0'), enemy['pn16CNah'])
    row = start['9NX83YVe'][0]
    team_json, party_json, units_json = (json.loads(row[k]) for k in ('INT1K9Pf', 'nZc1Ip3N', 'U42YcFNu'))
    check('BattleStart JSON texts parse', isinstance(party_json, list) and isinstance(units_json, list)
          and team_json['P_USER_ID'] == enemy['h7eY3sAK'], str(team_json))
    check('party has exactly one leader', sum(p['P_MEMBER_TYPE'] == '0' for p in party_json) == 1, party_json)
    check('units carry stats', all(int(u['P_HP']) > 0 and u['P_UNIT_ID'] != '0' for u in units_json), units_json[:1])
    check('BattleStart spends an orb', str(start['fEi17cnx'][0]['YS2JG9no']) == '2', start['fEi17cnx'][0]['YS2JG9no'])
    res = end['yTEvm85f'][0]
    check('BattleEnd win: +points', int(res['TJ9cQ8AY']) > int(res['jPoM1UV8']), res)
    check('BattleEnd karma', int(res['HTVh8a65']) > 0, res['HTVh8a65'])
    check('BattleEnd standing updated', end['8jBJ7uKR'][0]['U4pMNjy0'] == res['TJ9cQ8AY'])
    check('BattleEnd archive streak 1', end['PQ56vbkI'][0]['7sXWfE8j'] == '1', end['PQ56vbkI'][0])
    check('no rank-up rows yet', end['okd0y3Ir'] == [], end['okd0y3Ir'])
    again = a.call(BATTLE_END, {'yTEvm85f': [{'xL0p2MkY': row['xL0p2MkY'], '3pAQY5KW': '1'}]})
    # Both carry the achievement badges (h23iRjGN): the resend's is computed
    # anew, and nothing changed in between.
    check('resent BattleEnd is identical (no double pay)', again == end and 'h23iRjGN' in end,
          sorted(set(end) ^ set(again)))
    defender = b if enemy['h7eY3sAK'] == b.user_id else c if enemy['h7eY3sAK'] == c.user_id else None
    if defender:
        check('defender battle_cnt_accept 1', standing(defender)['20iEWRCV'] == '1', standing(defender))
    elif args.db:
        con = sqlite3.connect(args.db, timeout=10)
        accepted = con.execute('SELECT battle_cnt_accept FROM user_arena WHERE user_id = ?',
                               (enemy['h7eY3sAK'],)).fetchone()
        con.close()
        check('defender battle_cnt_accept counted', accepted is not None and accepted[0] >= 1, accepted)

    enemy_id = enemy['h7eY3sAK']
    enemy, start, end = fight(a, win=False)
    res = end['yTEvm85f'][0]
    check('BattleEnd loss: points unchanged', res['TJ9cQ8AY'] == res['jPoM1UV8'], res)
    check('loss resets attack streak', end['PQ56vbkI'][0]['7sXWfE8j'] == '0')

    # Open a battle and abandon it: Retire settles it as a loss and still succeeds.
    match = a.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]})
    start = a.call(BATTLE_START, {'9NX83YVe': [{'h7eY3sAK': match['fQ3XImM2'][0]['h7eY3sAK']}]})
    check('third battle spends the last orb', str(start['fEi17cnx'][0]['YS2JG9no']) == '0')
    restart = a.call(RESTART, {'60subGk3': [{'EfbZ7mh9': '1'}]})
    check('Restart resends enemy + own party', restart.get('9NX83YVe') == start['9NX83YVe']
          and json.loads(restart['5K5wsBoL'][0]['U42YcFNu']), str(restart)[:120])
    before = int(standing(a)['69vnphig'])
    retire = a.call(RETIRE, {'yTEvm85f': [{'xL0p2MkY': start['9NX83YVe'][0]['xL0p2MkY']}]})
    check('Retire succeeds and counts a loss', int(retire['8jBJ7uKR'][0]['69vnphig']) == before + 1, retire)
    junk = a.call(RETIRE, {'yTEvm85f': [{'xL0p2MkY': '99999999'}]})
    check('Retire with an unknown id still succeeds', '8jBJ7uKR' in junk, junk)

    a.call(MATCHING, {'fq2C03jh': [{'Z0Y4RoD7': '0'}]})
    enemy, start, end = fight(a, win=True)
    check('no orbs: BattleStart refused', end is None and '9NX83YVe' not in start, str(start)[:160])

    if db_exec(args.db, 'UPDATE user_info SET fight_point = 3, gems = 0 WHERE id = ?', a.user_id):
        db_exec(args.db, 'UPDATE user_arena SET points = 480 WHERE user_id = ?', a.user_id)
        enemy, start, end = fight(a, win=True)
        res = end['yTEvm85f'][0]
        check('rank-up 1 -> 2', (res['s07yWuZR'], res['C1w4H62i']) == ('1', '2'), res)
        check('rank 2 reward row: 1 gem, first', end['okd0y3Ir'] == [{'1ef7JNSC': '1', '4T0Q2Bh5': '', 'ZBwx5ct3': '1'}],
              end['okd0y3Ir'])
        check('gem paid', str(end['fEi17cnx'][0]['03UGMHxF']) == '1', end['fEi17cnx'][0].get('03UGMHxF'))
        db_exec(args.db, 'UPDATE user_arena SET points = 1990 WHERE user_id = ?', a.user_id)
        enemy, start, end = fight(a, win=True)
        rewards = end['okd0y3Ir']
        check('rank-up 2 -> 4 gives two reward rows', [r['4T0Q2Bh5'] for r in rewards] == ['35200:1', '35201:1'], rewards)
        check('max rank 4', end['8jBJ7uKR'][0]['8uJ1XGwx'] == '4', end['8jBJ7uKR'][0])
    else:
        print('SKIP  rank-up checks (no --db)')

    ranking = a.call(FRIEND_LIST)['BYaF62TE']
    ids = {r['h7eY3sAK'] for r in ranking}
    check('rankings list the defender, not self', enemy_id in ids and a.user_id not in ids,
          [(r['B5JQyV8j'], r['U4pMNjy0'], r['NH65Wj0f']) for r in ranking][:5])

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
