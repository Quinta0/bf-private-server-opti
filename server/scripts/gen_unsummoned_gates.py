"""Unsummoned lines: three banners for the playable lines no rail gate offered.

python3 -I scripts/gen_unsummoned_gates.py                 # rewrite gates 18530, 8210, 18060, rail tiles 9-11, prices
python3 -I scripts/gen_unsummoned_gates.py --root DIR      # the same on DIR/archive and DIR/mst (prod's copies)
python3 -I scripts/gen_unsummoned_gates.py --check         # exit 1 if the data differs
python3 -I scripts/gen_unsummoned_gates.py --rates         # print each pool and its chances
python3 -I scripts/gen_unsummoned_gates.py --verify --assets "$HOME/bf-archive/assets"
                                                           # field-check every stage of every line (check_collab_units)

The owner, 2026-10-21: 111 playable evolution lines (kind 0, not collaboration,
guest-only or failing the field check, with a deploy/archive/unit.json record)
were in no rail gate's pool, so no player could summon them.  Each banner here
reuses an original Global gem gate row (name, type 2, door, button) and rail
art the client ships, with the owner's prices (10 gems for 100% 7* or better,
3 for the catch-all):

GATE 18530 "World Heroes Summon" (2020-03-04) at 10 gems, on RAIL TILE 9 under
its own "World Heroes" banner (Odin, in the pool): the 13 Apex and World lines
that exist only as Omni, each named on an original Global gem gate
(gen_global_gate.summon_evidence).  All equally likely, so 100% Omni.  Its own
art is not in the archive; it shows the wiki's copy of it, uploaded to the CDN.

GATE 8210 "Special Summon" (2016-03-25, Soul Bound Saga) at 10 gems, on RAIL
TILE 10 under the "Heroes Across Time" banner: the eight Global heroes released
as 7* (Ultor, Tridon, Zedus, Hadaron, Aurelia, Andaria, Grandt, Elaina), each
named on an original gem gate and given at its 7* stage, as the Legacy gate
gives its lines.  All equally likely, so 100% 7*.  It shows its own art, those
heroes under "Special Summon", which prints no price.

GATE 18060 "Dawn Summon" (2019-12-18) at 3 gems, on RAIL TILE 11 under its own
"Dawn Summon" banner: every other such line, at its summon stage, with fair
summoning's weight for its rarity (DAWN_WEIGHT).  Left out: the lines in
LEFT_OUT, and while LEAVE_OUT_FREE holds, the lines the game gives away
(free_units.free_lines: a reward, an exchange, the Geodes) and every farmable
line (free_units.farmable_lines: caught at any rarity or dropped by a boss).  Its own art is
not in the archive, so it shows the plain Rare Summon background.

gme::isOnSummonRail reads the tile list, so nothing else changes in the server.
Run after gen_collab_gacha.py, gen_house_gates.py, gen_event_gates.py and
gen_global_gate.py, and before gate_info.py: each script rewrites only its own
gates, rows and tiles, and the Dawn Summon pool is what no other rail gate offers.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import free_units  # noqa: E402
import gen_collab_gacha as collab  # noqa: E402
import gen_event_gates as event    # noqa: E402
import gen_global_gate as global_gate  # noqa: E402
import gen_house_gates as house    # noqa: E402
from gate_info import caption_text, info_text  # noqa: E402

# (gate, gems, title, caption note, tile, tile art, background, members, stage)
# members: the base id of each line; stage: the rarity given, None = the summon stage.
WORLD = (18530, 10, 'World Heroes Summon', 'Every unit is Omni.', '9',
         'top_banner_EU_Odin_2020.png',          # "World Heroes" (Odin), 600x440
         'gacha_bg_world_heroes_bfr1.png',       # the wiki's Gacha_img_20200304O_s8.png, 640x632
         [710217, 710227, 720157, 720167, 720237, 730227, 730257, 730287,   # Natalamé, Surt, Gazolina, Viviane & Orah,
          740157, 740237, 740247, 750317, 760006], None)                    # Utheria, Gwenevere, Ophiuchus, Frigg, Belt,
                                                                            # Sheriff, Gilgamesh, Odin, Yggdrasil
HEROES = (8210, 10, 'Heroes Across Time', 'Every unit is 7★.', '10',
          'top_banner_Seria_Vohu_2020.png',      # "Heroes Across Time", 600x440
          'gacha_rare_bg_img_20160326_SBS.png',  # "Special Summon", the Soul Bound heroes, no price
          [810134, 820134, 830134, 840134, 850134, 860134,   # Ultor, Tridon, Andaria, Zedus, Aurelia, Hadaron
           810525, 830525], 7)                               # Grandt, Elaina
DAWN = (18060, 3, 'Dawn Summon', 'Units no other banner offers.', '11',
        'small_banner_DawnSummon.png',           # "Dawn Summon", 600x150
        collab.PLAIN_BG, None, None)
GATES = [WORLD, HEROES, DAWN]

# Fair summoning's weight per rarity: a 2* takes the 3* weight, an Omni the 7* weight.
DAWN_WEIGHT = {2: collab.FAIR_WEIGHT[3], **collab.FAIR_WEIGHT, 8: collab.FAIR_WEIGHT[7]}

# Lines kept out of the Dawn Summon, by base id.
LEFT_OUT = {
    30645: 'Young Sheep Ullah: 7* stage 30646 has no deploy/archive/unit.json record',
    60435: 'Zevalhua the Supreme: effects 10221BattleEffectWordOfZevaluaAdd(R).sam missing',
    61167: 'NA (Serial Code): the archive has no name for it',
}

# The owner, 2026-10-21: the lines the game gives away stay out of the Dawn Summon, and
# 2026-10-23 (ticket #126): so does every farmable line, caught at any rarity or
# dropped by a #82 boss (free_units.free_lines and free_units.farmable_lines).
LEAVE_OUT_FREE = True


def paths(root):
    if root is None:
        return collab.GACHA, collab.CATEGORY, collab.GACHA_MST
    return root / 'archive' / 'gacha.json', root / 'mst' / 'gacha_category_mst.json', root / 'mst' / 'gacha_mst.json'


class Data:
    """The unit tables, every evolution line and the lines no gate may take."""

    def __init__(self):
        self.units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
        self.names = {str(u['id']): u['name'] for u in collab.load(collab.UNIT_ARCHIVE)}
        self.line = collab.evolution_lines()
        self.lines: dict[str, list] = {}
        for uid in self.units:
            self.lines.setdefault(self.line(uid)[0], self.line(uid))
        self.collab = {self.line(str(m))[0] for *_, members, _ in collab.FRANCHISES for m in members}
        self.collab |= {self.line(str(m))[0] for m in collab.LEFT_OUT}
        self.broken = {self.line(str(k))[0] for k in (*house.LEFT_OUT, *event.LEFT_OUT, *global_gate.LEFT_OUT)}

    def playable(self, base: str) -> bool:
        u = self.units[base]
        return (u.get('xP9oi5Z2') == '0' and base not in self.collab and base not in self.broken
                and not event.guest_only(u) and base in self.names)

    def entry(self, uid: str, weight: int) -> dict:
        if uid not in self.names:
            sys.exit(f'{uid} has no deploy/archive/unit.json record')
        return {'unit_id': int(uid), 'weight': weight, 'name': self.names[uid]}


def member_pool(data: Data, members, stage):
    """The given lines, each at the stage of rarity `stage` (None: its summon stage)."""
    out = []
    for m in members:
        stages = data.line(str(m))
        if stages[0] != str(m) or not data.playable(stages[0]):
            sys.exit(f'{m} is not the base of a playable line')
        uid = stages[0] if stage is None else next(
            (u for u in stages if data.units[u]['7ofj5xa1'] == str(stage)), None)
        if uid is None:
            sys.exit(f'line {m} has no {stage}* stage')
        out.append(data.entry(uid, collab.FAIR_WEIGHT[7]))
    return out


def dawn_pool(data: Data, gacha, category, taken):
    """Every playable line in no other rail gate's pool and in neither member list."""
    own = {g[0] for g in GATES}
    rail = {int(x) for t in category['IBs49NiH'] for x in str(t['3rCmq58M']).split(',') if x} - own
    offered = {data.line(str(p['unit_id']))[0] for g in gacha if g['id'] in rail for p in g['pool']}
    free = set(free_units.free_lines(data.units, data.line)) | set(free_units.farmable_lines(data.units, data.line))
    skip = {str(k) for k in LEFT_OUT} | (free if LEAVE_OUT_FREE else set())
    out = []
    for base in sorted(data.lines, key=int):
        if base in offered or base in taken or base in skip or not data.playable(base):
            continue
        rarity = int(data.units[base]['7ofj5xa1'])
        out.append(data.entry(base, DAWN_WEIGHT[rarity]))
    return out


def pools(data: Data, gacha, category) -> dict:
    """gate -> pool, in GATES order."""
    out = {}
    for gate, *_, members, stage in GATES:
        if members is not None:
            out[gate] = member_pool(data, members, stage)
    taken = {data.line(str(p['unit_id']))[0] for pool in out.values() for p in pool}
    out[DAWN[0]] = dawn_pool(data, gacha, category, taken)
    return out


def record(spec, pool, units) -> dict:
    gate, _, title, note, _, _, background, *_ = spec
    return {
        'id': gate,
        'priority': 1,
        'background': background,
        'caption_msg': caption_text(title, gate, pool, units, note),
        'detail_msg': info_text(pool, units),
        'pool': pool,
    }


def tile(spec) -> dict:
    gate, _, _, _, tile_id, art, *_ = spec
    return {'vx9uyQVQ': tile_id, 'In7lGGLn': art, '2r4EoNt4': int(tile_id), '3rCmq58M': str(gate),
            'qA7M9EjP': 0, 'SzV0Nps7': 2147483647}


def merged_files(data: Data, root):
    gacha_path, category_path, mst_path = paths(root)
    gacha = json.loads(gacha_path.read_text(encoding='utf-8'))
    category = json.loads(category_path.read_text(encoding='utf-8'))
    built = pools(data, gacha, category)
    for spec in GATES:
        gate, title = spec[0], spec[2]
        rec = record(spec, built[gate], data.units)
        mark = f'<center>[<color=255:255:0>{title}'
        if any(g['id'] == gate and not str(g.get('caption_msg', '')).startswith(mark) for g in gacha):
            sys.exit(f'gate {gate} already has another archive record')
        if any(g['id'] == gate for g in gacha):
            gacha = [rec if g['id'] == gate else g for g in gacha]      # in place
        else:
            gacha = gacha + [rec]

    ours = {spec[4]: str(spec[0]) for spec in GATES}
    if any(t['vx9uyQVQ'] in ours and t['3rCmq58M'] != ours[t['vx9uyQVQ']] for t in category['IBs49NiH']):
        sys.exit(f'one of rail tiles {sorted(ours, key=int)} is taken by another gate')
    kept = [t for t in category['IBs49NiH'] if t['vx9uyQVQ'] not in ours]
    category = {'IBs49NiH': sorted(kept + [tile(s) for s in GATES], key=lambda t: int(t['2r4EoNt4']))}

    gacha_mst = json.loads(mst_path.read_text(encoding='utf-8'))
    for spec in GATES:
        gate, gems = spec[0], spec[1]
        rows = [r for r in gacha_mst[next(iter(gacha_mst))] if r['7Ffmi96v'] == str(gate)]
        if len(rows) != 1 or rows[0]['S1oz60Hc'] != '2':
            sys.exit(f'gate {gate} is not exactly one type 2 gem gate row in {mst_path}')
        rows[0]['03UGMHxF'] = str(gems)
        rows[0]['1Dg0vUX3'] = spec[6]    # the art it shows, not its own missing original
        rows[0]['4N27mkt1'] = spec[2]    # 8210 was "Special Summon"
    files = {gacha_path: json.dumps(gacha, indent=1, ensure_ascii=False),
             category_path: json.dumps(category, indent=2, ensure_ascii=False) + '\n',
             mst_path: json.dumps(gacha_mst, indent=1, ensure_ascii=False) + '\n'}
    return files, built


def verify(assets: Path, built) -> int:
    import check_collab_units as cc
    check = cc.Checker(assets)
    failures = 0
    for gate, pool in built.items():
        for p in pool:
            for uid in check.line(str(p['unit_id'])):
                fail, _ = check.stage(uid)
                for f in fail:
                    failures += 1
                    print(f'FAIL {gate} {p["unit_id"]} {p["name"]}: {uid}: {f}')
        print(f'gate {gate}: {len(pool)} lines checked')
    print(f'{failures} stage failure(s)')
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', type=Path, help='a deploy-shaped folder (archive/, mst/) to rewrite instead')
    ap.add_argument('--check', action='store_true', help='exit 1 if the files differ')
    ap.add_argument('--rates', action='store_true', help='print each pool and its chances')
    ap.add_argument('--verify', action='store_true', help='field-check every stage of every line')
    ap.add_argument('--assets', type=Path, help="the archive's assets/ folder, for --verify")
    args = ap.parse_args()
    data = Data()
    files, built = merged_files(data, args.root)
    if args.verify:
        if not args.assets:
            sys.exit('--verify needs --assets')
        sys.exit(1 if verify(args.assets, built) else 0)
    if args.rates:
        for gate, gems, title, *_ in GATES:
            pool = built[gate]
            pct = ', '.join(f'{k}* {v:.1f}%' for k, v in collab.chances(pool, data.units).items())
            print(f'{gate} {title} ({gems} gems): {len(pool)} units; {pct}')
            for p in pool:
                print(f'    {p["unit_id"]:>7} {p["name"]:<34} {data.units[str(p["unit_id"])]["7ofj5xa1"]}*'
                      f' {100.0 * p["weight"] / sum(q["weight"] for q in pool):5.2f}%')
        return
    if args.check:
        same = all(path.read_text(encoding='utf-8') == text for path, text in files.items())
        print('up to date' if same else 'differs')
        sys.exit(0 if same else 1)
    for path, text in files.items():
        path.write_text(text, encoding='utf-8')
    print('; '.join(f'gate {g} ({len(built[g])} units) at {gems} gems on rail tile {t}'
                    for g, gems, _, _, t, *_ in GATES))


if __name__ == '__main__':
    main()
