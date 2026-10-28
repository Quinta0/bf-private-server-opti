"""#81: Extra Skill and SP enhancement EXP boosts add to the leader skill's.

python3 scripts/test_sp_exp_boost_wire.py prepare SAVE     # while the server is stopped
python3 scripts/test_sp_exp_boost_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account (never point it at a save players
use), gives it the units below and lends a Zeruiah with her SP from the second
account; ids go to SAVE.sp81.json.  `run` talks to a server built from this
branch on that save.  Needs pycryptodome (.venv/bin/python).

Evidence (see gimuserver/gme/handlers/Mission.cpp, extraSkillExpBoostPercent
and spExpBoostPercent):
* Extra Skills carry process 97 too: Cheery Bjorn & Linlin 830538 and BF2
  Tilith 850808 "5% boost to EXP gained".  They work from any squad slot, the
  helper's included.
* SP nodes with effect type 5 + process 97 upgrade the LEADER skill's EXP
  boost: Infinity Starsage Zeruiah 1000010054 +12, BF2 Tilith 1100000419 +10,
  Bjorn & Linlin 1100000141 +5 and 1100000142 +10 (same series and lv, so the
  client's getFeEffectiveSkillInfo keeps both).
* Every source adds (Global wiki, Player EXP Boost rev 653737); players expect
  Tilith 20 + 5 + 10 = 35%, Zeruiah 20 + 12 = 32%, Bjorn 20 + 5 + 5 + 10 = 40%.

Mission 10 pays 20 EXP and mission 12 pays 29, rounded down, so every expected
number is exact arithmetic.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker, mission_start_body, mission_end_body, started_serial  # noqa: E402

ZERUIAH, TILITH, BJORN, BJORN_7, PLAIN = 830118, 850808, 830538, 830537, 20011
ZERUIAH_SP, TILITH_SP = '11@1000010054', '11@1100000419'
BJORN_SP, BJORN_SP_FIRST = '11@1100000141:1100000142', '11@1100000141'
NPC_HELPER = 'QCHELPER81'
TOKEN = 'sp81-wire-test-token'


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
    template = dict(db.execute('SELECT * FROM user_units WHERE user_id=? LIMIT 1', (me['id'],)).fetchone())
    template.pop('user_unit_id')

    def unit(species, sp='', owner=None, level=1, elgif=0):
        row = dict(template, user_id=owner or me['id'], unit_id=str(species), unit_lvl=level,
                   favorite_flg=0, fe_skill_info=sp, extra_passive_skill_id2=elgif)
        return db.execute(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})",
                          tuple(row.values())).lastrowid

    ids = {'user': me['id'], 'login': me['gumi_user_id'],
           'plain': unit(PLAIN), 'zeruiah': unit(ZERUIAH), 'zeruiah_sp': unit(ZERUIAH, ZERUIAH_SP),
           'tilith': unit(TILITH), 'tilith_sp': unit(TILITH, TILITH_SP),
           'bjorn_sp': unit(BJORN, BJORN_SP), 'bjorn_first': unit(BJORN, BJORN_SP_FIRST), 'bjorn': unit(BJORN),
           # No shipped Elgif skill boosts EXP; Bjorn's ES on the Elgif slot
           # proves the slot is read.
           'elgif': unit(PLAIN, elgif=830538)}
    db.execute('UPDATE user_info SET level=50, exp=0, energy=20000, energy_full_ts=0 WHERE id=?', (me['id'],))
    # Requests must carry the latest guest login's token (only its SHA-256 is stored).
    db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
               (hashlib.sha256(TOKEN.encode()).hexdigest(), me['gumi_user_id']))
    # Mission 12 needs 11 (MissionStart enforces the map's prerequisites).
    db.execute("INSERT INTO user_campaign_missions(user_id,mission_id,state) VALUES (?,'11',2)"
               ' ON CONFLICT(user_id,mission_id) DO UPDATE SET state=2', (me['id'],))
    # A REAL player lends their active-deck leader: Zeruiah with her SP.
    friend = db.execute('SELECT id, active_deck FROM user_info WHERE id<>? ORDER BY rowid LIMIT 1',
                        (me['id'],)).fetchone()
    lent = unit(ZERUIAH, ZERUIAH_SP, owner=friend['id'], level=999)
    led = db.execute('UPDATE user_decks SET user_unit_id=? WHERE user_id=? AND deck_type=1 AND deck_num=?'
                     ' AND member_type=0', (lent, friend['id'], friend['active_deck'])).rowcount
    assert led == 1, f'{friend["id"]} has {led} leader rows in deck {friend["active_deck"]}'
    ids['friend'] = friend['id']
    # A roster friend on the Bjorn & Linlin chain: at the account's Omni peak
    # the picker (and gme::helperLeader) field 830538.
    db.execute('DELETE FROM user_friends WHERE user_id=? AND friend_id=?', (me['id'], NPC_HELPER))
    db.execute("INSERT INTO user_friends(user_id,friend_id,handle_name,base_unit_id,is_dev,favorite)"
               " VALUES (?,?,?,?,0,0)", (me['id'], NPC_HELPER, 'QC Helper', BJORN_7))
    db.commit()
    db.close()
    Path(f'{save}.sp81.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.sp81.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()

    def call(name, body):
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(name, body=body)

    def play(mission, deck, helper='0'):
        body = mission_start_body(mission)
        body['9Q1Lq5FS'][0]['h7eY3sAK'] = helper
        start = call('MissionStart', body)
        assert 'error' not in start, start
        reply = call('MissionEnd', mission_end_body(
            mission, serial=started_serial(start), deck_units=[u[name] for name in deck]))
        return reply, int(reply.get('F5Vs19mb', [{}])[0].get('d96tuT2E', -1))

    cases = [
        ('no qualifying unit', 10, ['plain', 'zeruiah'], '0', 20),
        ('Zeruiah leads, no SP: her 20%', 10, ['zeruiah', 'plain'], '0', 24),
        ('Zeruiah leads with her SP: 20 + 12 = 32%', 10, ['zeruiah_sp', 'plain'], '0', 26),
        ('rounded down: 29 x 1.32 = 38.28', 12, ['zeruiah_sp', 'plain'], '0', 38),
        ('Zeruiah\'s SP only upgrades a LEADER skill', 10, ['plain', 'zeruiah_sp'], '0', 20),
        ('BF2 Tilith leads: 20 + Extra Skill 5 = 25%', 10, ['tilith', 'plain'], '0', 25),
        ('BF2 Tilith leads with her SP: 20 + 5 + 10 = 35%', 10, ['tilith_sp', 'plain'], '0', 27),
        ('Bjorn & Linlin lead with both SP: 20 + 5 + 5 + 10 = 40%', 10, ['bjorn_sp', 'plain'], '0', 28),
        ('Bjorn & Linlin with the first SP only: 30%', 10, ['bjorn_first', 'plain'], '0', 26),
        ('Bjorn & Linlin as a member: Extra Skill 5%', 10, ['plain', 'bjorn'], '0', 21),
        ('Zeruiah + SP leads, BF2 Tilith member: 32 + 5 = 37%', 10, ['zeruiah_sp', 'tilith'], '0', 27),
        ('an Elgif-given Extra Skill counts too', 10, ['plain', 'elgif'], '0', 21),
        ('a unit listed twice counts once', 10, ['plain', 'bjorn', 'bjorn'], '0', 21),
        ('real player\'s Zeruiah + SP as the helper: 32%', 10, ['plain'], u['friend'], 26),
        ('computer helper Bjorn & Linlin: LS 20 + Extra Skill 5', 10, ['plain'], NPC_HELPER, 25),
        ('both leaders Zeruiah + SP: 32 + 32 = 64%', 10, ['zeruiah_sp'], u['friend'], 32),
    ]
    for label, mission, deck, helper, expected in cases:
        reply, inc = play(mission, deck, helper)
        check(f'{label}: result shows {expected}', 'error' not in reply and inc == expected,
              (reply.get('error'), inc))
    return check.summary('SP / Extra Skill EXP boost')


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
