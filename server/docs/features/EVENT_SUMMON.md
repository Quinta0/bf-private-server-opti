# Event gates

Two rail tiles added on 2026-10-19 at the owner's request (ticket #52).
Both reopen original gates whose own art is still on the CDN. Their names, doors,
buttons and the 5-gem price come from the original rows. The pools and the copy
are authored by `scripts/gen_event_gates.py`, with fair summoning's weights
(3★ 2000, 4★ 700, 5★ and up 100 each). Run it after `gen_house_gates.py`.

| Tile | Gate | Art | Pool |
|---|---|---|---|
| 6 `top_banner_Halloween_Lydia_2019.png` (600×440) | 8020 "Limited Gate: Special Summon", Halloween 2015 | `gacha_rare_bg_img_halloween2015.png`: "Rare Summon", no price | 4★ Red Hood Ciara, Semira, Dark Charm Lunaris, Snow Cub Signas, Light Blade Alyut, Red Slash Farlon (91.3%); 7★ Dark Succubus Beatrix, Fallen Knight Galtier, Florence, Lydia (8.7%) |
| 7 `small_banner_01Special.png` (600×150) | 13330 Fire, 13320 Water, 13300 Earth, 13290 Thunder, 13280 Light, 13270 Dark: "Divine Summon: <element>", April 2017 | `gacha_rare_bg_img_20170413D1-D6_5G.png`: "100% <ELEMENT>", "5 per Summon", an Elgif every 2 summons | every 5★ unit of gate 11960 and every 7★ unit of gate 15850 of that element: 78-84 units, about 59% 5★ and 41% 7★ |

- Ciara and Semira are on no other gate. They are Global units that reach Omni.
  All Halloween and element lines pass `scripts/check_collab_units.py`'s field check.
- Guest-only lines (no leader skill at the summon stage) are left out, so the
  guest Adjutant Mel (51176, ticket #48) is not in the Light gate.
- Gun Goddess Serin and Chevalier Mikael are left out: a stage they evolve into
  plays an effect file no archive ships. Gate 11960 still gives them.
- The element art promises a bonus Elgif. The server gives none, so the
  caption says "No bonus Elgif."
- The Halloween tile is seasonal. To take it down, set `HALLOWEEN_TILE_ON` to
  False and rerun the script. Without a tile, gme::isOnSummonRail refuses the gate.
- Wire test: `tools/test_event_gates.py` in the root repository.
