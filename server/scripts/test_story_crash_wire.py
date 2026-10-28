"""Sparkling Rainbow Sky (10144) and Truth and Falsehood (10165), wire test against a running server.

    python3 -I scripts/test_story_crash_wire.py [--url http://127.0.0.1:9997]
        [--container bf-story-sql] [--db /data/gme.sqlite] [--tools <root>/tools]
        [--scripts <decoded F_MISSION_SCRIPT_MST json>]

The client's mission script for 10144 swaps Goddess Mora 3150 into "????" 36451
once she drops below 50% HP (MonsterParty::changeMonster finds the target by id),
and the game closed there while MissionStart placed 3150 without describing 36451.
For both Lem quests: MissionStart succeeds, no monster it sends triggers a script
swap whose target it does not send, the boss is the expected one, and the clear
is accepted.  Runs on the Mac with the guest-account and docker-exec SQL helpers
of the root repo's tools/test_fair_summon.py; --container may be a python sidecar
on the server's /data volume (the prod image has no python3).
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))  # -I drops the script dir
from gen_boss_waves import script_swaps  # noqa: E402

START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
SCRIPTS = Path.home() / 'bf-archive/mst-decoded/3-mst-translated/F_MISSION_SCRIPT_MST_Ver288.json'
QUESTS = {10144: (3154, 'Goddess Mora'), 10165: (3151, 'Guile Demon Mora')}   # mission -> its boss


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9997')
    ap.add_argument('--container', default='bf-story-sql')
    ap.add_argument('--db', default='/data/gme.sqlite', help='save path inside the container')
    ap.add_argument('--tools')
    ap.add_argument('--scripts', type=Path, default=SCRIPTS)
    args = ap.parse_args()
    sys.path.insert(0, str(find_tools(args.tools)))
    from test_fair_summon import Database, Player, gme_call, wire_error  # noqa: E402

    swaps = script_swaps(json.loads(args.scripts.read_text(encoding='utf-8')))
    failures = 0

    def check(label, ok, detail=''):
        nonlocal failures
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  ' + str(detail)}")
        failures += not ok

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    # Story quests lock behind their whole path; this tests battle data, so every
    # other story quest counts as cleared.
    story = [str(r['id']) for r in json.loads((ROOT / 'deploy/archive/mission.json').read_text(encoding='utf-8'))
             if 0 < r['id'] < 100000 and r['id'] not in QUESTS]
    db.rows('INSERT INTO user_campaign_missions (user_id, mission_id, state) SELECT ?, value, 2 FROM json_each(?)'
            ' WHERE true ON CONFLICT (user_id, mission_id) DO UPDATE SET state = 2', player.user_id, json.dumps(story))

    for mission, (boss, name) in QUESTS.items():
        player.set(energy=500, energy_full_ts=0)
        started = gme_call(args.url, *START, {'IKqx1Cn9': player.login, '9Q1Lq5FS': [{
            'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
            'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        check(f'{mission}: MissionStart succeeds', wire_error(started) is None, wire_error(started))
        if wire_error(started) is not None:
            continue
        described = {int(m['o49dYfpH']) for m in started.get('U0v5IeJo') or []}
        unsafe = {t: sorted(targets - described) for t, targets in swaps.get(mission, {}).items()
                  if t in described and targets - described}
        check(f'{mission}: no script swap to a monster MissionStart does not send', not unsafe, unsafe)
        placed = [int(g['o49dYfpH']) for g in started.get('75t0sx9z') or []]
        names = {int(m['o49dYfpH']): m for m in started.get('U0v5IeJo') or []}
        check(f'{mission}: the boss wave holds {name} ({boss})',
              boss in placed and name in json.dumps(names.get(boss), ensure_ascii=False), (placed, names.get(boss)))
        ended = gme_call(args.url, *END, {'IKqx1Cn9': player.login,
                                          'Kz7qfSs5': [{'k9cxD7Ba': started['Kz7qfSs5'][0]['k9cxD7Ba'],
                                                        'j3g5P4cq': '2'}],
                                          'rXvA1E5y': [{'Najhr8m6': '0', 'HTVh8a65': '0', '3MAT6quo': '',
                                                        '4T0Q2Bh5': ''}]})
        check(f'{mission}: the clear is accepted', wire_error(ended) is None, wire_error(ended))

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
