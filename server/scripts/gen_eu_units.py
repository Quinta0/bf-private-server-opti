"""Adds EU-exclusive units to the unit and skill tables, from the EU datamine.

    python3 -I scripts/gen_eu_units.py

The EU server's own MST is gone. github.com/cheahjs/bravefrontier_data keeps its
units in PARSED form (eu/info.json, eu/feskills.json; one record per unit is
saved under scripts/data/eu_unit_<id>.json). This script turns that back into
the client's raw rows. The parameter order of every proc and passive was read
off Global units, whose parsed datamine and raw rows we both have; an effect
key this script does not know stops it rather than being dropped.

What the datamine does not have comes from UNITS below:
  - battle effects (effect group ids): Dark Path Arthur's (750167) light
    effects, retimed to the unit's hits;
  - art placement in menus: the unit_mst fields of a Global omni whose full
    art has the same size;
  - category (series) and the SP options the Global client cannot play.

Art and animation files are not in any table: the unit fetches
content/unit/{img,cgg,cgs}/*_<id>.* from the CDN, and those must be uploaded
before a client sees the unit (a phone keeps a placeholder for good).

Writes deploy/mst/{unit,unit_cgs,skill,skill_level,leader_skill,
extra_passive_skill,fe_skill,unit_fe_skill,unit_fe_category}_mst.json and
deploy/archive/unit.json. Rows with the same ids are replaced, so a rerun is a
no-op. The client copies of these tables are published separately (new
table versions); the server reads deploy/mst, and sends extra skills in the
Initialize payload.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MST = ROOT / 'deploy/mst'

UNITS = {
    750257: {  # Trinity Goddess Htilit; CGG hand-rebuilt by a community member
        'category': '750250',
        'art_template': '710217',   # full art 1146x1024, Htilit's is 1129x1024
        'effects': {
            'unit': '68:125:1,70:500221:1,72:125:1',
            'bb': '66:500310:1,72:500310:1,76:500180:1@0:500:1',
            'sbb': '66:500220:1,70:500310:1,78:350:1@0:500:1',
            'ubb': '66:500310:1,72:600140:1,78:500010:1',
        },
        # The datamine parsed passive 55 without its buff; the EU text says
        # "reduces damage taken for 2 turns (25%) when HP is below 50%".
        # Global writes that as buff 36 (unit 50986: 36,25,-1,50,2,1).
        'es_raw': {'55': '36,25,-1,50,2,2'},
        # 70268/70269 add an element buff to the SBB in a form no Global SP
        # uses (raw 5,1&100&...).
        'skip_sp': {'70268', '70269'},
        # 70270 is a Mana Bubble Shield (EU proc 70001 "3,25,0": every ally
        # shielded for 25% of her REC), which the Global client cannot play
        # (procCommand has no 70001).  Global's nearest is a party barrier
        # (proc 62): 25% of her max REC (2610 + 600 imps) is ~800 per ally,
        # 4800 for six, on BB/SBB.  DARK, not light: the client keeps one
        # barrier per element and replaces it (BattleUnitBuffList::setMain),
        # and her extra skill's 4000 Light barrier is loaded after SP passives
        # (setPassiveList), so a Light one here would always be overwritten.
        # A Dark barrier is its own buff and absorbs once the Light one breaks.
        'sp_replace': {'70270': ('Adds Dark Barrier (4800 HP) to BB/SBB (Mana Bubble Shield)', [{'passive': {
            'passive id': '66', 'trigger on bb': True, 'trigger on sbb': True, 'triggered effect': [{
                'effect delay time(ms)/frame': '0.0/0', 'target area': 'aoe', 'target type': 'party',
                'proc id': '62', 'elemental barrier element': 'dark', 'elemental barrier hp': 4800,
                'elemental barrier def': 0, 'elemental barrier absorb dmg%': 100}]}}])},
    },
}

ELEMENT = {'all': 0, 'fire': 1, 'water': 2, 'earth': 3, 'thunder': 4, 'light': 5, 'dark': 6}
TARGET_TYPE = {'party': 1, 'enemy': 2, 'self': 3}
TARGET_AREA = {'single': 1, 'aoe': 2}
CONVERTED = {'attack': 1, 'defense': 2, 'recovery': 3, 'hp': 4}
GENDER = {'male': 1, 'female': 2, 'other': 0}
SP_KIND = {'passive': 1, 'add to bb': 2, 'add to sbb': 3, 'add to ubb': 4, 'add to passive': 5}
FE_CATEGORY_NAME = {'1': 'ステアップ系', '2': 'スパーク系', '11': '特殊'}
ATTACK_PROCS = {'1', '47'}
META = {'proc id', 'passive id', 'target area', 'target type', 'effect delay time(ms)/frame',
        'conditions', 'passive target'}


def num(v) -> str:
    v = float(v)
    return str(int(v)) if v == int(v) else str(v)


class Effect:
    """One parsed effect; every key read must be known, and every key must be read."""

    def __init__(self, data: dict):
        self.data, self.used = data, set(META)

    def get(self, key, default=0):
        self.used.add(key)
        return self.data.get(key, default)

    def prefixed(self, prefix: str):
        for key in self.data:
            if key.startswith(prefix):
                self.used.add(key)
                return self.data[key]
        return 0

    def done(self, values: list) -> list:
        left = set(self.data) - self.used
        if left:
            raise ValueError(f'unknown keys {sorted(left)} in {self.data}')
        return [num(v) if not isinstance(v, str) else v for v in values]


def proc_params(pid: str, data: dict) -> list:
    e = Effect(data)
    if pid == '1':
        return e.done([e.get('bb atk%'), e.get('bb flat atk'), 0, 0, 0, 0])
    if pid == '5':
        return e.done([ELEMENT[e.get('element buffed', 'all')], e.prefixed('atk% buff'), e.prefixed('def% buff'),
                       e.prefixed('rec% buff'), e.prefixed('crit% buff'), e.get('buff turns'), 0])
    if pid == '8':
        return e.done([0, e.get('max hp% increase'), 0])
    if pid == '24':
        return e.done([CONVERTED[e.get('converted attribute')], e.prefixed('atk% buff'), e.prefixed('def% buff'),
                       e.prefixed('rec% buff'), e.get('% converted turns'), 0])
    if pid == '30':
        elements = [ELEMENT[x] for x in e.get('elements added', [])]
        return e.done(elements + [0] * (6 - len(elements)) + [e.get('elements added turns'), 0])
    if pid == '47':
        if e.get('bb added atk% proportional to hp') != 'remaining':
            raise ValueError(f'proc 47 only supports "remaining": {data}')
        base = e.get('bb base atk%')
        return e.done([base, base + e.get('bb added atk% based on hp'), 0, e.get('bb flat atk'), 0, 0, 0, 0])
    if pid == '62':
        return e.done([ELEMENT[e.get('elemental barrier element')], e.get('elemental barrier hp'),
                       e.get('elemental barrier def'), e.get('elemental barrier absorb dmg%'), 0])
    if pid == '73':
        return e.done([e.get('atk down resist% (120)'), e.get('def down resist% (121)'),
                       e.get('rec down resist% (122)'), e.get('stat down immunity buff turns'), 0])
    raise ValueError(f'proc {pid} has no encoder: {data}')


def triggered(data: dict) -> str:
    """Passive 66: a proc added to BB/SBB/UBB."""
    e = Effect(data)
    flags = [int(bool(e.get(k, False))) for k in ('trigger on bb', 'trigger on sbb', 'trigger on ubb')]
    (eff,) = e.get('triggered effect')
    e.done([])
    pid = eff['proc id']
    return ','.join([pid, '&'.join(proc_params(pid, eff)), str(TARGET_TYPE[eff['target type']]),
                     str(TARGET_AREA[eff['target area']]), eff['effect delay time(ms)/frame'].split('/')[1]]
                    + [str(f) for f in flags])


def passive_params(pid: str, data: dict) -> str:
    if pid == '66':
        return triggered(data)
    e = Effect(data)
    if pid == '1':
        vals = [e.get('atk% buff'), e.get('def% buff'), e.get('rec% buff'), e.get('crit% buff'), e.get('hp% buff')]
    elif pid == '8':
        vals = [e.get('dmg% mitigation')]
    elif pid == '11':
        vals = [e.get('atk% buff'), e.get('def% buff'), e.get('rec% buff'), e.get('crit% buff'),
                e.get('hp above % buff requirement'), 1]
    elif pid == '24':
        vals = [e.get('dmg% to hp% when attacked low'), e.get('dmg% to hp% when attacked high'),
                e.get('dmg% to hp% when attacked chance%')]
    elif pid == '46':
        if e.get('buff proportional to hp') != 'remaining':
            raise ValueError(f'passive 46 only supports "remaining": {data}')
        vals = [e.get('atk% base buff'), e.get('atk% extra buff based on hp'), e.get('def% base buff'),
                e.get('def% extra buff based on hp'), e.get('rec% base buff'),
                e.get('rec% extra buff based on hp'), 0]
    elif pid == '143':
        vals = [e.get('increase atk cap')]
    else:
        raise ValueError(f'passive {pid} has no encoder: {data}')
    return ','.join(e.done(vals))


def hits(f: dict) -> str:
    pairs = zip(f['frame times'], f['hit dmg% distribution'])
    return ','.join(f'{t}:{d}:2:{2 + i % 2}' for i, (t, d) in enumerate(pairs))


def damage_frames(frames: list, procs: list) -> str:
    out, attacks = [], iter(frames)
    for pid in procs:
        if pid not in ATTACK_PROCS:
            out.append('0:100:2:1')
            continue
        f = next(attacks)
        if f['proc id'] != pid:
            raise ValueError(f'damage frames for proc {f["proc id"]}, expected {pid}')
        out.append(hits(f))
    if next(attacks, None) is not None:
        raise ValueError('more damage frames than attack procs')
    return '@'.join(out)


def skill_rows(skill: dict, effects: str) -> tuple[dict, dict]:
    first = skill['levels'][0]['effects']
    procs = [e['proc id'] for e in first]
    row = {
        'nj9Lw7mV': skill['id'], '0nxpBDz2': skill['name'], 'h6UL9A1B': '1', 'AR7y0sY4': '5',
        'hjAy9St3': '@'.join(procs),
        'moWQ30GH': '@'.join(str(TARGET_TYPE[e['target type']]) for e in first),
        'IZt73kLG': '@'.join(str(TARGET_AREA[e['target area']]) for e in first),
        'I7cmtqW8': '1', 'K2i8emcH': '1', 'diGe6u21': '-1', 'eyUo6a8c': effects,
        'qYCx73y2': '@'.join(e['effect delay time(ms)/frame'].split('/')[1] for e in first),
        '6Aou5M9r': damage_frames(skill['damage frames'], procs),
        'n9h7p02P': str(skill['drop check count']), '9pXSKmn1': '5', 'iNy0ZU5M': '0',
        'qp37xTDh': skill['desc'], 'ToZj4zyn': '1', '57BsydKH': '0', 'R8uaKdbf': ',,',
    }
    levels = []
    for lv, level in enumerate(skill['levels'], 1):
        if [e['proc id'] for e in level['effects']] != procs:
            raise ValueError(f'{skill["id"]} level {lv} changes its procs')
        params = '@'.join(','.join(proc_params(e['proc id'], e)) for e in level['effects'])
        levels.append(f'{lv}:{level["bc cost"] * 100}:{params}')
    return row, {'nj9Lw7mV': skill['id'], 'Kn51uR4Y': '|'.join(levels)}


def unit_row(u: dict, cfg: dict, template: dict) -> dict:
    s = u['stats']
    lo, hi, imp = s['_base'], s['_lord'], u['imp']
    attack, skill = u['movement']['attack'], u['movement']['skill']
    if attack['move type'] != '1' or attack['move speed type'] != skill['move speed type']:
        raise ValueError(f'movement {u["movement"]} needs a template check')
    row = dict(template)
    row.update({
        'pn16CNah': str(u['id']), 'utP1c0CD': u['name'], '7ofj5xa1': str(u['rarity']),
        'gvT2ds0Q': str(u['cost']), '9i2xhMaJ': str(GENDER[u['gender']]), '9PsmH7tz': cfg['category'],
        '5UvTp7q1': str(u['exp_pattern']),
        'UZ1Bj7w2': str(lo['hp']), '3WMz78t6': str(hi['hp']), 'i9Tn7kYr': str(lo['atk']), 'omuyP54D': str(hi['atk']),
        'q78KoWsg': str(lo['def']), '32INDST4': str(hi['def']), '92ij6UGB': str(lo['rec']), 'X9P3AN5d': str(hi['rec']),
        'imQJdg64': f'{imp["max hp"]}:{imp["max atk"]}:{imp["max def"]}:{imp["max rec"]}',
        'iNy0ZU5M': str(ELEMENT[u['element']]), 'eyUo6a8c': cfg['effects']['unit'],
        '6Aou5M9r': hits(u['damage frames']), 'n9h7p02P': str(u['drop check count']),
        # the EU datamine has skill move type 0, which no Global unit uses: move like the attack
        'J2hPXGo5': attack['move speed type'], 'mv4o39Uz': '1', 'XE7Yi5c3': '1', 'i74vGUFa': '120',
        'oS3kTZ2W': u['leader skill']['id'], 'cP83zNsv': u['extra skill']['id'], 'nj9Lw7mV': u['bb']['id'],
        'iEFZ6H19': u['sbb']['id'], 'cb0P4mp1': u['ubb']['id'], 'XuJL4pc5': str(u['guide_id']),
        'QqfI9mM4': f'unit_cgg_{u["id"]}.csv',
    })
    return row


def leader_row(ls: dict) -> dict:
    ids = [e['passive id'] for e in ls['effects']]
    return {'oS3kTZ2W': ls['id'], 'dJPf9a5v': ls['name'], 'hjAy9St3': '@'.join(ids),
            '2Smu5Mtq': '@'.join(passive_params(p, e) for p, e in zip(ids, ls['effects'])), 'qp37xTDh': ls['desc']}


def extra_row(es: dict, cfg: dict) -> dict:
    ids, params, conds = [], [], []
    for e in es['effects']:
        pid = e['passive id']
        if e['conditions'] or e['passive target'] != 'self':
            raise ValueError(f'extra skill conditions/targets need an encoder: {e}')
        ids.append(f'{pid},1')
        params.append(cfg['es_raw'].get(pid) or passive_params(pid, e))
        conds.append('1#0,0%0,0%0,0')
    return {'cP83zNsv': es['id'], '0nxpBDz2': es['name'], 'UVN5bGoD': '', 'h6UL9A1B': '1', 'outas79f': '0',
            'yu18xScw': '0', '7ofj5xa1': '0', 's2gM3deu': '@'.join(conds), 'moWQ30GH': '0',
            'hjAy9St3': '@'.join(ids), '2Smu5Mtq': '@'.join(params), 'qp37xTDh': es['desc']}


def sp_rows(uid: str, fe: dict, cfg: dict) -> tuple[list, list, list]:
    skills, links, order = [], [], {}
    for s in fe['skills']:
        if s['id'] in cfg['skip_sp']:
            continue
        desc, (effect,) = cfg.get('sp_replace', {}).get(s['id'], (s['skill']['desc'], s['skill']['effects']))
        ((kind, data),) = effect.items()
        pid = data.get('passive id') or data.get('proc id')
        params = passive_params(pid, data) if kind in ('passive', 'add to passive') \
            else ','.join(proc_params(pid, data))
        skills.append({'ri6D9yBi': s['id'], 'yRlCNyvk': desc, '9PsmH7tz': s['id'],
                       'D9wXQI2V': str(s['skill']['level']), 'G5wFDad6': str(s['skill']['bp']),
                       'jeR2rN3V': str(SP_KIND[kind]), 'hjAy9St3': pid, '2Smu5Mtq': params, 'qp37xTDh': ''})
        order[s['category']] = order.get(s['category'], 0) + 1
        links.append({'pn16CNah': uid, 'VgU78CYj': s['category'], 'XuJL4pc5': str(order[s['category']]),
                      'ri6D9yBi': s['id'], 'FHThxDv4': s['dependency'], '7XYkj2EU': s['dependency comment']})
    zeros = dict.fromkeys(('Rs7bCE3t', 'HTVh8a65', 'Xyt6rhx2', '0tna4Idu', '6GwnugW3', 'hdF8ND2H', 'nB7pFdR0',
                           '85X6JHQA', 'wh3YRU08', '7MxucW2J', 'j7fTS3ca', 'Hb8yfmv7'), '0')
    cats = [{'pn16CNah': uid, 'VgU78CYj': c, 'XLSB42ED': FE_CATEGORY_NAME[c], 'XuJL4pc5': str(i), **zeros}
            for i, c in enumerate(order, 1)]
    return skills, links, cats


def archive_record(row: dict) -> dict:
    return {'id': int(row['pn16CNah']), 'name': row['utP1c0CD'], 'rarity': int(row['7ofj5xa1']),
            'element': int(row['iNy0ZU5M']), 'effect_frame': row['eyUo6a8c'], 'damage_frame': row['6Aou5M9r'],
            'drop_check_count': int(row['n9h7p02P']), 'move_speed': int(row['J2hPXGo5']),
            'attack_move_type': int(row['mv4o39Uz']), 'back_move_type': int(row['h2L1YI90']),
            'after_image': int(row['D4Y5bWK7']), 'skill_move_type': int(row['XE7Yi5c3']),
            'cursor_disp_pos': row['jFX0a7qe'],
            'stats': [{'hp': int(row['UZ1Bj7w2']), 'atk': int(row['i9Tn7kYr']), 'def': int(row['q78KoWsg']),
                       'rec': int(row['92ij6UGB'])}],
            'bb_id': row['nj9Lw7mV'], 'sbb_id': row['iEFZ6H19']}


def merge(rows: list, new: list, key) -> list:
    """Replace rows whose key matches a new row, append the rest, keep everything else in place."""
    wanted = {key(r): r for r in new}
    out = [wanted.pop(key(r), r) for r in rows]
    return out + [r for r in new if key(r) in wanted]


def write_mst(name: str, wrapper: str, new: list, key):
    path = MST / name
    data = json.loads(path.read_text(encoding='utf-8'))
    data[wrapper] = merge(data[wrapper], new, key)
    if name == 'unit_cgs_mst.json':   # one row per line in this file
        body = ',\n'.join('    ' + json.dumps(r, ensure_ascii=False) for r in data[wrapper])
        path.write_text('{\n  "%s": [\n%s\n  ]\n}\n' % (wrapper, body), encoding='utf-8')
    else:
        path.write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')


def main():
    units = {r['pn16CNah']: r for r in json.loads((MST / 'unit_mst.json').read_text(encoding='utf-8'))['2r9cNSdt']}
    out = {k: [] for k in ('unit', 'cgs', 'skill', 'level', 'leader', 'extra', 'fe', 'ufe', 'cat', 'archive')}
    for uid, cfg in UNITS.items():
        src = json.loads((ROOT / f'scripts/data/eu_unit_{uid}.json').read_text(encoding='utf-8'))
        u, sid = src['unit'], str(uid)
        row = unit_row(u, cfg, units[cfg['art_template']])
        out['unit'].append(row)
        out['archive'].append(archive_record(row))
        out['cgs'].append({'pn16CNah': sid, 'Kn51uR4Y': f'1:unit_idle_cgs_{uid}.csv|2:unit_move_cgs_{uid}.csv|'
                                                        f'3:unit_atk_cgs_{uid}.csv|4:unit_atk_cgs_{uid}.csv'})
        for kind in ('bb', 'sbb', 'ubb'):
            skill, level = skill_rows(u[kind], cfg['effects'][kind])
            out['skill'].append(skill)
            out['level'].append(level)
        out['leader'].append(leader_row(u['leader skill']))
        out['extra'].append(extra_row(u['extra skill'], cfg))
        skills, links, cats = sp_rows(sid, src['feskills'], cfg)
        out['fe'] += skills
        out['ufe'] += links
        out['cat'] += cats
        print(f'{uid} {u["name"]}: {len(skills)} SP options, skills {u["bb"]["id"]}/{u["sbb"]["id"]}/{u["ubb"]["id"]}')

    write_mst('unit_mst.json', '2r9cNSdt', out['unit'], lambda r: r['pn16CNah'])
    write_mst('unit_cgs_mst.json', '07bx6EXN', out['cgs'], lambda r: r['pn16CNah'])
    write_mst('skill_mst.json', '8aiBoHg5', out['skill'], lambda r: r['nj9Lw7mV'])
    write_mst('skill_level_mst.json', '3ktU0D2e', out['level'], lambda r: r['nj9Lw7mV'])
    write_mst('leader_skill_mst.json', '2bGtP7vV', out['leader'], lambda r: r['oS3kTZ2W'])
    write_mst('extra_passive_skill_mst.json', 'z5U7utsm', out['extra'], lambda r: r['cP83zNsv'])
    write_mst('fe_skill_mst.json', '2h9r3yEY', out['fe'], lambda r: r['ri6D9yBi'])
    write_mst('unit_fe_skill_mst.json', 'kXes8fSi', out['ufe'], lambda r: (r['pn16CNah'], r['ri6D9yBi']))
    write_mst('unit_fe_category_mst.json', 'kvSPgNAk', out['cat'], lambda r: (r['pn16CNah'], r['VgU78CYj']))

    path = ROOT / 'deploy/archive/unit.json'
    records = merge(json.loads(path.read_text(encoding='utf-8')), out['archive'], lambda r: r['id'])
    path.write_text(json.dumps(sorted(records, key=lambda r: r['id']), indent=4, ensure_ascii=False) + '\n',
                    encoding='utf-8')


if __name__ == '__main__':
    main()
