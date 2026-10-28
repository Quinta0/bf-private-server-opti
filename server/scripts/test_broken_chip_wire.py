"""Glitch in the Heart's Beyond Machine first clear, wire test against a running server.

    python3 -I scripts/test_broken_chip_wire.py [--url http://127.0.0.1:9994]
        [--container bf-chip-sql] [--db /data/gme.sqlite] [--tools <root>/tools]

Checks (ticket #141):
  Beyond Machine (8301397)  the first clear presents a Gem, Excelsior Flux and Lead
                            x10 and one Broken Chip (802007), the piece Excelsior
                            Chip MK-1 needs; a replay presents nothing.
Runs on the Mac with the guest-account and docker-exec SQL helpers of the root
repo's tools/test_fair_summon.py; --container may be a python sidecar on the
server's /data volume (the prod image has no python3).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
BEYOND_MACHINE = 8301397
PREREQUISITES = ('8301393', '8301396')   # F_MISSION_MST HSRhkf70 of Beyond Machine
EXPECTED = sorted([(8, '', 1), (4, '802005', 10), (4, '802006', 10), (4, '802007', 1)])


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9994')
    ap.add_argument('--container', default='bf-chip-sql')
    ap.add_argument('--db', default='/data/gme.sqlite', help='save path inside the container')
    ap.add_argument('--tools')
    args = ap.parse_args()
    sys.path.insert(0, str(find_tools(args.tools)))
    from test_fair_summon import Database, Player, gme_call, wire_error  # noqa: E402

    failures = 0

    def check(label, ok, detail=''):
        nonlocal failures
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  ' + str(detail)}")
        failures += not ok

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    for mission in PREREQUISITES:
        db.rows('INSERT INTO user_campaign_missions (user_id, mission_id, state) VALUES (?, ?, 2)'
                ' ON CONFLICT (user_id, mission_id) DO UPDATE SET state = 2', player.user_id, mission)

    def clear(mission):
        player.set(energy=500, energy_full_ts=0)
        started = gme_call(args.url, *START, {'IKqx1Cn9': player.login, '9Q1Lq5FS': [{
            'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
            'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        if wire_error(started) is not None:
            return wire_error(started)
        ended = gme_call(args.url, *END, {'IKqx1Cn9': player.login,
                                          'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'],
                                                        'j3g5P4cq': '2'}],
                                          'rXvA1E5y': [{'Najhr8m6': '0', 'HTVh8a65': '0', '3MAT6quo': '',
                                                        '4T0Q2Bh5': ''}]})
        return wire_error(ended)

    def presents():
        return [tuple(p) for p in db.rows('SELECT present_type, target_id, target_cnt FROM user_presents'
                                          ' WHERE user_id = ? AND description = ? ORDER BY present_id',
                                          player.user_id, 'First clear reward')]

    before = presents()
    error = clear(BEYOND_MACHINE)
    new = sorted(presents()[len(before):])
    check('Beyond Machine: first clear presents a Gem, Flux x10, Lead x10 and the Broken Chip',
          error is None and new == EXPECTED, (error, new))
    before = presents()
    error = clear(BEYOND_MACHINE)
    check('a replay presents nothing', error is None and presents() == before, (error, presents()[len(before):]))

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
