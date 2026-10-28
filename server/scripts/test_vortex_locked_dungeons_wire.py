#!/usr/bin/env python3
"""Wire test: a locked Vortex dungeon waits for its quests; the Monday dungeons stay open (ticket #120).

    # server STOPPED, a TEST save:
    python3 -I scripts/test_vortex_locked_dungeons_wire.py --src . --prepare --save <save>/gme.sqlite
    # server running, on any day but a Japanese Monday:
    .venv/bin/python -I scripts/test_vortex_locked_dungeons_wire.py --src . --url http://127.0.0.1:9990

Glitch in the Heart (800139): Side: Terminus (8001393) lists Vektor's and
Galene's last quests (8301393, 8301396).  It used to be permitted anyway and
opened on an empty quest list.  Its first quest, Beyond Machine (8301397), lists
the same two; Vortex lists are read any-of (PermitPlace `satisfied`), so the
dungeon now opens with that quest, after either side.  Checks:
  1. with no Glitch clears: Vektor and Galene are permitted, Terminus is not;
     the Monday dungeons 100000/100050 and their quests are; Grand Quest's
     5000000 still is (its own collector);
  2. it stays held until Vektor ends; then the MissionEnd refresh permits
     Terminus and Beyond Machine together.

--prepare gives the save's first account a known token and 9,999 energy and
deletes its Glitch in the Heart clears (out/qa/vortex-locked/account.json).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_vortex_categories_wire import (DUNGEON, ERROR, MISSION, MISSION_END, MISSION_START, NUM, SERIAL,  # noqa: E402
                                         USER_INFO, Checks, call, end_body, permitted, start_body)

VEKTOR, TERMINUS, BEYOND_MACHINE = (8301391, 8301392, 8301393), 8001393, 8301397
MONDAY = {100000: (100000, 100001, 100002), 100050: (100050, 100051, 100052, 100053)}
GRAND_QUEST_DUNGEON = 5000000


def prepare(save: Path, account_file: Path) -> int:
    token = secrets.token_hex(16)
    with sqlite3.connect(save, timeout=30) as db:   # a TEST save, server stopped
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY rowid LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?',
                   (hashlib.sha256(token.encode()).hexdigest(), login))
        db.execute('UPDATE user_info SET energy=9999 WHERE id=?', (user,))
        db.execute('DELETE FROM user_campaign_missions WHERE user_id=? AND mission_id BETWEEN 8301391 AND 8301399',
                   (user,))
    account_file.parent.mkdir(parents=True, exist_ok=True)
    account_file.write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
    print(f'prepared user {user} in {save}: token in {account_file}')
    return 0


def clear(url: str, account: dict, mission: int, check: Checks) -> dict:
    start = call(url, account, *MISSION_START, start_body(mission))
    if not check(f'{mission} starts', ERROR not in start and NUM in start, json.dumps(start)[:300]):
        return {}
    end = call(url, account, *MISSION_END, end_body(int(start[NUM][0][SERIAL])))
    check(f'{mission} clears', ERROR not in end, json.dumps(end)[:300])
    return end


def run(url: str, account: dict) -> int:
    check = Checks()
    info = call(url, account, *USER_INFO, {})
    dungeons, missions = permitted(info, DUNGEON), permitted(info, MISSION)
    print('UserInfo, no Glitch in the Heart clears')
    check('Vektor and Galene are permitted', {8001391, 8001392} <= dungeons)
    check('Terminus is held back', TERMINUS not in dungeons)
    for dungeon, quests in MONDAY.items():
        check(f'Monday dungeon {dungeon} and its quests are permitted',
              dungeon in dungeons and set(quests) <= missions, str(sorted(set(quests) - missions)))
    check('Grand Quest dungeon 5000000 is still permitted', GRAND_QUEST_DUNGEON in dungeons)

    print('\nVektor')
    for mission in VEKTOR:
        end = clear(url, account, mission, check)
        if mission == VEKTOR[1]:
            check('Terminus is still held back before Vektor ends', TERMINUS not in permitted(end, DUNGEON))
    check('Terminus is permitted after Vektor', TERMINUS in permitted(end, DUNGEON))
    check('with Beyond Machine (8301397)', BEYOND_MACHINE in permitted(end, MISSION))

    print(f"\n{'FAILED' if check.failed else 'PASSED'} ({check.failed} failure(s))")
    return 1 if check.failed else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, default=Path('.'))
    ap.add_argument('--url', default='http://127.0.0.1:9990')
    ap.add_argument('--prepare', action='store_true')
    ap.add_argument('--save', type=Path)
    ap.add_argument('--account', type=Path)
    args = ap.parse_args()
    account_file = args.account or args.src / 'out/qa/vortex-locked/account.json'
    if args.prepare:
        return prepare(args.save, account_file)
    return run(f"{args.url.rstrip('/')}/bf/gme/action.php", json.loads(account_file.read_text()))


if __name__ == '__main__':
    sys.exit(main())
