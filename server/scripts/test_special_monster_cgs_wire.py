"""Special-monster animation rows: the Juggernaut crash (mission 85).

    python scripts/test_special_monster_cgs_wire.py out/qa/current/special_monster_cgs \
        --prepare --source SAVE.sqlite [--user ID]
    # start a server on --port whose save is <dir>/gme.sqlite, then:
    python scripts/test_special_monster_cgs_wire.py out/qa/current/special_monster_cgs \
        [--port 9975] [--content PATH_TO/assets/content] [--user ID]

--prepare makes <dir>/gme.sqlite a sqlite backup copy of SAVE (never the live save
itself) and, BEFORE the server opens it, plants a session token, tops up energy
and clears every story mission so the later bosses are not locked.  The run then
talks to the already running server.

THE CRASH.  MonsterUnit::initialize @0x1155FDC loads all four animation types for
every monster, through MonsterUnit::addCgs @0x1158924:

    GameUtils::getMonsterCgs(id, type) @0x1EAFD74 -> CommonUtils::getLocalPath(name)
    -> EdgeAnimeCgs::cgsWithFile -> CommonUtils::getCsvList -> CCFileUtils::getFileData

A type with no MonsterCgsMst row reads back as "", getLocalPath("") is the
writable directory itself, and getFileData fopen()s that directory and sizes it
with fseek(SEEK_END)/ftell.  On ext4 with hashed directory indexes ftell answers
0x7fffffffffffffff, new[] fails and the game aborts the moment the boss is
created.  The server used to omit the move slot (and the dummies' attack and
skill slots); f2fs and the Android Studio emulator answer the real directory
size, which is why it never crashed there.

So for every MonsterMst row on the /monster/ branch (unit id 0) the reply must
carry exactly one non-empty row per cgs type 1-4, naming a file that ships, whose
frames all index inside the monster's cgg frame table (EdgeAnimeCgg::getSprites
@0xFF783C tail-calls a NULL group past the last row).  A mission without special
monsters must carry no block at all (MonsterCgsMstResponse::readParam @0x13E4E20
starts with removeAllObjects).
"""
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bf_testkit import ROOT, Checker, Client, Fixture, backup_copy, mission_start_body  # noqa: E402

CONTROL_MISSION = 10   # ordinary story mission: every enemy is a re-skinned unit
CGS_TYPES = (1, 2, 3, 4)
# Session token planted in the fixture copy (gumi_live_users stores only its SHA-256).
TOKEN = 'special-monster-cgs-wire-test'


def special_missions(archive):
    """Missions with at least one drawable special monster (img_a and anm_cgg set).

    A visuals block without art (Xie Jing, 8360505/8360506) is refused by
    MissionArchiver before any cgs row is built, so it is not a case here."""
    out = []
    for mission in archive:
        for stage in mission['stages']:
            if any(m.get('visuals') and m['visuals']['img_a'] and m['visuals']['anm_cgg']
                   for m in stage['battle_monsters']):
                out.append(mission['id'])
                break
    return out


def csv_rows(path):
    return [line.split(',') for line in path.read_text(encoding='utf-8', errors='replace').splitlines()
            if line.strip()]


def pick_account(db, user_id):
    """(user id, gumi user id) of the account the run plays as."""
    if user_id:
        return db.execute('SELECT id, gumi_user_id FROM user_info WHERE id = ?', (user_id,)).fetchone()
    return db.execute("SELECT id, gumi_user_id FROM user_info WHERE tutorial_status >= 12"
                      " AND gumi_user_id IN (SELECT id FROM gumi_live_users) LIMIT 1").fetchone()


def prepare(fx, source, user_id):
    """Fixture save for the run; must happen before the server opens it."""
    backup_copy(Path(source), fx.db_path)
    rows = json.loads((ROOT / 'deploy/mst/mission_mst.json').read_text(encoding='utf-8'))['oXeC1Ak9']
    story = [r['j28VNcUW'] for r in rows if 1 <= int(r['9C64Qwe0']) <= 20]
    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, login = pick_account(db, user_id)
        db.execute('UPDATE user_info SET energy = 99999, energy_full_ts = 0 WHERE id = ?', (user,))
        db.execute('UPDATE gumi_live_users SET token_hash = ? WHERE id = ?',
                   (hashlib.sha256(TOKEN.encode()).hexdigest(), login))
        db.executemany('INSERT INTO user_campaign_missions (user_id, mission_id, state, attain_percent, clear_count)'
                       ' VALUES (?, ?, 2, 100, 1) ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2',
                       [(user, mission) for mission in story])
    print(f'prepared {fx.db_path} for {user}: token planted, {len(story)} story missions cleared')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fixture_dir')
    ap.add_argument('--port', type=int, default=9975)
    ap.add_argument('--content', default=str(ROOT / 'deploy' / 'game_content' / 'content'))
    ap.add_argument('--user', help='user_info.id to play as (default: first account past the tutorial)')
    ap.add_argument('--prepare', action='store_true', help='build the fixture save, then exit')
    ap.add_argument('--source', help='save to copy for --prepare')
    args = ap.parse_args()

    fx = Fixture(Path(args.fixture_dir).resolve(), args.port)
    if args.prepare:
        assert args.source, '--prepare needs --source'
        prepare(fx, args.source, args.user)
        return 0

    monster_dir = Path(args.content) / 'monster'
    assert (monster_dir / 'cgs').is_dir(), f'no monster/cgs under {args.content}'
    archive = json.loads((ROOT / 'deploy/archive/mission.json').read_text(encoding='utf-8'))
    missions = special_missions(archive)

    with sqlite3.connect(fx.db_path, timeout=30) as db:
        user, _ = pick_account(db, args.user)
    client = Client(fx, user)
    check = Checker()

    for mission_id in missions + [CONTROL_MISSION]:
        body = mission_start_body(mission_id)
        body['IKqx1Cn9'] = [{'iN7buP1i': TOKEN}]
        reply = client.call('MissionStart', body=body)
        if not check(f'{mission_id}: MissionStart answers', 'error' not in reply, reply.get('error')):
            continue
        monsters = {m['o49dYfpH']: m for m in reply.get('U0v5IeJo', [])}
        specials = {mid: m for mid, m in monsters.items() if m['pn16CNah'] == '0'}
        rows = reply.get('8hoyIF9Q')

        if not specials:
            check(f'{mission_id}: no special monster, so no MonsterCgsMst block', rows is None, rows)
            continue
        check(f'{mission_id}: special monster(s) {sorted(specials)} described', bool(specials))

        by_slot = {}
        for row in rows or []:
            by_slot.setdefault((row['o49dYfpH'], int(row['fpc3rbs7'])), []).append(row['0QxL2is7'])
        for mid, mst in sorted(specials.items()):
            cgg = monster_dir / 'cgg' / mst['QqfI9mM4']
            frame_count = len(csv_rows(cgg)) if cgg.is_file() else 0
            check(f'{mission_id}/{mid}: frame table {cgg.name} ships with frames', frame_count > 0, cgg)
            for cgs_type in CGS_TYPES:
                names = set(by_slot.get((mid, cgs_type), []))
                name = next(iter(names), '')
                label = f'{mission_id}/{mid}: cgs type {cgs_type}'
                if not check(f'{label} is one non-empty row', len(names) == 1 and name, names):
                    continue
                path = monster_dir / 'cgs' / name
                if not check(f'{label} file {name} ships', path.is_file(), path):
                    continue
                indexes = [int(r[0]) for r in csv_rows(path) if r[0].strip().lstrip('-').isdigit()]
                check(f'{label} frames index inside the {frame_count}-frame table',
                      indexes and all(0 <= i < frame_count for i in indexes), (name, indexes, frame_count))
        stray = sorted({mid for mid, _ in by_slot} - set(specials))
        check(f'{mission_id}: no animation rows for unit-backed monsters', not stray, stray)

    return check.summary('special monster cgs')


if __name__ == '__main__':
    sys.exit(main())
