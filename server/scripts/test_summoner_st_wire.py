"""#143: a Chapter 3 Summoner Helper pays Summoner Training Points, not Honor.

python3 scripts/test_summoner_st_wire.py prepare SAVE     # while the server is stopped
python3 scripts/test_summoner_st_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account (never a save players use), clears
the story below 1000 except quest 90 and the Chapter 3 chain before 21074, and
lends the second account's leader as the helper; ids go to SAVE.st143.json.
Needs pycryptodome (.venv/bin/python).

Help text: "You can gain Summoner Training (ST) Points by taking your Friends
as Summoner Helpers in battle ... In Brave Frontier's Chapter 3, you will not
gain Honor Points."  So the same helper pays H Honor on quest 90 and H ST
points (and no Honor) on Chapter 3 quest 21074; record 53010 ("100 Summoner
Training Points Accumulated") reads the lifetime ST total.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker, mission_start_body, mission_end_body, started_serial  # noqa: E402

TOKEN = 'st143-wire-test-token'
MST = Path(__file__).resolve().parent.parent / 'deploy' / 'mst'
REGULAR, EP3 = 90, 21074


def rows(name):
    data = json.loads((MST / name).read_text())
    return next(iter(data.values())) if isinstance(data, dict) else data


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
    friend = db.execute('SELECT id FROM user_info WHERE id<>? ORDER BY rowid LIMIT 1', (me['id'],)).fetchone()
    deck = [r[0] for r in db.execute('SELECT user_unit_id FROM user_units WHERE user_id=? LIMIT 2', (me['id'],))]
    db.execute('UPDATE user_info SET level=60, exp=0, energy=20000, energy_full_ts=0, friend_points=0 WHERE id=?',
               (me['id'],))
    story = [r['j28VNcUW'] for r in rows('mission_mst.json') if r['j28VNcUW'].isdigit()
             and 0 < int(r['j28VNcUW']) < 1000 and int(r['j28VNcUW']) != REGULAR]
    chapter3 = [r['j28VNcUW'] for r in rows('mission_ep3_mst.json') if int(r['j28VNcUW']) < EP3]
    for m in story + chapter3:
        db.execute("INSERT INTO user_campaign_missions(user_id,mission_id,state) VALUES (?,?,2)"
                   ' ON CONFLICT(user_id,mission_id) DO UPDATE SET state=2', (me['id'], m))
    db.execute('DELETE FROM user_campaign_missions WHERE user_id=? AND mission_id IN (?,?)',
               (me['id'], str(REGULAR), str(EP3)))
    db.execute('INSERT OR REPLACE INTO user_summoner (user_id, friend_point) VALUES (?, 7)', (me['id'],))
    db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
               (hashlib.sha256(TOKEN.encode()).hexdigest(), me['gumi_user_id']))
    db.commit()
    db.close()
    ids = {'user': me['id'], 'login': me['gumi_user_id'], 'deck': deck, 'helper': friend['id']}
    Path(f'{save}.st143.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.st143.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()

    def call(name, body):
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(name, body=body)

    def play(mission):
        body = mission_start_body(mission)
        body['9Q1Lq5FS'][0]['h7eY3sAK'] = u['helper']
        start = call('MissionStart', body)
        if 'error' in start or 'Kz7qfSs5' not in start:
            return start
        return call('MissionEnd', mission_end_body(mission, serial=started_serial(start), deck_units=u['deck']))

    def state():
        db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
        honor = db.execute('SELECT friend_points FROM user_info WHERE id=?', (u['user'],)).fetchone()[0]
        st, total = db.execute('SELECT friend_point, st_points_total FROM user_summoner WHERE user_id=?',
                               (u['user'],)).fetchone()
        db.close()
        return honor, st, total

    h0, st0, total0 = state()
    check('the migration seeds the lifetime total from the balance (7)', total0 == 7, total0)
    reply = play(REGULAR)
    h1, st1, total1 = state()
    pay = h1 - h0
    check(f'normal quest with a helper: Honor +{pay}, ST unchanged',
          'error' not in reply and pay > 0 and (st1, total1) == (st0, total0), (reply.get('error'), pay, st1))

    reply = play(EP3)
    h2, st2, total2 = state()
    check('Chapter 3 quest with the same helper: no Honor', 'error' not in reply and h2 == h1,
          (reply.get('error'), h1, h2))
    check(f'Chapter 3 quest: ST points +{pay} (balance and lifetime)',
          st2 - st1 == pay and total2 - total1 == pay, (st1, st2, total1, total2))
    s = (reply.get('n5mdIUqj') or [{}])[0]
    check('the reply carries the new ST balance (J3stQ7jd)', str(s.get('J3stQ7jd')) == str(st2), s.get('J3stQ7jd'))

    ach = call('GetAchievementInfo', {'1Q2pGAXE': [{'YQG2AE5h': '1', 'KT71m8Ae': '1', '3rhygS9K': '15'}]})
    row = next((x for x in ach.get('YTRJLG65', []) if x.get('M7SXoc31') == '53010'), {})
    check(f'record 53010 progress = lifetime ST {total2}', str(row.get('pG2n1A28')) == str(total2), row)
    return check.summary('Chapter 3 ST points (#143)')


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
