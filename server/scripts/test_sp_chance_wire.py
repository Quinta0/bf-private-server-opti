"""SPUP!? fusion SP chance, wire test against a running server.

    python3 -I scripts/test_sp_chance_wire.py --balance <src>/deploy/balance
        [--url http://127.0.0.1:9993] [--container bf-oe] [--db /data/gme.sqlite] [--tools <root>/tools]

An ordinary unit (unit_mst kind 0, with a BB) of another category fused into a
max-level, SBB 10, rarity 8 unit has a chance to give 1 SP ("SPUP!?" on the
fusion screen): fusion_sp_chance_pct + fusion_sp_chance_per_star_pct a star
above 1, from <balance_root>/server/tuning.json.  Checks:
  1. At 100%: five such units give +5 SP, and the reply's after-SP (3YVEeL2a) says so.
  2. At 100%, one SP under the limit: five units give +1.
  3. At 100%: Arton (kind 1, an imp) and an ineligible base (SBB 9) give nothing.
  4. At 0%: five units give nothing.
  5. No file (defaults 20% + 1%/star): 300 three-star units land within 25 of 66.
--balance is the server's balance_root on this machine; tuning.json is written
there and removed at the end.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
ARTON = 50612


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
    ap.add_argument('--balance', type=Path, required=True)
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
    fodder = next(i for i, u in sorted(units.items()) if u['xP9oi5Z2'] == '0' and u['nj9Lw7mV'] != '0'
                  and u['7ofj5xa1'] == '3' and u['9PsmH7tz'] != units[omni]['9PsmH7tz'])

    tuning = args.balance / 'server' / 'tuning.json'
    tuning.parent.mkdir(parents=True, exist_ok=True)

    def tune(pct=None):
        if pct is None:
            tuning.unlink(missing_ok=True)
        else:
            tuning.write_text(json.dumps({'fusion_sp_chance_pct': pct, 'fusion_sp_chance_per_star_pct': 0}))

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    uid = player.user_id
    player.set(zel=999_999_999)
    cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)')]
    template = dict(zip(cols, db.one('SELECT * FROM user_units WHERE user_id = ? LIMIT 1', uid)))
    template.pop('user_unit_id')

    def unit(mst=omni, **kw):
        row = dict(template, unit_id=str(mst), unit_lvl=int(units[mst]['EI1DF8Yt']), favorite_flg=0,
                   sbb_lvl=10, total_exp=99_999_999 if mst == omni else 0,
                   fe_sp=0, fe_used_sp=0, fe_max_sp=120)
        row.update(kw)
        db.rows(f"INSERT INTO user_units ({','.join(row)}) VALUES ({','.join('?' for _ in row)})", *row.values())
        return db.one('SELECT max(user_unit_id) FROM user_units WHERE user_id = ?', uid)[0]

    def sp(u):
        return db.one('SELECT fe_sp FROM user_units WHERE user_unit_id = ?', u)[0]

    def fuse(base, mats):
        return gme_call(args.url, *UNIT_MIX, {
            'IKqx1Cn9': player.login,
            '60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}],
            'mCE3rUu5': [{'Rs7bCE3t': '0'}],
            'Km35HAXv': [{'edy7fq3L': str(base), 'mnZ5K4Ii': '1'}]
                        + [{'edy7fq3L': str(unit(mst=m)), 'mnZ5K4Ii': '2'} for m in mats]})

    try:
        tune(100)
        base = unit()
        reply = fuse(base, [fodder] * 5)
        check('1: five SPUP!? units fuse', wire_error(reply) is None, wire_error(reply))
        check('1: +5 SP at 100%', sp(base) == 5, sp(base))
        check('1: reply after-SP is 5', find_key(reply, '3YVEeL2a') == '5', find_key(reply, '3YVEeL2a'))

        near = unit(fe_sp=19, fe_used_sp=100)
        fuse(near, [fodder] * 5)
        check('2: only +1 fits under the limit', sp(near) == 20, sp(near))

        imp = unit()
        reply = fuse(imp, [ARTON])
        check('3: an imp gives no SP', wire_error(reply) is None and sp(imp) == 0, (wire_error(reply), sp(imp)))
        low = unit(sbb_lvl=9)
        fuse(low, [fodder] * 5)
        check('3: SBB 9 base gains nothing', sp(low) == 0, sp(low))

        tune(0)
        zero = unit()
        fuse(zero, [fodder] * 5)
        check('4: 0% gives nothing', sp(zero) == 0, sp(zero))

        tune()
        many = unit()
        reply = fuse(many, [fodder] * 300)
        check('5: 300 units fuse at the defaults', wire_error(reply) is None, wire_error(reply))
        check('5: 22% of 300 lands near 66', abs(sp(many) - 66) <= 25, sp(many))
        print(f'      (300 three-star units gave {sp(many)} SP)')
    finally:
        tune()

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
