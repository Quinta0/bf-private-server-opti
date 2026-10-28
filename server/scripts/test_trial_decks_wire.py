"""Research Lab trial squads: TrialDeckGet (gY63Zvy7) and TrialDeckEdit (MbdL5D4K).

    python scripts/test_trial_decks_wire.py out/qa/current/trial_decks \
        --prepare --source SAVE.sqlite [--user ID]
    # start a server on --port whose save is <dir>/gme.sqlite, then:
    python scripts/test_trial_decks_wire.py out/qa/current/trial_decks [--port 9967] [--user ID]

--prepare makes <dir>/gme.sqlite a sqlite backup copy of SAVE (never the live save
itself) and, BEFORE the server opens it, plants a session token, tops up energy
and drops the account's trial squads, so the run starts from a first visit.
One check writes into the save while the server has it open, so with a Docker
server run the script inside the toolchain image too (a write from macOS through
the bind mount can fail with "disk I/O error").

THE BUG.  Tapping Trial No. 001 (dungeon_type 2) opens TrialPartyTopScene, whose
initConnect @0x1AA7D14 sends TrialDeckGet while the client holds no trial squad,
and MissionCheckScene::updateEvent @0x1883BC0 queues TrialDeckEdit before every
trial MissionStart.  Neither was registered, so the server sent every player
Home before the first trial battle.

Rows travel under PnjQ49wK both ways, every number quoted:
zsiAn9P1 deck, edy7fq3L unit, gr48vsdJ member type (0 leader), XuJL4pc5 slot.
Decks 0-2 are the Trial Zone squads, 10 the Ep3 trial's, 99 the new trial's.
"""
import argparse
import hashlib
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import (Checker, Client, Fixture, backup_copy, mission_end_body,  # noqa: E402
                        mission_start_body, started_serial)

TOKEN = 'trial-decks-wire-test'
TRIAL_001 = 2000000
GET = ('gY63Zvy7', '5QuLx4DG')
EDIT = ('MbdL5D4K', 'Bb1T0qdC')


def pick_account(db, user_id):
    if user_id:
        return db.execute('SELECT id, gumi_user_id FROM user_info WHERE id = ?', (user_id,)).fetchone()
    return db.execute("SELECT id, gumi_user_id FROM user_info WHERE tutorial_status >= 12"
                      " AND gumi_user_id IN (SELECT id FROM gumi_live_users)"
                      " ORDER BY (SELECT COUNT(*) FROM user_units u WHERE u.user_id = user_info.id) DESC"
                      " LIMIT 1").fetchone()


def prepare(fx, source, user_id):
    """Fixture save for the run; must happen before the server opens it."""
    backup_copy(Path(source), fx.db_path)
    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, login = pick_account(db, user_id)
        db.execute('UPDATE user_info SET energy = 99999, energy_full_ts = 0 WHERE id = ?', (user,))
        db.execute('UPDATE gumi_live_users SET token_hash = ? WHERE id = ?',
                   (hashlib.sha256(TOKEN.encode()).hexdigest(), login))
        if db.execute("SELECT 1 FROM sqlite_master WHERE name = 'user_trial_decks'").fetchone():
            db.execute('DELETE FROM user_trial_decks WHERE user_id = ?', (user,))
    print(f'prepared {fx.db_path} for {user}: token planted, energy topped up, trial squads dropped')


def row(deck, unit, member, slot):
    return {'zsiAn9P1': str(deck), 'edy7fq3L': str(unit), 'gr48vsdJ': str(member), 'XuJL4pc5': str(slot)}


def squads(reply):
    """{deck: {slot: (unit, member type)}} from a PnjQ49wK reply."""
    out = {}
    for r in reply.get('PnjQ49wK') or []:
        out.setdefault(int(r['zsiAn9P1']), {})[int(r['XuJL4pc5'])] = (int(r['edy7fq3L']), int(r['gr48vsdJ']))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fixture_dir')
    ap.add_argument('--port', type=int, default=9967)
    ap.add_argument('--user', help='user_info.id to play as (default: the past-tutorial account with most units)')
    ap.add_argument('--prepare', action='store_true', help='build the fixture save, then exit')
    ap.add_argument('--source', help='save to copy for --prepare')
    args = ap.parse_args()

    fx = Fixture(Path(args.fixture_dir).resolve(), args.port)
    if args.prepare:
        assert args.source, '--prepare needs --source'
        prepare(fx, args.source, args.user)
        return 0

    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, _ = pick_account(db, args.user)
        active = db.execute('SELECT active_deck FROM user_info WHERE id = ?', (user,)).fetchone()[0]
        expected0 = {slot: (unit, member) for slot, member, unit in db.execute(
            'SELECT disp_order, member_type, user_unit_id FROM user_decks'
            ' WHERE user_id = ? AND deck_type = 1 AND deck_num = ? AND user_unit_id > 0', (user, active))}
        units = [u for (u,) in db.execute(
            'SELECT user_unit_id FROM user_units WHERE user_id = ? ORDER BY user_unit_id', (user,))]
        stranger = db.execute('SELECT user_unit_id FROM user_units WHERE user_id <> ? LIMIT 1', (user,)).fetchone()
    client = Client(fx, user)
    check = Checker()

    def call(group_key, body=None, token=TOKEN):
        body = dict(body or {})
        body['IKqx1Cn9'] = [{'iN7buP1i': token}]
        body.setdefault('6FrKacq7', [{'Kn51uR4Y': '5EdKHavF'}])
        return client.call(*group_key, body=body)

    check(f'{user} owns at least 4 units for three squads ({len(units)})', len(units) >= 4, units)
    check(f'{user} has an active normal squad {active} to seed from', expected0, active)
    check('the save has another player\'s unit to try a foreign squad slot with', stranger, 'single-account save')

    # 1. First visit: squad 1 is the active normal squad.
    first = call(GET)
    check('TrialDeckGet answers (was: unregistered, player sent Home)', 'error' not in first, first.get('error'))
    rows = first.get('PnjQ49wK') or []
    check('every PnjQ49wK row carries the four keys, all quoted',
          rows and all(set(r) >= {'zsiAn9P1', 'edy7fq3L', 'gr48vsdJ', 'XuJL4pc5'}
                       and all(isinstance(r[k], str) for k in r) for r in rows), rows)
    check('first visit seeds only squad 1 (deck 0) with the active normal squad',
          squads(first) == {0: expected0}, (squads(first), expected0))
    check('squad 1 has exactly one leader',
          sum(1 for _, m in squads(first).get(0, {}).values() if m == 0) == 1, squads(first))
    again = call(GET)
    check('a second TrialDeckGet returns the same squads', squads(again) == squads(first), squads(again))

    # 2. The squad screen saves three squads plus the Strategy Zone squad, whose
    # reserves (member type 2) sit in slots 5-14 (max_reserve_num 10).
    a, b, c, d = units[:4]
    edit = [row(0, a, 0, 0), row(0, b, 1, 1), row(1, c, 0, 0), row(2, d, 0, 2),
            row(99, a, 0, 0), row(99, b, 2, 5), row(99, c, 2, 14)]
    bogus = [row(5, a, 0, 0), row(1, c, 7, 1), row(1, c, 1, 40), row(2, 'x', 1, 0),
             row(2, stranger[0] if stranger else 987654321, 1, 1)]
    reply = call(EDIT, {'PnjQ49wK': edit + bogus})
    check('TrialDeckEdit answers {} (was: unregistered, sent before every trial battle)',
          reply == {}, reply)
    want = {0: {0: (a, 0), 1: (b, 1)}, 1: {0: (c, 0)}, 2: {2: (d, 0)}, 99: {0: (a, 0), 5: (b, 2), 14: (c, 2)}}
    got = squads(call(GET))
    check('TrialDeckGet returns exactly the saved squads (Strategy Zone reserves included);'
          ' a foreign unit, deck 5, member type 7, slot 40 and junk dropped', got == want, (got, want))

    # 3. A Battle Hall edit carries only deck 10: the other squads stay.
    call(EDIT, {'PnjQ49wK': [row(10, b, 0, 0)]})
    got = squads(call(GET))
    check('an edit with only deck 10 keeps squads 1-3 and deck 99', got == {**want, 10: {0: (b, 0)}}, got)

    # 4. A Trial Zone edit replaces all three squads: squad 3 emptied, squad 2
    # changed, and squad 1 is no longer the active normal squad it was seeded from.
    call(EDIT, {'PnjQ49wK': [row(0, a, 0, 0), row(1, b, 0, 0)]})
    got = squads(call(GET))
    stored = {0: {0: (a, 0)}, 1: {0: (b, 0)}, 10: {0: (b, 0)}, 99: want[99]}
    check('a Trial Zone edit replaces squads 1-3, keeps decks 10 and 99 and is not re-seeded', got == stored, got)

    # 5. A malformed body never wipes the squads, and is not refused either.
    reply = call(EDIT, {'PnjQ49wK': 'garbage'})
    check('a malformed TrialDeckEdit answers {} and keeps the squads',
          reply == {} and squads(call(GET)) == got, reply)
    reply = call(EDIT, {})
    check('a TrialDeckEdit with no PnjQ49wK keeps the squads', reply == {} and squads(call(GET)) == got, reply)

    # 6. A unit sold since is left out of the reply.  (Written straight into the
    # save the server has open: fine on one host, flaky across a Docker mount.)
    with sqlite3.connect(fx.db_path, timeout=30) as db:
        db.execute('INSERT OR REPLACE INTO user_trial_decks (user_id, deck_num, disp_order, member_type, user_unit_id)'
                   ' VALUES (?, 2, 4, 1, 987654321)', (user,))
    reply = call(GET)
    check('a squad slot whose unit the player no longer owns is not sent',
          'error' not in reply and squads(reply) == stored, reply)

    # 7. The squads belong to the session's player.
    stale = call(GET, token='not-the-token')
    check('TrialDeckGet with a stale token is refused (session expired)', 'error' in stale, stale)

    # 8. The trial itself: edit, start, clear -- the path players never got past.
    reply = call(EDIT, {'PnjQ49wK': [row(0, a, 0, 0), row(0, b, 1, 1), row(0, c, 1, 2)]})
    check('TrialDeckEdit before the battle answers {}', reply == {}, reply)
    body = mission_start_body(TRIAL_001)
    body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
    start = client.call('MissionStart', body=body)
    if check('Trial No. 001 MissionStart answers', 'error' not in start, start.get('error')):
        serial = started_serial(start)
        check('MissionStart issued a battle serial (the run is recorded)', serial > 0, serial)
        body = mission_end_body(TRIAL_001, serial=serial, deck_units=[a, b, c])
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        end = client.call('MissionEnd', body=body)
        check('Trial No. 001 MissionEnd (clear) answers', 'error' not in end, end.get('error'))

    return check.summary('trial decks')


if __name__ == '__main__':
    sys.exit(main())
