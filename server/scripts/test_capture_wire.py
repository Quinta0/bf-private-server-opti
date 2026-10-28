"""Wire test: the capture rules overlay, server/capture.json (ticket #155, forge/Capture.hpp).

python3 scripts/test_capture_wire.py prepare SAVE            # while the server is stopped
python3 scripts/test_capture_wire.py run --token FILE --balance DIR --container NAME [--url URL]

`prepare` is test_boss_drops_wire's (borrows the save's highest-level account, opens every
quest, tops up its energy).  `run` talks to a server built from this branch whose balance root
is DIR on this machine; it writes DIR/server/capture.json between starts (the server re-reads it
on the next MissionStart) and removes it at the end.  Every start ends as a clear.  Needs
pycryptodome (.venv/bin/python).

Quest 540 Mischief and Malice, read from MissionStart's drop_info (LE6JkUp7):
  default   no file: a regular-wave unit R still drops (the archive's 40% floor);
  rate      R and the boss's base form Mimir (50040) at 100: both drop on every start;
  off       both "enabled": false: neither ever drops;
  quest     R off for every quest but 100 for quest 540: the quest's rule wins;
  drawn     the boss is drawn as Great Sage Mimir (50042) and drops its base form Mimir:
            "enabled": true at 100 on 50042 gives 50042 on every start and Mimir never;
            a rate alone or "enabled": false on 50042 leaves the Mimir drop as it was;
  chosen    a hand-authored 540 (server/missions/540.json) whose boss joins as Light Lord Mimir
            (50041) at level 12, with "enabled": true on the drawn 50042: exactly 50041 at 12;
  on        C, a unit of 540 that never drops today, "enabled": true at 100: drops every start;
  rejected  a file with a rate of 150 is refused whole: the last good rules (C on) stay;
  gone      the file removed: C never drops again.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, 'tools')
from test_boss_drops_wire import MIMIR, archive, drops, find_drop_info, prepare  # noqa: E402
from test_captured_missions import MISSION_END, MISSION_START, end_body, start_body  # noqa: E402
from test_vortex_tiles import call  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
QUEST = 540
RUNS = 30
DRAWN = 50042   # Great Sage Mimir, quest 540's boss, whose capture is its base form Mimir
CHOSEN, CHOSEN_LV = 50041, 12   # Light Lord Mimir: the form a mission overlay names as the boss's capture form


def units_in_archive() -> set[int]:
    return {int(u['id']) for u in json.loads((ROOT / 'deploy/archive/unit.json').read_text())}


def pick(record: dict, known: set[int]):
    """R: a regular-wave unit with the floor's capture; C: a unit there that never drops, the
    weakest authored no-capture monster (a minion: the boss itself drops its base form)."""
    regular, minions = None, []
    for stage in record['stages']:
        for m in stage['battle_monsters']:
            unit = m.get('unit_id') or 0
            if unit not in known or unit == MIMIR:
                continue
            if not m.get('authored_capture') and not stage.get('is_boss') and regular is None:
                regular = unit
            if m.get('authored_capture') and not m.get('unit_drop_chance'):
                minions.append((m.get('hp') or 0, unit))
    return regular, min(minions)[1] if minions else None


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('save', type=Path)
    r = sub.add_parser('run')
    r.add_argument('--token', type=Path, required=True)
    r.add_argument('--balance', type=Path, required=True)
    r.add_argument('--container', required=True)
    r.add_argument('--url', default='http://127.0.0.1:19982')
    args = ap.parse_args()
    if args.cmd == 'prepare':
        return prepare(args.save)

    login, user, token, _ = json.loads(args.token.read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    rules_file = args.balance / 'server' / 'capture.json'
    rules_file.parent.mkdir(parents=True, exist_ok=True)
    overlay = args.balance / 'server' / 'missions' / f'{QUEST}.json'
    regular, never = pick(archive()[QUEST], units_in_archive())
    print(f'quest {QUEST}: regular unit R={regular}, never-dropping unit C={never}, boss base Mimir={MIMIR}')
    fails = []

    def check(label, ok, detail=''):
        print(f'{"PASS" if ok else "FAIL"} {label}{": " + str(detail) if detail != "" else ""}')
        if not ok:
            fails.append(label)

    def rules(body):
        if body is None:
            rules_file.unlink(missing_ok=True)
        else:
            rules_file.write_text(body if isinstance(body, str) else json.dumps(body))
        # Docker Desktop's file sharing shows a changed file's size and time to the container
        # a moment late; on the VPS the overlay writer and the container share one disk.
        time.sleep(2)

    def starts(n=RUNS, levels=False):
        """The dropped units (with levels: (unit, level)) of each of n starts, each ended as a clear."""
        out = []
        for _ in range(n):
            started = call(url, user, login, token, *MISSION_START, start_body(QUEST))
            if 'b5PH6mZa' in started:
                raise SystemExit(f'MissionStart {QUEST} refused: {started["b5PH6mZa"]}')
            info = find_drop_info(started)
            out.append([(int(e.split(':')[2]), int(e.split(':')[3])) for w in info.split('@') if '|' in w
                         for e in w.split('|')[1].strip().split('-') if e] if levels
                        else [u for _, _, u in drops(info)])
            ended = call(url, user, login, token, *MISSION_END, end_body(int(started['Kz7qfSs5'][0]['k9cxD7Ba'])))
            if 'b5PH6mZa' in ended:
                raise SystemExit(f'MissionEnd {QUEST} refused: {ended["b5PH6mZa"]}')
        return out

    def every(runs, unit):
        return sum(unit in r for r in runs)

    if regular is None or never is None:
        raise SystemExit(f'quest {QUEST} no longer has the units this test needs (R={regular}, C={never})')
    try:
        rules(None)
        runs = starts()
        check('default: R still drops without a file', every(runs, regular) > 0, every(runs, regular))
        check('default: C never drops', every(runs, never) == 0, every(runs, never))

        rules({'units': {str(regular): {'rate': 100}, str(MIMIR): {'rate': 100}}})
        runs = starts()
        check('rate: R drops on every start', every(runs, regular) == RUNS, every(runs, regular))
        check('rate: Mimir drops on every start', every(runs, MIMIR) == RUNS, every(runs, MIMIR))

        rules({'units': {str(regular): {'enabled': False}, str(MIMIR): {'enabled': False}}})
        runs = starts()
        check('off: R never drops', every(runs, regular) == 0, every(runs, regular))
        check('off: Mimir never drops', every(runs, MIMIR) == 0, every(runs, MIMIR))

        rules({'units': {str(regular): {'enabled': False}}, 'missions': {str(QUEST): {str(regular): {'rate': 100}}}})
        runs = starts(10)
        check("quest: the quest's rule wins", every(runs, regular) == 10, every(runs, regular))

        rules({'units': {str(DRAWN): {'enabled': True, 'rate': 100}}})
        runs = starts(10)
        check('drawn: the boss itself drops on every start', every(runs, DRAWN) == 10, every(runs, DRAWN))
        check('drawn: its base form Mimir no longer drops', every(runs, MIMIR) == 0, every(runs, MIMIR))

        record = json.loads(json.dumps(archive()[QUEST]))
        for stage in record['stages']:
            for m in stage['battle_monsters']:
                if stage.get('is_boss') and m.get('unit_id') == DRAWN:
                    m.update(unit_drop_id=CHOSEN, unit_drop_level=CHOSEN_LV, unit_drop_chance=100,
                             authored_capture=True)
        overlay.parent.mkdir(parents=True, exist_ok=True)
        overlay.write_text(json.dumps(record))
        rules({'units': {str(DRAWN): {'enabled': True, 'rate': 100}}})
        time.sleep(3)   # the overlay folder is polled every 2 s
        runs = starts(10, levels=True)
        # (A regular-wave Light Lord Mimir keeps its own floor capture at level 1.)
        check('chosen: the boss joins as the chosen form at its level on every start',
              all((CHOSEN, CHOSEN_LV) in r for r in runs), runs)
        check('chosen: the drawn form never replaces it', every([[u for u, _ in r] for r in runs], DRAWN) == 0)
        overlay.unlink()
        time.sleep(3)

        rules({'units': {str(DRAWN): {'rate': 100}}})
        runs = starts()
        check('drawn rate only: the boss never drops', every(runs, DRAWN) == 0, every(runs, DRAWN))
        check('drawn rate only: Mimir still drops', every(runs, MIMIR) > 0, every(runs, MIMIR))

        rules({'units': {str(DRAWN): {'enabled': False}, str(MIMIR): {'rate': 100}}})
        runs = starts(10)
        check('drawn off: Mimir keeps its own rule', every(runs, MIMIR) == 10, every(runs, MIMIR))

        rules({'units': {str(never): {'enabled': True, 'rate': 100}}})
        runs = starts(10)
        check('on: C drops on every start', every(runs, never) == 10, every(runs, never))

        rules({'units': {str(never): {'enabled': True, 'rate': 150}}})
        runs = starts(10)
        check('rejected: the last good rules stay', every(runs, never) == 10, every(runs, never))
        log = subprocess.run(['docker', 'logs', args.container], capture_output=True, text=True)
        check('rejected: the log says why', 'capture.json REJECTED' in log.stdout + log.stderr)

        rules(None)
        runs = starts()
        check('gone: C never drops without a file', every(runs, never) == 0, every(runs, never))
    finally:
        overlay.unlink(missing_ok=True)
        rules(None)
    print(f'\n{len(fails)} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
