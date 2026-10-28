"""#133: the Colosseum, every request end to end.

python3 scripts/test_colosseum_wire.py prepare SAVE     # server stopped, after one boot (migrations)
python3 scripts/test_colosseum_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account as the player and the second as the
opponent (never point it at a save players use): the player gets no tickets
(the day's top-up must give 10), 5 gems, and a class-3 row left on a finished
season; the opponent gets saved class-1 squads and a season score.  Ids go to
SAVE.colo.json.  Needs pycryptodome (.venv/bin/python).

Wire shapes: packet-generator/assets/net/colosseum.kdl; rules: docs/features/COLOSSEUM.md.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker  # noqa: E402

TOKEN = 'colosseum133-wire-test-token'
REQ = {
    'ArenaInfo': ('oim9TU1D', 'oqQxdFfa'),
    'ResetInfo': ('Zw3WIoWu', 'KlwYMGF1'),
    'DeckEdit': ('rdZbaKBG', 'gY3YK5bD'),
    'BattleStart': ('ysP6qYcy', 'iFIHAT5V'),
    'BattleRestart': ('ohH9lfDE', 'j2bFgyri'),
    'BattleEnd': ('fGDPoQI3', '4V6oIoRC'),
    'FriendList': ('AoBvG2YN', 'Wj5fH1HV'),
    'GetRewardInfo': ('jv0dI88o', 'd7aPxZWw'),
    'RewardReceipt': ('PN5CVaWy', 'YBgVBy5A'),
    'ShopUse': ('xe8tiSf4', 'qthMXTQSkz3KfH9R'),
}
OLD_SEASON = '202001'
MY_SP, THEIR_SP, ELGIF_SKILL = '11@1000010054', '11@1100000141:1100000142', 1030000


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me, enemy = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 2').fetchall()

    def units(owner, n):
        rows = [r['user_unit_id'] for r in db.execute(
            'SELECT user_unit_id FROM user_units WHERE user_id=? ORDER BY user_unit_id LIMIT ?', (owner, n))]
        # Too few units: copies of the first one fill the squads.
        template = dict(db.execute('SELECT * FROM user_units WHERE user_id=? LIMIT 1', (owner,)).fetchone())
        template.pop('user_unit_id')
        while len(rows) < n:
            rows.append(db.execute(f"INSERT INTO user_units ({','.join(template)}) VALUES"
                                   f" ({','.join('?' for _ in template)})", tuple(template.values())).lastrowid)
        return rows

    mine, theirs = units(me['id'], 10), units(enemy['id'], 10)
    # #136: SP picks and an Elgif skill on the player's leader, SP on an
    # opponent defender that has its own Extra Skill (Bjorn & Linlin 830538).
    db.execute('UPDATE user_units SET fe_skill_info=?, extra_passive_skill_id2=? WHERE user_unit_id=?',
               (MY_SP, ELGIF_SKILL, mine[0]))
    db.execute("UPDATE user_units SET unit_id='830538', fe_skill_info=? WHERE user_unit_id=?", (THEIR_SP, theirs[5]))
    db.execute('UPDATE user_info SET colosseum_tickets=0, gems=5 WHERE id=?', (me['id'],))
    for row in (me, enemy):
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
                   (hashlib.sha256(TOKEN.encode()).hexdigest(), row['gumi_user_id']))

    def party(ids):
        return json.dumps([{'P_USER_UNIT_ID': str(u), 'P_MEMBER_TYPE': '0' if i == 0 else '1',
                            'P_DISPORDER': str(i)} for i, u in enumerate(ids)])

    for table in ('user_colosseum', 'user_colosseum_class', 'user_colosseum_season',
                  'colosseum_battle', 'user_colosseum_reward'):
        db.execute(f'DELETE FROM {table} WHERE {"attacker_id" if table == "colosseum_battle" else "user_id"} IN (?, ?)',
                   (me['id'], enemy['id']))
    db.execute("INSERT INTO user_colosseum_class (user_id, class_id, season_id, points, season_battle_cnt,"
               " battle_cnt, win_cnt, atk_party, def_party) VALUES (?, 1, strftime('%Y%m', 'now', '-8 hours'),"
               " 500, 4, 4, 3, ?, ?)", (enemy['id'], party(theirs[:5]), party(theirs[5:])))
    db.execute('INSERT INTO user_colosseum_class (user_id, class_id, season_id, points, season_battle_cnt)'
               ' VALUES (?, 3, ?, 300, 3)', (me['id'], OLD_SEASON))
    db.commit()
    db.close()
    ids = {'user': me['id'], 'login': me['gumi_user_id'], 'enemy': enemy['id'], 'mine': mine}
    Path(f'{save}.colo.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.colo.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()
    unit_mst = {r['pn16CNah']: r for r in list(json.loads(
        (Path(__file__).resolve().parents[1] / 'deploy/mst/unit_mst.json').read_text()).values())[0]}

    def call(name, body=None):
        group, key = REQ[name]
        body = dict(body or {})
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(group, key, body=body)

    cls = lambda n: [{'3mMAn6L5': str(n)}]
    tickets = lambda r: (r.get('fEi17cnx') or [{}])[0].get('lKuj3Ier')

    hub = call('ArenaInfo')
    classes = hub.get('HgYvs3am', [])
    me = (hub.get('FfBebG0R') or [{}])[0]
    check('hub: four classes', [r.get('3mMAn6L5') for r in classes] == ['1', '2', '3', '4'], classes[:1])
    check('hub: one user row with the season', len(hub.get('FfBebG0R', [])) == 1 and len(me.get('ZW19nYrU', '')) == 6, me)
    check('hub: the day\'s 10 free tickets', tickets(hub) == '10', tickets(hub))
    check('hub: no master rows (they free what the Top screen holds)',
          not any(k in hub for k in ('wmpLokaR', 'aCmTmaOx', 'kaQtidz4')), sorted(hub))
    check('hub: last season closed with a message', 'ended' in me.get('JQ23rIvk', ''), me.get('JQ23rIvk'))
    check('hub: past ranking listed', any(r.get('ZW19nYrU') == OLD_SEASON and r.get('3mMAn6L5') == '3'
                                         for r in hub.get('dSC5DzuN', [])), hub.get('dSC5DzuN'))
    check('hub: the message is handed out once', 'ended' not in (call('ArenaInfo').get('FfBebG0R') or [{}])[0].get('JQ23rIvk', ''))

    reset = call('ResetInfo')
    check('ChallengeArenaResetInfo: timer plus the hub rows (1801 sends only this)',
          't6bRQfln' in reset and len(reset.get('HgYvs3am', [])) == 4 and len(reset.get('FfBebG0R', [])) == 1,
          sorted(reset)[:12])

    mine = u['mine']
    members = [{'zsiAn9P1': '0', 'U9ABSYEp': '1', 'edy7fq3L': str(x), 'gr48vsdJ': '0' if i == 0 else '1',
                'XuJL4pc5': str(i)} for i, x in enumerate(mine[:5])]
    members += [{'zsiAn9P1': '1', 'U9ABSYEp': '2', 'edy7fq3L': str(x), 'gr48vsdJ': '0' if i == 0 else '1',
                 'XuJL4pc5': str(i)} for i, x in enumerate(mine[5:])]
    members.append({'zsiAn9P1': '0', 'U9ABSYEp': '1', 'edy7fq3L': '999999999', 'gr48vsdJ': '1', 'XuJL4pc5': '5'})
    supports = [{'zsiAn9P1': '0', 'U9ABSYEp': '1', 'peCeLuNq': '102', 'NungTq5g': '700'},
                {'zsiAn9P1': '1', 'U9ABSYEp': '2', 'peCeLuNq': '203', 'NungTq5g': '800'}]
    edit = call('DeckEdit', {'KtSHFZY7': cls(1), 'Pgz43hTE': members, 'vAPeUI3X': supports})
    check('DeckEdit: no class rows in the reply', 'HgYvs3am' not in edit and len(edit.get('FfBebG0R', [])) == 1, sorted(edit))
    c1 = next((r for r in call('ArenaInfo').get('HgYvs3am', []) if r.get('3mMAn6L5') == '1'), {})
    check('DeckEdit: squads saved, the unowned unit dropped',
          len(json.loads(c1.get('Enmv8X74', '[]'))) == 5 and len(json.loads(c1.get('UvOujGQX', '[]'))) == 5, c1)
    check('DeckEdit: formation and support saved',
          (c1.get('XRaf10Pu'), c1.get('dgtpPK70'), c1.get('5MjZwxqH'), c1.get('kPvAtLWr')) == ('102', '700', '203', '800'), c1)

    no_squad = call('BattleStart', {'KtSHFZY7': cls(2), 'fq2C03jh': [{'69vnphig': '1'}]})
    check('BattleStart: a class without squads is refused', 'error' in no_squad, no_squad)

    start = call('BattleStart', {'KtSHFZY7': cls(1), 'fq2C03jh': [{'69vnphig': '2'}]})
    enemy = (start.get('y7A1mZft') or [{}])[0]
    party = (start.get('7oXGyuVH') or [{}])[0]
    check('BattleStart: the other player\'s real squads', enemy.get('h7eY3sAK') == u['enemy'], enemy.get('h7eY3sAK'))
    check('BattleStart: enemy units and parties parse', len(json.loads(enemy.get('U42YcFNu', '[]'))) == 10
          and len(json.loads(enemy.get('UvOujGQX', '[]'))) == 5, enemy.get('U42YcFNu', '')[:80])
    check('BattleStart: own side as unit JSON', len(json.loads(party.get('U42YcFNu', '[]'))) == 10, party)
    lead = next((x for x in json.loads(party.get('U42YcFNu', '[]')) if x['P_USER_UNIT_ID'] == str(mine[0])), {})
    check('#136: own unit carries its SP picks and Elgif skill',
          lead.get('P_FE_INFO') == MY_SP and lead.get('P_EXTRA_PASSIVE_SKILL_ID2') == str(ELGIF_SKILL), lead)
    bjorn = next((x for x in json.loads(enemy.get('U42YcFNu', '[]')) if x['P_UNIT_ID'] == '830538'), {})
    check('#136: opponent unit carries its SP picks and own Extra Skill',
          bjorn.get('P_FE_INFO') == THEIR_SP and bjorn.get('P_EXTRA_PASSIVE_SKILL_ID') not in (None, '', '0'), bjorn)
    check('BattleStart: unit AI rows', len(start.get('89ausgc4', [])) > 0)
    check('BattleStart: two tickets spent', tickets(start) == '8', tickets(start))
    battle = enemy.get('1kisc6IF')

    restart = call('BattleRestart', {'KtSHFZY7': cls(1)})
    check('BattleRestart: the same match', (restart.get('y7A1mZft') or [{}])[0].get('1kisc6IF') == battle, restart.get('error'))

    def fights(r1, r2, num):
        return [{'1kisc6IF': num, '2QOPHXW1': '1', '3pAQY5KW': r1, 'mFID53JZ': '5' if r1 == '1' else '2',
                 'WDYR11jJ': '100,100,0,0,0', 'chy3HnIm': '0,0,0,0,0'},
                {'1kisc6IF': num, '2QOPHXW1': '2', '3pAQY5KW': r2, 'mFID53JZ': '5' if r2 == '1' else '1',
                 'WDYR11jJ': '50,0,0,0,0', 'chy3HnIm': '0,0,0,0,0'}]

    end = call('BattleEnd', {'KtSHFZY7': cls(1), 'BZ95Eg3M': fights('1', '1', battle)})
    rows = {r.get('2QOPHXW1'): r for r in end.get('BZ95Eg3M', [])}
    summary = rows.get('3', {})
    check('BattleEnd: fight rows 1, 2 and the summary 3', sorted(rows) == ['1', '2', '3'], list(rows))
    check('BattleEnd: sweep x2 tickets = 2 x (40 + 40 + 20) = 200 CBP',
          summary.get('3pAQY5KW') == '1' and summary.get('43QyxTS5') == '0' and summary.get('OUY9jpPA') == '200'
          and summary.get('tvWWL9wI') == '40' and summary.get('69vnphig') == '2', summary)
    after = (end.get('FfBebG0R') or [{}])[0]
    c1 = next((r for r in end.get('HgYvs3am', []) if r.get('3mMAn6L5') == '1'), {})
    check('BattleEnd: CBP to spend and season score', after.get('qnccUrMN') == '200' and c1.get('z1rMbo8n') == '200', (after, c1))
    check('BattleEnd: ranked behind the 500-CBP opponent', c1.get('2rqxaZ6K') == '2', c1.get('2rqxaZ6K'))
    again = call('BattleEnd', {'KtSHFZY7': cls(1), 'BZ95Eg3M': fights('1', '1', battle)})
    check('BattleEnd: a resend pays nothing', (again.get('FfBebG0R') or [{}])[0].get('qnccUrMN') == '200', again.get('FfBebG0R'))

    bot = call('BattleStart', {'KtSHFZY7': cls(4), 'fq2C03jh': [{'69vnphig': '1'}]})
    check('BattleStart: Warrior class needs squads too', 'error' in bot)
    call('DeckEdit', {'KtSHFZY7': cls(4), 'Pgz43hTE': members[:10], 'vAPeUI3X': supports})
    bot = call('BattleStart', {'KtSHFZY7': cls(4), 'fq2C03jh': [{'69vnphig': '1'}]})
    benemy = (bot.get('y7A1mZft') or [{}])[0]
    bunits = json.loads(benemy.get('U42YcFNu', '[]'))
    costs = [int(unit_mst[x['P_UNIT_ID']]['gvT2ds0Q']) for x in bunits if x['P_UNIT_ID'] in unit_mst]
    check('BattleStart: nobody else -> a generated squad', benemy.get('h7eY3sAK', '').startswith('COLOBOT'), benemy.get('h7eY3sAK'))
    check('BattleStart: generated units within Warrior\'s Cost 45', len(costs) == 10 and max(costs) <= 45, costs)
    check('#136: generated units carry their own Extra Skills',
          any(x.get('P_EXTRA_PASSIVE_SKILL_ID') not in ('', '0') for x in bunits), [x.get('P_EXTRA_PASSIVE_SKILL_ID') for x in bunits])
    lost = call('BattleEnd', {'KtSHFZY7': cls(4), 'BZ95Eg3M': fights('0', '0', benemy.get('1kisc6IF'))})
    lsum = next((r for r in lost.get('BZ95Eg3M', []) if r.get('2QOPHXW1') == '3'), {})
    check('BattleEnd: two lost fights = 2 x 10 CBP', lsum.get('3pAQY5KW') == '0' and lsum.get('OUY9jpPA') == '20', lsum)

    ranks = call('FriendList', {'KtSHFZY7': [{'3mMAn6L5': '1', 'ZW19nYrU': me.get('ZW19nYrU')}]})
    top = [(r.get('h7eY3sAK'), r.get('2rqxaZ6K'), r.get('U4pMNjy0')) for r in ranks.get('BYaF62TE', [])
           if r.get('NH65Wj0f') == '1']
    mine_row = [r for r in ranks.get('BYaF62TE', []) if r.get('NH65Wj0f') == '0']
    check('FriendList: everyone (type 1) ranks the opponent first, the player second',
          top == [(u['enemy'], '1', '500'), (u['user'], '2', '200')], top)
    check('FriendList: the friends list (type 0) holds the player', [r.get('h7eY3sAK') for r in mine_row] == [u['user']], mine_row)
    total = call('FriendList', {'KtSHFZY7': [{'3mMAn6L5': '1', 'ZW19nYrU': '0'}]})
    check('FriendList: "0" is the all-time ranking', any(r.get('h7eY3sAK') == u['user'] for r in total.get('BYaF62TE', [])),
          total.get('BYaF62TE'))

    shop = call('GetRewardInfo', {'1x6T2Trz': [{'mQhKzRxu': '1', 'VgU78CYj': '1'}]})
    check('GetRewardInfo: stage 1, all exchangeable', len(shop.get('xNCYRb5G', [])) == 6
          and all(r.get('rPk8gtY5') == '1' for r in shop['xNCYRb5G']) and len(shop.get('U6MWwoW4', [])) == 4, shop.get('xNCYRb5G'))
    got = call('RewardReceipt', {'BXeof2p1': [{'wk0nBxCQ': '0'}], 'zcN7zXLn': [{'N1b6SUW4': '101', 'VgU78CYj': '1', 'H6k1LIxC': '1'}]})
    r101 = next((r for r in got.get('xNCYRb5G', []) if r.get('N1b6SUW4') == '101'), {})
    check('RewardReceipt: 100,000 Zel for 200 CBP', r101.get('rPk8gtY5') == '2'
          and (got.get('FfBebG0R') or [{}])[0].get('qnccUrMN') == '20', (r101, got.get('FfBebG0R')))
    check('RewardReceipt: twice is refused', 'error' in call('RewardReceipt', {'zcN7zXLn': [{'N1b6SUW4': '101'}]}))
    check('RewardReceipt: a later stage is refused', 'error' in call('RewardReceipt', {'zcN7zXLn': [{'N1b6SUW4': '201'}]}))
    check('RewardReceipt: too little CBP is refused', 'error' in call('RewardReceipt', {'zcN7zXLn': [{'N1b6SUW4': '106'}]}))
    ids = lambda r: [x.get('N1b6SUW4') for x in r.get('xNCYRb5G', [])]
    moved = call('GetRewardInfo', {'1x6T2Trz': [{'mQhKzRxu': '2', 'VgU78CYj': '1'}]})
    check('Go to Next Stage: the player moves on to stage 2', ids(moved)[:1] == ['201']
          and (moved.get('FfBebG0R') or [{}])[0].get('mQhKzRxu') == '2', (ids(moved), moved.get('FfBebG0R')))
    stage_list = lambda r: [x.get('mQhKzRxu') for x in r.get('U6MWwoW4', [])]
    # #140: the client's next stage is the first row that is not its stage.
    check('#140: the stage list starts at the player\'s stage', stage_list(moved) == ['2', '3', '4'], stage_list(moved))
    check('Go to Next Stage: stage 1 leftovers stay behind',
          'error' in call('RewardReceipt', {'zcN7zXLn': [{'N1b6SUW4': '102', 'VgU78CYj': '1'}]}))
    check('Go to Next Stage: a stage left behind does not reopen',
          ids(call('GetRewardInfo', {'1x6T2Trz': [{'mQhKzRxu': '1', 'VgU78CYj': '1'}]}))[:1] == ['201'])
    check('Go to Next Stage: never two stages at once',
          ids(call('GetRewardInfo', {'1x6T2Trz': [{'mQhKzRxu': '4', 'VgU78CYj': '1'}]}))[:1] == ['201'])
    third = call('GetRewardInfo', {'1x6T2Trz': [{'mQhKzRxu': '3'}]})
    check('#140: from stage 2 the player moves on to stage 3', ids(third)[:1] == ['301']
          and stage_list(third) == ['3', '4'], (ids(third), stage_list(third)))

    shopuse = call('ShopUse', {'32ibWjFG': [{'60IsqxDt': '8', '03UGMHxF': '1', '5gXxT7LZ': '5'}]})
    check('ShopUse 8: 1 gem for 5 tickets (8 - 1 + 5 = 12)', tickets(shopuse) == '12', tickets(shopuse))

    db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
    defended = db.execute('SELECT battle_cnt_accept, win_cnt_accept FROM user_colosseum_class WHERE user_id=? AND class_id=1',
                          (u['enemy'],)).fetchone()
    gifts = db.execute("SELECT COUNT(*) FROM user_presents WHERE user_id=? AND description LIKE 'Colosseum%'",
                       (u['user'],)).fetchone()[0]
    db.close()
    check('defender: challenged once, did not hold', defended == (1, 0), defended)
    check('gift box: season reward + exchange', gifts == 2, gifts)
    return check.summary('Colosseum')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['prepare', 'run'])
    ap.add_argument('save', nargs='?')
    ap.add_argument('--save', dest='save_opt')
    ap.add_argument('--url', default='http://127.0.0.1:9981')
    args = ap.parse_args()
    save = Path(args.save or args.save_opt)
    if args.mode == 'prepare':
        prepare(save)
        return 0
    return run(save, args.url)


if __name__ == '__main__':
    sys.exit(main())
