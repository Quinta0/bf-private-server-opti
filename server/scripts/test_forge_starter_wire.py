#!/usr/bin/env python3
"""Wire test: the Balance Forge starter pack (server/starter.json, the tutorial-completion presents).

    PYTHONPATH=<repo>/tools <repo>/.venv/bin/python \
        scripts/test_forge_starter_wire.py [--src wt/forge] [--port 9987] [--assets DIR] [--keep]

Boots one throwaway server (container bf-gifts-starter, save in volume bf-gifts-starter-data, a
scratch balance root) and never restarts it: every step rewrites server/starter.json, and
forgeStatus.php (which looks at the file on every request) must report the new bytes.

  defaults  no file: forgeStatus says so and a fresh account that finishes the tutorial
            (TutorialUpdate with end flag 1) gets 5 gems, 10,000 zel and 3 potions; a second
            TutorialUpdate adds nothing, and one with end flag 0 gives nothing
  pack      a file with gems, zel, karma, two item lines and two units: a fresh account gets
            exactly those presents (one per currency and item line, one per unit)
  reset     a player whose presents are gone (what an account reset leaves) gets the pack again
  broken    unknown key, missing key, wrong format, gems past 9,999, a fraction, an item twice,
            an unknown item or unit, an empty pack, not JSON: each is refused with a reason in
            forgeStatus, and a fresh account gets the defaults
  removed   the file deleted: back to the defaults, "present" false

Removes its container and volume (unless --keep).
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

from forge_wire_common import Server, gme_call, sha, wire_error, write_atomic
from test_fair_summon import Database, Player

CONTAINER, VOLUME = 'bf-gifts-starter', 'bf-gifts-starter-data'
TUTORIAL_UPDATE = ('T1nCVvx4', '7hqzmR3T')
GIFT_TEXT = 'Tutorial Complete Bonus'
DEFAULT_ROWS = sorted([(8, '', 5), (3, '', 10000), (4, '20000', 3)])
DEFAULT_PACK = {'gems': 5, 'zel': 10000, 'karma': 0, 'items': [{'id': 20000, 'count': 3}], 'units': []}


def pack(**over) -> dict:
    body = {'format': 1, 'gems': 25, 'zel': 30000, 'karma': 500,
            'items': [{'id': 20000, 'count': 2}, {'id': 10001, 'count': 4}],
            'units': [{'id': 10011}, {'id': 50011}]}
    return {**body, **over}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9987)
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive' / 'assets')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    src = args.src.resolve()
    failures = 0

    def check(label: str, ok, detail='') -> bool:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  -- ' + str(detail)[:400]}", flush=True)
        return bool(ok)

    work = Path(tempfile.mkdtemp(prefix='forge-starter-'))
    server = Server(src, work, args.port, args.assets, CONTAINER, VOLUME)
    path = work / 'balance' / 'server' / 'starter.json'
    try:
        server.start()
        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')

        def finish(p: Player, end: str = '1') -> str | None:
            login = [dict(p.login[0], **{'9sQM2XcN': '12', 'sv6BEI8X': end})]
            return wire_error(gme_call(server.url, *TUTORIAL_UPDATE, {'6FrKacq7': [{'Kn51uR4Y': 'starter'}],
                                                                       'IKqx1Cn9': login}))

        def presents(p: Player) -> list:
            return sorted((int(t), str(i), int(n)) for t, i, n, d in db.rows(
                'SELECT present_type, target_id, target_cnt, description FROM user_presents '
                'WHERE user_id = ? AND receipt_type = 1', p.user_id) if d == GIFT_TEXT)

        def starter(data: bytes | None = None, gone: bool = False) -> dict:
            """forgeStatus's starter part, once it reports `data` (or no file): the bind mount's
            attribute cache can hide a rewrite for a moment, as rules.json's test also allows."""
            deadline = time.time() + 20
            while True:
                st = server.status().get('starter') or {}
                if (data is None and not gone) or time.time() > deadline:
                    return st
                if (gone and st.get('present') is False) or (data is not None and st.get('sha256') == sha(data)):
                    return st
                time.sleep(0.5)

        # -- defaults -------------------------------------------------------------------------
        st = starter()
        check('no file: forgeStatus reports the starter engine, no file, the defaults in force',
              st.get('engine') is True and st.get('present') is False and st.get('applied') is False
              and st.get('error') is None and st.get('pack') == DEFAULT_PACK, st)
        a = Player(server.url, db)
        check('a fresh account has no completion presents before the tutorial ends', presents(a) == [], presents(a))
        check('TutorialUpdate with end flag 0 gives nothing', finish(a, '0') is None and presents(a) == [], presents(a))
        check('TutorialUpdate with end flag 1 succeeds', finish(a) is None)
        check('defaults: 5 gems, 10,000 zel and 3 potions', presents(a) == DEFAULT_ROWS, presents(a))
        finish(a)
        check('a second TutorialUpdate adds nothing', presents(a) == DEFAULT_ROWS, presents(a))

        # -- a pack --------------------------------------------------------------------------------
        data = json.dumps(pack(), indent=1).encode()
        write_atomic(path, data)
        st = starter(data)
        check('a good file: forgeStatus has its sha256, applied, no error',
              st.get('present') is True and st.get('sha256') == sha(data) and st.get('applied') is True
              and st.get('error') is None, st)
        check('forgeStatus lists the pack in force',
              st.get('pack') == {k: v for k, v in pack().items() if k != 'format'}, st.get('pack'))
        b = Player(server.url, db)
        finish(b)
        want = sorted([(8, '', 25), (3, '', 30000), (11, '', 500), (4, '20000', 2), (4, '10001', 4),
                       (6, '10011', 1), (6, '50011', 1)])
        check('a fresh account gets exactly the pack: one present per part, one per unit', presents(b) == want,
              presents(b))
        finish(b)
        check('still once', presents(b) == want, presents(b))
        check('the account made before the change keeps what it had', presents(a) == DEFAULT_ROWS, presents(a))

        # -- reset ---------------------------------------------------------------------------------
        db.rows('DELETE FROM user_presents WHERE user_id = ?', a.user_id)
        finish(a)
        check('presents gone (as after a reset): the tutorial gives the current pack again', presents(a) == want,
              presents(a))

        # -- broken files --------------------------------------------------------------------------
        broken = {
            'an unknown key': (json.dumps(pack(bonus=1)), 'bonus'),
            'a missing key': (json.dumps({k: v for k, v in pack().items() if k != 'units'}), 'units'),
            'format 2': (json.dumps(pack(format=2)), 'format'),
            'gems past 9,999': (json.dumps(pack(gems=10000)), '9,999'),
            'negative zel': (json.dumps(pack(zel=-1)), 'zel'),
            'a fraction': (json.dumps(pack(karma=1.5)), 'read'),
            'an item twice': (json.dumps(pack(items=[{'id': 20000, 'count': 1}, {'id': 20000, 'count': 2}])), 'twice'),
            'a count of 0': (json.dumps(pack(items=[{'id': 20000, 'count': 0}])), 'count'),
            'an unknown item': (json.dumps(pack(items=[{'id': 99999999, 'count': 1}])), '99999999'),
            'an unknown unit': (json.dumps(pack(units=[{'id': 99999999}])), '99999999'),
            'eleven units': (json.dumps(pack(units=[{'id': 10011}] * 11)), 'at most'),
            'an empty pack': (json.dumps(pack(gems=0, zel=0, karma=0, items=[], units=[])), 'nothing'),
            'not JSON': ('{"format": 1,', 'read'),
        }
        for label, (text, word) in broken.items():
            write_atomic(path, text.encode())
            st = starter(text.encode())
            check(f'{label}: refused, the defaults in force, the reason names it',
                  st.get('present') is True and st.get('applied') is False and st.get('sha256') == sha(text.encode())
                  and word in (st.get('error') or '') and st.get('pack') == DEFAULT_PACK, st)
        c = Player(server.url, db)
        finish(c)
        check('after a refused file a fresh account gets the defaults', presents(c) == DEFAULT_ROWS, presents(c))

        # -- removed -------------------------------------------------------------------------------
        write_atomic(path, data)
        check('a good file again is taken', starter(data).get('applied') is True)
        path.unlink()
        st = starter(gone=True)
        check('file removed: no file, the defaults in force', st.get('present') is False and st.get('applied') is False
              and st.get('pack') == DEFAULT_PACK and st.get('error') is None, st)
        logs = server_logs(CONTAINER)
        check('the server logs each refusal', 'starter.json REJECTED' in logs, logs[-500:])
    finally:
        if not args.keep:
            server.remove()
            shutil.rmtree(work, ignore_errors=True)
    print(f'{failures} failure(s)')
    return 1 if failures else 0


def server_logs(container: str) -> str:
    import subprocess
    done = subprocess.run(['docker', 'logs', container], capture_output=True, text=True, timeout=60)
    return done.stdout + done.stderr


if __name__ == '__main__':
    sys.exit(main())
