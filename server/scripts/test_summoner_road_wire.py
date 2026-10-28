"""#135: the Summoner as a unit — fusion, the equipped weapon, the gender change.

python3 scripts/test_summoner_road_wire.py prepare SAVE     # while the server is stopped
python3 scripts/test_summoner_road_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account (never a save players use), gives it
fusion material, Zel and 5 gems, a Fire Summoner at 0 Summoning Arts EXP and no
weapon rows; ids go to SAVE.su135.json.  Needs pycryptodome
(.venv/bin/python).

Expected numbers come from deploy/mst/unit_ep3_mst.json (process 1 = SP,
2 = Summoning Arts EXP): Fire Ghost 10202 15 EXP / 2,000 Zel, Fire God 10204
750 / 100,000, Lawson Frog 10522 2 SP / 100,000.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker  # noqa: E402

TOKEN = 'su135-wire-test-token'
MST = Path(__file__).resolve().parent.parent / 'deploy' / 'mst'
GHOST, GOD, LAWSON, PLAIN = 10202, 10204, 10522, 20011


def rows(name):
    data = json.loads((MST / name).read_text())
    return next(iter(data.values())) if isinstance(data, dict) else data


def level_for(exp, curve):
    level = 1
    for lv, need in sorted(curve):
        if lv <= level:
            continue
        if exp < need:
            break
        level = lv
    return level


FIRE_ARTS = [(int(r['D9wXQI2V']), int(r['d96tuT2E']))
             for r in rows('summoner_element_level_mst.json') if r['iNy0ZU5M'] == '1']


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
    template = dict(db.execute('SELECT * FROM user_units WHERE user_id=? LIMIT 1', (me['id'],)).fetchone())
    template.pop('user_unit_id')

    def unit(species):
        row = dict(template, unit_id=str(species), favorite_flg=0)
        return db.execute(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})",
                          tuple(row.values())).lastrowid

    ids = {'user': me['id'], 'login': me['gumi_user_id'],
           'ghost1': unit(GHOST), 'ghost2': unit(GHOST), 'god': unit(GOD), 'lawson': unit(LAWSON),
           'plain': unit(PLAIN)}
    db.execute('UPDATE user_info SET zel=1000000, gems=5 WHERE id=?', (me['id'],))
    db.execute('INSERT OR REPLACE INTO user_summoner (user_id, sex, element, exp1, level1, sp, arm_id)'
               " VALUES (?, 1, 1, 0, 1, 0, '10')", (me['id'],))
    db.execute('DELETE FROM user_summoner_arms WHERE user_id=?', (me['id'],))
    db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
               (hashlib.sha256(TOKEN.encode()).hexdigest(), me['gumi_user_id']))
    db.commit()
    db.close()
    Path(f'{save}.su135.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.su135.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()

    def call(name, body, key=None):
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(name, key, body=body) if key else c.call(name, body=body)

    def summoner(reply):
        return (reply.get('n5mdIUqj') or [{}])[0]

    # The equipped Brave Sword is held, so Summoner's Home > Manage Squad can
    # read its level (the crash in SummonerPartyEditTopScene).
    info = call('UserInfo', {})
    arms = [a.get('RVVgyuor') for a in info.get('dhMmbm5p', [])]
    check('UserInfo: the equipped Brave Sword (10) is in the weapon list', '10' in arms, arms)

    # Chapter 3 squads (deck_type 4) are seeded with the Summoner ("-1") leading
    # each of the ten squads, at the centre (disp_order 2, member_type 0).
    ep3 = [d for d in info.get('dX7S2Lc1', []) if str(d.get('U9ABSYEp')) == '4']
    check('UserInfo: ten Chapter 3 squads, each led by the Summoner (-1)',
          len(ep3) == 10 and all(str(d.get('edy7fq3L')) == '-1' and str(d.get('gr48vsdJ')) == '0'
                                 and str(d.get('XuJL4pc5')) == '2' for d in ep3), ep3[:2])

    # A Chapter 3 squad edit carrying the Summoner saves (user_unit_id was u32,
    # so "-1" failed the whole DeckEdit).
    squad = [{'edy7fq3L': '-1', 'U9ABSYEp': '4', 'zsiAn9P1': '0', 'gr48vsdJ': '0', 'XuJL4pc5': '2'},
             {'edy7fq3L': str(u['plain']), 'U9ABSYEp': '4', 'zsiAn9P1': '0', 'gr48vsdJ': '1', 'XuJL4pc5': '0'}]
    edit = call('m2Ve9PkJ', {'dX7S2Lc1': squad, 'Ti62XfZK': [{'Z0Y4RoD7': '0'}],
                             '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}]}, 'd7UuQsq8')
    db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
    saved = db.execute('SELECT user_unit_id, disp_order FROM user_decks WHERE user_id=? AND deck_type=4'
                       ' AND deck_num=0 ORDER BY disp_order', (u['user'],)).fetchall()
    db.close()
    check('DeckEdit: a Chapter 3 squad with the Summoner and a unit saves',
          'error' not in edit and saved == [(u['plain'], 0), (-1, 2)], (edit.get('error'), saved))

    # Fusion follows unit_ep3_mst: 2 Ghosts + 1 God = 780 Summoning Arts EXP for
    # the current element (Fire), Lawson Frog 2 SP, the plain unit is refused.
    materials = [{'inU8Q4gL': str(u[k]), 'mnZ5K4Ii': str(i), '29MgiJIQ': '0'}
                 for i, k in enumerate(['ghost1', 'ghost2', 'god', 'lawson', 'plain'])]
    mix = call('nv4d3O7F', {'8Z2NQrx1': materials, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}]}, 'vM65bAB4')
    s = summoner(mix)
    check('SummonerMix: no error', 'error' not in mix, mix.get('error'))
    check('SummonerMix: Fire Summoning Arts EXP 15 + 15 + 750 = 780', str(s.get('FTTADDzF')) == '780', s)
    check(f'SummonerMix: Fire Arts Lv {level_for(780, FIRE_ARTS)}',
          str(s.get('Gbe7pD2H')) == str(level_for(780, FIRE_ARTS)), s.get('Gbe7pD2H'))
    check('SummonerMix: Lawson Frog pays 2 SP', str(s.get('iIDqD50O')) == '2', s.get('iIDqD50O'))
    db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
    left = {r[0] for r in db.execute('SELECT user_unit_id FROM user_units WHERE user_id=?', (u['user'],))}
    zel = db.execute('SELECT zel FROM user_info WHERE id=?', (u['user'],)).fetchone()[0]
    db.close()
    check('SummonerMix: the four materials are gone, the plain unit stays',
          not left & {u['ghost1'], u['ghost2'], u['god'], u['lawson']} and u['plain'] in left, left)
    check('SummonerMix: 2,000 + 2,000 + 100,000 + 100,000 Zel', zel == 1000000 - 204000, zel)

    # ShopUse 12: a gender change costs one gem; asking for the same gender is free.
    for label, gems in [('first change to 2 costs 1 gem', 4), ('same gender again costs nothing', 4)]:
        r = call('xe8tiSf4', {'32ibWjFG': [{'60IsqxDt': '12', '03UGMHxF': '1', '9i2xhMaJ': '2'}]},
                 'qthMXTQSkz3KfH9R')
        team = (r.get('fEi17cnx') or [{}])[0]
        check(f'ShopUse 12: {label}', 'error' not in r and str(summoner(r).get('9i2xhMaJ')) == '2',
              (r.get('error'), summoner(r).get('9i2xhMaJ')))
        db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
        have = db.execute('SELECT gems FROM user_info WHERE id=?', (u['user'],)).fetchone()[0]
        db.close()
        check(f'ShopUse 12: {label} (gems {gems})', have == gems, (have, team.get('vKr4YrhP')))
    return check.summary('Summoner as a unit (#135)')


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
