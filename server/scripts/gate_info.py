"""Gate info text: what a summon gate gives and at what odds (ticket #30).

python3 -I scripts/gate_info.py          # write the text of the rail gates no generator owns (1000, 2000)
python3 -I scripts/gate_info.py --check  # exit 1 if any rail gate's text is not built from its pool

Every gate a rail tile offers shows, before a gem is spent:
  - caption_msg, the text box under the gate art (SummonsDetailScene,
    SummonsScrlItem): the gate's title, then its units, best rarity first,
    A to Z, as many as CAPTION_CHARS allows, then "and N more", with a line
    under the title that points at the full list, example.com/gates/<id>;
  - detail_msg, the Summon confirm dialog (SummonsConfirmScene): the chance of
    each rarity, its number of units and the chance of one of them.
Both boxes are fixed-size labels that wrap but do not scroll (2.19.6 client,
checked on the emulator): the caption box holds about six lines of 42
characters, the confirm dialog about four lines before its text runs over the
Summon button.  So the full list of a big pool does not fit.

The text names no price: the summon screens print GachaInfo's need_gems
themselves, so the text stays right whatever price the gacha_mst row carries
(the temporary prices on prod live only in its gacha_mst.json).

The generators (gen_collab_gacha.py, gen_house_gates.py, gen_event_gates.py)
build their gates' text with caption_text() and info_text(); this script
writes it for the hand-authored rail gates 1000 (Honor) and 2000 (tutorial).
Run it last.
"""
import argparse
import json
import re
import sys
from pathlib import Path

CAPTION_CHARS = 200     # names after the title line: five lines of the caption box
LIST_URL = 'example.com/gates/{}'   # the full list, site/tools/build_gate_pages.py in the root repository
NOTE_CHARS = 42         # one caption line, given up to a note
OWNED_HERE = {1000: 'Honor Summon', 2000: 'Rare Summon'}
TITLE = re.compile(r'^<center>\[<color=255:255:0>(.*?)</color>\]')
# Weekly featured gates carry this line under the title; the server fills it
# with "Featured until Mon 27 Oct, 08:00 UTC" (gme/common/SummonRotation.hpp).
ROTATION = Path(__file__).resolve().parents[1] / 'deploy/archive/summon_rotation.json'
FEATURED_LINE = '<color=0:255:255>{featured_until}</color><br>'


def rotation_gates() -> set[int]:
    """The gates of the weekly featured pool."""
    if not ROTATION.is_file():
        return set()
    return {p['gate'] for p in json.loads(ROTATION.read_text(encoding='utf-8'))['pool']}


def _pct(x: float, digits: int = 1) -> str:
    return f'{x:.2f}%' if x < 1 else f'{x:.{digits}f}%'


def _star(rarity: int) -> str:
    return f'<color=255:0:0>{rarity}★</color>'


def _groups(pool, units) -> dict:
    groups = {}
    for p in pool:
        if p['weight'] > 0:
            groups.setdefault(int(units[str(p['unit_id'])]['7ofj5xa1']), []).append(p)
    return dict(sorted(groups.items(), reverse=True))


def info_text(pool, units) -> str:
    """The confirm dialog: each rarity's chance and one unit's chance."""
    groups = _groups(pool, units)
    total = sum(p['weight'] for ps in groups.values() for p in ps)
    rates = []
    for rarity, ps in groups.items():
        share = [100.0 * p['weight'] / total for p in ps]
        each = (_pct(share[0], 2) if min(share) == max(share)
                else f'{_pct(min(share), 2)}-{_pct(max(share), 2)}')
        rates.append(f'{_star(rarity)} {_pct(sum(share))}' + (f' ({len(ps)})' if len(groups) > 3
                     else f': 1 unit, {each}' if len(ps) == 1 else f': {len(ps)} units, {each} each'))
    per_line = 1 if len(rates) <= 3 else 2   # four lines fit; past three rarities, two to a line
    lines = ['  '.join(rates[i:i + per_line]) for i in range(0, len(rates), per_line)]
    return '<center>[<color=255:255:0>Rates</color>]<br>' + '<br>'.join(lines) + '<center>'


def caption_text(title: str, gate: int, pool, units, note: str = '') -> str:
    """The text box under the gate art: title, the featured line of a pool gate,
    an optional note, the units.  When they do not all fit, a line under the
    title points at the full list."""
    featured = gate in rotation_gates()
    budget = CAPTION_CHARS - (NOTE_CHARS if note else 0) - (NOTE_CHARS if featured else 0)
    head = f'<center>[<color=255:255:0>{title}</color>]<br>'
    if featured:
        head += FEATURED_LINE
    if note:
        head += f'<color=255:255:0>{note}</color><br>'
    names, left_out = _names(pool, units, budget)
    if left_out:
        names, left_out = _names(pool, units, budget - NOTE_CHARS)
        head += f'<color=0:255:0>Full list: {LIST_URL.format(gate)}</color><br>'
        names += f', and {left_out} more'
    return head + names + '<center>'


def _names(pool, units, budget: int) -> tuple[str, int]:
    """Units best rarity first, A to Z, within budget; and how many are left out."""
    groups = _groups(pool, units)
    count = sum(len({p['name'] for p in ps}) for ps in groups.values())
    parts, used, shown = [], 0, 0
    for rarity, ps in groups.items():
        names = sorted({p['name'] for p in ps}, key=str.casefold)
        fits = []
        for name in names:
            if used + len(name) + 2 > budget:
                break
            fits.append(name)
            used += len(name) + 2
        shown += len(fits)
        if fits:
            parts.append(f'{_star(rarity)} ' + ', '.join(fits))
        if len(fits) < len(names):
            break
    return ' '.join(parts), count - shown


def _collab():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gen_collab_gacha  # the generators import this module
    return gen_collab_gacha


def rail_gates() -> list[int]:
    collab = _collab()
    category = json.loads(collab.CATEGORY.read_text(encoding='utf-8'))
    return [int(x) for t in category['IBs49NiH'] for x in str(t['3rCmq58M']).split(',') if x]


def stale(record, units) -> bool:
    """True when the record's text is not what its pool gives."""
    m = TITLE.match(record.get('caption_msg') or '')
    if not m or record.get('detail_msg') != info_text(record['pool'], units):
        return True
    rest = record['caption_msg'][m.end() + len('<br>'):].removeprefix(FEATURED_LINE)
    note = re.match(r'<color=255:255:0>(.*?)</color><br>', rest)
    return record['caption_msg'] != caption_text(m.group(1), record['id'], record['pool'], units,
                                                 note.group(1) if note else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if any rail gate text differs')
    args = ap.parse_args()
    collab = _collab()
    units = {r['pn16CNah']: r for r in collab.load(collab.UNIT_MST)}
    gacha = json.loads(collab.GACHA.read_text(encoding='utf-8'))
    by_id = {g['id']: g for g in gacha}
    if args.check:
        bad = [g for g in rail_gates() if stale(by_id[g], units)]
        print('up to date' if not bad else f'differs: {bad}')
        sys.exit(1 if bad else 0)
    out = [dict(g, caption_msg=caption_text(OWNED_HERE[g['id']], g['id'], g['pool'], units),
                detail_msg=info_text(g['pool'], units)) if g['id'] in OWNED_HERE else g for g in gacha]
    collab.GACHA.write_text(json.dumps(out, indent=1, ensure_ascii=False), encoding='utf-8')
    print(f'wrote the text of gates {", ".join(map(str, OWNED_HERE))}')


if __name__ == '__main__':
    main()
