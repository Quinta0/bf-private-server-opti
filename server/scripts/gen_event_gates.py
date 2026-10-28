"""Event gates: the Halloween Summon and the six element Divine Summons.

python3 -I scripts/gen_event_gates.py          # rewrite the gate records, rail tiles 6 and 7 and the prices
python3 -I scripts/gen_event_gates.py --check  # exit 1 if the data differs
python3 -I scripts/gen_event_gates.py --rates  # print each gate's pool and chances

The owner, 2026-10-19 (ticket #52): more banners.  Both tiles reopen
original 2015-2017 gates whose own art is still on the CDN, so name, type 2,
door, button and price (5 gems) are the game's.  The pools and the copy are
authored, with fair summoning's weights (gen_collab_gacha.FAIR_WEIGHT).

GATE 8020 "Limited Gate: Special Summon" -- the Halloween 2015 gate (min 4*,
rates up for Ciara, Semira, Lunaris, Signas, Alyut and Farlon), on RAIL TILE 6
under the 2019 "Halloween Summon" banner (Lydia, who is in the pool).  Its pool
is those six 4* lines and the Halloween 7* lines, each at its summon stage.
Its art prints "Rare Summon" and no price.  Seasonal: take tile 6 down after
Halloween by setting HALLOWEEN_TILE_ON to False and running the script.

GATES 13270-13330 "Divine Summon: Dark/Light/Thunder/Earth/Water/Fire" -- the
April 2017 element gates (min 5*, "100% <ELEMENT>"), paged by RAIL TILE 7.  Each
pool is every 5* line of gate 11960 and every 7* line of gate 15850 (the 7*
gate's own filter, gen_house_gates.legacy_pool) of that element, so every unit
in them is one fair summoning already gives.  Their art prints "5 per Summon"
and a bonus Elgif every 2 summons; the server gives no bonus, so the copy says so.

Guest-only lines (no leader skill at the summon stage, such as the guest
Adjutant Mel 51176, ticket #48) are left out of every pool here, and
so are the lines in LEFT_OUT, which fail the field check.

gme::isOnSummonRail reads the tile lists, so these gates can be pulled and
nothing else changes in the server.  Each generator rewrites only its own
gates, rows and tiles; this one reads gate 11960's record and runs
gen_house_gates.legacy_pool(), so run it after gen_house_gates.py.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_collab_gacha as collab  # noqa: E402
import gen_house_gates as house    # noqa: E402
from gate_info import caption_text, info_text  # noqa: E402

EVENT_GEMS = 5

HALLOWEEN = 8020
HALLOWEEN_TILE = '6'
HALLOWEEN_TILE_ART = 'top_banner_Halloween_Lydia_2019.png'   # "Halloween Summon", 600x440
HALLOWEEN_TILE_ON = True
HALLOWEEN_BG = 'gacha_rare_bg_img_halloween2015.png'         # "Rare Summon", no price printed
# Any unit id of each line.  4*: the 2015 rate-up units (Ciara and Semira are on
# no other gate).  7*: the Halloween units of 2016-2019.
HALLOWEEN_MEMBERS = [810104, 860104, 60313, 20323, 50403, 10333,   # Ciara, Semira, Lunaris, Signas, Alyut, Farlon
                     860517, 860427, 820577, 830787]               # Beatrix, Galtier, Florence, Lydia

ELEMENT_TILE = '7'
ELEMENT_TILE_ART = 'tile_divine_summon_bfr1.png'             # "Divine Summon", 600x150, the Dark gate's title band
# (element name, unit_mst element, gate, its own art).  Pager order.
ELEMENTS = [
    ('Fire', 1, 13330, 'gacha_rare_bg_img_20170413D6_5G.png'),
    ('Water', 2, 13320, 'gacha_rare_bg_img_20170413D5_5G.png'),
    ('Earth', 3, 13300, 'gacha_rare_bg_img_20170413D4_5G.png'),
    ('Thunder', 4, 13290, 'gacha_rare_bg_img_20170413D3_5G.png'),
    ('Light', 5, 13280, 'gacha_rare_bg_img_20170413D2_5G.png'),
    ('Dark', 6, 13270, 'gacha_rare_bg_img_20170413D1_5G.png'),
]
ALL_STAR_RARITY = 5   # what the element gates take from gate 11960

# Lines that fail the field check (scripts/check_collab_units.py's Checker) at a
# stage they evolve into, by any unit id of the line.  Gate 11960 still has them.
LEFT_OUT = {
    820005: 'Gun Goddess Serin: 6* effect 820006BattleEffectBulletBBRAdd.sam missing',
    840315: 'Chevalier Mikael: Omni effect 5286BattleEffectTalesArcheThunderBladeAdd.sam missing',
}


def guest_only(unit: dict) -> bool:
    return unit.get('oS3kTZ2W', '0') == '0'


def pool_entry(base: str, units, names) -> dict:
    return {'unit_id': int(base), 'weight': collab.FAIR_WEIGHT[int(units[base]['7ofj5xa1'])], 'name': names[base]}


def halloween_pool(units, names, line):
    pool = []
    for member in HALLOWEEN_MEMBERS:
        base = line(str(member))[0]
        if units[base]['xP9oi5Z2'] != '0' or int(units[base]['7ofj5xa1']) not in collab.FAIR_WEIGHT \
                or guest_only(units[base]) or base not in names:
            sys.exit(f'Halloween member {member} (line {base}) cannot be summoned')
        pool.append(pool_entry(base, units, names))
    return pool


def element_pools(units, names, all_star, line):
    left_out = {s for k in LEFT_OUT for s in line(str(k))}
    stars = [str(p['unit_id']) for p in all_star['pool']
             if int(units[str(p['unit_id'])]['7ofj5xa1']) == ALL_STAR_RARITY]
    sevens = [str(p['unit_id']) for p in house.legacy_pool()]
    pools = {}
    for _, element, gate, _ in ELEMENTS:
        bases = [b for b in stars + sevens
                 if units[b]['iNy0ZU5M'] == str(element) and not guest_only(units[b]) and b not in left_out]
        pools[gate] = [pool_entry(b, units, names) for b in bases]
    return pools


def halloween_record(pool, units) -> dict:
    return {
        'id': HALLOWEEN,
        'priority': 1,
        'background': HALLOWEEN_BG,
        'caption_msg': caption_text('Halloween Summon', HALLOWEEN, pool, units),
        'detail_msg': info_text(pool, units),
        'pool': pool,
    }


def element_record(name, gate, background, pool, units) -> dict:
    return {
        'id': gate,
        'priority': 1,
        'background': background,
        'caption_msg': caption_text(f'Divine Summon: {name}', gate, pool, units, 'No bonus Elgif.'),
        'detail_msg': info_text(pool, units),
        'pool': pool,
    }


def build():
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    names = {str(u['id']): u['name'] for u in collab.load(collab.UNIT_ARCHIVE)}
    line = collab.evolution_lines()
    gacha = {g['id']: g for g in json.loads(collab.GACHA.read_text(encoding='utf-8'))}
    if house.ALL_STAR not in gacha:
        sys.exit(f'gate {house.ALL_STAR} has no archive record')
    records = [halloween_record(halloween_pool(units, names, line), units)]
    pools = element_pools(units, names, gacha[house.ALL_STAR], line)
    for name, _, gate, background in ELEMENTS:
        records.append(element_record(name, gate, background, pools[gate], units))
    return records, units


def tiles():
    out = [{'vx9uyQVQ': ELEMENT_TILE, 'In7lGGLn': ELEMENT_TILE_ART, '2r4EoNt4': int(ELEMENT_TILE),
            '3rCmq58M': ','.join(str(e[2]) for e in ELEMENTS), 'qA7M9EjP': 0, 'SzV0Nps7': 2147483647}]
    if HALLOWEEN_TILE_ON:
        out.append({'vx9uyQVQ': HALLOWEEN_TILE, 'In7lGGLn': HALLOWEEN_TILE_ART, '2r4EoNt4': int(HALLOWEEN_TILE),
                    '3rCmq58M': str(HALLOWEEN), 'qA7M9EjP': 0, 'SzV0Nps7': 2147483647})
    return out


def merged_files(records):
    gacha = json.loads(collab.GACHA.read_text(encoding='utf-8'))
    ours = {r['id'] for r in records}
    marks = ('<center>[<color=255:255:0>Halloween Summon', '<center>[<color=255:255:0>Divine Summon: ')
    clash = [g['id'] for g in gacha if g['id'] in ours and not str(g.get('caption_msg', '')).startswith(marks)]
    if clash:
        sys.exit(f'gate ids already used by other archive records: {clash}')
    # In place, so other generators' records keep their spot.
    by_id = {r['id']: r for r in records}
    present = {g['id'] for g in gacha}
    gacha = [by_id.get(g['id'], g) for g in gacha] + [r for r in records if r['id'] not in present]

    category = json.loads(collab.CATEGORY.read_text(encoding='utf-8'))
    kept = [t for t in category['IBs49NiH'] if t['vx9uyQVQ'] not in (HALLOWEEN_TILE, ELEMENT_TILE)]
    category = {'IBs49NiH': sorted(kept + tiles(), key=lambda t: int(t['2r4EoNt4']))}

    gacha_mst = json.loads(collab.GACHA_MST.read_text(encoding='utf-8'))
    left = set(ours)
    for row in gacha_mst[next(iter(gacha_mst))]:
        if int(row['7Ffmi96v']) in left:
            if row['S1oz60Hc'] != '2':
                sys.exit(f'gate {row["7Ffmi96v"]} is not a type 2 gem gate')
            row['03UGMHxF'] = str(EVENT_GEMS)
            left.discard(int(row['7Ffmi96v']))
    if left:
        sys.exit(f'gacha_mst rows missing: {sorted(left)}')
    return (json.dumps(gacha, indent=1, ensure_ascii=False),
            json.dumps(category, indent=2, ensure_ascii=False) + '\n',
            json.dumps(gacha_mst, indent=1, ensure_ascii=False) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if the files differ')
    ap.add_argument('--rates', action='store_true', help='print each pool and its chances')
    args = ap.parse_args()
    records, units = build()
    if args.rates:
        for r in records:
            pct = ', '.join(f'{k}* {v:.1f}%' for k, v in collab.chances(r['pool'], units).items())
            print(f'{r["id"]} ({EVENT_GEMS} gems): {len(r["pool"])} units; {pct}')
            total = sum(p['weight'] for p in r['pool'])
            for p in r['pool']:
                print(f'    {p["unit_id"]:>7} {p["name"]:<34} {units[str(p["unit_id"])]["7ofj5xa1"]}*  '
                      f'{100.0 * p["weight"] / total:5.2f}%')
        return
    gacha_text, category_text, gacha_mst_text = merged_files(records)
    if args.check:
        same = (collab.GACHA.read_text(encoding='utf-8') == gacha_text
                and collab.CATEGORY.read_text(encoding='utf-8') == category_text
                and collab.GACHA_MST.read_text(encoding='utf-8') == gacha_mst_text)
        print('up to date' if same else 'differs')
        sys.exit(0 if same else 1)
    collab.GACHA.write_text(gacha_text, encoding='utf-8')
    collab.CATEGORY.write_text(category_text, encoding='utf-8')
    collab.GACHA_MST.write_text(gacha_mst_text, encoding='utf-8')
    print(f'wrote {len(records)} event gates at {EVENT_GEMS} gems; rail tiles {ELEMENT_TILE}'
          + (f' and {HALLOWEEN_TILE}' if HALLOWEEN_TILE_ON else f' (tile {HALLOWEEN_TILE} off)'))


if __name__ == '__main__':
    main()
