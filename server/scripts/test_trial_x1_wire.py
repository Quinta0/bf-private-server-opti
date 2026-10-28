"""Trial No. X1 (8360021) wire test against a running server (ticket #36).

    python3 -I scripts/test_trial_x1_wire.py [--url http://127.0.0.1:9993]
        [--container bf-x1] [--db /save/gme.sqlite] [--tools <root>/tools]

The client's mission script swaps Cursed Estia 87500128 -> Princess Estia
87500129 -> Xenon & Estia 87500130 by id (MonsterParty::changeMonster), so
MissionStart must describe all three and place only the first.  Checks the
reply against deploy/archive/mission.json and ai.json, then a loss, the first
clear and a replay.  Runs on the Mac: it borrows the guest-account and
docker-exec SQL helpers of the root repo's tools/test_fair_summon.py, so the
save is never written from the host.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MISSION_ID = 8360021
SCRIPT_IDS = [87500128, 87500129, 87500130]   # F_MISSION_SCRIPT_MST row 8360021
START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
# F_MISSION_MST SiYs27Cj: a Gem, the Xentar sphere 818910 and Sphere Frog 20302.
FIRST_CLEAR = sorted([(8, '', 1), (7, '818910', 1), (6, '20302', 1)])


def find_tools(explicit):
    for candidate in ([Path(explicit)] if explicit else []) + [p / 'tools' for p in ROOT.parents]:
        if (candidate / 'test_fair_summon.py').is_file():
            return candidate
    raise SystemExit('tools/test_fair_summon.py not found; pass --tools')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9993')
    ap.add_argument('--container', default='bf-x1')
    ap.add_argument('--db', default='/save/gme.sqlite', help='save path inside the container')
    ap.add_argument('--tools')
    args = ap.parse_args()
    sys.path.insert(0, str(find_tools(args.tools)))
    from test_fair_summon import Database, Player, gme_call, wire_error  # noqa: E402

    rec = next(m for m in json.loads((ROOT / 'deploy/archive/mission.json').read_text(encoding='utf-8'))
               if m['id'] == MISSION_ID)
    ais = {a['id']: a for a in json.loads((ROOT / 'deploy/archive/ai.json').read_text(encoding='utf-8'))}
    described = {m['id']: m for st in rec['stages'] for m in st['battle_monsters']}
    described.update({m['id']: m for m in rec.get('script_monsters', [])})

    failures = 0

    def check(label, ok, detail=''):
        nonlocal failures
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  ' + str(detail)}")
        failures += not ok

    db = Database(None, args.container, args.db)
    player = Player(args.url, db)
    runs = {}

    def start():
        player.set(energy=500, energy_full_ts=0)
        reply = gme_call(args.url, *START, {'IKqx1Cn9': player.login, '9Q1Lq5FS': [{
            'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION_ID), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
            'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        if wire_error(reply) is None:
            runs['id'] = reply['Kz7qfSs5'][0]['k9cxD7Ba']
        return reply

    def end(status):
        return gme_call(args.url, *END, {'IKqx1Cn9': player.login,
                                         'Kz7qfSs5': [{'k9cxD7Ba': str(runs['id']), 'j3g5P4cq': str(status)}],
                                         'rXvA1E5y': [{'Najhr8m6': '0', 'HTVh8a65': '0', '3MAT6quo': '', '4T0Q2Bh5': ''}]})

    def snapshot():
        zel, exp, level = db.one('SELECT zel, exp, level FROM user_info WHERE id = ?', player.user_id)
        presents = db.rows('SELECT present_type, target_id, target_cnt FROM user_presents'
                           ' WHERE user_id = ? AND description = ? ORDER BY present_id',
                           player.user_id, 'First clear reward')
        clear = db.one('SELECT state, clear_count FROM user_campaign_missions WHERE user_id = ? AND mission_id = ?',
                       player.user_id, str(MISSION_ID))
        return {'zel': zel, 'exp': exp, 'level': level, 'presents': [tuple(p) for p in presents],
                'clear': tuple(clear) if clear else None}

    # --- the archive record has the client script's shape ----------------------
    check('archive: Cursed Estia opens alone, the other two forms are script_monsters',
          [[m['id'] for m in st['battle_monsters']] for st in rec['stages']] == [SCRIPT_IDS[:1]]
          and [m['id'] for m in rec.get('script_monsters', [])] == SCRIPT_IDS[1:])

    # --- MissionStart ---------------------------------------------------------------
    resp = start()
    check('MissionStart 8360021 succeeds', wire_error(resp) is None, wire_error(resp))
    if wire_error(resp) is not None:
        return 1
    battles = resp.get('pj41dy9g') or []
    placed = [g['o49dYfpH'] for g in resp.get('75t0sx9z') or []]
    rows = {int(m['o49dYfpH']): m for m in resp.get('U0v5IeJo') or []}
    check('one battle', [b['etM5TCb9'] for b in battles] == ['1'], battles)
    check('only Cursed Estia 87500128 is placed', placed == ['87500128'], placed)
    check('MonsterMst rows for 87500128, 87500129 and 87500130 (the swap targets), no stand-in 56853',
          sorted(rows) == SCRIPT_IDS, sorted(rows))
    for mid in SCRIPT_IDS:
        row, want = rows.get(mid, {}), described[mid]
        skills = '@'.join(f"{s['skill_id']}:1" for s in want['skills'])
        check(f'{mid} {want["name"]}: hp {want["hp"]:,}, unit {want["unit_id"]}, ai {want["ai_id"]}, its skills',
              (row.get('e7DK0FQT'), row.get('pn16CNah'), row.get('i74vGUFa'), row.get('F4bQ7r8C'))
              == (str(want['hp']), str(want['unit_id']), str(want['ai_id']), skills),
              {k: row.get(k) for k in ('e7DK0FQT', 'pn16CNah', 'i74vGUFa', 'F4bQ7r8C')})
    sent = {}
    for a in resp.get('89ausgc4') or []:
        sent.setdefault(int(a['4eEVw5hL']), []).append(int(a['yu18xScw']))
    for mid in SCRIPT_IDS:
        ai = ais[described[mid]['ai_id']]
        check(f"AI {ai['id']} ({ai['name']}): every row sent, in order",
              sent.get(ai['id']) == [r['priority'] for r in ai['actions']], sent.get(ai['id']))

    # --- pay: loss, first clear, replay (Research Lab pays the first clear only) ---
    s0 = snapshot()
    end(3)
    s1 = snapshot()
    check('a loss pays nothing and records no clear',
          (s1['zel'], s1['exp'], s1['presents'], s1['clear']) == (s0['zel'], s0['exp'], s0['presents'], None),
          f'{s0} -> {s1}')
    start()
    s0 = snapshot()
    reply = end(2)
    s1 = snapshot()
    new = sorted(s1['presents'][len(s0['presents']):])
    check(f"first clear: MissionEnd succeeds, {rec['zel']:,} Zel, EXP, Gem + Xentar sphere + Sphere Frog",
          wire_error(reply) is None and s1['zel'] - s0['zel'] == rec['zel']
          and (s1['level'], s1['exp']) != (s0['level'], s0['exp']) and new == FIRST_CLEAR and s1['clear'] == (2, 1),
          f'{wire_error(reply)} {s0} -> {s1}')
    start()
    s0 = snapshot()
    end(2)
    s1 = snapshot()
    check('a replay pays nothing', (s1['zel'], s1['exp'], s1['presents']) == (s0['zel'], s0['exp'], s0['presents'])
          and s1['clear'] == (2, 2), f'{s0} -> {s1}')

    print(f'\n{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
