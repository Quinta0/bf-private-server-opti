"""Author the Summoners' Research Lab trials into the mission and AI archives.

Idempotent: rerunning replaces the same records.  `--check` reports whether the
files already match without writing anything.

    python scripts/gen_research_lab.py [--check]

Built: Trial No. 001 (2000000), Trial No. 002 (2000001), Trial No. 003
(2000002) and The Creation God (2100004).  Evidence, authored choices and
gaps: docs/features/TRIAL_003.md, docs/features/RESEARCH_LAB.md and
docs/features/CREATION_GOD.md.  The AI grammar is documented in
packet-generator/assets/archive/ai.kdl; rows are evaluated in list order and
`priority` is only a resume key.

Sources (Global wiki, read 2026-09-25 through the MediaWiki API):
  * Trial No. 001 rev 661563, Trial No. 002 rev 661687, Trial No. 003 rev
    661740, Template:Maxwell Turns rev 502228, Summoners' Research Lab rev
    653535; The Creation God rev 663846 and Template:MaxwellSZTurns rev
    535212 (read 2026-09-26).
  * The client's own F_MISSION_SCRIPT_MST (LocalState Ver288_F2Dz3QHU.dat):
    2000000 opens on Brave Knight Karl 22651 and swaps in Ice Warrior Karl
    22701 below 50%; 2000001 is Grahdens 63051 alone with dialogue at the
    start, 40% and 1%; 2000002 swaps Juggernaut 1002 -> Demon Abaddon 1102 ->
    Creator Maxwell 54501 below 1% each.
  * deploy/mst/skill_mst.json + skill_level_mst.json: each boss's original
    skill band, matched to the wiki by hit count and parameters.

AILMENTS.  Where the wiki lists what CAN be inflicted ("May be inflicted with
Paralysis"), every unlisted ailment is authored as an immunity (100) and a
"very/highly resistant" one as 80.  The slot order is binary-confirmed (see
AilmentResists in archive/mission.kdl).  A boss the wiki says nothing about
keeps 0 across the board.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MISSIONS = ROOT / 'deploy/archive/mission.json'
AIS = ROOT / 'deploy/archive/ai.json'
MISSION_MST = ROOT / 'deploy/mst/mission_mst.json'

AILMENTS = ('poison', 'weak', 'sick', 'injury', 'curse', 'paralysis')


def cond(term, n):
    return {'type': term, 'parameter': n}


def row(priority, self_conditions, action='skill', percent=100.0, target=2,
        counts=True, flags=(), search='random', party=()):
    return {
        'priority': priority,
        'percent': percent,
        'act_target': target,
        'search_term': search,
        'self_conditions': self_conditions,
        'party_conditions': list(party),
        'action': {
            'type': action,
            'flag_changes': list(flags),
            # JSON keys are historical (see ai.kdl): counts_as_action,
            # move_wait_frames, end_wait_frames.
            'unknown_bool': counts,
            'unknown_int_1': 0,
            'unknown_int_2': 0,
        },
    }


def opposing(term):
    """Party condition: any unit on the player's side passes `term`.

    target_id 5 is the opposing party; a target_parameter other than all /
    aliveall means one unit suffices (checkAiActionTerm @0x112BFD0).
    """
    return {'target_id': 5, 'target_parameter': 'non', 'type': term, 'parameters': 'non'}


def susceptible_only(*ailments, resisted=None):
    """Immune to everything except the listed ailments (wiki susceptibility)."""
    out = {a: (0 if a in ailments else 100) for a in AILMENTS}
    if resisted:
        out.update(resisted)
    return out


def monster(mid, name, unit_id, position, hp, ai_id, act_max, skill_ids,
            act_rate=None, resists=None, visuals=None, act_min=1):
    m = {
        'id': mid,
        'name': name,
        'unit_id': unit_id,
        'position': position,
        # ATK/DEF are undocumented for every lab boss; all keep the 2000/800 the
        # archive's other Maxwell-era bosses use.  AUTHORED.
        'hp': hp,
        'atk': 2000,
        'def': 800,
        'ai_id': ai_id,
        'act_min': act_min,
        'act_max': act_max,
        'wait': 5,
        'unit_drop_id': 0,
        'unit_drop_level': 0,
        'unit_drop_type': 0,
        'unit_drop_chance': 0,
        'authored_capture': True,
        'zel_max_drop': 0,
        'zel_drop_count': 0,
        'karma_max_drop': 0,
        'karma_drop_count': 0,
        'treasure_chest_chance': 0,
        'treasure_drops': [],
        'skills': [{'skill_id': i, 'show_name': True} for i in skill_ids],
    }
    if act_rate is not None:
        m['act_rate'] = act_rate
    if resists:
        m['ailment_resists'] = {k: resists.get(k, 0) for k in AILMENTS}
    if visuals:
        m['visuals'] = visuals
    return m


def mission(mid, name, zel, exp, opening, scripted=(), battles=None):
    """`battles`, when given, is one opening monster per battle (stage)."""
    openings = battles if battles is not None else [opening]
    last = len(openings) - 1
    record = {
        'id': mid,
        'name': name,
        'zel': zel,
        'karma': 0,
        'exp': exp,
        'energy_cost': 50,
        'stages': [{'is_boss': i == last, 'first_attack_rate': 0, 'battle_monsters': [m]}
                   for i, m in enumerate(openings)],
    }
    if scripted:
        record['script_monsters'] = list(scripted)
    return record


def story_visuals(missions, mission_id, monster_id):
    for stage in missions[mission_id]['stages']:
        for m in stage['battle_monsters']:
            if m['id'] == monster_id:
                return m['visuals']
    raise SystemExit(f'mission {mission_id} no longer carries monster {monster_id}')


# ---------------------------------------------------------------------------
# Trial No. 001 -- Brave Knight Karl, then Ice Warrior Karl.  Wiki: BKK has
# 400,000 HP and "only half" must go (the script swaps him below 50%, full HP
# again as IWK, 500,000).  Unit-backed on the mock units' own sprites.
# ---------------------------------------------------------------------------
def trial_001(_missions):
    # BKK: 1 Frozen Axe 6140, 2 Flood Offensive 6143, 3 Verdant Axe 6141,
    # 4 Lightning Axe 6142.  Frozen Axe every 5 turns; below 75% Flood
    # Offensive (read as once, a self-buff) and the two axes.
    bkk_ai = {'id': 20000001, 'name': 'Trial No. 001 - Brave Knight Karl', 'actions': [
        row(1, [cond('skill', 1), cond('actbetween', 5), cond('turn_limited_act', 1)]),
        row(2, [cond('skill', 2), cond('hp_pr_under', 75), cond('limited_act', 1)], target=1, counts=False),
        row(3, [cond('skill', 3), cond('hp_pr_under', 75)], percent=35.0),
        row(4, [cond('skill', 4), cond('hp_pr_under', 75)], percent=35.0),
        row(5, [], action='attack'),
    ]}
    # IWK: 1 Verdant Axe 6141, 2 Lightning Axe 6142, 3 Flashing Axe Combo
    # 6151, 4 Blue Execution 6154, 5 Cursed Blue Slash 6150, 6 Tidal
    # Offensive 6152.  Below 75% Flashing Axe; below 50% Blue Execution (read
    # as once, it is the 500% nuke) and Cursed Blue Slash; below 15% Tidal
    # Offensive (once, a self-buff).
    iwk_ai = {'id': 20000002, 'name': 'Trial No. 001 - Ice Warrior Karl', 'actions': [
        row(1, [cond('skill', 6), cond('hp_pr_under', 15), cond('limited_act', 1)], target=1, counts=False),
        row(2, [cond('skill', 4), cond('hp_pr_under', 50), cond('limited_act', 1)]),
        row(3, [cond('skill', 5), cond('hp_pr_under', 50)], percent=30.0),
        row(4, [cond('skill', 3), cond('hp_pr_under', 75)], percent=30.0),
        row(5, [cond('skill', 1)], percent=30.0),
        row(6, [cond('skill', 2)], percent=30.0),
        row(7, [], action='attack'),
    ]}
    bkk = monster(22651, 'Brave Knight Karl', 20233, '180:302', 400_000, bkk_ai['id'], 2,
                  (6140, 6143, 6141, 6142), act_rate=50.0,
                  resists=susceptible_only('poison'))            # "Can be inflicted with Poison"
    # Position unused: the script swaps him in at (0,0) = where BKK stands.
    iwk = monster(22701, 'Ice Warrior Karl', 20234, '180:302', 500_000, iwk_ai['id'], 2,
                  (6141, 6142, 6151, 6154, 6150, 6152), act_rate=50.0,
                  resists=susceptible_only('paralysis', 'injury'))  # "...Paralysis and Injury"
    record = mission(2000000, 'Trial No. 001', 500_000, 20_000, bkk, [iwk])
    return record, [bkk_ai, iwk_ai], '8:0:1:0:0,6:20233:1:0:0'


# ---------------------------------------------------------------------------
# Trial No. 002 -- Grahdens alone, a long HP ladder (wiki rev 661687).
# ---------------------------------------------------------------------------
def trial_002(_missions):
    # 1 Ignite Heaven 7410, 2 Cursed Blue Slash 7320, 3 Ground Rock 7430,
    # 4 Lightning Volcano 7420, 5 Soul Rejection 7310, 6 "I'm tired. Time for a
    # break." 7440, 7 Sacred Change 7380, 8 Life Shower 7400, 9 Holy Light
    # 7340, 10 Darkness Change 7381, 11 Sharp Gaze 7350, 12 Light Attack 7450,
    # 13 Vanishing Wave 7360, 14 Apocalypse 7370, 15 Instant Barrage 7330,
    # 16 Steel Fortification 7390, 17 Apocalypse Zero 7371.
    #
    # One action a turn, so each threshold move takes its turn and several
    # thresholds crossed together play out one per turn in threshold order
    # (AUTHORED precedence).  Buffs and heals are free.  "After 5 turns" is
    # the break: 7440 once on turn 6 (or the next multiple of 6 it can act on)
    # while HP is still 70% or more; it raises flag 2, which opens the
    # after-break pool.  Below 1%: Apocalypse Zero, then two idle turns.
    ai = {'id': 20000011, 'name': 'Trial No. 002 - Grahdens', 'actions': [
        row(1, [cond('skill', 17), cond('hp_pr_under', 1), cond('limited_act', 1)]),
        row(2, [cond('hp_pr_under', 1), cond('limited_act', 2)], action='wait'),
        row(3, [cond('skill', 5), cond('act', 1), cond('limited_act', 1)]),
        row(4, [cond('skill', 6), cond('actbetween', 6), cond('hp_pr_over', 70), cond('limited_act', 1)],
            flags=(2, 1)),
        row(5, [cond('skill', 10), cond('hp_pr_under', 80), cond('limited_act', 1)]),
        row(6, [cond('skill', 5), cond('hp_pr_under', 70), cond('limited_act', 1)]),
        row(7, [cond('skill', 14), cond('hp_pr_under', 40), cond('limited_act', 1)]),
        row(8, [cond('skill', 15), cond('hp_pr_under', 30), cond('limited_act', 1)], target=1, counts=False),
        row(9, [cond('skill', 8), cond('hp_pr_under', 20), cond('limited_act', 1)], target=1, counts=False),
        row(10, [cond('skill', 16), cond('hp_pr_under', 20), cond('limited_act', 1)], target=1, counts=False),
        row(11, [cond('skill', 7), cond('hp_pr_under', 15), cond('limited_act', 1)]),
        row(12, [cond('skill', 8), cond('hp_pr_under', 5), cond('limited_act', 1)], target=1, counts=False),
        # Random pools.  Percentages AUTHORED.
        row(13, [cond('skill', 11), cond('hp_pr_under', 40)], percent=20.0),
        row(14, [cond('skill', 12), cond('hp_pr_under', 40)], percent=20.0),
        row(15, [cond('skill', 13), cond('hp_pr_under', 40)], percent=15.0),
        row(16, [cond('skill', 9), cond('flg_on', 2)], percent=20.0),
        row(17, [cond('skill', 7), cond('flg_on', 2)], percent=15.0),
        row(18, [cond('skill', 8), cond('flg_on', 2)], percent=10.0, target=1),
        row(19, [cond('skill', 1)], percent=25.0),
        row(20, [cond('skill', 2)], percent=25.0),
        row(21, [cond('skill', 3)], percent=25.0),
        row(22, [cond('skill', 4)], percent=25.0),
        row(23, [], action='attack'),
    ]}
    grahdens = monster(63051, 'Grahdens', 60324, '180:302', 500_000, ai['id'], 1,
                       (7410, 7320, 7430, 7420, 7310, 7440, 7380, 7400, 7340, 7381,
                        7350, 7450, 7360, 7370, 7330, 7390, 7371))
    record = mission(2000001, 'Trial No. 002', 1_000_000, 30_000, grahdens)
    return record, [ai], '8:0:1:0:0,6:60324:1:0:0'


# ---------------------------------------------------------------------------
# Trial No. 003 -- Juggernaut -> Demon Abaddon -> Creator Maxwell.
# See docs/features/TRIAL_003.md.
# ---------------------------------------------------------------------------
def trial_003(missions):
    # Juggernaut -- 1 Magnetron 1902, 2 Graviton 1904, 3 Neoplasma 1903.
    juggernaut_ai = {'id': 20000021, 'name': 'Trial No. 003 - Juggernaut', 'actions': [
        row(1, [cond('skill', 3), cond('actbetween', 4), cond('turn_limited_act', 1)]),
        row(2, [cond('skill', 1)], percent=30.0),
        row(3, [cond('skill', 2)], percent=30.0),
        row(4, [], action='attack'),
    ]}
    # Demon Abaddon -- 1 Banishment 1954, 2 Necro Curse 1955, 3 Black Force
    # 1952, 4 Evil Hole 1953, 5 Death Gate 1956.  Death Gate x2 below 50% and
    # x4 below 30%, each once, as free-action bursts; AUTHORED PRECEDENCE: the
    # 30% burst sets flag 1, which retires an unused 50% burst.
    abaddon_ai = {'id': 20000022, 'name': 'Trial No. 003 - Demon Abaddon', 'actions': [
        row(1, [cond('skill', 5), cond('hp_pr_under', 30), cond('limited_act', 4)],
            counts=False, flags=(1, 1)),
        row(2, [cond('skill', 5), cond('hp_pr_under', 50), cond('limited_act', 2), cond('flg_off', 1)],
            counts=False),
        row(3, [cond('skill', 4), cond('actbetween', 4), cond('turn_limited_act', 1)]),
        row(4, [cond('skill', 1)], percent=30.0),
        row(5, [cond('skill', 2)], percent=30.0),
        row(6, [cond('skill', 3)], percent=30.0),
        row(7, [], action='attack'),
    ]}
    # Creator Maxwell -- 1 Genesis 2052, 2 Rune 2056, 3 Sacred Song 2054,
    # 4 Destiny 2059, 5 Meteor 2055, 6 Resurrection 2057, 7 Endless 2058.
    maxwell_ai = {'id': 20000023, 'name': 'Trial No. 003 - Creator Maxwell', 'actions': [
        row(1, [cond('skill', 4), cond('actbetween', 5), cond('hp_pr_under', 30), cond('turn_limited_act', 2)],
            counts=False),
        row(2, [cond('skill', 4), cond('actbetween', 5), cond('hp_pr_over', 30), cond('turn_limited_act', 1)],
            counts=False),
        row(3, [cond('skill', 7), cond('hp_pr_under', 20), cond('limited_act', 1)]),
        row(4, [cond('skill', 3), cond('actbetween', 4), cond('turn_limited_act', 1)], target=1, counts=False),
        row(5, [cond('skill', 2), cond('actbetween', 4), cond('turn_limited_act', 1)], counts=False),
        row(6, [cond('actbetween', 4)], action='turn_end'),
        row(7, [cond('actbetween', 5)], action='turn_end'),
        row(8, [cond('skill', 1), cond('hp_pr_under', 30), cond('limited_act', 1)]),
        row(9, [cond('skill', 6), cond('hp_pr_under', 50)], percent=30.0),
        row(10, [cond('skill', 5), cond('hp_pr_under', 70)], percent=30.0),
        row(11, [cond('skill', 1)], percent=40.0),
        row(12, [], action='attack'),
    ]}
    juggernaut = monster(1002, 'Juggernaut', 0, '180:340', 500_000, juggernaut_ai['id'], 2,
                         (1902, 1904, 1903), act_rate=50.0,
                         resists=susceptible_only('paralysis'),   # "May be inflicted with Paralysis"
                         visuals=story_visuals(missions, 85, 1000))
    # Positions of the two scripted monsters are placeholders: changeMonster
    # takes the script's own coordinates (70,187 and 90,151).
    abaddon = monster(1102, 'Demon Abaddon', 0, '140:374', 500_000, abaddon_ai['id'], 2,
                      (1954, 1955, 1952, 1953, 1956), act_rate=50.0,
                      resists=susceptible_only('poison', resisted={'poison': 80}),  # "highly resistant, not immune"
                      visuals=story_visuals(missions, 265, 1100))
    maxwell = monster(54501, 'Creator Maxwell', 50525, '180:302', 1_000_000, maxwell_ai['id'], 1,
                      (2052, 2056, 2054, 2059, 2055, 2057, 2058),
                      resists=susceptible_only('weak', resisted={'weak': 80}))     # "very resistant"
    record = mission(2000002, 'Trial No. 003', 2_000_000, 50_000, juggernaut, [abaddon, maxwell])
    return record, [juggernaut_ai, abaddon_ai, maxwell_ai], '8:0:1:0:0,6:50525:1:0:0'


# ---------------------------------------------------------------------------
# The Creation God (Strategy Zone, 2100004) -- wiki rev 663846 and
# Template:MaxwellSZTurns rev 535212.  See docs/features/CREATION_GOD.md.
#
# The client's F_MISSION_SCRIPT_MST row 2100004 (monsters "1104,5000851"):
#   battle 1  Juggernaut 1004 is made insensitive at the start; below 1% it
#             is swapped for Demon Abaddon 1104 at (70,187).
#   battle 2  Creator Maxwell 54508 is made insensitive; dialogue below 70%;
#             below 50% she is swapped in place for Inception God Maxwell
#             5000851 and the BGM changes; dialogue below 90/50/15%.
# MissionMst gives two battles, so the swapped-in forms are script_monsters.
# Skills are the re-issue band 5001050-5001094, which matches this page line
# for line (Magnetism's -70% BB, Meteor's 50,000, Code Meteor's 55,000,
# Destiny's 250%, Death Gate's 700%).  The swap keeps the party's turn count,
# which is what the wiki's "count starts at the beginning of the fight" and
# "turn counting from the Creator Maxwell still applies" describe.
# ---------------------------------------------------------------------------
def creation_god(missions):
    # Juggernaut -- 1 Magnetron 5001050, 2 Graviton 5001052, 3 Magnetism
    # 5001053, 4 Neoplasma Blast 5001051 (every 4 turns).
    juggernaut_ai = {'id': 20000031, 'name': 'The Creation God - Juggernaut', 'actions': [
        row(1, [cond('skill', 4), cond('actbetween', 4), cond('turn_limited_act', 1)]),
        row(2, [cond('skill', 3)], percent=25.0),
        row(3, [cond('skill', 1)], percent=30.0),
        row(4, [cond('skill', 2)], percent=30.0),
        row(5, [], action='attack'),
    ]}
    # Demon Abaddon -- 1 Fool's Chains 5001060, 2 Black Hole 5001061, 3 Black
    # Force 5001062, 4 Evil Hole 5001063, 5 Necro Curse 5001065, 6 Banishment
    # 5001064 (every 3 turns), 7 Death Gate 5001066 (four below 66%, four
    # below 30%).  Bursts are free actions; AUTHORED PRECEDENCE as in Trial
    # 003: the 30% burst sets flag 1, which retires an unused 66% burst.
    abaddon_ai = {'id': 20000032, 'name': 'The Creation God - Demon Abaddon', 'actions': [
        row(1, [cond('skill', 7), cond('hp_pr_under', 30), cond('limited_act', 4)],
            counts=False, flags=(1, 1)),
        row(2, [cond('skill', 7), cond('hp_pr_under', 66), cond('limited_act', 4), cond('flg_off', 1)],
            counts=False),
        row(3, [cond('skill', 6), cond('actbetween', 3), cond('turn_limited_act', 1)]),
        row(4, [cond('skill', 4)], percent=20.0),
        row(5, [cond('skill', 5)], percent=20.0),
        row(6, [cond('skill', 3)], percent=20.0),
        row(7, [cond('skill', 2)], percent=20.0),
        row(8, [cond('skill', 1)], percent=25.0),
        row(9, [], action='attack'),
    ]}
    # Creator Maxwell -- 1 Genesis 5001070, 2 Rune 5001074, 3 Sacred Song
    # 5001072, 4 Destiny 5001077, 5 Meteor 5001073, 6 Resurrection 5001075,
    # 7 Endless 5001076.  Destiny every 5 turns and twice on every 10th: the
    # /5 row fires once, the /10 row adds the second (a lone "twice" row would
    # also pass on 5, 15, 25).  Sacred Song + Rune every 4 turns.  Those turns
    # are spent on the schedule (turn_end), so Endless (<70%, once) and
    # Resurrection (<75%, once) wait for the next ordinary turn.
    creator_ai = {'id': 20000033, 'name': 'The Creation God - Creator Maxwell', 'actions': [
        row(1, [cond('skill', 4), cond('actbetween', 5), cond('turn_limited_act', 1)], counts=False),
        row(2, [cond('skill', 4), cond('actbetween', 10), cond('turn_limited_act', 1)], counts=False),
        row(3, [cond('skill', 3), cond('actbetween', 4), cond('turn_limited_act', 1)], target=1, counts=False),
        row(4, [cond('skill', 2), cond('actbetween', 4), cond('turn_limited_act', 1)], counts=False),
        row(5, [cond('actbetween', 4)], action='turn_end'),
        row(6, [cond('actbetween', 5)], action='turn_end'),
        row(7, [cond('skill', 7), cond('hp_pr_under', 70), cond('limited_act', 1)]),
        row(8, [cond('skill', 6), cond('hp_pr_under', 75), cond('limited_act', 1)]),
        row(9, [cond('skill', 5)], percent=30.0),
        row(10, [cond('skill', 1)], percent=50.0),
        row(11, [], action='attack'),
    ]}
    # Inception God Maxwell -- 1 Primal Genesis 5001080, 2 Code Meteor
    # 5001085, 3 Verse Genesis 5001083, 4 Sacred Anthem 5001084, 5 Akashic
    # Rune 5001086, 6 Destiny Rune 5001087, 7 Overdrive 5001094, 8 Existence
    # 5001082, 9 Return to the Origin 5001090, 10 Rebirth's Beginnings
    # 5001091, 11 Destruction and Creation 5001092, 12 Ultimate Creation
    # 5001093.  Flags: 1 = Overdrive cast (Existence next), 2 = answering a
    # UBB this turn, 11/12 = first/second UBB answered.
    #
    #  * Overdrive (<15%, once) and the Existence turn after it come first and
    #    use the only counted action, so both turns cancel everything else.
    #  * UBB: `ubb_use` on the opposing party passes when a player unit's last
    #    action was a UBB (BattleUnit::exitAction keeps the used skill until
    #    that unit acts again; initDead clears it).  Return to the Origin every
    #    time, then Rebirth's Beginnings / Destruction and Creation / Ultimate
    #    Creation for the first / second / third.  The Existence turn is
    #    already spent, which is the wiki's "other than on the Overdrive turn".
    #  * Destiny Rune every 5 turns on the highest-HP unit, and a second on
    #    every 10th on the lowest (the wiki's "lowest & highest").
    #  * Primal Genesis every turn (free), then one action: Verse Genesis
    #    below 50%, Code Meteor, or an attack.
    inception_ai = {'id': 20000034, 'name': 'The Creation God - Inception God Maxwell', 'actions': [
        row(1, [cond('skill', 8), cond('flg_on', 1), cond('limited_act', 1)], flags=(1, 0)),
        row(2, [cond('skill', 7), cond('hp_pr_under', 15), cond('limited_act', 1)], target=1, flags=(1, 1)),
        row(3, [cond('skill', 9), cond('turn_limited_act', 1)], counts=False, flags=(2, 1),
            party=[opposing('ubb_use')]),
        row(4, [cond('skill', 10), cond('flg_on', 2), cond('limited_act', 1)], target=1, counts=False,
            flags=(2, 0, 11, 1), party=[opposing('ubb_use')]),
        row(5, [cond('skill', 11), cond('flg_on', 2), cond('flg_on', 11), cond('limited_act', 1)], target=1,
            counts=False, flags=(2, 0, 12, 1), party=[opposing('ubb_use')]),
        row(6, [cond('skill', 12), cond('flg_on', 2), cond('flg_on', 12), cond('limited_act', 1)], target=1,
            counts=False, flags=(2, 0), party=[opposing('ubb_use')]),
        row(7, [cond('skill', 6), cond('actbetween', 5), cond('turn_limited_act', 1)], counts=False,
            search='hp_max'),
        row(8, [cond('skill', 6), cond('actbetween', 10), cond('turn_limited_act', 1)], counts=False,
            search='hp_min'),
        row(9, [cond('skill', 4), cond('actbetween', 4), cond('turn_limited_act', 1)], target=1, counts=False),
        row(10, [cond('skill', 5), cond('actbetween', 4), cond('turn_limited_act', 1)], counts=False),
        row(11, [cond('skill', 1), cond('turn_limited_act', 1)], counts=False),
        row(12, [cond('skill', 3), cond('hp_pr_under', 50)], percent=50.0),
        row(13, [cond('skill', 2)], percent=30.0),
        row(14, [], action='attack'),
    ]}
    juggernaut = monster(1004, 'Juggernaut', 0, '180:340', 4_000_000, juggernaut_ai['id'], 2,
                         (5001050, 5001052, 5001053, 5001051), act_rate=50.0,
                         # "Inflicts Injury, Paralysis and Weakness" -- its own skills only
                         # paralyse, so read as what it can be inflicted with.
                         resists=susceptible_only('injury', 'paralysis', 'weak'),
                         visuals=story_visuals(missions, 85, 1000))
    # Position is a placeholder: the script places Abaddon at (70,187).
    abaddon = monster(1104, 'Demon Abaddon', 0, '140:374', 6_000_000, abaddon_ai['id'], 2,
                      (5001060, 5001061, 5001062, 5001063, 5001065, 5001064, 5001066), act_rate=50.0,
                      visuals=story_visuals(missions, 265, 1100))   # "Inflicts all status ailments"
    # Both Maxwells are unit-backed (the page draws them with {{UnitImage}});
    # the wiki gives no ailment information for either, so 0 across the board.
    creator = monster(54508, 'Creator Maxwell', 50525, '180:302', 13_000_000, creator_ai['id'], 1,
                      (5001070, 5001074, 5001072, 5001077, 5001073, 5001075, 5001076))
    inception = monster(5000851, 'Inception God Maxwell', 51147, '180:302', 22_200_000, inception_ai['id'], 1,
                        (5001080, 5001085, 5001083, 5001084, 5001086, 5001087, 5001094, 5001082,
                         5001090, 5001091, 5001092, 5001093))
    record = mission(2100004, 'The Creation God', 3_000_000, 100_000, None, [abaddon, inception],
                     battles=[juggernaut, creator])
    return record, [juggernaut_ai, abaddon_ai, creator_ai, inception_ai], '8:0:1:0:0,6:51147:1:0:0'


# ---------------------------------------------------------------------------
# Trial No. X1 (8360021) -- Cursed Estia -> Princess Estia -> Xenon & Estia.
# See docs/features/TRIAL_X1.md (ticket #36).
#
# The client's F_MISSION_SCRIPT_MST row 8360021 (monsters
# "87500128,87500129,87500130"), one battle:
#   start     Cursed Estia 87500128 is made insensitive (HP floors at 1).
#   87500128 below 1%: effect 600550, swap in place to Princess Estia
#             87500129 (full HP), who is made insensitive too.
#   87500129 below 1%: effect, swap in place to Xenon & Estia 87500130
#             (full HP, killable: no command 7).
#   87500130 below 20%: dialogue only.
# changeMonster finds the new form by id alone and dereferences a missing
# MonsterMst row, so both later forms must be script_monsters with exactly
# these ids.  It keeps the monster's AI flags and that turn's action budget,
# so each form uses its own flag numbers (1-9, 11-19, 21-29).
#
# Wiki rev 663267 (2020-08-12) and its history from rev 119940, Deathmax's
# 2015-02-11 datamine (the AI trigger lists below), Zugon's 2015-02-28
# recorded turn tables (r/bravefrontier 2xglsl).  Skills: the bands
# 8450031-34, 8400101-05 and 8400201-05 match the wiki name for name and by
# hits and parameters; Lunar Shroud 8400206 was never seen in the fight.
# ---------------------------------------------------------------------------
def trial_x1(_missions):
    # Cursed Estia -- 1 Brutal Assault 8450031, 2 Brutal Rampage 8450032,
    # 3 Plagued Blood 8450033, 4 Cursed Vengeance 8450034.  4-5 actions a
    # turn, "no discernible pattern"; datamine triggers: every 4th turn, every
    # 3rd turn, <70%, <50%, <40%, <30% every 2nd turn.  AUTHORED mapping:
    # Plagued Blood opens turn 1 and every 4th turn, Cursed Vengeance (the
    # most common) every 3rd turn, once below 50%, Plagued Blood once below
    # 40%, Brutal Rampage joins below 70% and comes twice every 2nd turn
    # below 30%.
    cursed_ai = {'id': 20000041, 'name': 'Trial No. X1 - Cursed Estia', 'actions': [
        row(1, [cond('skill', 3), cond('act', 1), cond('limited_act', 1)]),
        row(2, [cond('skill', 2), cond('hp_pr_under', 30), cond('actbetween', 2), cond('turn_limited_act', 2)]),
        row(3, [cond('skill', 3), cond('hp_pr_under', 40), cond('limited_act', 1)]),
        row(4, [cond('skill', 4), cond('hp_pr_under', 50), cond('limited_act', 1)]),
        row(5, [cond('skill', 3), cond('actbetween', 4), cond('turn_limited_act', 1)]),
        row(6, [cond('skill', 4), cond('actbetween', 3), cond('turn_limited_act', 1)]),
        row(7, [cond('skill', 4)], percent=35.0),
        row(8, [cond('skill', 2), cond('hp_pr_under', 70)], percent=30.0),
        row(9, [cond('skill', 1)], percent=30.0),
        row(10, [], action='attack'),
    ]}
    # Princess Estia -- 1 Royal Starstrike 8400101, 2 Golden Starlight
    # 8400102, 3 Banishing Starfall 8400103, 4 Star Seal 8400104, 5 Grand Star
    # Seal 8400105.  5 actions; Star Seal and Grand Star Seal are the most
    # common; players' buffs were wiped first thing on her arrival.
    # Datamine: <30% use 4 times, <60% use 2 times, every 3rd turn, random.
    # AUTHORED: Banishing Starfall once on arrival and every 3rd turn; Star
    # Seal x2 once below 60%; Grand Star Seal x4 once below 30% (it sets flag
    # 11, which retires an unused 60% pair, Trial 003's precedence).
    princess_ai = {'id': 20000042, 'name': 'Trial No. X1 - Princess Estia', 'actions': [
        row(1, [cond('skill', 3), cond('limited_act', 1)]),
        row(2, [cond('skill', 5), cond('hp_pr_under', 30), cond('limited_act', 4)], flags=(11, 1)),
        row(3, [cond('skill', 4), cond('hp_pr_under', 60), cond('limited_act', 2), cond('flg_off', 11)]),
        row(4, [cond('skill', 3), cond('actbetween', 3), cond('turn_limited_act', 1)]),
        row(5, [cond('skill', 4)], percent=30.0),
        row(6, [cond('skill', 5)], percent=30.0),
        row(7, [cond('skill', 2)], percent=15.0),
        row(8, [cond('skill', 1)], percent=30.0),
        row(9, [], action='attack'),
    ]}
    # Xenon & Estia -- 1 Tag Assault 8400201, 2 Tag Deception 8400204,
    # 3 Elysian Grandeur 8400202, 4 Heartened Zeal 8400203, 5 Royal Radiance
    # 8400205.  5 actions.  Wiki: Tag Deception 5 times every 3 turns;
    # Elysian Grandeur once a turn for 4 turns below 40%, stopping below 10%;
    # Heartened Zeal 4 times in a row below 50%; Royal Radiance 4 times in a
    # row.  Datamine: <70% every 4th turn, <50% use 4 times, <50% every 4th
    # turn.  AUTHORED: Heartened Zeal once every 4th turn below 70%, Royal
    # Radiance x4 every 4th turn below 50%.  The burst rows come first, so a
    # Tag Deception turn below 40% opens with Elysian Grandeur (the recorded
    # EG, TD, TD, TD, TD).
    xenon_ai = {'id': 20000043, 'name': 'Trial No. X1 - Xenon & Estia', 'actions': [
        row(1, [cond('skill', 4), cond('hp_pr_under', 50), cond('limited_act', 4)]),
        row(2, [cond('skill', 3), cond('hp_pr_under', 40), cond('hp_pr_over', 10),
                cond('limited_act', 4), cond('turn_limited_act', 1)]),
        row(3, [cond('skill', 5), cond('hp_pr_under', 50), cond('actbetween', 4), cond('turn_limited_act', 4)]),
        row(4, [cond('skill', 4), cond('hp_pr_under', 70), cond('actbetween', 4), cond('turn_limited_act', 1)]),
        row(5, [cond('skill', 2), cond('actbetween', 3), cond('turn_limited_act', 5)]),
        row(6, [cond('skill', 1)], percent=40.0),
        row(7, [], action='attack'),
    ]}
    # Cursed Estia's own atlas (monster_anime_87600100.png) has no cgg file
    # anywhere, so she borrows Damsel Estia's unit art (850014, Light).
    # AUTHORED.  HP is not documented for her: 400,000 (Trial No. 001's
    # opening boss).  Princess 600,000 and Xenon & Estia 950,000 are the
    # wiki's.  Resists: the wiki lists what can be inflicted.
    cursed = monster(87500128, 'Cursed Estia', 850014, '180:302', 400_000, cursed_ai['id'], 5,
                     (8450031, 8450032, 8450033, 8450034), act_rate=50.0, act_min=4,
                     resists=susceptible_only('poison'))
    # Positions unused: the script swaps both in at (0,0) = in place.
    princess = monster(87500129, 'Princess Estia', 850015, '180:302', 600_000, princess_ai['id'], 5,
                       (8400101, 8400102, 8400103, 8400104, 8400105), act_min=5,
                       resists=susceptible_only('poison', 'injury', 'weak', 'sick'))
    xenon = monster(87500130, 'Xenon & Estia', 850025, '180:302', 950_000, xenon_ai['id'], 5,
                    (8400201, 8400204, 8400202, 8400203, 8400205), act_min=5,
                    resists=susceptible_only('poison', 'weak', 'injury', 'sick'))
    record = mission(8360021, 'Trial No. X1', 3_000_000, 60_000, cursed, [princess, xenon])
    # F_MISSION_MST's own rewards: a Gem, the Xentar sphere 818910 and
    # Sphere Frog 20302.
    return record, [cursed_ai, princess_ai, xenon_ai], '8:0:1:0:0,7:818910:1:0:0,6:20302:1:1:0'


TRIALS = (trial_001, trial_002, trial_003, creation_god, trial_x1)


def dump(data, indent):
    # LF and UTF-8, as the committed archive files are (later generators
    # rewrote them that way); CRLF here rewrote all 49 MB of mission.json.
    return (json.dumps(data, indent=indent, ensure_ascii=False) + '\n').encode('utf-8')


def set_clear_rewards(text, mission_id, rewards):
    """Replace one row's SiYs27Cj in place, so no other byte of the table moves."""
    start = text.index(f'"j28VNcUW": "{mission_id}"')
    key = '"SiYs27Cj": "'
    at = text.index(key, start) + len(key)
    end = text.index('"', at)
    return text[:at] + rewards + text[end:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    missions_raw = MISSIONS.read_bytes()
    by_id = {m['id']: m for m in json.loads(missions_raw)}
    ais_raw = AIS.read_bytes()
    ai_by_id = {a['id']: a for a in json.loads(ais_raw)}
    mst_raw = MISSION_MST.read_bytes()
    mst_text = mst_raw.decode('utf-8')

    for build in TRIALS:
        record, ais, rewards = build(by_id)
        by_id[record['id']] = record
        for ai in ais:
            ai_by_id[ai['id']] = ai
        mst_text = set_clear_rewards(mst_text, record['id'], rewards)

    new_missions = dump([by_id[k] for k in sorted(by_id)], 1)
    new_ais = dump([ai_by_id[k] for k in sorted(ai_by_id)], 4)
    new_mst = mst_text.encode('utf-8')

    changes = [(p, new) for p, old, new in ((MISSIONS, missions_raw, new_missions),
                                            (AIS, ais_raw, new_ais),
                                            (MISSION_MST, mst_raw, new_mst)) if old != new]
    if args.check:
        print('up to date' if not changes else 'stale: ' + ', '.join(p.name for p, _ in changes))
        return 1 if changes else 0
    for path, data in changes:
        path.write_bytes(data)
    print('wrote', ', '.join(p.name for p, _ in changes) or 'nothing (already current)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
