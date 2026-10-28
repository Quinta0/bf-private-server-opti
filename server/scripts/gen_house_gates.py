"""House gates: the owner's price for the main gem gate and the 7* gate.

python3 -I scripts/gen_house_gates.py          # rewrite gate 11960's price and pool, gate 15850 and rail tile 5
python3 -I scripts/gen_house_gates.py --check  # exit 1 if the data differs
python3 -I scripts/gen_house_gates.py --rates  # print the 7* pool
python3 -I scripts/gen_house_gates.py --verify --assets "$HOME/bf-archive/assets"
                                               # field-check every stage of every 7* line (check_collab_units)

GATE 11960 "Rare Summon: All Star" -- the big "Shiny" tile (rail tile 1), the
fair-summoning gem gate (docs/features/FAIR_SUMMON.md).  The owner, 2026-10-18:
its pool is 3*-5* only, so it costs ALL_STAR_GEMS.  Written into the row's
need_gems (what GachaAction charges and every summon screen prints, see
gen_collab_gacha.py); its detail text is gate_info.info_text() of its pool.
The owner, 2026-10-21, on the players' forum thread "Part 1 of original units
that shouldn't be in banner": the pool drops every unit whose guide number
(unit_mst XuJL4pc5) falls in ALL_STAR_DROPPED, 40 old 3* and 4* units (the
bandit bosses, the summoned beasts, the Vortex heroes, ...).  The owner,
2026-10-23: it also drops the lines the game gives away (free_units.py),
such as Karl and Grahdens.

GATE 15850 "Legacy Summon" -- the original end-of-service 7* gate (2018-2022,
4 gems, "7* units from Kulyuk to Ewan"), reused as the house 7* gate at
LEGACY_GEMS.  Its pool is every line RELEASED as a 7* (summon stage rarity 7,
the units that evolve to Omni), kind 0, at its summon stage, all equally
likely.  Left out: collaboration units, which have their own gates
(gen_collab_gacha.FRANCHISES), and the lines in LEFT_OUT, which fail the field
check (scripts/check_collab_units.py's Checker: a battle effect file no archive
ships, or no deploy/archive/unit.json record), and the lines the game gives
away (free_units.py: Garrett, Mel, Merlin).  Its own art prints "4 per
Summon", so the gate shows the plain Rare Summon background instead.

RAIL TILE 5 opens gate 15850 under the original "Veteran Summon" banner, the
end-of-service tile that held the Legacy gate.  gme::isOnSummonRail reads the
tile list, so the gate can be pulled and nothing else changes in the server.

Run gen_collab_gacha.py first when both change: each script rewrites only its
own gates, rows and tile.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import free_units  # noqa: E402
import gen_collab_gacha as collab  # noqa: E402
from gate_info import caption_text, info_text  # noqa: E402

ALL_STAR = 11960
ALL_STAR_GEMS = 1
ALL_STAR_DROPPED = ((43, 110), (171, 220), (254, 286))   # guide numbers, inclusive
LEGACY = 15850
LEGACY_GEMS = 5     # owner, 2026-10-21: the Veteran banner is 5 gems
TILE_ID = '5'
TILE_ART = 'small_banner_02Veteran.png'    # "Veteran Summon", 600x150, no price printed
WEIGHT = collab.FAIR_WEIGHT[7]

# Lines released as 7* that fail the field check, by summon-stage id.
LEFT_OUT = {
    11056: 'Dragon Demon Barion: effects 5327BattleEffectMajinFire_s00* missing',
    30946: 'Nature Demon Mordlim: effects 5324BattleEffectMajinTree_s00* missing',
    820157: 'Egg Seeking Cotton: effects 820157/820158BattleEffect*AddR missing',
    830277: 'Commander Wannahon: Omni effect "83027BattleEffectPunchBack R.sam" missing',
    850277: 'Artificer Ceulfan: Omni effect 850278BattleEffectFinalSBBBackR missing',
    860356: 'Twilight Priestess Neferet: effects 820157BattleEffect*AddR missing',
    20816: 'JP-only unit: no deploy/archive/unit.json record',
    50876: 'JP-only unit: no deploy/archive/unit.json record, leader skill 10700 missing',
    50976: 'JP-only unit: no deploy/archive/unit.json record',
    51026: 'JP-only unit: no deploy/archive/unit.json record',
    60806: 'JP-only unit: no deploy/archive/unit.json record',
    60936: 'JP-only unit: no deploy/archive/unit.json record',
}


def legacy_pool():
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    names = {str(u['id']): u['name'] for u in collab.load(collab.UNIT_ARCHIVE)}
    line = collab.evolution_lines()
    collab_lines = {line(str(m))[0] for *_, members, _ in collab.FRANCHISES for m in members}
    collab_lines |= {line(str(m))[0] for m in collab.LEFT_OUT}
    left_out = {str(k) for k in LEFT_OUT} | set(free_units.free_lines(units, line))
    bases = {line(uid)[0] for uid, u in units.items() if u.get('7ofj5xa1') == '7'}
    pool = []
    for base in sorted(bases, key=int):
        u = units[base]
        if u.get('7ofj5xa1') != '7' or u.get('xP9oi5Z2') != '0' or base in collab_lines or base in left_out:
            continue
        if base not in names:
            sys.exit(f'{base} passes the filters but has no deploy/archive/unit.json record: add it to LEFT_OUT')
        pool.append({'unit_id': int(base), 'weight': WEIGHT, 'name': names[base]})
    return pool


def all_star_pool(pool, units):
    """Gate 11960's pool without the units whose guide number ALL_STAR_DROPPED names or whose line is free."""
    line = collab.evolution_lines()
    free = free_units.free_lines(units, line)

    def dropped(p):
        guide = int(units[str(p['unit_id'])].get('XuJL4pc5') or 0)
        return any(lo <= guide <= hi for lo, hi in ALL_STAR_DROPPED) or line(str(p['unit_id']))[0] in free
    return [p for p in pool if not dropped(p)]


def legacy_record(pool, units) -> dict:
    return {
        'id': LEGACY,
        'priority': 1,
        'background': collab.PLAIN_BG,
        'caption_msg': caption_text('Legacy Summon: 7★ Units', LEGACY, pool, units),
        'detail_msg': info_text(pool, units),
        'pool': pool,
    }


def gem_text(gems: int) -> str:
    return f'{gems} gem' if gems == 1 else f'{gems} gems'


def merged_files(pool):
    gacha = json.loads(collab.GACHA.read_text(encoding='utf-8'))
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    found = {g['id'] for g in gacha}
    if ALL_STAR not in found:
        sys.exit(f'gate {ALL_STAR} has no archive record')
    out = []
    for g in gacha:
        if g['id'] == LEGACY:
            continue          # re-added below; replaced in place when present
        if g['id'] == ALL_STAR:
            kept = all_star_pool(g['pool'], units)
            g = dict(g, caption_msg=caption_text('Rare Summon: All Star', ALL_STAR, kept, units),
                     detail_msg=info_text(kept, units), pool=kept)
        out.append(g)
    record = legacy_record(pool, units)
    spot = next((i for i, g in enumerate(gacha) if g['id'] == LEGACY), None)
    if spot is None:
        out.append(record)
    else:
        out.insert(sum(1 for g in gacha[:spot] if g['id'] != LEGACY), record)

    category = json.loads(collab.CATEGORY.read_text(encoding='utf-8'))
    tile = {'vx9uyQVQ': TILE_ID, 'In7lGGLn': TILE_ART, '2r4EoNt4': int(TILE_ID), '3rCmq58M': str(LEGACY),
            'qA7M9EjP': 0, 'SzV0Nps7': 2147483647}
    tiles = [t for t in category['IBs49NiH'] if t['vx9uyQVQ'] != TILE_ID] + [tile]
    category = {'IBs49NiH': sorted(tiles, key=lambda t: int(t['2r4EoNt4']))}

    gacha_mst = json.loads(collab.GACHA_MST.read_text(encoding='utf-8'))
    prices = {str(ALL_STAR): ALL_STAR_GEMS, str(LEGACY): LEGACY_GEMS}
    for row in gacha_mst[next(iter(gacha_mst))]:
        if row['7Ffmi96v'] in prices:
            if row['S1oz60Hc'] != '2':
                sys.exit(f'gate {row["7Ffmi96v"]} is not a type 2 gem gate')
            row['03UGMHxF'] = str(prices.pop(row['7Ffmi96v']))
    if prices:
        sys.exit(f'gacha_mst rows missing: {sorted(prices)}')
    return (json.dumps(out, indent=1, ensure_ascii=False),
            json.dumps(category, indent=2, ensure_ascii=False) + '\n',
            json.dumps(gacha_mst, indent=1, ensure_ascii=False) + '\n')


def verify(assets: Path, pool) -> int:
    import check_collab_units as cc
    check = cc.Checker(assets)
    failures = 0
    for p in pool:
        for uid in check.line(str(p['unit_id'])):
            fail, _ = check.stage(uid)
            for f in fail:
                print(f'FAIL {p["unit_id"]} {p["name"]}: {uid}: {f}')
            failures += bool(fail)
    print(f'{len(pool)} lines checked, {failures} stage(s) failing')
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if the files differ')
    ap.add_argument('--rates', action='store_true', help='print the 7* pool')
    ap.add_argument('--verify', action='store_true', help='field-check the 7* pool (needs --assets)')
    ap.add_argument('--assets', type=Path, help="the archive's assets/ folder (content/ and mst/)")
    args = ap.parse_args()
    pool = legacy_pool()
    if args.rates:
        for p in pool:
            print(f'{p["unit_id"]:>7} {p["name"]:<34} {100.0 / len(pool):5.2f}%')
        print(f'{len(pool)} units; left out: {len(LEFT_OUT)}')
        return
    if args.verify:
        if not args.assets:
            ap.error('--verify needs --assets')
        sys.exit(1 if verify(args.assets, pool) else 0)
    gacha_text, category_text, gacha_mst_text = merged_files(pool)
    if args.check:
        same = (collab.GACHA.read_text(encoding='utf-8') == gacha_text
                and collab.CATEGORY.read_text(encoding='utf-8') == category_text
                and collab.GACHA_MST.read_text(encoding='utf-8') == gacha_mst_text)
        print('up to date' if same else 'differs')
        sys.exit(0 if same else 1)
    collab.GACHA.write_text(gacha_text, encoding='utf-8')
    collab.CATEGORY.write_text(category_text, encoding='utf-8')
    collab.GACHA_MST.write_text(gacha_mst_text, encoding='utf-8')
    print(f'gate {ALL_STAR}: {gem_text(ALL_STAR_GEMS)}; gate {LEGACY}: {len(pool)} 7* units at '
          f'{LEGACY_GEMS} gems; rail tile {TILE_ID}')


if __name__ == '__main__':
    main()
