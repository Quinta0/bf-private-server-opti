"""Global Exclusive Summon: every Global-exclusive line that reaches Omni, 10 gems.

python3 -I scripts/gen_global_gate.py                 # rewrite gate 8410's record, rail tile 8 and its price
python3 -I scripts/gen_global_gate.py --root DIR      # the same on DIR/archive and DIR/mst (prod's copies)
python3 -I scripts/gen_global_gate.py --check         # exit 1 if the data differs
python3 -I scripts/gen_global_gate.py --rates         # print the pool
python3 -I scripts/gen_global_gate.py --verify --assets "$HOME/bf-archive/assets"
                                                      # field-check every stage of every line (check_collab_units)

The owner, 2026-10-20, on a community suggestion in the thread "Separate
Global Exclusive Banner with JP Units Banner on Veteran Summon": a separate
banner at GEMS gems per summon whose pool is only the Global-exclusive units
that have an Omni evolution, all equally likely, each given at its summon stage
(as the Legacy gate does).  No end date.  The Veteran Summon (gate 15850) is
unchanged.

ONLY SUMMON UNITS (the owner, 2026-10-21, after players found free units in the
first pool of 204): a line stays only when an original Global gem gate named it --
in its name or either of its texts, by a stage's full name or by a word of it no
other line uses (summon_evidence).  Reward, exchange and event units were never
named on a gem gate, so they drop out, and so does any summon unit only ever
pooled without being named.

GLOBAL-EXCLUSIVE means a six-digit unit id starting with 8: the game's own
numbering for units made for Global (Ciara 810104, Semira 860104, Beatrix
860517, ...); JP units have five-digit ids.  A LINE is one evolution chain
(gen_collab_gacha.evolution_lines) and qualifies when any of its stages is
rarity 8.  Left out: collaboration units (their own gates,
gen_collab_gacha.FRANCHISES), guest-only lines (no leader skill at the summon
stage, as gen_event_gates does), lines without a deploy/archive/unit.json
record, the lines the other generators' LEFT_OUT lists and this LEFT_OUT
keep out because a stage fails the field check, and the lines the game gives
away (free_units.py; owner, 2026-10-23: Berserk Ezra, D'Artagnan, Florence
Nightingale and Sun Tse left this gate).

GATE 8410 "Limited Gate: Global Exclusive" (2016-04-14) is the original row
reused, at GEMS gems.  Its own art prints "6 per Summon" and a bonus Elgif, so
the gate shows the plain Rare Summon background instead.  RAIL TILE 8 opens it
under the 7th Anniversary banner, which shows Kranus the Argent (in the pool).
gme::isOnSummonRail reads the tile list, so nothing else changes in the server.

Run after gen_collab_gacha.py, gen_house_gates.py and gen_event_gates.py: each
script rewrites only its own gates, rows and tiles.
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import free_units  # noqa: E402
import gen_collab_gacha as collab  # noqa: E402
import gen_event_gates as event    # noqa: E402
import gen_house_gates as house    # noqa: E402
from gate_info import caption_text, info_text  # noqa: E402

GATE = 8410
GEMS = 10
TITLE = 'Global Exclusive Summon'
TILE = '8'
TILE_ART = 'top_banner_7thAnniversary_Kranus_2020.png'   # "7th Anniversary Brave Frontier Summon", 600x440
WEIGHT = collab.FAIR_WEIGHT[7]
OMNI = '8'

# Lines with a stage that fails the field check, by any unit id of the line.
LEFT_OUT = {
    850154: 'Cottontail Carrol: Omni effects 85015BattleEffectRainbowBlast(3)AddR.sam missing',
    850688: 'Ptah, Master of Truth: 5 effect files (850688BattleEffectGlowAdd.sam, ...) missing',
    860158: "Demonic Witch Xie'Jing: 820158BattleEffectIcesAddR.sam, 820158BattleEffectPushhAddR.sam missing",
    860798: 'Evie, Twilight Heir: 8 effect files (860798BattleEffectOrbDrop.sam, ...) missing',
}


# Evidence summon_evidence gets wrong, by base id: True = named on a gem gate,
# False = not.  A short full name matches inside a longer one.
EVIDENCE_FIXES = {
    850328: False,   # Ezra (Light): matched only inside "Berserk Ezra" (player report, 2026-10-21)
    860978: True,    # Berserk Ezra (Dark): the Ezra the gate named
}


# Words of unit names that also appear in gate texts as ordinary words.
NOT_NAMES = {'Opal', 'Legacy', 'Heroes'}


def _tokens(name: str) -> set:
    """The capitalised words of a unit name (before any comma), 3 letters or more."""
    return {w for w in re.findall(r"[A-Z][\w'\-]+", name.split(',')[0]) if len(w) >= 3} - NOT_NAMES


def gate_texts() -> list:
    """(gate id, plain text) of every original gem gate: its name and both of its texts.
    gacha_mst's texts are the game's own (the generators only rewrite prices)."""
    m = json.loads(collab.GACHA_MST.read_text(encoding='utf-8'))
    strip = lambda s: re.sub(r'<[^>]*>', ' ', s or '')
    return [(r['7Ffmi96v'], strip(f"{r['4N27mkt1']} {r['3sdHQb69']} {r['W2c9g0Je']}"))
            for r in m[next(iter(m))] if r['S1oz60Hc'] == '2']


def summon_evidence(lines, all_lines, names) -> dict:
    """base id -> (gate, name) for every line an original Global gem gate named: a stage's
    full name, or a word of it that no other line's names use.  The owner, 2026-10-21:
    only units that were ever in a Global rare summon gate, never reward or exchange units."""
    texts = gate_texts()
    owners: dict = {}
    for base, stages in all_lines.items():       # every line of the game, JP ones too
        for u in stages:
            for t in _tokens(names.get(u, '')):
                owners.setdefault(t, set()).add(base)
    found = {}
    for base, stages in lines.items():
        for n in sorted({names.get(u, '') for u in stages} - {''}):
            probes = [n.split(',')[0]] + sorted(t for t in _tokens(n) if owners[t] == {base})
            for probe in probes:
                pat = re.compile(r'(?<!\w)' + re.escape(probe) + r'(?!\w)')
                gate = next((g for g, text in texts if pat.search(text)), None)
                if gate:
                    found[base] = (gate, probe)
                    break
            if base in found:
                break
    return found


def global_exclusive(base: str) -> bool:
    return len(base) == 6 and base.startswith('8')


def candidate_lines():
    """(base id -> every stage of its line) for every Global-exclusive line reaching Omni."""
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    line = collab.evolution_lines()
    lines: dict[str, set] = {}
    for uid in units:
        lines.setdefault(line(uid)[0], set()).add(uid)
    return {b: s for b, s in lines.items()
            if global_exclusive(b) and any(units[u].get('7ofj5xa1') == OMNI for u in s)}, units, line, lines


def pool():
    lines, units, line, all_lines = candidate_lines()
    names = {str(u['id']): u['name'] for u in collab.load(collab.UNIT_ARCHIVE)}
    collab_lines = {line(str(m))[0] for *_, members, _ in collab.FRANCHISES for m in members}
    collab_lines |= {line(str(m))[0] for m in collab.LEFT_OUT}
    broken = {line(str(k))[0] for k in (*house.LEFT_OUT, *event.LEFT_OUT, *LEFT_OUT)}
    broken |= set(free_units.free_lines(units, line))
    summoned = set(summon_evidence(lines, all_lines, names))
    summoned = (summoned | {str(b) for b, ok in EVIDENCE_FIXES.items() if ok}) - {str(b) for b, ok in EVIDENCE_FIXES.items() if not ok}
    out = []
    for base in sorted(lines, key=int):
        u = units[base]
        if (u.get('xP9oi5Z2') != '0' or base in collab_lines or base in broken
                or event.guest_only(u) or base not in names or base not in summoned):
            continue
        out.append({'unit_id': int(base), 'weight': WEIGHT, 'name': names[base]})
    return out, units


def record(entries, units) -> dict:
    return {
        'id': GATE,
        'priority': 1,
        'background': collab.PLAIN_BG,
        'caption_msg': caption_text(TITLE, GATE, entries, units, 'Every unit reaches Omni.'),
        'detail_msg': info_text(entries, units),
        'pool': entries,
    }


def tile() -> dict:
    return {'vx9uyQVQ': TILE, 'In7lGGLn': TILE_ART, '2r4EoNt4': int(TILE), '3rCmq58M': str(GATE),
            'qA7M9EjP': 0, 'SzV0Nps7': 2147483647}


def paths(root):
    if root is None:
        return collab.GACHA, collab.CATEGORY, collab.GACHA_MST
    return root / 'archive' / 'gacha.json', root / 'mst' / 'gacha_category_mst.json', root / 'mst' / 'gacha_mst.json'


def merged_files(rec, root):
    gacha_path, category_path, mst_path = paths(root)
    gacha = json.loads(gacha_path.read_text(encoding='utf-8'))
    mark = f'<center>[<color=255:255:0>{TITLE}'
    if any(g['id'] == GATE and not str(g.get('caption_msg', '')).startswith(mark) for g in gacha):
        sys.exit(f'gate {GATE} already has another archive record')
    if any(g['id'] == GATE for g in gacha):
        gacha = [rec if g['id'] == GATE else g for g in gacha]      # in place
    else:
        gacha = gacha + [rec]

    category = json.loads(category_path.read_text(encoding='utf-8'))
    if any(t['vx9uyQVQ'] == TILE and t['3rCmq58M'] != str(GATE) for t in category['IBs49NiH']):
        sys.exit(f'rail tile {TILE} is taken by another gate')
    kept = [t for t in category['IBs49NiH'] if t['vx9uyQVQ'] != TILE]
    category = {'IBs49NiH': sorted(kept + [tile()], key=lambda t: int(t['2r4EoNt4']))}

    gacha_mst = json.loads(mst_path.read_text(encoding='utf-8'))
    rows = [r for r in gacha_mst[next(iter(gacha_mst))] if r['7Ffmi96v'] == str(GATE)]
    if len(rows) != 1 or rows[0]['S1oz60Hc'] != '2':
        sys.exit(f'gate {GATE} is not exactly one type 2 gem gate row in {mst_path}')
    rows[0]['03UGMHxF'] = str(GEMS)
    return {gacha_path: json.dumps(gacha, indent=1, ensure_ascii=False),
            category_path: json.dumps(category, indent=2, ensure_ascii=False) + '\n',
            mst_path: json.dumps(gacha_mst, indent=1, ensure_ascii=False) + '\n'}


def verify(assets: Path) -> int:
    import check_collab_units as cc
    check = cc.Checker(assets)
    entries, _ = pool()
    failures = 0
    for p in entries:
        for uid in check.line(str(p['unit_id'])):
            fail, _ = check.stage(uid)
            for f in fail:
                failures += 1
                print(f'FAIL {p["unit_id"]} {p["name"]}: {uid}: {f}')
    print(f'{len(entries)} lines checked, {failures} stage failure(s)')
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', type=Path, help='a deploy-shaped folder (archive/, mst/) to rewrite instead')
    ap.add_argument('--check', action='store_true', help='exit 1 if the files differ')
    ap.add_argument('--rates', action='store_true', help='print the pool and its chances')
    ap.add_argument('--verify', action='store_true', help='field-check every stage of every line')
    ap.add_argument('--assets', type=Path, help="the archive's assets/ folder, for --verify")
    args = ap.parse_args()
    if args.verify:
        if not args.assets:
            sys.exit('--verify needs --assets')
        sys.exit(1 if verify(args.assets) else 0)
    entries, units = pool()
    if args.rates:
        pct = ', '.join(f'{k}* {v:.1f}%' for k, v in collab.chances(entries, units).items())
        print(f'{GATE} ({GEMS} gems): {len(entries)} units; {pct}')
        for p in entries:
            print(f'    {p["unit_id"]:>7} {p["name"]:<34} {units[str(p["unit_id"])]["7ofj5xa1"]}*')
        return
    files = merged_files(record(entries, units), args.root)
    if args.check:
        same = all(path.read_text(encoding='utf-8') == text for path, text in files.items())
        print('up to date' if same else 'differs')
        sys.exit(0 if same else 1)
    for path, text in files.items():
        path.write_text(text, encoding='utf-8')
    print(f'wrote gate {GATE} ({len(entries)} units) at {GEMS} gems on rail tile {TILE}')


if __name__ == '__main__':
    main()
