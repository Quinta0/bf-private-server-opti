"""One Vortex tile for the special quests (ticket #152).

python3 -I scripts/gen_special_quests_area.py

Players asked for the one-time and daily special quests to stop filling the Vortex
list.  The client marks them itself: their areas carry the description
"曜日や時限でダンジョンが登場するエリア" (areas whose dungeons come by weekday or for a
limited time).  Of those, the ones that are NOT weekday rotation or the pinned Parade
Garden (Gathering of Souls, Ruins of Light and Dark, Parade Garden stay) move under a
new category 100006 "Special Quests", the way 100004/100005 hold Seasons Past.

Only parent_area_id (sHMai2ZG) changes, so day gates (per dungeon, from the banner),
permits (an area is permitted whatever its parent, a category when a child leads to a
battle) and progress (keyed by mission) are untouched.  The client draws a category's
children from its downloaded F_AREA_MST, so the same change ships there as Ver702
(see the commit message for the republish steps).  Idempotent.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
AREA_MST = ROOT / 'deploy' / 'mst' / 'area_mst.json'
CATEGORY = '100006'
# The special tiles a player sees (the archive has a battle in each), by area id
SPECIAL = {
    '110029': '至高の楽園43 (Garden of Elgifs)', '110019': '至高の楽園33 (Garden of Imps)',
    '101420': 'Year-End Dungeon', '102610': 'Mysterious Paradise', '102650': 'Ultimate Paradise',
    '102710': 'Heavenly Paradise', '105900': 'Gathering of World Heroes', '105810': 'イリュリア城',
    '105800': 'ネオニューヨーク', '105700': '王都グランシェルト', '105710': 'コロシアムへの道',
    '104000': "Holy Knight's Castle", '103900': 'Soul Island Forest', '103400': 'Songstress of Light',
}


def apply(rows: list) -> list:
    """The table with the category added and SPECIAL moved under it."""
    by_id = {r['VjCY7rX4']: r for r in rows}
    missing = [a for a in SPECIAL if a not in by_id]
    if missing:
        raise SystemExit(f'areas not in the table: {missing}')
    out = []
    for r in rows:
        if r['VjCY7rX4'] in SPECIAL:
            r = dict(r, sHMai2ZG=CATEGORY)
        if r['VjCY7rX4'] != CATEGORY:
            out.append(r)
    template = by_id['100005']                       # Seasons Past, a category tile
    out.append(dict(template, VjCY7rX4=CATEGORY, V84mzqoX='Special Quests', XuJL4pc5='9000',
                    H0UB8pqm=by_id['102710'].get('H0UB8pqm') or template['H0UB8pqm'],
                    qp37xTDh='Special Quests'))
    return out


def main() -> int:
    text = AREA_MST.read_text(encoding='utf-8')
    doc = json.loads(text)
    key = next(iter(doc))
    doc[key] = apply(doc[key])
    AREA_MST.write_text(json.dumps(doc, indent=1 if text.startswith('{\n ') and not text.startswith('{\n  ') else 2,
                                   ensure_ascii=False) + ('\n' if text.endswith('\n') else ''), encoding='utf-8')
    print(f'{len(SPECIAL)} special tiles under {CATEGORY} "Special Quests"')
    return 0


if __name__ == '__main__':
    sys.exit(main())
