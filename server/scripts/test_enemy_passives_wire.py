"""Wire test: an enemy's passive effects from the battle's start, and who acts first (ticket #162, #163).

python3 scripts/test_enemy_passives_wire.py prepare SAVE            # while the server is stopped
python3 scripts/test_enemy_passives_wire.py run --token FILE --balance DIR [--url URL]

`prepare` is test_boss_drops_wire's.  `run` writes a hand-built battle for quest 540 into
DIR/server/missions/540.json (the server rescans that folder live), starts the quest and reads
MissionStart's reply, then removes the file.
  passives  the boss's passive_procs "1@8" / passive_params "900,0,0,0,0@30" reach the client as
            MonsterMst passive_process_id (hjAy9St3) and passive_process_param (2Smu5Mtq);
  first     wave 1 at first_attack_rate 100 reaches the client as BattleGroupMst 5aetPz3C "100",
            the other waves "0" (#163: the field Ellie's Folly uses, here from a Forge battle);
  refused   a battle with 2 passive ids but 1 number group is refused whole: the last good one stays;
  gone      the file removed: the game's own battle, no passives.
"""
import argparse
import copy
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, 'tools')
from test_boss_drops_wire import archive, prepare, walk  # noqa: E402
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402

QUEST = 540
IDS, PARAMS = '1@8', '900,0,0,0,0@30'


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('save', type=Path)
    r = sub.add_parser('run')
    r.add_argument('--token', type=Path, required=True)
    r.add_argument('--balance', type=Path, required=True)
    r.add_argument('--url', default='http://127.0.0.1:19982')
    args = ap.parse_args()
    if args.cmd == 'prepare':
        return prepare(args.save)

    login, user, token, _ = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    folder = args.balance / 'server' / 'missions'
    folder.mkdir(parents=True, exist_ok=True)
    battle = folder / f'{QUEST}.json'
    fails = []

    def check(label, ok, detail=''):
        print(f'{"PASS" if ok else "FAIL"} {label}{": " + str(detail) if detail != "" else ""}')
        if not ok:
            fails.append(label)

    def start():
        started = call(url, user, login, token, *MISSION_START, start_body(QUEST))
        if 'b5PH6mZa' in started:
            raise SystemExit(f'MissionStart {QUEST} refused: {started["b5PH6mZa"]}')
        call(url, user, login, token, *MISSION_END, end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba'])))
        rows = [n for n in walk(started) if isinstance(n, dict)]
        passives = [(n['hjAy9St3'], n['2Smu5Mtq']) for n in rows if '2Smu5Mtq' in n and n.get('hjAy9St3')]
        firsts = [str(n['5aetPz3C']) for n in rows if '5aetPz3C' in n]
        return passives, firsts

    def write(record):
        battle.write_text(json.dumps(record))
        time.sleep(8)                     # the server rescans the folder every few seconds

    base = copy.deepcopy(archive()[QUEST])
    good = copy.deepcopy(base)
    good['stages'][0]['first_attack_rate'] = 100
    boss = max(good['stages'][-1]['battle_monsters'], key=lambda m: int(m['hp']))
    boss.update(passive_procs=IDS, passive_params=PARAMS)
    try:
        battle.unlink(missing_ok=True)
        time.sleep(8)
        passives, firsts = start()
        check('archive: no passives', passives == [], passives)
        write(good)
        passives, firsts = start()
        check('passives reach the client', (IDS, PARAMS) in passives, passives)
        check('wave 1 acts first, the rest do not', firsts[:1] == ['100'] and set(firsts[1:]) <= {'0'}, firsts)
        bad = copy.deepcopy(good)
        b = max(bad['stages'][-1]['battle_monsters'], key=lambda m: int(m['hp']))
        b['passive_params'] = '900,0,0,0,0'
        write(bad)
        passives, _ = start()
        check('refused: the last good battle stays', (IDS, PARAMS) in passives, passives)
        battle.unlink()
        time.sleep(8)
        passives, firsts = start()
        check('gone: the game\'s own battle again', passives == [] and set(firsts) <= {'0'}, (passives, firsts))
    finally:
        battle.unlink(missing_ok=True)
    print(f'\n{len(fails)} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
