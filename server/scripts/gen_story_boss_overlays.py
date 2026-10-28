"""The Mistral/Cordelica story bosses and the Trial Abaddon, from the Global wiki
and the forum thread (missing-content: "Extra Bosses in Mistral + Cordelica", "Abbadon
regular and Trial"), as two ai.json AIs plus three battle-builder files.

  python3 scripts/gen_story_boss_overlays.py <server checkout> <out dir>

Appends AIs 900000140 (Skramya) and 900000141 (Grand Jelly) to deploy/archive/ai.json
(once) and writes 90.json and 20070.json for
<balance_root>/server/missions.  Boss HP for Grand Jelly is the wiki's 300,000; the
other stats are estimates for an operator to tune."""
import copy, json, sys
from pathlib import Path
src, out = Path(sys.argv[1]), Path(sys.argv[2])

def row(prio, pct, conds, kind='skill', flags=(), early=False):
    return {'priority': prio, 'percent': float(pct), 'act_target': 2, 'search_term': 'random',
            'self_conditions': [{'type': t, 'parameter': p} for t, p in conds], 'party_conditions': [],
            'action': {'type': kind, 'flag_changes': list(flags), 'unknown_bool': not early,
                       'unknown_int_1': 0, 'unknown_int_2': 0}}

# Skramya: skills 1 Call Bind, 2 To a bright new world!, 3 Frozen Pillar, 4 Snow Flake.
SKRAMYA = {'id': 900000140, 'name': 'wiki Skramya (Asleep in the Ice)', 'actions': [
    row(1, 100, [('skill', 2), ('hp_pr_under', 25), ('limited_act', 1)], early=True),   # the <25% special
    row(2, 30, [('skill', 1)]), row(3, 30, [('skill', 4)]), row(4, 25, [('skill', 3)]),
    row(5, 100, [], kind='attack')]}
# Grand Jelly: 1 Liquid Solution, 2 Drink it down., 3 Decomposition, 4 Extreme Softening,
# 5 Jiggly, 6 Violently Jiggly, 7 Melt the World.
JELLY = {'id': 900000141, 'name': 'wiki Grand Jelly (The Amorphus Being)', 'actions': [
    row(1, 100, [('skill', 7), ('actbetween', 9), ('turn_limited_act', 1)], early=True),
    row(2, 100, [('skill', 6), ('actbetween', 8), ('turn_limited_act', 1)], early=True),
    row(3, 100, [('skill', 5), ('actbetween', 7), ('turn_limited_act', 1)], early=True),
    row(4, 100, [('skill', 5), ('actbetween', 6), ('turn_limited_act', 1)], early=True),
    row(5, 100, [('skill', 4), ('hp_pr_under', 50), ('limited_act', 1)], early=True),
    row(6, 20, [('skill', 3)]), row(7, 20, [('skill', 2)]), row(8, 60, [('skill', 1)]),
    row(9, 100, [], kind='attack')]}

ai_path = src / 'deploy/archive/ai.json'
text = ai_path.read_text(encoding='utf-8')
ais = json.loads(text)
assert json.dumps(ais, indent=4, ensure_ascii=False) + ('\n' if text.endswith('\n') else '') == text, 'ai.json format'
have = {a['id'] for a in ais}
fresh = [a for a in (SKRAMYA, JELLY) if a['id'] not in have]   # idempotent: added once
if fresh:
    ai_path.write_text(json.dumps(ais + fresh, indent=4, ensure_ascii=False)
                       + ('\n' if text.endswith('\n') else ''), encoding='utf-8')

records = {r['id']: r for r in json.loads((src / 'deploy/archive/mission.json').read_text())}
skills = {int(r['nj9Lw7mV']) for r in json.loads((src / 'deploy/mst/skill_mst.json').read_text())[next(iter(json.loads((src / 'deploy/mst/skill_mst.json').read_text())))]}

def visuals(n, element):
    return {'img_a': f'monster_anime_{n:04d}.png', 'anm_cgg': f'monster_cgg_{n:04d}.csv',
            'cgs_idle': f'monster_idle_cgs_{n:04d}.csv', 'cgs_move': '', 'cgs_atk': f'monster_atk_cgs_{n:04d}.csv',
            'cgs_skill': f'monster_atk_cgs_{n:04d}.csv', 'element': element, 'effect_frame': '24:124:1',
            'damage_frame': '30:100:2:1', 'drop_check_count': 1, 'move_speed': 2, 'attack_move_type': 1,
            'back_move_type': 1, 'after_image': 1, 'skill_move_type': 1, 'cursor_disp_pos': '0,-40',
            'hp_disp_pos': '0,-60'}

def boss(mid, name, mon_id, art, element, hp, atk, df, ai_id, skill_ids, resists=None):
    rec = copy.deepcopy(records[mid])
    stage = next(s for s in rec['stages'] if s.get('is_boss'))
    m = {'id': mon_id, 'name': name, 'unit_id': 0, 'visuals': visuals(art, element), 'position': '160:302',
         'hp': hp, 'atk': atk, 'def': df, 'ai_id': ai_id, 'act_min': 1, 'act_max': 2, 'wait': 5,
         'unit_drop_id': 0, 'unit_drop_level': 0, 'unit_drop_type': 0, 'unit_drop_chance': 0,
         'authored_capture': True, 'zel_max_drop': 0, 'zel_drop_count': 0, 'karma_max_drop': 0,
         'karma_drop_count': 0, 'treasure_chest_chance': 0, 'treasure_drops': [],
         'skills': [{'skill_id': s, 'show_name': True} for s in skill_ids]}
    if resists:
        m['ailment_resists'] = resists
    assert all(s in skills for s in skill_ids), skill_ids
    stage['battle_monsters'] = [m]
    return rec

def trial_abaddon():
    rec = copy.deepcopy(records[8382000])
    stage = next(s for s in rec['stages'] if s.get('is_boss'))
    ab = next(m for m in stage['battle_monsters'] if m['name'] == 'Demon Abaddon')
    ab['ai_id'] = 20000022            # the original "Trial No. 003 - Demon Abaddon" AI
    # 1-3 random: Banishment, Necro Curse, Black Force; 4 every 4 turns: Evil Hole; 5 below 50%: Fool's Chains
    ab['skills'] = [{'skill_id': s, 'show_name': True} for s in (1954, 1955, 1952, 1953, 1950)]
    ab['ailment_resists'] = {'poison': 80, 'weak': 100, 'sick': 100, 'injury': 100, 'curse': 100, 'paralysis': 100}
    return rec

out.mkdir(parents=True, exist_ok=True)
built = {
    90: boss(90, 'Skramya', 2400, 24, 2, 150000, 1500, 600, SKRAMYA['id'], (9170, 9171, 9172, 9173)),
    20070: boss(20070, 'Grand Jelly', 2500, 25, 3, 300000, 1200, 500, JELLY['id'],
                (9180, 9181, 9182, 9183, 9184, 9185, 9186)),
    # 8382000 (Trial 03) is no longer written: the archive carries the 2014 capture of it
    # (gen_captured_missions --replace-ids), which beats this reconstruction.
}
for rec in built.values():
    # Battle files allow chest weights 1-1000 (MissionOverlay); some archive waves carry 10000.
    for stage in rec['stages']:
        for mon in stage['battle_monsters']:
            for drop in mon.get('treasure_drops') or []:
                if 'weight' in drop:
                    drop['weight'] = max(1, min(1000, drop['weight']))
for mid, rec in built.items():
    (out / f'{mid}.json').write_text(json.dumps(rec, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print(mid, rec['name'], [(m['name'], m['hp'], m['ai_id'], len(m.get('skills') or [])) for s in rec['stages'] if s.get('is_boss') for m in s['battle_monsters']])
