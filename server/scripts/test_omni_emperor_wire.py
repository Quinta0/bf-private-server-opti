"""Omni Emperor fusion, wire test against a running server.

    python3 -I scripts/test_omni_emperor_wire.py [--url http://127.0.0.1:9993]
        [--container bf-oe] [--db /data/gme.sqlite] [--tools <root>/tools]

An Omni Emperor (750005) gives a max-level, SBB 10 Omni unit 5 SP and 5 more
SP limit, but only at Omni+3 and only while the limit is under 130 (the
client's UnitMixElemSelectScene::checkElemUnit @0x1C09B98).  Checks:
  1. Omni+3 unit, limit 120: one Emperor -> SP +5, limit 125, and the reply's
     result limit (ksED59z9) says 125.
  2. Two more Emperors on it: only one fits under 130 -> limit 130, SP +5.
  3. Omni+2 unit: an Emperor changes neither SP nor limit.
  4. A full +3 unit given a Burst Emperor (5 SP) and an Omni Emperor gains
     only the Omni Emperor's 5: the Burst Emperor has no room left.
Runs on the Mac with the guest-account and docker-exec SQL helpers of the root
repo's tools/test_fair_summon.py.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
OMNI_EMPEROR, BURST_EMPEROR = 750005, 10313


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def find_key(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        node = list(node.values())
    if isinstance(node, list):
        for child in node:
            found = find_key(child, key)
            if found is not None:
                return found
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9993')
    ap.add_argument('--container', default='bf-oe')
    ap.add_argument('--db', default='/data/gme.sqlite', help='save path inside the container')
    ap.add_argument('--tools')
    args = ap.parse_args()
    sys.path.insert(0, str(find_tools(args.tools)))
    from test_fair_summon import Database, Player, gme_call, wire_error  # noqa: E402

    failures = 0

    def check(label, ok, detail=''):
        nonlocal failures
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  ' + str(detail)}")
        failures += not ok

    units = {int(r['pn16CNah']): r for r in next(iter(json.loads(
        (ROOT / 'deploy/mst/unit_mst.json').read_text(encoding='utf-8')).values()))}
    omni = next(i for i, u in sorted(units.items()) if u['7ofj5xa1'] == '8' and u['gvT2ds0Q'] == '60')

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    uid = player.user_id
    player.set(zel=99_999_999)
    cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)')]
    template = dict(zip(cols, db.one('SELECT * FROM user_units WHERE user_id = ? LIMIT 1', uid)))
    template.pop('user_unit_id')

    def unit(mst=omni, **kw):
        row = dict(template, unit_id=str(mst), unit_lvl=int(units[mst]['EI1DF8Yt']), favorite_flg=0,
                   sbb_lvl=10, total_exp=99_999_999, omni_level=0, fe_sp=0, fe_used_sp=0, fe_max_sp=100)
        row.update(kw)
        db.rows(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", *row.values())
        return db.one('SELECT max(user_unit_id) FROM user_units WHERE user_id = ?', uid)[0]

    def sp(u):
        return tuple(db.one('SELECT fe_sp, fe_max_sp FROM user_units WHERE user_unit_id = ?', u))

    def fuse(base, mats):
        return gme_call(args.url, *UNIT_MIX, {
            'IKqx1Cn9': player.login,
            '60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}],
            'mCE3rUu5': [{'Rs7bCE3t': '0'}],
            'Km35HAXv': [{'edy7fq3L': str(base), 'mnZ5K4Ii': '1'}]
                        + [{'edy7fq3L': str(unit(mst=m)), 'mnZ5K4Ii': '2'} for m in mats]})

    plus3 = unit(omni_level=3, fe_sp=50, fe_used_sp=70, fe_max_sp=120)
    reply = fuse(plus3, [OMNI_EMPEROR])
    check('1: Omni Emperor fuses into a +3 unit', wire_error(reply) is None, wire_error(reply))
    check('1: +5 SP and +5 limit', sp(plus3) == (55, 125), sp(plus3))
    check('1: reply limit is 125', find_key(reply, 'ksED59z9') == '125', find_key(reply, 'ksED59z9'))

    reply = fuse(plus3, [OMNI_EMPEROR, OMNI_EMPEROR])
    check('2: two more fuse', wire_error(reply) is None, wire_error(reply))
    check('2: only one fits under 130', sp(plus3) == (60, 130), sp(plus3))

    plus2 = unit(omni_level=2, fe_sp=10, fe_used_sp=100, fe_max_sp=110)
    reply = fuse(plus2, [OMNI_EMPEROR])
    check('3: Omni Emperor fuses into a +2 unit', wire_error(reply) is None, wire_error(reply))
    check('3: +2 unit unchanged', sp(plus2) == (10, 110), sp(plus2))

    full = unit(omni_level=3, fe_sp=20, fe_used_sp=100, fe_max_sp=120)
    reply = fuse(full, [BURST_EMPEROR, OMNI_EMPEROR])
    check('4: Burst + Omni Emperor fuse into a full +3 unit', wire_error(reply) is None, wire_error(reply))
    check('4: SP fills to the raised limit only', sp(full) == (25, 125), sp(full))

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
