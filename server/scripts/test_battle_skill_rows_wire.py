"""Battle-only skill rows in MissionStart: the Research Lab boss crash.

    python scripts/test_battle_skill_rows_wire.py out/qa/current/battle_skill_rows \
        --prepare --source SAVE.sqlite [--user ID]
    # start a server on --port whose save is <dir>/gme.sqlite, then:
    python scripts/test_battle_skill_rows_wire.py out/qa/current/battle_skill_rows [--port 9976] [--user ID]

--prepare makes <dir>/gme.sqlite a sqlite backup copy of SAVE (never the live save
itself) and, BEFORE the server opens it, plants a session token, tops up energy
and clears every mission so none is locked.

THE CRASH.  A skill row whose ToZj4zyn is "0" -- every monster-only skill, 21,174
of F_SKILL_MST's 33,381 -- is filed by UnitSkillMstResponse::readParam
@0x13F9E04 in UnitSkillMstList's TEMPORARY list, which
GameUtils::battleTemporaryMstRemove @0x1176238 empties on every Home and after
every mission result.  GameScene::requestMonsterSummonFiles @0x16433CC looks each
monster skill up at battle load and calls getProcessID on the result unchecked,
so a boss with such a skill (Trial No. 001's Karl: 6140-6143) crashed the game
the moment its battle loaded (SIGSEGV at 0x48).

So MissionStart must carry, under 8aiBoHg5 (the key that APPENDS,
UnitSkillMstResponse(true)), the full F_SKILL_MST row of every ToZj4zyn "0"
skill its monsters (scripted forms included) use, and nothing else; a mission
whose monsters use none must carry no 8aiBoHg5 block at all.
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import ROOT, Checker, Client, Fixture, backup_copy, mission_start_body  # noqa: E402

CONTROL_MISSIONS = (10, 85)   # no monster skills at all
TOKEN = 'battle-skill-rows-wire-test'
SKILLS = {r['nj9Lw7mV']: r for r in
          json.loads((ROOT / 'deploy/mst/skill_mst.json').read_text(encoding='utf-8'))['8aiBoHg5']}


def monster_skill_ids(mission):
    """Every skill id the mission's monsters (and scripted forms) name."""
    ids = set()
    groups = [stage['battle_monsters'] for stage in mission['stages']]
    groups += [[chest['monster']] for chest in mission.get('mimic_chests') or []]
    groups += [mission.get('script_monsters') or []]
    for monsters in groups:
        for monster in monsters:
            ids.update(str(s['skill_id']) for s in monster.get('skills') or [])
    return ids


def pick_account(db, user_id):
    if user_id:
        return db.execute('SELECT id, gumi_user_id FROM user_info WHERE id = ?', (user_id,)).fetchone()
    return db.execute("SELECT id, gumi_user_id FROM user_info WHERE tutorial_status >= 12"
                      " AND gumi_user_id IN (SELECT id FROM gumi_live_users) LIMIT 1").fetchone()


def prepare(fx, source, user_id, missions):
    """Fixture save for the run; must happen before the server opens it."""
    backup_copy(Path(source), fx.db_path)
    rows = json.loads((ROOT / 'deploy/mst/mission_mst.json').read_text(encoding='utf-8'))['oXeC1Ak9']
    # Every mission, so no prerequisite (story or event chain) locks one out.
    clear = {r['j28VNcUW'] for r in rows} | {str(m) for m in missions}
    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, login = pick_account(db, user_id)
        db.execute('UPDATE user_info SET energy = 99999, energy_full_ts = 0 WHERE id = ?', (user,))
        db.execute('UPDATE gumi_live_users SET token_hash = ? WHERE id = ?',
                   (hashlib.sha256(TOKEN.encode()).hexdigest(), login))
        db.executemany('INSERT INTO user_campaign_missions (user_id, mission_id, state, attain_percent, clear_count)'
                       ' VALUES (?, ?, 2, 100, 1) ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2',
                       [(user, mission) for mission in sorted(clear)])
    print(f'prepared {fx.db_path} for {user}: token planted, {len(clear)} missions cleared')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fixture_dir')
    ap.add_argument('--port', type=int, default=9976)
    ap.add_argument('--user', help='user_info.id to play as (default: first account past the tutorial)')
    ap.add_argument('--prepare', action='store_true', help='build the fixture save, then exit')
    ap.add_argument('--source', help='save to copy for --prepare')
    args = ap.parse_args()

    fx = Fixture(Path(args.fixture_dir).resolve(), args.port)
    archive = {m['id']: m for m in json.loads((ROOT / 'deploy/archive/mission.json').read_text(encoding='utf-8'))}
    skilled = sorted(mid for mid, m in archive.items() if monster_skill_ids(m))
    if args.prepare:
        assert args.source, '--prepare needs --source'
        prepare(fx, args.source, args.user, skilled)
        return 0

    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, _ = pick_account(db, args.user)
    client = Client(fx, user)
    check = Checker()
    check(f'the archive has missions whose monsters use skills ({len(skilled)})', skilled)

    for mission_id in skilled + list(CONTROL_MISSIONS):
        body = mission_start_body(mission_id)
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        reply = client.call('MissionStart', body=body)
        if not check(f'{mission_id}: MissionStart answers', 'error' not in reply, reply.get('error')):
            continue
        used = monster_skill_ids(archive[mission_id])
        missing = sorted(used - set(SKILLS))
        check(f'{mission_id}: every monster skill is in skill_mst.json', not missing, missing)
        want = {s for s in used if SKILLS.get(s, {}).get('ToZj4zyn') == '0'}
        rows = reply.get('8aiBoHg5')
        if not want:
            check(f'{mission_id}: no battle-only skill, so no 8aiBoHg5 block', rows is None, rows)
            continue
        got = {r.get('nj9Lw7mV'): r for r in rows or []}
        check(f'{mission_id}: one 8aiBoHg5 row per battle-only skill {sorted(want)}',
              rows is not None and set(got) == want and len(got) == len(rows), sorted(got))
        for skill_id in sorted(want & set(got)):
            row, expected = got[skill_id], SKILLS[skill_id]
            diff = {k: (row.get(k), v) for k, v in expected.items() if v is not None and row.get(k) != v}
            check(f'{mission_id}/{skill_id}: row equals the F_SKILL_MST row, ToZj4zyn "0"',
                  not diff and row.get('ToZj4zyn') == '0', diff)
        permanent = sorted(s for s in got if SKILLS.get(s, {}).get('ToZj4zyn') != '0')
        check(f'{mission_id}: no permanent (ToZj4zyn "1") row re-sent', not permanent, permanent)

    return check.summary('battle skill rows')


if __name__ == '__main__':
    sys.exit(main())
