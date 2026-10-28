"""Builds the Collab Summon gates: one gate per collaboration franchise, all
paged through by a single "Collab" tile on the summon rail.

python3 -I scripts/gen_collab_gacha.py          # rewrite the gate records, the rail tile and the prices
python3 -I scripts/gen_collab_gacha.py --check  # exit 1 if the data differs
python3 -I scripts/gen_collab_gacha.py --rates  # print each gate's pool and chances

WHAT IS ORIGINAL -- every gate id is a real gacha_mst row from the franchise's
own Global collaboration (or a "Collaboration Summon" row where its own row did
not survive), so name, type 2, door, button and price are the game's: 5 gems,
3 for the Vocaloid rerun gate.  The units, their skills and their art are the
game's.  Background art is the gate's own where the archive still has it
(Vocaloid, FFBE, KOF, Guilty Gear); the others show the plain "Rare Summon"
background, because their art is not in the archive.

WHAT IS AUTHORED -- the pools and the copy.  Each pool holds every unit of the
franchise at its summon stage, the lowest stage of its evolution line, which is
how the original collab gates gave them out; players evolve them as usual.  A
unit's weight is what fair summoning's gate 11960 gives its rarity (3* 2000,
4* 700, 5* 100).  Fair summoning has no rarer tier, so 6* and 7* units take the
5* weight: on a gate that starts at 5* every unit is equally likely, and 4*
units are seven times as likely as anything above them, as on 11960.

HOUSE PRICES -- HOUSE_PRICES below replaces a gate's original price.  The
generator writes the house price into the row's need_gems in
deploy/mst/gacha_mst.json.  need_gems is what
GachaAction charges, and every price the summon screens print (SummonsScrlItem,
SummonsDetailScene, SummonsConfirmScene) is GachaInfo's, built from that row;
nothing in the client reads the raw GachaMst price.

LEFT OUT (see LEFT_OUT below, with reasons):
  - lines that start at Omni (rarity 8): no stage to summon below Omni, and a
    gate that hands out Omni units is what fair summoning removed (except
    OMNI_ALLOWED);
  - lines the game gives away (free_units.py: a first clear, a 6* or better
    capture, the Bazaar or GIVEN), such as All Might, the first clear of
    "All Might Fistfight" (owner, 2026-10-23: no free unit in a paid pool);
  - lines whose summon stage, or a stage it evolves into, plays a Brave Burst
    effect file that is in no archive (F_EFFECT_MST names it, nothing ships it).
    Checked with scripts/check_collab_units.py against the archive.

The rail tile is GachaCategoryMst row 4 (gacha_category_mst.json); its
3rCmq58M lists the gates in pager order.  gme::isOnSummonRail reads the same
list, so these gates can be pulled and nothing else changes in the server.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import free_units  # noqa: E402
from gate_info import caption_text, info_text  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GACHA = ROOT / 'deploy' / 'archive' / 'gacha.json'
CATEGORY = ROOT / 'deploy' / 'mst' / 'gacha_category_mst.json'
GACHA_MST = ROOT / 'deploy' / 'mst' / 'gacha_mst.json'
UNIT_MST = ROOT / 'deploy' / 'mst' / 'unit_mst.json'
UNIT_EVO = ROOT / 'deploy' / 'mst' / 'unit_evo_mst.json'
UNIT_ARCHIVE = ROOT / 'deploy' / 'archive' / 'unit.json'

TILE_ID = '4'
TILE_ART = 'top_banner_TG_2_trimmed.png'   # "Tokyo Ghoul x Brave Frontier: Special Collaboration", 600x440
PLAIN_BG = 'gacha_rare_bg_img.png'         # "Rare Summon", no price or bonus printed on it
# House gate art (owner, 2026-10-21): these gates' own art is in no archive.  Trigun's
# is the Brave Frontier wiki's copy (Gacha_img_20200415T_s50.png, 640x632); the others
# are the collaboration's own banner centred on PLAIN_BG.  Uploaded to the CDN's R2
# under these new names (R2 is add-only, so a changed file needs a new name).
DEEMO_BG = 'gacha_bg_deemo_bfr1.png'                       # sp_quest_banner_deemo.png, its logo strip
FAIRY_TAIL_BG = 'gacha_bg_fairytail_bfr2.png'              # the "Special Summon: Fairy Tail" promo (posted on Discord)
TOKYO_GHOUL_BG = 'gacha_bg_tokyoghoul_bfr2.png'            # the collaboration's key art (posted on Discord)
TRIGUN_BG = 'gacha_bg_trigun_bfr2.png'                     # "Trigun Collaboration!" promo (Discord), no bonus printed
CHAIN_CHRONICLE_BG = 'gacha_bg_chainchronicle_bfr1.png'    # banner_chainchronicle.png
MHA_BG = 'gacha_bg_mha_bfr1.png'                           # "Limited Time Collaboration" promo (Discord)
COLLAB_MIX_BG = 'gacha_bg_collabmix_bfr1.png'              # Tales of Link key art (Discord)
SMT_BG = 'gacha_bg_smt_bfr1.png'                           # "Special Collaboration!" promo (Discord)
# Original event art that prints a rate-up and a bonus Elgif every 6 summons.
# Neither exists here, so these gates say so where the others say "Summon now!".
EVENT_ART = {'gacha_rare_bg_img_20161109_x2.png', 'gacha_rare_bg_img_20170222_x2.png',
             'gacha_rare_bg_img_20170601R_x2.png'}

# Gate 11960's weight per rarity (docs/features/FAIR_SUMMON.md).  6* and 7*
# take the top tier's weight; Omni lines are left out.
FAIR_WEIGHT = {3: 2000, 4: 700, 5: 100, 6: 100, 7: 100}

# Omni-only lines a collab gate still hands out, at the 7* weight.  Not a key
# 8 in FAIR_WEIGHT: the house and event gates read that table too.
OMNI_ALLOWED: dict[str, str] = {}   # All Might (850588) left: a first clear gives him

# (title, gate id, background, any unit id of each evolution line, a short
# name for the pool, unused since the caption lists the units).  Pager order.  Franchises with fewer than three
# summonable units share the Collab Mix gate: on a gate of its own, Zero
# (Phantom of the Kill) would be a guaranteed 7* for 5 gems.
FRANCHISES = [
    ('Vocaloid', 8560, 'gacha_rare_bg_img_20160503_VS.png',
     [30493, 60583, 810204, 820204, 840204, 850204, 850196, 820297, 850297, 830297], None),
    ('DEEMO', 15800, DEEMO_BG, [50563, 60864], None),
    ('Final Fantasy Brave Exvius', 11300, 'gacha_rare_bg_img_20161109_x2.png',
     [10985, 20915, 51075, 810397, 820397, 840397, 850397, 820387], None),
    ('The King of Fighters', 12810, 'gacha_rare_bg_img_20170222_x2.png',
     [810176, 810186, 820176, 830177, 830187, 840176, 850176, 850727, 860177, 860186, 860727, 860738], None),
    ('Guilty Gear', 13620, 'gacha_rare_bg_img_20170601R_x2.png',
     [810377, 820377, 830377, 830387, 840377, 850377, 850387, 860377, 11077, 40927, 51207], None),
    ('Shin Megami Tensei IV Apocalypse', 10480, SMT_BG,
     [10855, 20784, 30764, 40755, 50944, 50955, 60964, 840367, 850367, 860367], 'SMT IV Apocalypse'),
    ('My Hero Academia', 15480, MHA_BG, [810587, 820587, 830587, 840587, 850588], None),
    ('Fairy Tail', 14360, FAIRY_TAIL_BG, [810547, 820547, 830647, 840547, 850547, 850647, 850637, 860547, 860637, 860647], None),
    ('Tokyo Ghoul', 16920, TOKYO_GHOUL_BG, [810767, 830767, 840767, 850767, 810778], None),
    ('Trigun', 18850, TRIGUN_BG, [820867, 830867, 840867, 860867, 840878], None),
    ('Chain Chronicle', 15840, CHAIN_CHRONICLE_BG, [10704, 20654, 30654, 40624, 50774, 60794, 810265, 850265], None),
    ('Collab Mix', 15280, COLLAB_MIX_BG,
     [10764, 50844, 50854, 10573, 50633, 20543, 30533, 40513, 60633,   # Tales of
      810325, 840325, 840155,                                          # Rune Story
      810837, 820837, 830838, 860838,                                  # Samurai Champloo
      820507, 850507, 850977,                                          # The Alchemist Code
      50703, 60713, 860237],                                           # Phantom of the Kill
     'Tales of, Rune Story, Samurai Champloo, The Alchemist Code and Phantom of the Kill'),
]

# Owner-set prices, gate -> (original need_gems, house need_gems).  The owner,
# 2026-10-16/17: collab gates were too strong for their price.  Final Fantasy
# Brave Exvius and Guilty Gear go to 10 gems; the franchises the owner wanted
# much more expensive go higher, most of all where a 7* is guaranteed from a
# small pool.  Owner, 2026-10-18 ("fair, yet worth the trouble"): Fairy Tail,
# My Hero Academia, Tokyo Ghoul and Trigun 15, Vocaloid 10, DEEMO 3, SMT IV
# Apocalypse 10; Chain Chronicle and Collab Mix stay at 5.  Owner, 2026-10-21,
# final: every collab gate is 5 gems (with the Veteran gate 5, the Omni gate 10,
# the elemental gates 5 and the shiny All Star gate 1).
HOUSE_PRICES = {
    8560: (3, 5),    # Vocaloid (Miku): 4* 91.3%, 6* 2.2%, 7* 6.5%
    15800: (5, 5),    # DEEMO
    11300: (5, 5),   # Final Fantasy Brave Exvius: 6* 37.5%, 7* 62.5%
    12810: (5, 5),   # The King of Fighters: 6* 60%, 7* 40%
    13620: (5, 5),   # Guilty Gear: every pull a 7*, from 8 units
    10480: (5, 5),   # Shin Megami Tensei IV Apocalypse
    14360: (5, 5),   # Fairy Tail: every pull a 7*, from 10 units
    15480: (5, 5),   # My Hero Academia: every pull a 7*, from 4 units
    16920: (5, 5),   # Tokyo Ghoul: every pull a 7*, from 4 units
    18850: (5, 5),   # Trigun: every pull a 7*, from 4 units
}

# Evolution lines kept out of the pools, by any unit id of the line.
LEFT_OUT = {
    # Brave Burst effect files named by F_EFFECT_MST but shipped in no archive.
    10573: 'Stahn: BB/SBB effects 5280/5281BattleEffectTalesStan* missing',
    50633: 'Cress: BB/SBB effects 5288/5289BattleEffectTalesCless* missing',
    20543: 'Rutee: BB/SBB effects 5282/5283BattleEffectTalesRutee* missing',
    30533: 'Rita: BB/SBB effects 5284/5285BattleEffectTalesRita* missing',
    40513: 'Arche: BB/SBB effects 5286/5287BattleEffectTalesArche* missing',
    60633: 'Yuri: BB/SBB effects 5290/5291BattleEffectTalesYuri* missing',
    50703: 'Tyrfing: 4*/5* BB effects 5312/5313BattleEffectPhantomofkillTyrfing* missing',
    60713: 'Laevateinn: 4*/5* BB effects 5310/5311BattleEffectPhantomofkillLaevatein* missing',
    860177: 'Iori Yagami: 7*/Omni effects 86017BattleEffect7Iori*, 860178BattleEffectIori* missing',
    820507: 'Lofia: 7*/Omni effect 20010BattleEffectBgBlackBgF60.sam missing',
}


def load(path: Path):
    data = json.loads(path.read_text(encoding='utf-8'))
    return data[next(iter(data))] if isinstance(data, dict) else data


def evolution_lines():
    """unit id -> every stage of its evolution line, lowest first."""
    nxt = {r['pn16CNah']: r['74VFwuTd'] for r in load(UNIT_EVO)}
    prev = {v: k for k, v in nxt.items()}

    def line(uid: str) -> list[str]:
        while uid in prev:
            uid = prev[uid]
        out = [uid]
        while out[-1] in nxt and nxt[out[-1]] not in out:
            out.append(nxt[out[-1]])
        return out
    return line


def build():
    units = {r['pn16CNah']: r for r in load(UNIT_MST)}
    names = {str(u['id']): u['name'] for u in load(UNIT_ARCHIVE)}
    gates = {r['7Ffmi96v']: r for r in load(GACHA_MST)}
    line = evolution_lines()
    left_out = {str(k): v for k, v in LEFT_OUT.items()}
    free = free_units.free_lines(units, line)

    records, report = [], []
    for title, gate, background, members, blurb in FRANCHISES:
        row = gates[str(gate)]
        original, gems = HOUSE_PRICES.get(gate, (int(row['03UGMHxF']),) * 2)
        # A gate with a house price holds whatever price was written last, so
        # only an unpriced gate must still carry its original price.
        if row['S1oz60Hc'] != '2' or original not in (3, 5) \
                or (gate not in HOUSE_PRICES and int(row['03UGMHxF']) != original):
            sys.exit(f'gate {gate} is not a plain 3/5-gem rare gate: {row["4N27mkt1"]}')
        pool, skipped = [], []
        for member in members:
            stages = line(str(member))
            reason = next((left_out[s] for s in stages if s in left_out), None)
            base = stages[0]
            rarity = int(units[base]['7ofj5xa1'])
            if reason is None and base in free:
                reason = f'{names.get(base, base)}: free, {free[base]}'
            if reason is None and rarity not in FAIR_WEIGHT and base not in OMNI_ALLOWED:
                reason = f'{names.get(base, base)}: only exists as Omni ({base})'
            if base not in names:
                reason = reason or f'{base}: no deploy/archive/unit.json record'
            if reason:
                skipped.append(reason)
                continue
            pool.append({'unit_id': int(base), 'weight': FAIR_WEIGHT.get(rarity, FAIR_WEIGHT[7]), 'name': names[base]})
        records.append(gate_record(title, gate, background, pool, units))
        report.append((title, gate, row['4N27mkt1'], gems, pool, skipped, units))
    return records, report


def chances(pool, units) -> dict[int, float]:
    total = sum(p['weight'] for p in pool)
    by_rarity = {}
    for p in pool:
        r = int(units[str(p['unit_id'])]['7ofj5xa1'])
        by_rarity[r] = by_rarity.get(r, 0) + p['weight']
    return {r: 100.0 * w / total for r, w in sorted(by_rarity.items())}


def gate_record(title, gate, background, pool, units) -> dict:
    return {
        'id': gate,
        'priority': 1,
        'background': background,
        'caption_msg': caption_text(f'Collab Summon: {title}', gate, pool, units,
                                    'Event art: no rate-up, no bonus Elgif.' if background in EVENT_ART else ''),
        'detail_msg': info_text(pool, units),
        'pool': pool,
    }


def tile() -> dict:
    return {
        'vx9uyQVQ': TILE_ID,
        'In7lGGLn': TILE_ART,
        '2r4EoNt4': int(TILE_ID),
        '3rCmq58M': ','.join(str(f[1]) for f in FRANCHISES),
        'qA7M9EjP': 0,
        'SzV0Nps7': 2147483647,
    }


def merged_files(records):
    gacha = json.loads(GACHA.read_text(encoding='utf-8'))
    ours = {r['id'] for r in records}
    clash = [g['id'] for g in gacha if g['id'] in ours and not str(g.get('caption_msg', '')).startswith(
        '<center>[<color=255:255:0>Collab Summon')]
    if clash:
        sys.exit(f'gate ids already used by other archive records: {clash}')
    # In place, so other generators' records (gen_house_gates.py) keep their spot.
    by_id = {r['id']: r for r in records}
    present = {g['id'] for g in gacha}
    gacha = [by_id.get(g['id'], g) for g in gacha] + [r for r in records if r['id'] not in present]
    category = json.loads(CATEGORY.read_text(encoding='utf-8'))
    tiles = [t for t in category['IBs49NiH'] if t['vx9uyQVQ'] != TILE_ID] + [tile()]
    category = {'IBs49NiH': sorted(tiles, key=lambda t: int(t['2r4EoNt4']))}
    # The house prices.  To go back to a gate's original price, set its house
    # price to the original rather than deleting the entry.
    # The row names the art the gate shows, so no screen falls back to its own
    # missing original (the summon list then shows a bare "Special Summon").
    backgrounds = {gate: background for _, gate, background, *_ in FRANCHISES}
    gacha_mst = json.loads(GACHA_MST.read_text(encoding='utf-8'))
    for row in gacha_mst[next(iter(gacha_mst))]:
        house = HOUSE_PRICES.get(int(row['7Ffmi96v']))
        if house:
            row['03UGMHxF'] = str(house[1])
        if int(row['7Ffmi96v']) in backgrounds:
            row['1Dg0vUX3'] = backgrounds[int(row['7Ffmi96v'])]
    return (json.dumps(gacha, indent=1, ensure_ascii=False),
            json.dumps(category, indent=2, ensure_ascii=False) + '\n',
            json.dumps(gacha_mst, indent=1, ensure_ascii=False) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if the files differ')
    ap.add_argument('--rates', action='store_true', help='print each pool and its chances')
    args = ap.parse_args()
    records, report = build()
    if args.rates:
        for title, gate, name, gems, pool, skipped, units in report:
            pct = ', '.join(f'{r}* {p:.1f}%' for r, p in chances(pool, units).items())
            print(f'{gate} {name!r} ({gems} gems) -- {title}: {len(pool)} units; {pct}')
            total = sum(p['weight'] for p in pool)
            for p in pool:
                print(f'    {p["unit_id"]:>7} {p["name"]:<28} {units[str(p["unit_id"])]["7ofj5xa1"]}*  '
                      f'{100.0 * p["weight"] / total:5.2f}%')
            for s in skipped:
                print(f'    left out: {s}')
        return
    gacha_text, category_text, gacha_mst_text = merged_files(records)
    if args.check:
        same = (GACHA.read_text(encoding='utf-8') == gacha_text
                and CATEGORY.read_text(encoding='utf-8') == category_text
                and GACHA_MST.read_text(encoding='utf-8') == gacha_mst_text)
        print('up to date' if same else 'differs')
        sys.exit(0 if same else 1)
    GACHA.write_text(gacha_text, encoding='utf-8')
    CATEGORY.write_text(category_text, encoding='utf-8')
    GACHA_MST.write_text(gacha_mst_text, encoding='utf-8')
    print(f'wrote {len(records)} collab gates, rail tile {TILE_ID} and {len(HOUSE_PRICES)} house prices')


if __name__ == '__main__':
    main()
