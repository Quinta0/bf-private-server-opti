"""Checks that every unit the Collab Summon gates can give is safe to field:
every stage of its evolution line, not only the stage the gate gives.

python3 -I scripts/check_collab_units.py --assets "$HOME/bf-archive/assets"
python3 -I scripts/check_collab_units.py --assets ... --left-out   # also report the LEFT_OUT lines

For each stage it checks:
  - a unit_mst row and a deploy/archive/unit.json record (GachaAction builds the
    unit from the archive);
  - BB, SBB and UBB in skill_mst, the leader skill in leader_skill_mst and the
    extra skill in extra_passive_skill_mst;
  - every battle effect its normal attack, BB, SBB and UBB play: skill_mst and
    unit_mst effect frames name EffectGroupMst ids, whose frames name
    EffectMst ids, whose Heg8ZDQ7 names the .sam or effect_anime/cgg/cgs
    files.  Both tables are the client's own, read from the archive's mst/
    (Ver*_BQJq29Zh*.dat, Ver*_0VIAZHT9.dat; AES keys from libgame), because the
    server does not serve them.  A file must exist somewhere under content/,
    which is how the server answers DLC requests (findContentByName);
  - art: unit_ills_full/battle/thum, and the animation unit_mst names
    (a unit_anime_*.sam, or unit_cgg_*.csv with its anime png and cgs);
  - evolution materials (kind 1 unit, kind 2 item) exist.

Effect ids that are in neither table are reported as warnings, not failures:
38 units of the base game do the same (Kranus, Roglizer, Rhyne, ...).

Needs no pycryptodome: AES goes through the openssl command line.
"""
import argparse
import base64
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_collab_gacha as collab  # noqa: E402

ROOT = collab.ROOT
# file key -> AES key, as DataMstManager registers them in libgame
EFFECT_GROUP_MST = ('BQJq29Zh', '3FWEGtX9')
EFFECT_MST = ('0VIAZHT9', '9ojr0tPC')
ART = ('unit_ills_full_{}.png', 'unit_ills_battle_{}.png', 'unit_ills_thum_{}.png')
EVO_KINDS = ('Xyt6rhx2', '0tna4Idu', '6GwnugW3', 'hdF8ND2H', 'nB7pFdR0', 'IZUvR489', 'bNRUuatB', '2BFgYLjg', '18Oz7z8k')
EVO_MATS = ('85X6JHQA', 'wh3YRU08', '7MxucW2J', 'j7fTS3ca', 'Hb8yfmv7', 'Voht18AP', '3g8brFoq', 'agp4CKEV', 'eKPWNoLn')


def read_mst_table(mst_dir: Path, file_key: str, aes_key: str) -> list:
    parts = {}
    for f in mst_dir.iterdir():
        m = re.fullmatch(rf'Ver(\d+)_{file_key}(?:_(\d+))?\.dat', f.name)
        if m:
            parts.setdefault(int(m.group(1)), []).append((int(m.group(2) or 0), f))
    if not parts:
        sys.exit(f'{mst_dir} has no {file_key} table')
    rows = []
    for _, f in sorted(parts[max(parts)]):
        hexkey = aes_key.encode().ljust(16, b'\0').hex()
        plain = subprocess.run(['openssl', 'enc', '-d', '-aes-128-ecb', '-K', hexkey, '-nopad'],
                               input=base64.b64decode(f.read_bytes()), capture_output=True, check=True).stdout
        rows.extend(json.loads(plain[:-plain[-1]]))
    return rows


class Checker:
    def __init__(self, assets: Path):
        load = collab.load
        mst = ROOT / 'deploy' / 'mst'
        self.units = {r['pn16CNah']: r for r in load(mst / 'unit_mst.json')}
        self.skills = {}
        for r in load(mst / 'skill_mst.json'):
            self.skills.setdefault(r['nj9Lw7mV'], r)
        self.leader = {r['oS3kTZ2W'] for r in load(mst / 'leader_skill_mst.json')}
        self.extra = {r['cP83zNsv'] for r in load(mst / 'extra_passive_skill_mst.json')}
        self.items = {r['kixHbe54'] for r in load(mst / 'item_mst.json')}
        self.evo = {r['pn16CNah']: r for r in load(mst / 'unit_evo_mst.json')}
        self.archive = {str(u['id']) for u in load(collab.UNIT_ARCHIVE)}
        self.groups = {r['0UwMg18F']: r for r in read_mst_table(assets / 'mst', *EFFECT_GROUP_MST)}
        self.effects = {r['1TED5ZSi']: r for r in read_mst_table(assets / 'mst', *EFFECT_MST)}
        self.files = set()
        for _, _, names in os.walk(assets / 'content'):
            self.files.update(names)
        self.line = collab.evolution_lines()

    def effect_problems(self, frames: str) -> tuple[set, set]:
        """(effect ids no table knows, effect files no archive ships)"""
        unknown, missing = set(), set()
        for part in (frames or '').split('@'):
            for frame in part.split(','):
                bits = frame.split(':')
                if len(bits) < 2 or bits[1].strip() in ('', '-1', '0'):
                    continue
                ref = bits[1].strip()
                if ref in self.groups:
                    ids = [i for f in self.groups[ref]['eyUo6a8c'].split(',') if len(f.split(':')) > 1
                           for i in f.split(':')[1].split('@') if i not in ('', '-1', '0')]
                else:
                    ids = [ref]
                for eid in ids:
                    effect = self.effects.get(eid)
                    if effect is None:
                        unknown.add(eid)
                        continue
                    missing.update(n.lstrip('/') for n in effect['Heg8ZDQ7'].split(':')
                                   if n and n.lstrip('/') not in self.files)
        return unknown, missing

    def stage(self, uid: str) -> tuple[list, list]:
        """(failures, warnings) for one unit."""
        fail, warn = [], []
        row = self.units.get(uid)
        if row is None:
            return [f'{uid}: no unit_mst row'], []
        if uid not in self.archive:
            fail.append('no deploy/archive/unit.json record')
        for label, key, table in (('BB', 'nj9Lw7mV', self.skills), ('SBB', 'iEFZ6H19', self.skills),
                                  ('UBB', 'cb0P4mp1', self.skills), ('LS', 'oS3kTZ2W', self.leader),
                                  ('ES', 'cP83zNsv', self.extra)):
            if row.get(key, '0') not in ('0', '') and row[key] not in table:
                fail.append(f'{label} {row[key]} has no row')
        unknown, missing = self.effect_problems(row['eyUo6a8c'])
        for key in ('nj9Lw7mV', 'iEFZ6H19', 'cb0P4mp1'):
            skill = self.skills.get(row.get(key, '0'))
            if skill:
                u, m = self.effect_problems(skill['eyUo6a8c'])
                unknown |= u
                missing |= m
        if missing:
            fail.append('effect files in no archive: ' + ', '.join(sorted(missing)))
        if unknown:
            warn.append('effect ids in no effect table: ' + ', '.join(sorted(unknown, key=int)))
        anim = row.get('QqfI9mM4', '')
        art = [a.format(uid) for a in ART]
        if anim.endswith('.sam'):
            art.append(anim)
        else:
            aid = anim.removeprefix('unit_cgg_').removesuffix('.csv') or uid
            art += [f'unit_anime_{aid}.png', f'unit_cgg_{aid}.csv', f'unit_idle_cgs_{aid}.csv', f'unit_atk_cgs_{aid}.csv']
        lost = [a for a in art if a not in self.files]
        if lost:
            fail.append('art missing: ' + ', '.join(lost))
        evo = self.evo.get(uid)
        if evo:
            for kind_key, mat_key in zip(EVO_KINDS, EVO_MATS):
                kind, mat = evo.get(kind_key), evo.get(mat_key, '0')
                if mat in ('0', ''):
                    continue
                if (kind == '1' and mat not in self.units) or (kind == '2' and mat not in self.items) \
                        or kind not in ('1', '2'):
                    fail.append(f'evolution material kind {kind} id {mat} does not exist')
        return fail, warn


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--assets', required=True, type=Path, help="the archive's assets/ folder (content/ and mst/)")
    ap.add_argument('--left-out', action='store_true', help='also check the lines LEFT_OUT keeps out')
    args = ap.parse_args()
    check = Checker(args.assets)
    gacha = {g['id']: g for g in json.loads(collab.GACHA.read_text(encoding='utf-8'))}

    roots = []
    for title, gate, *_ in collab.FRANCHISES:
        if gate not in gacha:
            sys.exit(f'gate {gate} ({title}) has no archive record; run scripts/gen_collab_gacha.py')
        roots += [(title, str(p['unit_id'])) for p in gacha[gate]['pool']]
    if args.left_out:
        roots += [('left out', str(uid)) for uid in collab.LEFT_OUT]

    failures = 0
    for title, root in roots:
        for uid in check.line(root):
            fail, warn = check.stage(uid)
            name = check.units.get(uid, {}).get('utP1c0CD', '?')
            for f in fail:
                print(f'FAIL {title}: {uid} {name}: {f}')
            for w in warn:
                print(f'warn {title}: {uid} {name}: {w}')
            failures += bool(fail)
    print(f'{len(roots)} lines checked, {failures} stage(s) failing')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
