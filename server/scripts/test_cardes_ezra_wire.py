"""Trial No. 004 and Battle No. X2 first clears, wire test against a running server.

    python3 -I scripts/test_cardes_ezra_wire.py [--url http://127.0.0.1:9994]
        [--container bf-ce-sql] [--db /data/gme.sqlite] [--tools <root>/tools]

The server needs deploy/balance-overlays/trials/2000003.json in its balance
folder's server/missions.  Checks:
  Trial No. 004 (2000003)  two fights, Mad God Zebra alone and then Cardes the
                           Malevolent alone; the first clear presents a Gem and
                           Cardes (60425), a replay presents nothing.
  Battle No. X2 (82100002) the first clear presents a Gem and Ezra (850328).
  Trial No. X3 (8360041)   Temptress Xie'Jing is drawn on Demonic Witch Xie'Jing
                           (860158), not Brave Knight Karl; the first clear
                           presents a Gem and Temptress Xie'Jing (860156).
The server also needs deploy/balance-overlays/trials/8360041.json.
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
TRIAL = 2000003
X2 = 82100002
X3 = 8360041
PREREQUISITES = ('666', '2000002', '82100001',    # F_MISSION_MST HSRhkf70 of the three
                 '2000000', '8360021', '8300031')
ZEBRA, CARDES, EZRA = 63650, 1600, '850328'
XIE_JING = 87600400
GEM = (8, '', 1)


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9994')
    ap.add_argument('--container', default='bf-ce-sql')
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

    def start(mission):
        player.set(energy=500, energy_full_ts=0)
        return gme_call(args.url, *START, {'IKqx1Cn9': player.login, '9Q1Lq5FS': [{
            'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
            'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}]})

    def clear(mission):
        started = start(mission)
        if wire_error(started) is not None:
            return started, wire_error(started)
        ended = gme_call(args.url, *END, {'IKqx1Cn9': player.login,
                                          'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'],
                                                        'j3g5P4cq': '2'}],
                                          'rXvA1E5y': [{'Najhr8m6': '0', 'HTVh8a65': '0', '3MAT6quo': '',
                                                        '4T0Q2Bh5': ''}]})
        return started, wire_error(ended)

    def presents():
        return [tuple(p) for p in db.rows('SELECT present_type, target_id, target_cnt FROM user_presents'
                                          ' WHERE user_id = ? AND description = ? ORDER BY present_id',
                                          player.user_id, 'First clear reward')]

    # --- Trial No. 004: two fights, Cardes as the first-clear reward ---------------
    before = presents()
    started, error = clear(TRIAL)
    check('Trial No. 004: MissionStart and the first clear succeed', error is None, error)
    battles = [b['etM5TCb9'] for b in started.get('pj41dy9g') or []]   # each battle's boss flag
    placed = [int(g['o49dYfpH']) for g in started.get('75t0sx9z') or []]
    units = {int(m['o49dYfpH']): m.get('pn16CNah') for m in started.get('U0v5IeJo') or []}
    check('two battles, the second the boss battle', battles == ['0', '1'], battles)
    check('Mad God Zebra (60234) fights first and alone, then Cardes the Malevolent (60425)',
          placed == [ZEBRA, CARDES] and units.get(ZEBRA) == '60234' and units.get(CARDES) == '60425',
          (placed, units))
    new = sorted(presents()[len(before):])
    check('first clear presents a Gem and Cardes the Malevolent', new == sorted([GEM, (6, '60425', 1)]), new)
    before = presents()
    _, error = clear(TRIAL)
    check('a replay presents nothing', error is None and presents() == before, (error, presents()[len(before):]))

    # --- Battle No. X2: Ezra as the first-clear reward ------------------------------
    before = presents()
    _, error = clear(X2)
    new = sorted(presents()[len(before):])
    check('Battle No. X2: first clear presents a Gem and Ezra',
          error is None and new == sorted([GEM, (6, EZRA, 1)]), (error, new))

    # --- Trial No. X3: Xie'Jing drawn as Xie'Jing, and given on the first clear --------
    before = presents()
    started, error = clear(X3)
    placed = [int(g['o49dYfpH']) for g in started.get('75t0sx9z') or []]
    units = {int(m['o49dYfpH']): m.get('pn16CNah') for m in started.get('U0v5IeJo') or []}
    check("Trial No. X3: Temptress Xie'Jing is drawn on Demonic Witch Xie'Jing (860158), not Karl",
          error is None and placed == [XIE_JING] and units.get(XIE_JING) == '860158', (error, placed, units))
    new = sorted(presents()[len(before):])
    check("Trial No. X3: first clear presents a Gem and Temptress Xie'Jing",
          new == sorted([GEM, (6, '860156', 1)]), new)

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
