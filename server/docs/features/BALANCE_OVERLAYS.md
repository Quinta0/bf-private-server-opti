# Balance overlays (server side)

The server can take game-data changes without a new image from optional files you author yourself
under its `balance_root` (prod: `/opt/bf/prod/balance`, mounted read-only at `/app/balance`). This page
is the contract for the boot-applied overlays and the status endpoint. Code: `gimuserver/forge/`.
Wire test: `scripts/test_forge_overlays_wire.py`.

| Path under `balance_root` | What | Applied |
|---|---|---|
| `server/mst/<deploy mst file>` | rows / groups / object keys of one deploy/mst table | at boot |
| `server/archive/gacha.json`, `event_exchange.json`, `ai.json` | archive records by id | at boot |
| `server/archive/summon_rotation.json`, `event_rotation.json`, `boss_drops.json` | whole-file replacement | at boot |
| `GET /bf/gme/forgeStatus.php` | what the server accepted, per file, with sha256 and plain reasons | live |

Write every file atomically (temp file in the same folder, then rename), then restart the game
container. A broken file never stops the boot: it is logged (`forge: mst <file> REJECTED: <reason>`),
reported in forgeStatus.php, and the image's own data loads exactly as it would without the overlay.
With no `server/` files the server reads exactly what it read before (same files, same parse).

## MST overlays: `server/mst/<file>`

```json
{"format": 1, "table": "gacha_mst.json", "wrapper": "5Y4GJeo3", "key": ["7Ffmi96v"],
 "rows": [{"7Ffmi96v": "18060", "...": "every column of the base row"}], "delete": [["15280"]]}
```

- `format` 1; `table` = the file name; `wrapper` optional (must equal the base's single top-level key).
- Exactly one mode:
  - `rows` (+ optional `delete`): full rows; same key replaces the row in place, a new key is
    appended, `delete` lists key tuples (a bare value is fine for a one-column key).
  - `groups` + `group_key`: `[{"key": [...], "rows": [...]}]`; each group replaces every base row with
    that key where the first one was; a new group is appended; `"rows": []` deletes the group.
  - `object`: for one-object tables (`defines_mst`, `login_campaign_mst`, `daily_task_bonus_mst`,
    `video_ad_slot_game_info_mst`): listed keys replace existing ones, same JSON type.
- Keys the server knows (a file naming another key is refused; `key` may be left out for these):
  gacha_mst `7Ffmi96v`, gacha_category_mst `vx9uyQVQ`, mission_mst `j28VNcUW`, dungeon_mst `MHx05sXt`,
  area_mst `VjCY7rX4` (not the first column), unit_evo_mst `pn16CNah`, achievement_trade_mst
  `Mdgsh04u`, guild_point_exchange_mst `Yxo3bEic`, daily_task_prize_mst `d83aQ39U` (not the first
  column), login_campaign_reward_mst `H1Dkq93v` + `n0He37p1`, item_mst `kixHbe54`,
  unit_selector_gacha_ticket_mst `XIvaD6Jp`, summon_ticket_v2_mst `b0D2iq2d`. Groups are refused for
  these. Any other table needs `key`, and it must be unique in the base table ("use groups" otherwise).
- Every row must have exactly the columns of some base row and, per column, a JSON type the base uses
  (a whole number is accepted where the base has decimals). Missing columns are an error, never a default.
- Never deleted (may be replaced): mission_mst 1, 2, 10, 6000000; gacha_mst 1000, 2000, 11960;
  gacha_category_mst tiles 1 and 3.
- Table checks: gacha_category_mst tile ids not 21-23, 101-199, 9101-9104, 9111-9112; a NEW gacha_mst
  gate not in 20161-20213; gacha_effect_mst stays sorted by rarity; area_mst parent null, "0" or an
  existing area; mission_mst `SiYs27Cj` (and `S2ChOKo6`, optional leading `daily:N`) parse as
  `type:id:amount:0:0[:elgif]` with types the present box pays.
- After the merge the table must pass the same typed parse the server always does.
- Refused: `unit_mst.json` (use `server/unit_mst.json`), `version_info_mst.json` (use `content/`).
  A file no loader reads (e.g. `gacha_info_mst.json`) is reported "does not read".
- `GachaArchiver` takes gacha_mst from the cache, so one merge feeds Initialize, GachaInfo and the
  price a pull charges.

## Archive overlays: `server/archive/<file>`

Records by id (`gacha.json`, `event_exchange.json`, `ai.json`):

```json
{"format": 1, "file": "event_exchange.json",
 "records": [{"id": "13-7-30004", "token_id": 13, "name": "Divine Sword", "price": 3, "limit": 2,
              "reward_type": 7, "target_id": 30004, "quantity": 1, "source": "forge"}],
 "delete": ["13-6-730322"]}
```

- Each record has exactly the shape of one element of the image's file (unknown or missing fields are
  errors; optional fields stay optional). It replaces the record with its id whole, or is appended.
- Each record is checked on its own: a bad one is listed under `rejected` with a reason and the rest
  still apply. Only `event_exchange.json` takes `delete`.
- gacha.json: the gate has a gacha_mst row (after the mst overlay), is not in 20161-20213, 1..5000 pool
  entries, weights 1..1,000,000 summing within int32, every unit in unit.json with a summon effect for
  its rarity. Price, name and type stay in gacha_mst.
- event_exchange.json: id 1-64 chars, a known event token, price/limit/quantity >= 1, reward type
  4/5/6/7 that resolves, an Elgif skill only on a unit offer, id not used by a featured event.
  Bought counts stay in `user_exchange_purchases`, so a new limit applies to what was already bought.
- ai.json: id > 0, 1..64 actions, unique priorities, chance 0..100, act_target 1..4, at most 5 self
  and 4 party conditions and 12 flag changes, no `:` `@` `#` in any text but the name. Applied
  before the hand-built missions are checked, so a hand-built battle may name a new AI id.

Whole files (`summon_rotation.json`, `event_rotation.json`, `boss_drops.json`): the image's file
shape, replacing it. The base loader's checks apply strictly (summon_rotation: a Monday
`first_week`, every gate in gacha.json and once, enough gates for the slots, slot tiles not static
tiles, `after_tile` a static tile, grace 0..86400). A rejected file means the image's file loads.

Bazaar safety: event_exchange and the featured events' offers share one catalog that would stop the
boot on a bad offer. The server tries the overlay files, then drops the exchange overlay, then the
rotation overlay (each reported), so only a broken image file can still stop the boot.

## `GET /bf/gme/forgeStatus.php`

Public, no secrets, no paths. Per file: `sha256` of the exact bytes read (compare with the file you
wrote), `base_sha256` (mst: the image's table the rows were merged onto), `mode`, counts, `applied`,
`rejected` (archive records) and `error` (one plain sentence). Also `server_commit`
(env `BF_SERVER_COMMIT`, else the commit CMake saw), `booted_at`, `balance_root` and a `rules` block
(`present`; until the rules engine is built in, a present rules.json reports that nothing in it is
enforced).
