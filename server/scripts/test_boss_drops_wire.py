"""Wire test: farmable bosses (ticket #82).

python3 scripts/test_boss_drops_wire.py prepare SAVE            # while the server is stopped
python3 scripts/test_boss_drops_wire.py run --token FILE --container NAME [--url URL]

`prepare` borrows the save's highest-level account (tools/test_vortex_tiles), writes its
token to SAVE.token.json, opens every quest, tops up its energy and gives it a Lv 1 Lava
(10122) that did not come from a drop: the "summoned" copy the Merit check compares
against.  Never point it at a save players use.  `run` talks to a server built from this
branch with prod's tables (archive, mst and the balance folder with the Karma dungeon
overlays) and reads what each trade-in paid from NAME's log.  Every start ends as a clear.
Needs pycryptodome (.venv/bin/python).

Checks, from MissionStart's drop_info (LE6JkUp7, "stage|idx:0:unit:lv:type-...|..."):
  base form   540 Mischief and Malice: its boss Great Sage Mimir (50042, a 3-star line)
              drops the base form Mimir (50040), at about 20%; Cait Sith beside it (a
              minion) never.  Its regular waves keep exactly the drops they had: each slot
              only ever drops its own unit (the archive's capture floor, untouched).
  regular     961 The Red Gate: Fire God Lava and Drake Queen Aisha in the regular waves
              (#47 took their capture) never drop, though the boss is Fire God Lava too.
  rarity      961's boss drops base Lava (10122, an Omni line) at about 2%, far rarer.
  karma       the Karma dungeon (Souls Training Ground overlays 100000-100002, 100050-100053)
              never drops a unit, though its boss Hell King Hades is a normal unit.
  merit       a Lava reported from a 961 clear trades in for 25% (at least 1) of what the
              prepared copy pays.
"""
import argparse
import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, 'tools')
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import borrow_test_account, call  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DELIVER = ('vsaXI4M0', '2Lj5hIEG')
MIMIR, LAVA = 50040, 10122
KARMA_DUNGEON = (100000, 100001, 100002, 100050, 100051, 100052, 100053)


def archive() -> dict:
    return {m['id']: m for m in json.loads((ROOT / 'deploy/archive/mission.json').read_text())}


def prepare(save: Path) -> int:
    login, user, token = borrow_test_account(save.parent)
    with sqlite3.connect(save) as db:
        db.row_factory = sqlite3.Row
        db.execute('UPDATE user_info SET energy = 99999, achieve_point = 0 WHERE id = ?', (user,))
        db.execute('DELETE FROM user_achievement_deliver WHERE user_id = ?', (user,))
        # Every quest open: MissionStart refuses one whose prerequisites are not cleared.
        db.executemany('INSERT INTO user_campaign_missions(user_id, mission_id, state) VALUES (?, ?, 2)'
                       ' ON CONFLICT(user_id, mission_id) DO UPDATE SET state = 2',
                       [(user, str(m)) for m in [*archive(), *KARMA_DUNGEON]])
        template = dict(db.execute('SELECT * FROM user_units WHERE user_id = ? LIMIT 1', (user,)).fetchone())
        template.pop('user_unit_id')
        template.update(unit_id=str(LAVA), unit_lvl=1, favorite_flg=0, eqip_item_id=0, eqip_item_id2=0, source='')
        uid = db.execute(f"INSERT INTO user_units ({','.join(template)}) VALUES ({','.join('?' * len(template))})",
                         tuple(template.values())).lastrowid
    save.with_suffix('.token.json').write_text(json.dumps([login, user, token, uid]))
    print(f'prepared {save}: user {user}, summoned Lava copy {uid}')
    return 0


def find_drop_info(reply):
    """The drop_info string, wherever MissionStart nests it."""
    for node in walk(reply):
        if isinstance(node, dict) and 'LE6JkUp7' in node:
            return node['LE6JkUp7']
    return ''


def drops(info: str) -> list:
    """(stage, slot, unit) of every unit drop this start rolled; stage counts from 1."""
    out = []
    for wave in info.split('@') if info else []:
        stage, units = wave.split('|')[:2]
        for entry in units.strip().split('-') if units.strip() else []:
            idx, _, unit = entry.split(':')[:3]
            out.append((int(stage), int(idx), int(unit)))
    return out


def walk(node):
    yield node
    if isinstance(node, dict):
        for value in node.values():
            yield from walk(value)
    elif isinstance(node, list):
        for value in node:
            yield from walk(value)


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('save', type=Path)
    r = sub.add_parser('run')
    r.add_argument('--token', type=Path, required=True)
    r.add_argument('--container', required=True)
    r.add_argument('--url', default='http://127.0.0.1:19982')
    r.add_argument('--runs', type=int, default=300)
    args = ap.parse_args()
    if args.cmd == 'prepare':
        return prepare(args.save)

    login, user, token, summoned = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    missions = archive()
    fails = []

    def check(label, ok, detail=''):
        print(f'{"PASS" if ok else "FAIL"} {label}{": " + str(detail) if detail != "" else ""}')
        if not ok:
            fails.append(label)

    def run(mission, units=''):
        started = call(url, user, login, token, *MISSION_START, start_body(mission))
        if 'b5PH6mZa' in started:
            raise SystemExit(f'MissionStart {mission} refused: {started["b5PH6mZa"]}')
        body = end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba']))
        body['rXvA1E5y'][0]['3MAT6quo'] = units
        ended = call(url, user, login, token, *MISSION_END, body)
        if 'b5PH6mZa' in ended:
            raise SystemExit(f'MissionEnd {mission} refused: {ended["b5PH6mZa"]}')
        return drops(find_drop_info(started)), ended

    def tally(mission, runs):
        seen = []
        for _ in range(runs):
            seen += run(mission)[0]
        return seen

    def monster(mission, stage, slot):
        return missions[mission]['stages'][stage - 1]['battle_monsters'][slot]

    # ---- 540: base form, minion, regular waves untouched -----------------------
    got = tally(540, args.runs)
    boss = [u for s, i, u in got if (s, i) == (9, 0)]
    check('540: the boss drops only its base form Mimir 50040', set(boss) <= {MIMIR} and boss, sorted(set(boss)))
    check('540: the minion Cait Sith (stage 9 slot 1) never drops', not [1 for s, i, _ in got if (s, i) == (9, 1)])
    strangers = sorted({(s, i, u) for s, i, u in got if s != 9 and u != monster(540, s, i)['unit_id']})
    check('540: a regular slot only ever drops its own unit, as before', not strangers, strangers)
    low = len(boss) / args.runs
    check('540: Mimir (3-star line) drops at about 20%', 0.12 <= low <= 0.28, f'{len(boss)}/{args.runs}')

    # ---- 961: regular waves stripped by #47 stay silent; Omni line rarity --------
    lava_runs = args.runs * 2
    got = tally(961, lava_runs)
    stripped = {(s + 1, i) for s, stage in enumerate(missions[961]['stages']) if not stage['is_boss']
                for i, m in enumerate(stage['battle_monsters']) if m.get('authored_capture')}
    check(f'961: none of the {len(stripped)} regular Fire God Lava / Drake Queen Aisha slots drops',
          not [1 for s, i, _ in got if (s, i) in stripped], sorted({(s, i) for s, i, _ in got}))
    boss = [u for s, i, u in got if (s, i) == (10, 0)]
    check('961: the boss drops only base Lava 10122', set(boss) <= {LAVA} and len(boss) == len(got),
          sorted({(s, i, u) for s, i, u in got}))
    omni = len(boss) / lava_runs
    check('961: an Omni line drops at about 2%, far rarer than a 3-star line',
          boss and omni <= 0.05 and omni < low / 3, f'{len(boss)}/{lava_runs} vs {low:.0%}')

    # ---- the Karma dungeon stays drop-free ---------------------------------------
    karma = {m: tally(m, 60) for m in KARMA_DUNGEON}
    check('Karma dungeon: no unit drops in 60 runs of each of its 7 battles', not any(karma.values()),
          {m: d for m, d in karma.items() if d})

    # ---- Merit: dropped vs summoned -------------------------------------------
    _, ended = run(961, f'{LAVA}:1:1')
    caught = [int(row['edy7fq3L']) for row in walk(ended)
              if isinstance(row, dict) and str(row.get('pn16CNah')) == str(LAVA) and 'edy7fq3L' in row]
    caught = max(caught) if caught else 0
    check('a reported Lava is added to the box', caught > 0 and caught != summoned, caught)

    def deliver(unit_id):
        reply = call(url, user, login, token, *DELIVER, {
            '1Q2pGAXE': [{'KT71m8Ae': '3', '3rhygS9K': '8', 'M7SXoc31': ''}],
            'dE79UwNE': [{'Rs7bCE3t': '0', 'idfCDG70': '0', 'edy7fq3L': str(unit_id), 'n6E8iMf3': ''}]})
        log = subprocess.run(['docker', 'logs', args.container], capture_output=True, text=True)
        paid = re.findall(rf'AchievementDeliver: {user} delivered 1 to flow 8 for (\d+) Merit',
                          log.stdout + log.stderr)
        return reply, int(paid[-1]) if paid else None

    _, full = deliver(summoned)
    _, dropped = deliver(caught)
    expected = max(1, (full or 0) * 25 // 100)
    check('merit: the summoned copy pays its full price', bool(full), full)
    check('merit: the dropped copy pays 25% of it', dropped == expected,
          f'dropped {dropped}, summoned {full}, expected {expected}')

    print('ALL PASS' if not fails else f'{len(fails)} FAILED: {fails}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
