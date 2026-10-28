# Collab Summon

Players asked for the collaboration units Brave Frontier Global had (Deemo,
the Vocaloids, Final Fantasy, King of Fighters, ...). The units, their skills
and their art are all in the game's data; only the gates were gone. This adds
one **Collab** tile to the summon rail. It pages through twelve gates, one per
franchise, plus a Collab Mix gate for the franchises with fewer than three
units. Since the weekly rotation (SUMMON_ROTATION.md) the tile no longer ships:
each collab gate is open in its featured weeks, on a slot tile of its own.

`scripts/gen_collab_gacha.py` writes the gates (`deploy/archive/gacha.json`)
and the tile (`deploy/mst/gacha_category_mst.json`, row 4).
`scripts/check_collab_units.py` checks that every unit a gate can give is safe
to field. The wire test is `tools/test_collab_summon.py` in the root
repository.

## Rules

The gates are ordinary rare gates (gacha_mst type 2), so every fair-summoning
rule applies unchanged (docs/features/FAIR_SUMMON.md): need_gems × count, at
most 10 per request, rail-only, guarded debits, unit types rolled from
F_UNIT_TYPE_MST. No collab gate has SummonTicketV2Mst types, so a plain Summon
Ticket pays for one pull on any of them (the same rule as gate 2000).

**Weights.** A unit's weight is what fair summoning's gate 11960 gives its
rarity: 3★ 2000, 4★ 700, 5★ 100. Fair summoning has no rarer tier, so 6★ and
7★ units take the 5★ weight. On a gate that starts at 5★ every unit is equally
likely; 4★ units are seven times as likely as anything above them, as on 11960.

**Stage.** A gate gives the lowest stage of each evolution line, which is how
the original collab gates gave units out. Players evolve them as usual; all
evolution materials exist.

## Gates

The tile pages through them in this order. Each gate id is a real gacha_mst
row from that franchise's own Global collaboration, or a "Collaboration Summon"
row where the franchise's own row did not survive. So name, door, button and
original price are the original ones. Gems is the price charged now: a house
price (`HOUSE_PRICES` in the generator) replaces the row's need_gems, and the
gate's text quotes it (see "Prices" below).

| # | Gate | gacha_mst name | Gems | Units | Split | Art |
|---|---|---|---|---|---|---|
| 1 | 8560 | Special Summon: Vocaloid Rerun | 15 (was 3) | 10 | 4★ 91.3%, 6★ 2.2%, 7★ 6.5% | own |
| 2 | 15800 | Collaboration Summon II | 5 | 2 (DEEMO) | 4★ 87.5%, 5★ 12.5% | plain |
| 3 | 11300 | Global Launch: Rain & Fina | 10 (was 5) | 8 (FFBE) | 6★ 37.5%, 7★ 62.5% | own, event text |
| 4 | 12810 | Special Summon: KOF 1 | 15 (was 5) | 10 | 6★ 60%, 7★ 40% | own, event text |
| 5 | 13620 | Guilty Gear Summon | 10 (was 5) | 8 | 7★ | own, event text |
| 6 | 10480 | Global Launch: SMT4A Units! | 5 | 10 | 5★ 40%, 6★ 30%, 7★ 30% | plain |
| 7 | 15480 | Collaboration Summon I | 30 (was 5) | 4 (My Hero Academia) | 7★ | plain |
| 8 | 14360 | Fairy Tail Summon | 25 (was 5) | 10 | 7★ | plain |
| 9 | 16920 | Tokyo Ghoul Summon I | 30 (was 5) | 4 | 7★ | plain |
| 10 | 18850 | Trigun Summon I | 30 (was 5) | 4 | 7★ | plain |
| 11 | 15840 | Collaboration Summon IV | 5 | 8 (Chain Chronicle) | 5★ | plain |
| 12 | 15280 | Collaboration Summon | 5 | 11 (Collab Mix) | 5★ 54.5%, 7★ 45.5% | plain |

"Own" art is the gate's original background from the archive. "Plain" is
`gacha_rare_bg_img.png` ("Rare Summon"), because the franchise's gate art is
not in the archive. The FFBE, KOF and Guilty Gear art prints "Rates Up ×2" and
"Bonus Premium Elgif with every 6 summons". Neither exists here, so those gates'
captions say "Event art: no rate-up, no bonus Elgif." under the title. The Vocaloid art prints "3 gems per Summon", its original
price; the gate now costs 15 (see "Prices").

The tile art is `top_banner_TG_2_trimmed.png` ("Tokyo Ghoul × Brave Frontier:
Special Collaboration", 600×440). It is the only collab rail banner in the
archive that is not a Trigun daily banner. Being 600×440, it draws as a large
tile, like the event tile at the top.

### Pools

- **Vocaloid:** Diva Hatsune Miku, Duetto Megurine Luka, Meiko, Kaito, Vocals
  Kagamine Rin and Clavier Kagamine Len (4★); Sakura Miku (6★); Twinkle Snow
  Miku, Priestess Snow Miku and Senbonzakura Miku (7★).
- **DEEMO:** Deemo and the Girl (4★); Dark Deemo and the Girl (5★).
- **Final Fantasy Brave Exvius:** Rain, Lasswell and Fina (6★, the 2016
  units); Rain, Lasswell, Lid, Fina and Charlotte (7★, the 2018 units with
  Omni evolutions).
- **The King of Fighters:** Mai Shiranui, Kyo Kusanagi, Kula Diamond,
  Benimaru Nikaido, Athena Asamiya and Omega Rugal (6★); Terry Bogard, Leona,
  Chizuru Kagura and Orochi (7★).
- **Guilty Gear:** Johnny, May, Dizzy, Baiken, Millia, Jack-O', Leo Whitefang
  and Ramlethal (7★).
- **Shin Megami Tensei IV Apocalypse:** Asahi, Nozomi, Flynn and Dagda (5★);
  Lucifer, Merkabah and Krishna (6★); Hallelujah, Gaston and Toki (7★).
- **My Hero Academia:** Katsuki Bakugo, Tsuyu Asui, Ochaco Uraraka and Izuku
  Midoriya (7★).
- **Fairy Tail:** Natsu Dragneel, Gray Fullbuster, Gajeel Redfox, Erza
  Scarlet, Lucy Heartfilia, Wendy Marvell, Mavis Vermilion, Mard Geer,
  Acnologia and Zeref (7★).
- **Tokyo Ghoul:** Juzo Suzuya, Shu Tsukiyama, Touka Kirishima and Ken
  Kaneki (7★).
- **Trigun:** Meryl & Milly, Nicholas D. Wolfwood, Vash the Stampede and
  Legato Bluesummers (7★).
- **Chain Chronicle:** Velnar, Yuni, Lindsey, Symphonia, Yuuri, Phoena,
  Aludra and Barienna (5★).
- **Collab Mix:** Barbatos, Dhaos and Milla Maxwell (Tales of, 5★); Randolph,
  Ravenna and Mira (Rune Story, 5★); Mugen and Jin (Samurai Champloo, 7★);
  Chloe and Logi (The Alchemist Code, 7★); Zero (Phantom of the Kill, 7★).
  On a gate of its own, Zero would be a guaranteed 7★ for 5 gems.

## Left out

| Units | Why |
|---|---|
| Stahn, Cress, Rutee, Rita, Arche, Yuri (Tales of) | Their BB/SBB effects (`5280`–`5291BattleEffectTales*.sam`) are named by F_EFFECT_MST but are in no archive. |
| Tyrfing, Laevateinn (Phantom of the Kill) | The 4★/5★ BB effects (`5310`–`5313BattleEffectPhantomofkill*.sam`) are missing the same way. |
| Iori Yagami (KOF, 860177/8) | The 7★ and Omni effects (`86017BattleEffect7Iori*`, `860178BattleEffectIori*`) are missing. |
| Lofia (The Alchemist Code) | The 7★/Omni BB plays `20010BattleEffectBgBlackBgF60.sam`, which is missing. |
| Sol, Ky, Elphelt (Guilty Gear); Orochi Iori (KOF); All Might (MHA); Yamori (Tokyo Ghoul); Millions Knives (Trigun); Fuu, Shoryu (Samurai Champloo); Sun Tse, D'Artagnan, Florence Nightingale (Brave Frontier Heroes) | These lines exist only as Omni. A gate that hands out Omni units is what fair summoning removed. |
| JP-only units: MEIKO 10783-5 and KAITO 20713-5, Claudia 20955/6 (Crystal of Reunion), Samasa 40734 (Terra Battle), Rogi 50975 (Alchemist Code), the six Taka no Tsume units 10904, 20844, 30804, 40794, 51014, 60994 | No English name and no `deploy/archive/unit.json` record, so GachaAction cannot build them. |
| Brave Frontier 2 / Last Summoner characters (Roy, Sasha, Hilda, René, Joshua, Mariano, Sheena, Teresa, Gilbert, Carlos, Vanessa) | Gumi's own sister game, and all of them are Omni-only. |
| Lawson Frog/Metal/Crystal/Jewel God, Nekky (Famitsu) | Brand mascots and EXP fodder, not characters. |

Missing effect files can be a crash, a DLC error or just an empty Brave Burst;
that has not been tried. So the whole evolution line is left out, not only the
broken stage. Re-check with `scripts/check_collab_units.py --left-out` if those
files turn up.

## How it was checked

- `scripts/check_collab_units.py --assets <archive>` walks every stage of every
  pooled line. It checks: the unit_mst row and archive record; BB, SBB, UBB,
  leader and extra skill rows; every battle effect file the normal attack and
  bursts play (skill → EffectGroupMst → EffectMst → `.sam` or
  effect_anime/cgg/cgs, from the client's own tables in the archive's `mst/`);
  full, battle and thumbnail art and the animation unit_mst names; and the
  evolution materials. Result: 89 lines, nothing failing.
  - It warns about three stages whose effects name ids that neither effect
    table knows: Dolce and Crescendo Megurine Luka (5★/6★, 870025x/870026x)
    and Mugen's Omni SBB (2446/2447). 38 units of the base game do the same,
    Kranus, Roglizer and Rhyne among them. Crescendo Luka's and Mugen Omni's
    SBBs were fired in battle (below) and play without trouble.
- All 2,374 files those units and gates need are in the production CDN's name
  index (`re/work/assets-sweep/upload/name-index.current.json`).
- Wire test (port 9972, Multi Summon on): 56 checks pass. Each gate is listed
  with its art, copy and price. Each gate charges need_gems per pull, gives
  only its pool, and gives every unit of it. A plain Summon Ticket pays one
  pull. A short balance is refused. A collab row with no record is refused.
  The rates match by chi-square: Vocaloid by rarity, FFBE by unit, 2,000 pulls
  each.

- On one emulator (v2.19.6 client, local APK, port 9972): the Collab tile
  draws as a large tile under Honor and pages through the twelve gates in
  order. The gate detail shows the rates and price. A pull on the Vocaloid gate
  costs 3 gems and reveals Clavier Kagamine Len with full art, leader skill and
  BB. In "Start of Adventure" and "Monsters of the Prairie" with Crescendo
  Megurine Luka (6★), Mugen (Omni), Rain (7★), Deemo and the Girl (6★) and Ken
  Kaneki (7★): Luka's, Mugen's, Rain's and Kaneki's SBBs and Deemo's BB played
  their effects. The client did not crash, and no collab file was a DLC miss.
  The only miss was `event_total_token_earned1.png`, which is not in the
  archive and is unrelated (production's CDN has a stand-in).

## Client facts found on the way

- A tile with several gates pages through them in the **reverse** of the order
  their GachaInfo rows arrive. `GachaArchiver::populateAllPackets` used to emit
  rows in `unordered_map` order. It now emits them in reverse `gacha.json`
  order, so every tile pages in file order.
- The pager's "n/12" counter wraps onto two lines from 10/12 on (it shows
  "10/1" over "2"). It is cosmetic; the original rail never had ten gates on one
  tile.
- `priority` (`yu18xScw`) does not change the paging order.

## Open decisions (owner)

- **Per-franchise targeting.** On the 4-unit gates (MHA, Tokyo Ghoul, Trigun),
  a chosen 7★ is a 25% pull for 5 gems. One mixed gate with all ~90 units would
  be closer to how hard the original collab rate-ups were.
- **Prices.** 5 gems is the original collab price; the Vocaloid gate's row
  says 3, which its art prints. The owner found the gates too strong for that
  and set house prices (2026-10-16/17): Final Fantasy Brave Exvius and Guilty
  Gear 10 gems; Vocaloid (Miku) and The King of Fighters 15; Fairy Tail 25; My
  Hero Academia, Tokyo Ghoul and Trigun 30, because a 7★ is guaranteed and the
  pool has only 4 units. DEEMO, SMT IV Apocalypse, Chain Chronicle and Collab
  Mix keep 5. The Vocaloid gate's art still prints "3 gems per Summon"; the
  confirm dialog shows 15.

  How a house price works: `scripts/gen_collab_gacha.py` writes it into the
  row's need_gems in `deploy/mst/gacha_mst.json`. The
  server charges need_gems, and every price the client shows comes from the
  GachaInfo the server builds from that row (`SummonsScrlItem`,
  `SummonsDetailScene`, `SummonsConfirmScene`; the raw GachaMst price has no
  reader), so the shown and the charged price cannot differ.
  `tools/test_summon_prices.py` (root repository) checks both over the wire
  against the table.
- **Plain Summon Tickets** become usable on every collab gate.
- **Omni-only lines** (Sol, Ky, Elphelt, All Might, ...) could join at the
  5★ weight if guaranteed-Omni pulls are acceptable for these gates.
