"""Crimson Scout Reda (10716) evolves into Ruby Successor Reda (10717).

python3 -I scripts/gen_reda_evo.py

unit_evo_mst stopped at 10715 -> 10716, so Ruby Successor Reda had no way in.
The recipe is the Global wiki's ("Crimson_Scout_Reda", evolution table): Fire
Mecha God x2, Miracle Totem, Fire Totem, Fire Pot x2, Metal Mimic and Memory
Spinel, for 3,000,000 Zel and 1,000,000 Karma. The unit materials use the ids
of Reda's 6* -> 7* row; Memory Spinel (item 110080) is an item material, kind 2
in slot 8, the way 10016 -> 10017 asks for a Legend Stone.

Memory Spinel has no source yet on purpose (the balance team decides where it
comes from), so nobody can evolve Reda until it gets one.

The client keeps its own copy (F_UNIT_EVO_MST) and builds the evolve request
from it: publish a new version with the same row, or evolutions of 10716 fail.
A rerun replaces the row, so it changes nothing.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / 'deploy/mst/unit_evo_mst.json'
WRAPPER = 'yY3Mvp51'

KINDS = ('Xyt6rhx2', '0tna4Idu', '6GwnugW3', 'hdF8ND2H', 'nB7pFdR0', 'IZUvR489', 'bNRUuatB', '2BFgYLjg', '18Oz7z8k')
MATS = ('85X6JHQA', 'wh3YRU08', '7MxucW2J', 'j7fTS3ca', 'Hb8yfmv7', 'Voht18AP', '3g8brFoq', 'agp4CKEV', 'eKPWNoLn')
UNIT, ITEM = '1', '2'

# (kind, id) per slot; unused slots keep kind 1 and id 0, as the shipped rows do
RECIPE = [(UNIT, '10354'), (UNIT, '10354'), (UNIT, '50354'), (UNIT, '10133'), (UNIT, '10191'),
          (UNIT, '10191'), (UNIT, '60224'), (ITEM, '110080'), (UNIT, '0')]


def row() -> dict:
    r = {'pn16CNah': '10716', '74VFwuTd': '10717', 'Rs7bCE3t': '3000000', 'HTVh8a65': '1000000'}
    r.update({k: kind for k, (kind, _) in zip(KINDS, RECIPE)})
    r['EK7jR4rB'] = '1'
    r.update({m: target for m, (_, target) in zip(MATS, RECIPE)})
    r['MVidsUNV'] = '0'
    return r


def main():
    data = json.loads(PATH.read_text(encoding='utf-8'))
    new = row()
    rows = [r for r in data[WRAPPER] if (r['pn16CNah'], r['74VFwuTd']) != (new['pn16CNah'], new['74VFwuTd'])]
    at = next((i + 1 for i, r in enumerate(rows) if r['pn16CNah'] == '10715'), len(rows))
    rows.insert(at, new)
    body = ',\n'.join('    ' + json.dumps(r, ensure_ascii=False) for r in rows)
    PATH.write_text('{\n  "%s": [\n%s\n  ]\n}\n' % (WRAPPER, body), encoding='utf-8')
    print(f'{len(rows)} rows; 10716 -> 10717 at {at}')


if __name__ == '__main__':
    main()
