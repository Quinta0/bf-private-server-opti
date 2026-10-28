# Trial No. X1 (mission 8360021)

Dated 2026-10-19 (ticket #36). Built by `scripts/gen_research_lab.py`
(`trial_x1`); lab-wide rules are in [RESEARCH_LAB.md](RESEARCH_LAB.md).
Server-, model- and client-tested (emulator run below).

## What was wrong

The archive record was a generated stand-in (template Trial No. 001) with
Cursed Estia 87500128 (Brave Knight Karl's art), Princess Estia 87500129 and a
"Xenon & Estia" 56853 in one wave. The client's own `F_MISSION_SCRIPT_MST` row
8360021 swaps 87500128 -> 87500129 -> 87500130 below 1% HP. 87500130 was never
sent, so `MonsterParty::changeMonster` @0x10C3160 read a missing MonsterMst row
(`MonsterMstList::getObject` returns null, `MonsterUnit::initialize` faults in
`getBadStateResists`) and the game closed when Princess Estia was beaten.

## The client script (one battle)

| Event | Trigger | Commands |
|---|---|---|
| 1 | start | dialogue 124-125; Cursed Estia insensitive (HP floors at 1) |
| 2 | 87500128 below 1% | effect 600550; swap in place to Princess Estia 87500129 (full HP); dialogue 127-129; she is made insensitive |
| 3 | 87500129 below 1% | effect; swap in place to Xenon & Estia 87500130 (full HP, killable); dialogue 132 |
| 4 | 87500130 below 20% | dialogue 133-135 |
| 5 | game over | dialogue 136 |

The swap keeps the monster's AI flags and that turn's action budget, so each
form uses its own flag numbers (1-9, 11-19, 21-29).

## The encounter as built

| Form | HP | Art | Skills (index order) | Actions | Can be inflicted with |
|---|---|---|---|---|---|
| Cursed Estia 87500128 (placed) | 400,000 (authored) | unit 850014 Damsel Estia (authored) | Brutal Assault 8450031, Brutal Rampage 8450032, Plagued Blood 8450033, Cursed Vengeance 8450034 | 4-5 | poison |
| Princess Estia 87500129 (script) | 600,000 (wiki) | unit 850015 | Royal Starstrike 8400101, Golden Starlight 8400102, Banishing Starfall 8400103, Star Seal 8400104, Grand Star Seal 8400105 | 5 | poison, injury, weaken, sickness |
| Xenon & Estia 87500130 (script) | 950,000 (wiki) | unit 850025 | Tag Assault 8400201, Tag Deception 8400204, Elysian Grandeur 8400202, Heartened Zeal 8400203, Royal Radiance 8400205 | 5 | poison, weaken, injury, sickness |

AIs 20000041-43. Sources: wiki rev 663267 and its history from rev 119940,
Deathmax's 2015-02-11 datamine (trigger lists), Zugon's 2015-02-28 recorded
turn tables. Behaviour per form is commented in `trial_x1`; the main rules:

* Cursed Estia: Plagued Blood on turn 1 and every 4th turn, Cursed Vengeance
  every 3rd turn, once below 50% and at random; Plagued Blood once below 40%;
  Brutal Rampage joins below 70% and comes twice every 2nd turn below 30%.
* Princess Estia: Banishing Starfall on arrival and every 3rd turn; Star Seal
  x2 once below 60%; Grand Star Seal x4 once below 30% (retires an unused 60%
  pair).
* Xenon & Estia: Tag Deception x5 every 3rd turn; Heartened Zeal x4 once below
  50% and once every 4th turn below 70%; Elysian Grandeur once a turn for 4
  turns below 40%, never below 10%; Royal Radiance x4 every 4th turn below 50%.

Rewards are F_MISSION_MST's: 3,000,000 Zel, 60,000 EXP, a Gem, the Xentar
sphere 818910 and Sphere Frog 20302, first clear only.

## Authored choices and gaps

* Cursed Estia's HP and every form's ATK 2000 / DEF 800 are not documented.
* Cursed Estia's own atlas (`monster_anime_87600100.png`) has no
  `monster_cgg_87600100.csv` anywhere (not on R2), so she uses unit art 850014.
  Uploading a reconstructed cgg would allow the original sprite.
* The datamine's "target >50% BB" for Tag Deception and the "<20%" / "<30%"
  Xenon & Estia triggers are not modelled. Lunar Shroud 8400206 is unused
  (never seen in the fight).
* The client pre-downloads skill effect files only for placed monsters
  (`DLCManager::requestUnitFilesForMission`), so script monsters' effects load
  at cast time; no problem was seen on the emulator.

## Tests

* `python3 -I scripts/test_research_lab_ai.py`: 20 X1 checks (structure against
  the client script, HP, skills, art, disjoint flags, schedules and thresholds).
* `python3 -I scripts/test_trial_x1_wire.py --url http://127.0.0.1:<port>
  --container <name> --db <save path in the container>`: MissionStart places only
  87500128 and describes 87500128/87500129/87500130 with their AI rows; a loss,
  the first clear and a replay pay as the lab rules say. 14/14 on 2026-10-19.
* Emulator (emulator-5558, FrTestA, local server with a test-only overlay
  lowering HP/ATK/DEF so a Lv. 1 squad gets through): Cursed Estia -> Princess
  Estia -> Xenon & Estia with every dialogue, no crash, cleared with 3,000,000
  Zel and 60,000 XP.
