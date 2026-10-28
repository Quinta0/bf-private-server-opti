"""Elgif / Extra Skills (ticket #38), across a deploy.

    python3 -I scripts/test_extra_skills_wire.py --phase seed  --state s.json [--url ...] [--container ...]
    (stop the old build, start the new one on the same save)
    python3 -I scripts/test_extra_skills_wire.py --phase check --state s.json [--url ...] [--container ...]

seed runs on the build BEFORE the change: a player who owns Sero-Anya and two
Elgifs (granted through presents, so through the old addUserUnit) and keeps one
Elgif present unclaimed.  check runs on the new build over the same save and
verifies the migration, then the wire:

  * every old Elgif (and the queued one) now carries a "Memories" skill;
  * UserInfo sends Sero-Anya's own Extra Skill 850627 in cP83zNsv;
  * UnitMix with an Elgif writes its skill into the base's LjY4DfRg, the
    Elgif is gone, the Zel is spent; a second Elgif replaces it;
  * a present naming a skill grants an Elgif with that skill, one naming none
    rolls a drop skill; the first clear of 107900 queues 1030000.

Edits to the save go through docker exec (tools/test_fair_summon.py's Database).
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ELGIF = '50792'
SERO_ANYA = '850627'           # unit_mst cP83zNsv 850627 (her own Extra Skill)
DROP_SKILLS = {1012700, 1012710, 1012720, 1012730, 1012740, 1012750, 1012760, 1012770,
               1012780, 1012790, 1015800, 1015810, 1015820, 1015830, 1015840}
NAMED_SKILL = 1030000          # "World Records", what mission 107900 names
MISSION = 107900               # SiYs27Cj '6:50792:1:1:1:1030000'
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
UNIT_MIX = ('Mw08CIg2', 'JnegC7RrN3FoW8dQ')
PRESENT_RECEIPT = ('bV5xa0ZW', 'X2QFqAKfomPIg3rG')
START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--phase', choices=['seed', 'check'], required=True)
    ap.add_argument('--state', required=True, help='JSON file the seed phase writes and check reads')
    ap.add_argument('--url', default='http://127.0.0.1:19971')
    ap.add_argument('--container', default='bf-esregen')
    ap.add_argument('--db', default='/save/gme.sqlite', help='save path inside the container')
    ap.add_argument('--tools')
    args = ap.parse_args()
    sys.path.insert(0, str(find_tools(args.tools)))
    from test_fair_summon import Database, Player, gme_call, wire_error  # noqa: E402

    db = Database(None, args.container, args.db)
    failures = 0

    def check(label, ok, detail=''):
        nonlocal failures
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  ' + str(detail)}")
        failures += not ok

    def call(player, action, body):
        return gme_call(args.url, *action, dict(body, IKqx1Cn9=player.login))

    def units(reply):
        return {u['edy7fq3L']: u for u in reply.get('4ceMWH6k') or []}

    def queue(player, unit_id, skill=None):
        cols = 'user_id, present_type, target_id, target_cnt, description'
        vals = [player.user_id, 6, unit_id, 1, 'es-regen test']
        if skill is not None:
            cols += ', unit_extra_skill'
            vals.append(skill)
        db.rows(f'INSERT INTO user_presents ({cols}) VALUES ({", ".join("?" * len(vals))})', *vals)
        return db.one('SELECT MAX(present_id) FROM user_presents WHERE user_id = ?', player.user_id)[0]

    def claim(player, present_id):
        before = {r[0] for r in db.rows('SELECT user_unit_id FROM user_units WHERE user_id = ?', player.user_id)}
        reply = call(player, PRESENT_RECEIPT, {'o6uWU0Z7': [{'i1WQkh4G': '0', 'S1B82FHK': str(present_id)}]})
        after = {r[0] for r in db.rows('SELECT user_unit_id FROM user_units WHERE user_id = ?', player.user_id)}
        return reply, sorted(after - before)

    if args.phase == 'seed':
        player = Player(args.url, db)
        granted = {}
        for unit_id in (SERO_ANYA, ELGIF, ELGIF):
            reply, new = claim(player, queue(player, unit_id))
            check(f'seed: present of unit {unit_id} claimed on the old build', wire_error(reply) is None and len(new) == 1,
                  wire_error(reply) or new)
            granted.setdefault(unit_id, []).extend(new)
        pending = queue(player, ELGIF)
        state = {'login': player.login, 'user_id': player.user_id, 'units': granted, 'pending_present': pending}
        Path(args.state).write_text(json.dumps(state))
        return 1 if failures else 0

    state = json.loads(Path(args.state).read_text())
    player = object.__new__(Player)
    player.base, player.db, player.login, player.user_id = args.url, db, state['login'], state['user_id']
    sero = str(state['units'][SERO_ANYA][0])
    elgifs = [str(u) for u in state['units'][ELGIF]]

    # --- #38: migration ------------------------------------------------------------------
    rows = db.rows('SELECT user_unit_id, add_extra_passive_skill_id FROM user_units WHERE user_id = ? AND unit_id = ?',
                   player.user_id, ELGIF)
    check('migration: both old Elgifs carry a drop skill', len(rows) == 2 and all(r[1] in DROP_SKILLS for r in rows), rows)
    queued = db.one('SELECT unit_extra_skill FROM user_presents WHERE present_id = ?', state['pending_present'])
    check('migration: the unclaimed Elgif present carries a drop skill', queued and queued[0] in DROP_SKILLS, queued)
    skills = {str(r[0]): r[1] for r in rows}

    sent = units(call(player, USER_INFO, {}))
    check(f'UserInfo: Sero-Anya carries her own Extra Skill {SERO_ANYA} (cP83zNsv)',
          sent.get(sero, {}).get('cP83zNsv') == SERO_ANYA, {k: sent.get(sero, {}).get(k) for k in ('cP83zNsv', 'LjY4DfRg', 'T4rewHa9')})
    check('UserInfo: each Elgif sends its skill in T4rewHa9',
          all(sent.get(e, {}).get('T4rewHa9') == str(skills[e]) for e in elgifs),
          {e: sent.get(e, {}).get('T4rewHa9') for e in elgifs})

    # --- #38: fusion ---------------------------------------------------------------------
    def fuse(material, zel=100):
        return call(player, UNIT_MIX, {'60subGk3': [{'81GjwoWy': '1', '2vnqRIr3': '2'}],
                                       'mCE3rUu5': [{'Rs7bCE3t': str(zel)}],
                                       'Km35HAXv': [{'edy7fq3L': sero, 'mnZ5K4Ii': '1'},
                                                    {'edy7fq3L': material, 'mnZ5K4Ii': '2'}]})

    def fusion_price(base, materials):
        # The server charges the client's own fusion price, whatever the request names
        # (GameUtils::getMixGold @0x1EAF2C0): (base lv + material lv) x 100 per material,
        # x0.25 when the base is at its max level.  Sero-Anya is far below it here.
        lv = {u: db.one('SELECT unit_lvl FROM user_units WHERE user_unit_id = ?', int(u))[0]
              for u in [base, *materials]}
        return sum((lv[base] + lv[m]) * 100 for m in materials)

    zel0 = db.one('SELECT zel FROM user_info WHERE id = ?', player.user_id)[0]
    price = fusion_price(sero, [elgifs[0]])
    reply = fuse(elgifs[0])
    base = units(reply).get(sero, {})
    check('UnitMix with an Elgif: base gets its skill in LjY4DfRg, Elgif gone, Zel spent',
          wire_error(reply) is None and base.get('LjY4DfRg') == str(skills[elgifs[0]])
          and elgifs[0] not in units(reply) and base.get('cP83zNsv') == SERO_ANYA
          and db.one('SELECT zel FROM user_info WHERE id = ?', player.user_id)[0] == zel0 - price,
          (wire_error(reply), {k: base.get(k) for k in ('cP83zNsv', 'LjY4DfRg')}))
    again = units(call(player, USER_INFO, {})).get(sero, {})
    check('UserInfo after the fusion still shows it (saved)', again.get('LjY4DfRg') == str(skills[elgifs[0]]),
          again.get('LjY4DfRg'))
    reply = fuse(elgifs[1])
    check('a second Elgif replaces the first skill',
          units(reply).get(sero, {}).get('LjY4DfRg') == str(skills[elgifs[1]]), units(reply).get(sero, {}).get('LjY4DfRg'))

    # --- #38: grants -----------------------------------------------------------------------
    for label, skill, ok in (('names 1030000', NAMED_SKILL, lambda s: s == NAMED_SKILL),
                             ('names none', None, lambda s: s in DROP_SKILLS),
                             ('queued before the deploy', 'pending', lambda s: s == queued[0])):
        pid = state['pending_present'] if skill == 'pending' else queue(player, ELGIF, skill)
        reply, new = claim(player, pid)
        got = db.one('SELECT add_extra_passive_skill_id FROM user_units WHERE user_unit_id = ?', new[0])[0] if new else None
        check(f'an Elgif present that {label} grants an Elgif with that skill', wire_error(reply) is None and ok(got),
              (wire_error(reply), got))

    def start():
        reply = call(player, START, {'9Q1Lq5FS': [{
            'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
            'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        return reply, (reply.get('Kz7qfSs5') or [{}])[0].get('k9cxD7Ba')

    player.set(energy=500, energy_full_ts=0)
    reply, run = start()
    if wire_error(reply) is not None:
        check(f'MissionStart {MISSION}', False, wire_error(reply))
    else:
        end = call(player, END, {'Kz7qfSs5': [{'k9cxD7Ba': str(run), 'j3g5P4cq': '2'}],
                                 'rXvA1E5y': [{'Najhr8m6': '0', 'HTVh8a65': '0', '3MAT6quo': '', '4T0Q2Bh5': ''}]})
        got = db.rows("SELECT target_id, unit_extra_skill FROM user_presents WHERE user_id = ? AND description = 'First clear reward'",
                      player.user_id)
        check(f'first clear of {MISSION} queues an Elgif carrying {NAMED_SKILL}',
              wire_error(end) is None and [tuple(r) for r in got] == [(ELGIF, NAMED_SKILL)], (wire_error(end), got))

    print('ALL PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
