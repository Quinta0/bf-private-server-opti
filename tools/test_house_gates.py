"""Wire test for the house gates (server scripts/gen_house_gates.py): gate 11960
"Rare Summon: All Star" at its house price, and gate 15850 "Legacy Summon", the
house 7* gate on rail tile 5.

Checks, against a running server and its save database:
  A. GachaList carries tile 1 (11960) and tile 5 (15850), and both GachaInfo
     rows with the archive's copy and the gacha_mst price.  The tile art and
     the backgrounds exist in the assets folder.
  B. 11960 charges its price per pull and gives a 3*-5* unit.
  C. 15850 charges its price per pull, gives only 7* units of its own pool,
     and over enough pulls every unit of the pool at least once (so each one
     has an archive record the server can build); a pull the player cannot
     afford is refused and charges nothing.

Reuses the fair-summon harness (tools/test_fair_summon.py): throwaway guest
accounts, save edits made inside the server's container.

Usage: python3 -I tools/test_house_gates.py --src wt/prices [--url http://127.0.0.1:9976]
Run it against a test server, never a save with real players.
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import (NOT_ENOUGH, WIRE_RULE, Database, DumpLogs, Player,  # noqa: E402
                              container_on_port, succeeded, wire_error)

ALL_STAR, LEGACY = 11960, 15850
ALL_STAR_TILE, LEGACY_TILE = '1', '5'
CHAPTER_DONE = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9976')
    ap.add_argument('--src', type=Path, required=True, help='server checkout the server runs from')
    ap.add_argument('--container', help="Docker container (default: the one publishing the --url port)")
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    deploy = args.src / 'deploy'
    container = container_on_port(args.url) if args.container is None else (args.container or None)
    db = Database(deploy / 'gme.sqlite', container, args.container_db)
    logs = DumpLogs(deploy / 'log')
    mst = {int(r['7Ffmi96v']): r for r in json.loads(
        (deploy / 'mst/gacha_mst.json').read_text(encoding='utf-8'))['5Y4GJeo3']}
    archive = {g['id']: g for g in json.loads((deploy / 'archive/gacha.json').read_text(encoding='utf-8'))}
    rarity = {u['id']: u['rarity'] for u in json.loads((deploy / 'archive/unit.json').read_text(encoding='utf-8'))}
    tiles = {t['vx9uyQVQ']: t for t in json.loads(
        (deploy / 'mst/gacha_category_mst.json').read_text(encoding='utf-8'))['IBs49NiH']}
    features = json.loads((deploy / 'system/features.json').read_text(encoding='utf-8'))
    max_pulls = 10 if int(features.get('multisummon', 0)) else 1
    content = set()
    for sub in ('gacha', 'banner'):
        content.update(p.name for p in (args.assets / 'content' / sub).glob('*'))

    # ---- A. the tiles and the gate rows -------------------------------------------------
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, gems=0, summon_tickets=0)
    listing = player.gacha_list()
    rail = {str(c.get('vx9uyQVQ')): str(c.get('3rCmq58M')) for c in listing.get('IBs49NiH', [])}
    for tile_id, gate in ((ALL_STAR_TILE, ALL_STAR), (LEGACY_TILE, LEGACY)):
        check(f'A: GachaList carries tile {tile_id} with gate {gate}', rail.get(tile_id) == str(gate), rail.get(tile_id))
        check(f'A: tile {tile_id} art {tiles[tile_id]["In7lGGLn"]} is in the assets',
              tiles[tile_id]['In7lGGLn'] in content)
    infos = {int(g['7Ffmi96v']): g for g in listing.get('1IR86sAv', [])}
    for gate in (ALL_STAR, LEGACY):
        info, record = infos.get(gate), archive.get(gate)
        ok = (info is not None and record is not None and info.get('3sdHQb69') == record['caption_msg']
              and str(info.get('03UGMHxF')) == mst[gate]['03UGMHxF'])
        check(f'A: gate {gate} {mst[gate]["4N27mkt1"]!r} is listed with its copy and '
              f'{mst[gate]["03UGMHxF"]}-gem price', ok,
              '' if ok else {k: info.get(k) for k in ('1Dg0vUX3', '03UGMHxF')} if info else 'not listed')
        if record and record.get('background'):
            check(f'A: gate {gate} background {record["background"]} is in the assets',
                  record['background'] in content)

    # ---- B. 11960 ------------------------------------------------------------------------
    cost = int(mst[ALL_STAR]['03UGMHxF'])
    player.set(gems=cost * 3, summon_tickets=0)
    before = player.unit_count()
    pull = player.pull(ALL_STAR, 1)
    got = [int(u['pn16CNah']) for u in pull.reply.get('qC2tJs4E', [])] if succeeded(pull.reply) else []
    check(f'B: one pull on {ALL_STAR} costs {cost} gem(s) and adds one unit',
          succeeded(pull.reply) and player.balance()['gems'] == cost * 2 and player.unit_count() == before + 1,
          wire_error(pull.reply) or player.balance())
    check(f'B: ...a 3*-5* unit of its pool', len(got) == 1 and got[0] in {p['unit_id'] for p in archive[ALL_STAR]['pool']}
          and 3 <= rarity.get(got[0], 0) <= 5, got)

    # ---- C. 15850, the 7* gate -------------------------------------------------------------
    pool = {p['unit_id'] for p in archive[LEGACY]['pool']}
    cost = int(mst[LEGACY]['03UGMHxF'])
    share = 1 / len(pool)
    need = 10
    while (1 - share) ** need > 1e-4:
        need += 10
    need = -(-need // max_pulls) * max_pulls
    player.set(gems=cost * need, summon_tickets=0)
    seen, bad, rolled, ok = Counter(), set(), 0, True
    before = player.unit_count()
    while rolled < need:
        n = min(max_pulls, need - rolled)
        pull = player.pull(LEGACY, n)
        if not succeeded(pull.reply):
            ok = False
            check(f'C: pulls on {LEGACY} succeed', False, wire_error(pull.reply))
            break
        for unit in pull.reply['qC2tJs4E']:
            uid = int(unit['pn16CNah'])
            seen[uid] += 1
            if uid not in pool or rarity.get(uid) != 7:
                bad.add(uid)
        rolled += n
    if ok:
        balance = player.balance()
        check(f'C: {rolled} pulls on {LEGACY} cost {cost} gems each and added {rolled} units',
              balance['gems'] == 0 and player.unit_count() - before == rolled,
              {'gems left': balance['gems'], 'units added': player.unit_count() - before})
        check(f'C: {LEGACY} gives only 7* units of its pool, and every one of them ({len(pool)})',
              not bad and set(seen) == pool,
              {'foreign': sorted(bad), 'never': sorted(pool - set(seen))} if bad or set(seen) != pool else '')
    player.set(gems=cost - 1, summon_tickets=0)
    units = player.unit_count()
    refused = player.pull(LEGACY, 1)
    got_err = wire_error(refused.reply)
    logged = logs.reason(refused.marker) if logs.available else None
    check(f'C: a pull on {LEGACY} with {cost - 1} gems is refused',
          got_err is not None and f'Error: "{WIRE_RULE}"' in got_err and (logged is None or logged == NOT_ENOUGH),
          f'wire={got_err!r} log={logged!r}')
    check('C: ...and charges and grants nothing', player.balance()['gems'] == cost - 1 and player.unit_count() == units)

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
