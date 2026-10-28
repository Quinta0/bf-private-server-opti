"""#132: a cleared quest pays the Summoner Avatar (gme/common/SummonerProgress).

python3 scripts/test_summoner_exp_wire.py prepare SAVE     # while the server is stopped
python3 scripts/test_summoner_exp_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account (never point it at a save players
use), clears every story quest below 1000 except 90 so 90 can start, and
gives it a Brave Sword row; ids go to SAVE.su132.json.  Needs pycryptodome
(.venv/bin/python).

Expected numbers come straight from deploy/mst:
* A normal quest pays the step in floor(lifetime player EXP / 3200) (and
  / 320000 for Summoning Arts, * 3 / 320000 for the arm).  prepare parks the
  account at lifetime EXP 319999 mod 320000, so quest 90's 2000 EXP crosses
  all three boundaries: +1 Summoner EXP, +1 element EXP, +1 arm EXP.
* Chapter 3 quest 21074 is "EXP:860,ELEM_EXP_RATE:2,ARM_EXP_RATE:8,
  SUMMON_SLOT:2,FIRST_CLEAR_EXP:3000": first clear 3860 EXP (3000 bonus),
  860 * 2 // 100 = 17 element EXP, 860 * 8 // 100 = 68 arm EXP, pedestals 2.
* SummonerLevelMst is cumulative: Lv 2 at 80 EXP pays 5 SP.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker, mission_start_body, mission_end_body, started_serial  # noqa: E402

TOKEN = 'su132-wire-test-token'
MST = Path(__file__).resolve().parent.parent / 'deploy' / 'mst'
REGULAR, EP3 = 90, 21074


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


SUMMONER = [(int(r['D9wXQI2V']), int(r['d96tuT2E'])) for r in rows('summoner_level_mst.json')]
SP_AT = {int(r['D9wXQI2V']): int(r['iIDqD50O']) for r in rows('summoner_level_mst.json')}
FIRE_ARTS = [(int(r['D9wXQI2V']), int(r['d96tuT2E']))
             for r in rows('summoner_element_level_mst.json') if r['iNy0ZU5M'] == '3']


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
    deck = [r[0] for r in db.execute('SELECT user_unit_id FROM user_units WHERE user_id=? LIMIT 2', (me['id'],))]
    chunks = {int(r['D9wXQI2V']): int(r['d96tuT2E']) for r in rows('user_level_mst.json')}
    level, cum = 1, 0
    while True:
        level += 1
        cum += chunks[level]
        rest = (319999 - cum) % 320000
        if level >= 50 and rest < chunks[level + 1]:
            break
    db.execute('UPDATE user_info SET level=?, exp=?, energy=20000, energy_full_ts=0 WHERE id=?',
               (level, rest, me['id']))
    db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
               (hashlib.sha256(TOKEN.encode()).hexdigest(), me['gumi_user_id']))
    story = [r['j28VNcUW'] for r in rows('mission_mst.json') if r['j28VNcUW'].isdigit()
             and 0 < int(r['j28VNcUW']) < 1000]
    chapter3 = [r['j28VNcUW'] for r in rows('mission_ep3_mst.json') if int(r['j28VNcUW']) < EP3]
    for m in story + chapter3:
        db.execute("INSERT INTO user_campaign_missions(user_id,mission_id,state) VALUES (?,?,2)"
                   ' ON CONFLICT(user_id,mission_id) DO UPDATE SET state=2', (me['id'], m))
    db.execute('DELETE FROM user_campaign_missions WHERE user_id=? AND mission_id IN (?,?)',
               (me['id'], str(REGULAR), str(EP3)))
    db.execute('INSERT OR REPLACE INTO user_summoner (user_id, element, exp, level, sp) VALUES (?,3,79,1,10)',
               (me['id'],))
    db.execute("INSERT OR REPLACE INTO user_summoner_arms (user_id, summoner_arm_id, exp, level)"
               " VALUES (?, '10', 0, 1)", (me['id'],))
    db.commit()
    db.close()
    ids = {'user': me['id'], 'login': me['gumi_user_id'], 'deck': deck}
    Path(f'{save}.su132.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.su132.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()

    def call(name, body):
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(name, body=body)

    def play(mission, status=2):
        start = call('MissionStart', mission_start_body(mission))
        if 'error' in start or 'Kz7qfSs5' not in start:
            return start
        return call('MissionEnd', mission_end_body(
            mission, status=status, serial=started_serial(start), deck_units=u['deck']))

    def summoner(reply):
        return (reply.get('n5mdIUqj') or [None])[0]

    # A normal quest: +1 EXP takes 79 to 80, Lv 2's threshold, which pays 5 SP.
    reply = play(REGULAR)
    s = summoner(reply)
    check('regular quest: no error', 'error' not in reply, reply.get('error'))
    check('regular quest: summoner row in the reply', s is not None, list(reply)[:40])
    if s:
        check('regular quest: EXP 79 + 1 = 80', int(s['d96tuT2E']) == 80, s['d96tuT2E'])
        check('regular quest: Lv 2 (80 reached)', int(s['D9wXQI2V']) == 2, s['D9wXQI2V'])
        check(f'regular quest: SP 10 + {SP_AT[2]}', int(s['iIDqD50O']) == 10 + SP_AT[2], s['iIDqD50O'])
        check('regular quest: element 3 EXP +1', int(s['MYFUzcTo']) == 1, s['MYFUzcTo'])
    check('regular quest: no Chapter 3 result block', 'M4w9LT7t' not in reply, reply.get('M4w9LT7t'))

    # A lost quest pays the Summoner nothing.
    reply = play(REGULAR, status=3)
    check('lost quest: no summoner row', 'error' not in reply and summoner(reply) is None,
          (reply.get('error'), summoner(reply)))

    # Chapter 3, first clear.
    reply = play(EP3)
    check('Chapter 3 quest starts and ends', 'error' not in reply, reply.get('error') or reply.get('Kz7qfSs5'))
    r = (reply.get('M4w9LT7t') or [{}])[0]
    s = summoner(reply) or {}
    exp = 80 + 3860
    check('Chapter 3 first clear: exp "3860,3000"', r.get('d96tuT2E') == '3860,3000', r)
    check('Chapter 3 first clear: elem "17,0", arm "68,0"',
          r.get('1xI7xvK3') == '17,0' and r.get('NqVAPbLC') == '68,0', r)
    lv = level_for(exp, SUMMONER)
    sp = sum(SP_AT[x] for x in range(3, lv + 1))
    check(f'Chapter 3 first clear: before Lv 2, Lv {lv}, SP +{sp}',
          str(r.get('81CsGAR9')) == '2' and str(r.get('wPb5uMR9')) == '1' and str(r.get('iIDqD50O')) == str(sp)
          and int(s.get('D9wXQI2V', 0)) == lv and int(s.get('d96tuT2E', 0)) == exp, (r, s.get('D9wXQI2V')))
    check(f'Chapter 3 first clear: element 3 EXP 1 + 17 -> Lv {level_for(18, FIRE_ARTS)}',
          int(s.get('MYFUzcTo', -1)) == 18 and int(s.get('6PuPnB04', -1)) == level_for(18, FIRE_ARTS), s)
    check('Chapter 3 first clear: 2 Summoning Pedestals', int(s.get('SWfnWyuQ', 0)) == 2, s.get('SWfnWyuQ'))

    reply = play(EP3)
    r = (reply.get('M4w9LT7t') or [{}])[0]
    check('Chapter 3 replay: exp "860,0"', r.get('d96tuT2E') == '860,0', r)

    # Achievements read user_summoner live: "Summoner Level 10 Reached" (52010,
    # category 1, condition 15) now shows the Summoner's level as progress.
    ach = call('GetAchievementInfo', {'1Q2pGAXE': [{'YQG2AE5h': '1', 'KT71m8Ae': '1', '3rhygS9K': '15'}]})
    row = next((x for x in ach.get('YTRJLG65', []) if x.get('M7SXoc31') == '52010'), {})
    lv = level_for(exp + 860, SUMMONER)
    check(f'achievement 52010 progress = Summoner Lv {lv}', str(row.get('pG2n1A28')) == str(lv), row)

    db = sqlite3.connect(f'file:{save}?mode=ro', uri=True)
    arm = db.execute("SELECT exp FROM user_summoner_arms WHERE user_id=? AND summoner_arm_id='10'",
                     (u['user'],)).fetchone()
    check('Brave Sword holds 1 + 68 + 68 arm EXP', arm and arm[0] == 137, arm)
    db.close()
    return check.summary('Summoner EXP (#132)')


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
