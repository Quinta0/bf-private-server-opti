"""A borrowed helper fights with its SP picks and Extra Skills (player report).

python3 scripts/test_helper_sp_elgif_wire.py prepare SAVE     # while the server is stopped
python3 scripts/test_helper_sp_elgif_wire.py run --save SAVE [--url URL]

`prepare` takes the save's first account (never point it at a save players
use), makes the second account its friend and has that friend lead with a
Zeruiah carrying her SP and an Elgif skill.  It puts the same unit in the
first account's Squad 10 (own helper) and adds a computer-run friend; ids go
to SAVE.helper82.json.  `run` talks to a server built from this branch on that
save.  Needs pycryptodome (.venv/bin/python).

FriendGet's picker (xZH6EIQ7, ReinforcementInfo) must carry, for a real
player's unit, what the battle reads off it: fe_skill_info Fnxab5CN
(getFeEffectiveSkillInfo), the unit's own Extra Skill cP83zNsv and the Elgif
slot LjY4DfRg (getExtraPassiveSkillIDList).  A computer helper keeps them
empty.  MissionEnd pays the helper's Elgif skill EXP boost like its own.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import Client, Checker, mission_start_body, mission_end_body, started_serial  # noqa: E402

ZERUIAH, PLAIN, BJORN_7 = 830118, 20011, 830537
ZERUIAH_SP = '11@1000010054'
# Bjorn & Linlin's Extra Skill (5% EXP) on the Elgif slot, so MissionEnd shows
# the slot is read; extra_passive_skill_mst names its id after the unit, as it
# does Zeruiah's own.
ELGIF_SKILL, ZERUIAH_OWN_SKILL = 830538, 830118
NPC_HELPER = 'QCHELPER82'
FRIEND_GET = ('2o4axPIC', 'EoYuZ2nbImhCU1c0')
PICKER = 'xZH6EIQ7'
HELPER_SQUAD = 9  # user_decks.deck_num of Squad 10
TOKEN = 'helper82-wire-test-token'


def prepare(save: Path):
    db = sqlite3.connect(save)
    db.row_factory = sqlite3.Row
    me = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
    template = dict(db.execute('SELECT * FROM user_units WHERE user_id=? LIMIT 1', (me['id'],)).fetchone())
    template.pop('user_unit_id')

    def unit(species, owner, sp='', elgif=0, level=1):
        row = dict(template, user_id=owner, unit_id=str(species), unit_lvl=level,
                   favorite_flg=0, fe_skill_info=sp, extra_passive_skill_id2=elgif)
        return db.execute(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})",
                          tuple(row.values())).lastrowid

    ids = {'user': me['id'], 'login': me['gumi_user_id'], 'plain': unit(PLAIN, me['id'])}
    db.execute('UPDATE user_info SET level=50, exp=0, energy=20000, energy_full_ts=0 WHERE id=?', (me['id'],))
    # Requests must carry the latest guest login's token (only its SHA-256 is stored).
    db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
               (hashlib.sha256(TOKEN.encode()).hexdigest(), me['gumi_user_id']))

    # A REAL friend leads with Zeruiah, her SP and an Elgif skill.
    friend = db.execute('SELECT id, active_deck FROM user_info WHERE id<>? ORDER BY rowid LIMIT 1',
                        (me['id'],)).fetchone()
    lent = unit(ZERUIAH, friend['id'], ZERUIAH_SP, ELGIF_SKILL, level=999)
    led = db.execute('UPDATE user_decks SET user_unit_id=? WHERE user_id=? AND deck_type=1 AND deck_num=?'
                     ' AND member_type=0', (lent, friend['id'], friend['active_deck'])).rowcount
    assert led == 1, f'{friend["id"]} has {led} leader rows in deck {friend["active_deck"]}'
    now = int(time.time())
    for a, b in ((me['id'], friend['id']), (friend['id'], me['id'])):
        db.execute('INSERT INTO user_relations(user_id,other_id,state,favorite,created_at) VALUES (?,?,1,0,?)'
                   ' ON CONFLICT(user_id,other_id) DO UPDATE SET state=1', (a, b, now))
    ids['friend'] = friend['id']

    # The same unit as the account's OWN helper (Squad 10).
    own = unit(ZERUIAH, me['id'], ZERUIAH_SP, ELGIF_SKILL)
    db.execute('DELETE FROM user_decks WHERE user_id=? AND deck_type=1 AND deck_num=?', (me['id'], HELPER_SQUAD))
    db.execute('INSERT INTO user_decks(user_id,user_unit_id,deck_type,deck_num,member_type,disp_order)'
               ' VALUES (?,?,1,?,0,0)', (me['id'], own, HELPER_SQUAD))

    # A computer-run friend: no SP, no Elgif.
    db.execute('DELETE FROM user_friends WHERE user_id=? AND friend_id=?', (me['id'], NPC_HELPER))
    db.execute("INSERT INTO user_friends(user_id,friend_id,handle_name,base_unit_id,is_dev,favorite)"
               " VALUES (?,?,?,?,0,0)", (me['id'], NPC_HELPER, 'QC Helper', BJORN_7))
    db.commit()
    db.close()
    Path(f'{save}.helper82.json').write_text(json.dumps(ids))
    print('prepared', ids)


def run(save: Path, url: str) -> int:
    u = json.loads(Path(f'{save}.helper82.json').read_text())
    c = Client.__new__(Client)
    c.user, c.login, c.url = u['user'], u['login'], f'{url}/bf/gme/action.php'
    check = Checker()

    def call(name, body, key=None):
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        return c.call(name, key, body=body)

    picker = call(FRIEND_GET[0], {}, key=FRIEND_GET[1]).get(PICKER) or []
    by_id = {h.get('h7eY3sAK'): h for h in picker}

    def slots(card):
        return (card.get('Fnxab5CN', ''), str(card.get('cP83zNsv', '0')), str(card.get('LjY4DfRg', '0')))

    want = (ZERUIAH_SP, str(ZERUIAH_OWN_SKILL), str(ELGIF_SKILL))
    friend = by_id.get(u['friend'], {})
    check("the real friend's Zeruiah carries her SP, own Extra Skill and Elgif skill",
          slots(friend) == want, slots(friend))
    own = next((h for h in picker if str(h.get('h7eY3sAK', '')).startswith('MYUNIT')), {})
    check('the own Squad 10 helper carries them too', slots(own) == want, slots(own))
    npc = by_id.get(NPC_HELPER, {})
    check('a computer-run helper stays empty', bool(npc) and slots(npc) == ('', '0', '0'), slots(npc))

    # MissionEnd: the helper's leader skill 20 + SP 12 + Elgif Extra Skill 5 = 37%
    # on mission 10's 20 EXP (without the Elgif skill it is 26, #81's case).
    body = mission_start_body(10)
    body['9Q1Lq5FS'][0]['h7eY3sAK'] = u['friend']
    start = call('MissionStart', body)
    assert 'error' not in start, start
    reply = call('MissionEnd', mission_end_body(10, serial=started_serial(start), deck_units=[u['plain']]))
    inc = int(reply.get('F5Vs19mb', [{}])[0].get('d96tuT2E', -1))
    check("MissionEnd pays the helper's Elgif EXP boost: 20 x 1.37 = 27", 'error' not in reply and inc == 27,
          (reply.get('error'), inc))
    return check.summary('helper SP / Extra Skills')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['prepare', 'run'])
    ap.add_argument('save', nargs='?')
    ap.add_argument('--save', dest='save_opt')
    ap.add_argument('--url', default='http://127.0.0.1:9982')
    args = ap.parse_args()
    save = Path(args.save or args.save_opt)
    if args.mode == 'prepare':
        prepare(save)
        return 0
    return run(save, args.url)


if __name__ == '__main__':
    sys.exit(main())
