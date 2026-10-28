# Fair summoning

Summoning follows the rules of the original Brave Frontier Global, as far as
the game's own data and client show them. This page lists what is original,
what is authored by this server, and what was removed.

## Rules the server enforces (GachaAction)

Each rule copies something the original client already enforces. The server
used to trust the request instead.

| Rule | Source |
|---|---|
| A pull costs `need_gems` × count, or `need_friend_point` × count on Honor gates. Multi Summon gives no discount. | gacha_mst `03UGMHxF` / `J3stQ7jd`; client `SummonsDetailScene::setGachaInfo` |
| At most 10 pulls per request, or 1 when FeatureCheck `multisummon` is off. A count of 0 (tuto15) counts as 1. | `@0x16A6894-0x16A68A8`: min(affordable, 10) |
| Only gates on a permanent rail tile (`gacha_category_mst.json`) can be pulled. | the summon screen only opens listed gates |
| Gate 2000 (type 20000, "Tutorial Gacha") only while `tutorial_end_flag` is 0, the client's own test for "in the tutorial". It is also hidden from GachaList and UserInfo after that. The main tutorial runs chapters 1..12 (chapter 10 is tuto15, the free summon) and the client raises the flag only on reaching 12; chapters 50..54 (tuto10-14) are a post-tutorial "triggerable" tutorial that this client never enters. | `GachaActionScene::initConnect`; `UserInfo::isTutoPlaying @0x1295AFC`; `TutorialStepScene::checkChapter @0x1B927A4`; `UserInfo::setTriggerableTutorial @0x1296ED4` |
| Selector gates are ticket-only; `UnitSelectorGachaTicket` redeems them. | `SummonsDetailScene::initSummontickets` |
| Summon Tickets pay only on gates with id ≥ 2000 whose type is not 10004/10005, so never on Honor gates. `324b023k` says what pays: 0 the gate's currency, 1 the plain Summon Ticket counter, 2 the gate's own V2 tickets. The client sends 2 on a gate that has SummonTicketV2Mst types (here the Brave Summon Ticket, type 8, names 11960) and 1 on any other, so neither kind is accepted on the other kind of gate, and there is no fallback from one to the other. | `@0x16A1C24-0x16A1C4C`; `SummonsDetailScene::touchBegan @0x16A7F08-0x16A812C` |
| Every debit (gems and Honor, plain Summon Tickets, V2 tickets) is a guarded relative UPDATE inside the pull's transaction, so two requests racing on one balance cannot both be paid for. The V2 guard is defensive: with one database connection, the V2 read and debit already run in one transaction. | — |
| The unit type is rolled from F_UNIT_TYPE_MST: Lord 23, Anima 23, Breaker 22, Guardian 22, Oracle 10, Rex 0. | `unit_type_mst.json` `ChD5b0jR` |

Every rail gate's caption (the box under the gate art) lists its units, best
rarity first, and its detail text (the Summon confirm dialog) gives each
rarity's chance and one unit's chance (ticket #30). Both boxes wrap
but do not scroll, so a big pool lists about 200 characters of names and
"and N more". The text names no price: the client prints need_gems itself.
`scripts/gate_info.py` builds the text; `--check` covers every rail gate.

The helpers live in `gimuserver/gme/common/SummonGates.hpp`. The wire test is
`tools/test_fair_summon.py` in the root repository.

## Removed: the test gates

Gates 99005-99008 ("Test Gate: 5/6/7 Star/Omni") were added on 2026-09-13
(commit 70e82f7) to test the summon animation. Each gave a guaranteed 5★, 6★,
7★ or Omni for 1 gem, or for one plain Summon Ticket. They were never in the
original game. Their pools (`deploy/archive/gacha.json`), gate rows
(`deploy/mst/gacha_mst.json`, back to the original 1305 rows) and rail tile 4
(`gacha_category_mst.json`) are gone.

`gacha_effect_mst.json` rows 900/901 (the Omni reveal, rarity 8) were authored
with gate 99008 but are kept, because Omni selector picks need them.
`scripts/test_summon_reveal_wire.py` section G now replaces gate 11960's pool
with Lukroar alone in its QA copy of the archive.

## What is authored, not original

- **Pools and weights.** The original server never shipped its pools or rates.
  Gumi published rates per unit group, not per star rarity. Every pool in
  `deploy/archive/gacha.json` was written for this server, and a rarity's share
  depends on how many units of that rarity the pool holds.
- **Gate 11960 "Rare Summon: All Star".** A one-day event from 2016-12-17,
  reused as the main gem banner. It costs 3 gems; from 2018 on almost every gem
  gate cost 5. Its pool has 667 of the 771 kind-0 3-5★ units in unit_mst,
  about 62% / 32% / 6% by rarity. The 104 left out are 58 Global-only 8xxxxx
  units, 11 JP-only units, collaboration units (Tales of, Vocaloid, Deemo), a
  few event units and five elemental variants.
  The collaboration units have their own gates on the Collab tile
  (docs/features/COLLAB_SUMMON.md).
- **Gate 2000.** The original row says 3-5★. Here the tutorial pull draws from
  3★ units only.
- **Gate 1000 (Honor).** Its gacha_mst window ended in 2017. At end of service
  the Honor gate was 1001 "Super Honor Summon" (200 Honor).
- **Gate 19000.** A 2020 step-up row, reused as the weekly 6★ selector.
- **Event gates (rail tiles 6 and 7).** Gate 8020, the Halloween 2015 gate, and
  gates 13270-13330, the April 2017 element Divine Summons, at their original
  5 gems. Their pools are authored by `scripts/gen_event_gates.py`
  (docs/features/EVENT_SUMMON.md).

## Open decisions (owner)

- Multi Summon: the client supports up to 10 per request at no discount. The
  2022-04-24 recording shows only a single Summon button, so Gumi had it off at
  end of service. `deploy/system/features.json` `multisummon` turns it on or off.
- Price of 11960: 3 gems (current) or 5 gems (the 2018-2022 norm).
- Plain Summon Tickets on the rail: this server's summon_ticket_v2_mst points
  the Brave Summon Ticket (type 8) at 11960, so the client only ever offers
  Brave Summon Tickets there, and plain Summon Tickets pay nowhere on the
  current rail except the tutorial gate. Pointing type 8 back at its original
  gate 17160 makes plain tickets usable on 11960 again.
- End-of-service rail: Veteran = Brave Summon 17160 (Brave Summon Ticket only,
  50 units) + Legacy 15850 (4 gems, 7★). This needs authored pools, and 17160
  must refuse gems because its row says 5.
