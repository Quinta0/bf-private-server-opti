"""Brave Burst level from fusion (ticket #119), wire test against a running server.

    python3 -I scripts/test_bb_fusion_wire.py [--url http://127.0.0.1:9993]
        [--container bf-bb] [--db /data/gme.sqlite] [--tools <root>/tools]

A copy of the base unit (same evolution line) and an Omni Frog each raise the
burst level by 1; other fodder with a Brave Burst of its own may raise it on a
roll; BB-less fodder never does.  BB fills first, then SBB, each to its
skill's top level in SkillLevelMst (10).  Checks:
  1. One copy: BB 1 -> 2, in the database, the result screen (1ZbHB6Im
     8hVR4Fjr) and the unit cache (4ceMWH6k 3NbeC8AB).
  2. One Omni Frog: BB 2 -> 3.
  3. BB 9 / SBB 1 with two copies and an Omni Frog: BB 10, SBB 3 (0xgWq5bD).
  4. BB 10 / SBB 10 with a copy: stays 10 / 10.
  5. 30 BB-less fodder: BB unchanged.
  6. 60 same-element fodder with a BB: some rolls hit and some miss (the
     server's "N sure, M rolled" log line), and the levels moved by M.
Runs on the Mac with the guest-account and docker-exec SQL helpers of the root
repo's tools/test_fair_summon.py.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
OMNI_FROG = 750003
SPECIAL = {750003, 750004, 750005, 20302, 40432, 10452, 20442, 30432, 50612, 50792, 60812}


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
    ap.add_argument('--container', default='bf-bb')
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

    def has_bb(u):
        return u['nj9Lw7mV'] not in ('', '0')

    def plain(i, u):
        return i not in SPECIAL and u.get('PXD4v2KY', '0') in ('', '0') \
            and not 10010 <= int(u['xP9oi5Z2'] or 0) <= 10016

    # A fire unit with an SBB, a second form of it, fire fodder with a BB from
    # another line, and fire fodder with no BB at all.
    base = next(i for i, u in sorted(units.items()) if u['iNy0ZU5M'] == '1' and has_bb(u)
                and u['iEFZ6H19'] not in ('', '0') and plain(i, u)
                and sum(v['9PsmH7tz'] == u['9PsmH7tz'] for v in units.values()) > 1)
    line = units[base]['9PsmH7tz']
    copy = next(i for i, u in sorted(units.items()) if u['9PsmH7tz'] == line and i != base)
    fodder = next(i for i, u in sorted(units.items()) if u['iNy0ZU5M'] == '1' and has_bb(u)
                  and u['9PsmH7tz'] != line and plain(i, u))
    bbless = next(i for i, u in sorted(units.items()) if u['iNy0ZU5M'] == '1' and not has_bb(u)
                  and plain(i, u) and u['xP9oi5Z2'] == '2')
    print(f'base {base}, copy {copy}, fodder {fodder}, BB-less {bbless}')

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    uid = player.user_id
    player.set(zel=99_999_999)
    cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)')]
    template = dict(zip(cols, db.one('SELECT * FROM user_units WHERE user_id = ? LIMIT 1', uid)))
    template.pop('user_unit_id')

    def unit(mst, **kw):
        u = units[mst]
        row = dict(template, unit_id=str(mst), unit_lvl=1, total_exp=0, favorite_flg=0,
                   bb_id=u['nj9Lw7mV'], sbb_id=u['iEFZ6H19'],
                   bb_lvl=1 if has_bb(u) else 0, sbb_lvl=0, omni_level=0)
        row.update(kw)
        db.rows(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", *row.values())
        return db.one('SELECT max(user_unit_id) FROM user_units WHERE user_id = ?', uid)[0]

    def levels(u):
        return tuple(db.one('SELECT bb_lvl, sbb_lvl FROM user_units WHERE user_unit_id = ?', u))

    def fuse(base_id, mats):
        mat_ids = [unit(m) for m in mats]
        return gme_call(args.url, *UNIT_MIX, {
            'IKqx1Cn9': player.login,
            '60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}],
            'mCE3rUu5': [{'Rs7bCE3t': '0'}],
            'Km35HAXv': [{'edy7fq3L': str(base_id), 'mnZ5K4Ii': '1'}]
                        + [{'edy7fq3L': str(m), 'mnZ5K4Ii': '2'} for m in mat_ids]})

    def cached_bb(reply, base_id):
        roster = find_key(reply, '4ceMWH6k') or []
        row = next((r for r in roster if str(r.get('edy7fq3L')) == str(base_id)), {})
        return row.get('3NbeC8AB')

    a = unit(base, bb_lvl=1)
    reply = fuse(a, [copy])
    check('1: a copy fuses', wire_error(reply) is None, wire_error(reply))
    check('1: BB 1 -> 2', levels(a) == (2, 0), levels(a))
    check('1: result screen says BB 2', find_key(reply, '8hVR4Fjr') == '2', find_key(reply, '8hVR4Fjr'))
    check('1: unit cache says BB 2', cached_bb(reply, a) == '2', cached_bb(reply, a))

    reply = fuse(a, [OMNI_FROG])
    check('2: an Omni Frog fuses', wire_error(reply) is None, wire_error(reply))
    check('2: BB 2 -> 3', levels(a) == (3, 0), levels(a))

    b = unit(base, bb_lvl=9, sbb_lvl=1)
    reply = fuse(b, [copy, copy, OMNI_FROG])
    check('3: two copies and an Omni Frog fuse', wire_error(reply) is None, wire_error(reply))
    check('3: BB 10, SBB 3', levels(b) == (10, 3), levels(b))
    check('3: result screen says SBB 3', find_key(reply, '0xgWq5bD') == '3', find_key(reply, '0xgWq5bD'))

    c = unit(base, bb_lvl=10, sbb_lvl=10)
    reply = fuse(c, [copy])
    check('4: a copy fuses into a maxed unit', wire_error(reply) is None, wire_error(reply))
    check('4: stays 10 / 10', levels(c) == (10, 10), levels(c))

    d = unit(base, bb_lvl=1)
    reply = fuse(d, [bbless] * 30)
    check('5: 30 BB-less fodder fuse', wire_error(reply) is None, wire_error(reply))
    check('5: BB stays 1', levels(d) == (1, 0), levels(d))

    e = unit(base, bb_lvl=1, sbb_lvl=1)
    reply = fuse(e, [fodder] * 60)
    check('6: 60 same-element fodder fuse', wire_error(reply) is None, wire_error(reply))
    logs = subprocess.run(['docker', 'logs', '--tail', '400', args.container],
                          capture_output=True, text=True).stdout
    hits = re.findall(r'\((\d+) sure, (\d+) rolled\)', logs)
    sure, rolled = map(int, hits[-1]) if hits else (-1, -1)
    gained = sum(levels(e)) - 2
    check('6: no sure levels, some rolls hit, some missed', sure == 0 and 0 < rolled < 60, (sure, rolled))
    check('6: levels moved by the hits (capped at 10 / 10)', gained == min(rolled, 18), (levels(e), rolled))

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
