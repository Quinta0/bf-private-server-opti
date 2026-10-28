# Classic Raid round 2: camp rest, raid items, crafting, raid shop

Band `camp_items` of the round-2 raid work (2026-10-16). Requests covered:
RaidCampRestStart, RaidCampRestGet, RaidCampRestEnd, RaidItemDelete,
RaidItemEdit, RaidItemMix, RaidShopUse. Builds on `RAIDS_SPEC.md` (round 1);
read that first for transport, identity and the room tables.

Evidence labels as in round 1: **OBSERVED** = read from `libgame.so` (address
given) or the shipped master data; **INFERRED** = reasoned from those, not yet
seen on a device. Nothing in this band has been seen on a device. Addresses
are VAs (file offset == VA). The annotated dumper used for this pass resolves
`adrp+add` strings and demangles `bl` targets; the summaries in
`re/work/raid/summ.py` style were enough for everything else.

---

## 0. One-paragraph summary

The camp is a field sub-scene (1256) the player enters from the camp point's
Rest button; it polls `RaidCampRestGet` every RaidWorldMst interval (4 s) and
redraws the six unit HP bars from `wc2DiMY1`; leaving sends `RaidCampRestEnd`
and rebuilds the field (1242), which immediately bounces back to the camp if
the player's `81B4TvEK` row still says `MFz8YRS6 = 1`. Raid items live in
three client lists: the persistent 5-slot raid item set (`Fihk73ZR`), the
pre-mission reserve (`GvV1w4yM`) and the per-mission bag (`UsDq1E6o`, 50
stacks). `RaidItemEdit` and `RaidItemMix` are fire-and-forget: the client
applies the change locally, queues the request in `ConnectRequestList`, and
`StepScene` drains the queue on the next step transition; the server must
replay the same move (bag <-> set, or bag materials -> one product) and should
answer with the authoritative lists, never with an error. `RaidItemDelete` is
dead code in this build (no caller). `RaidShopUse` is a gem purchase: type 100
refills raid orbs (aube) outside a room, type 101 refills lives (`fP1r39bd`)
inside a mission; gems are an in-game currency on this server (no IAP path is
served), so both spend existing gems and nothing else.

## 1. Requests at a glance

| request | group / aes | url | sent from | connect type | queued? |
|---|---|---|---|---|---|
| RaidCampRestStart | `I7nVUv0h` / `qy27SYu9` | `/actionSymbol/3y1rmPSp.php` | RaidFieldScene::connectCampStart @0x198DEA0 (point button 111) | 17 (0x11) | no |
| RaidCampRestGet | `diIe8ux7` / `G5uind2r` | `/actionSymbol/ZPs40D7i.php` | RaidFieldCampScene::connectCampGet @0x195E404 (poll) | 18 (0x12) | no, `isAsync()==1` @0x13BD638 |
| RaidCampRestEnd | `f9Dh4djx` / `0TCX6S4R` | `/actionSymbol/Z2bh0SEw.php` | RaidFieldCampScene::connectCampEnd @0x195E8AC (leave button, touch object 120) | 19 (0x13) | no |
| RaidItemEdit | `DZLI83hx` / `3Lxpf5KE` | `/actionSymbol/1aF2cKyH.php` | RaidItemEditScene, RaidItemEditNumScene, RaidItemEditSelectScene, RaidFieldItemScene (see 4.2) | none | yes |
| RaidItemMix | `K17JUYR2` / `c6wBM9sH` | `/actionSymbol/HUVv5SR7.php` | RaidUtils::createItemMixConnectList @0x11C74B8 (from the mix scenes' onSceneInvisible) | none | yes |
| RaidItemDelete | `N43JmIf2` / `jvm3q24f` | `/actionSymbol/7r4LhzGJ.php` | **nobody** (no call to its ctor anywhere, no `C1Ev@plt` stub) | - | - |
| RaidShopUse | `4zJ0WmTX` / `RSvU7dh6` | `/actionSymbol/1P3VapS0.php` | RaidSceneBase::shopConnect(type, cost) @0x19CE378 | 21 (type 100) / 22 (type 101/102) | no |

All non-queued raid requests go through `GameScene::accessPhpRaid` (normal
game server, round-1 fact). Queued ones go through `StepScene::updateEvent`
@0x164628C, which special-cases exactly `K17JUYR2`, `DZLI83hx`, `XTban70i` and
`bv9xt3m6`: it sends them with `accessPhpRaid(RaidInfo::getWorld())`, and if
`RaidInfo::getWorld()` is null it silently skips the send (OBSERVED
@0x16463B8..0x16463DC).

## 2. Error handling the server can rely on (all OBSERVED)

The connect type is kept in `RaidInfo` (`RaidSceneBase::checkConnectEnd`
@0x19CEF08 returns it and resets it to 0). Scenes act on the type in their own
`connectEndCheck`/`updateEvent`.

Server refusal mapping (server doc in `Handlers.hpp`, confirmed by
`GameScene::checkResponseMessage` @0x1615E4C: cmd 2 -> notice -4000):

| server result | client notice | raid scene reaction |
|---|---|---|
| `HandleResult::refuseToRetry(msg)` | -4000 | Camp (@0x195E5FC), field (@0x198C100), field item (@0x1963D94), item mix list/detail (@0x19FC58C / @0x19F8DDC): message, then `connectErrorInit` @0x19CF898 -> scene **1201 RaidInitConnect** (re-runs RaidUserInfo; resumes the room if `login_*` still name a live one). RaidQuickCheckScene @0x19AC214: -4000 -> `connectNull`, player stays. RaidRoomTopScene: `roomNoticeOK` first, then `connectErrorInit` unless that handled it. |
| `HandleResult::refuseToHome(msg)` | -3992 | `GameScene::noticeOK` rebuilds Home; the raid scenes add nothing (their `cbnz` falls through). Player leaves the raid; the mission keeps running server side. |
| plain error | Close | app exits. Never use. |
| notice code 0 | - | `connectNull` (connect type cleared, scene stays). |

RaidFieldScene additionally reacts to codes 500 (rebuild field) and 501
(`errorBreak` -> 1201); the server has no way to emit those today and does not
need to.

Rule for this band: interactive requests (camp, shop) refuse with
`refuseToRetry` (message, then a clean raid re-init); queued requests
(ItemEdit, ItemMix, Delete) never refuse at all, because their error would be
shown on a `StepScene`, which is not a raid scene: they log, change nothing,
and reply with the server's authoritative lists (the `ItemEdit.cpp`
precedent).

## 3. Camp rest

### 3.1 Client flow (OBSERVED)

```
1242 RaidFieldScene, standing on a camp point
  point buttons (touchPointBtn @0x198D5DC, ids via jump table @0x235EA76):
     111 Rest   -> connectCampStart          (RaidCampRestStart, type 17)
     112 Mix    -> changeScene 1280 RaidItemMixListScene
     113 Harvest-> pickCheck -> 1257 RaidFieldPickScene
     114 Battle -> RaidInfo::setMovePoint, battleScene
     115 Items  -> changeScene 1243 RaidFieldItemScene
  fleldPointBtnSet @0x198EEC0: point_type 0 shows 113+114, point_type 1 (camp) shows 111.
  (Where 112/115 are drawn was not located; their handlers exist.)

  CampStart success (connectEndCheck @0x198A8DC, type 17 -> 0x198AAD4):
     if fieldEffectCheck() (result / boss encounter pending) -> stop
     me = RaidMissionUesrInfoList.getObjectUserID(my id)   <- no null check before getCampFlg
     if me.CampFlg == 0 -> questConnectReadyUpSet (sends RaidMissionInfo, type 14)
     setRenewCampTime; changeScene 1256           <- ALWAYS, whatever CampFlg says

1256 RaidFieldCampScene
  initialize: layout_raidFieldCampScene.csv (campFrame, 6x unitAnim, 6x unitHp, campBtn)
  updateEvent @0x195E264 every frame:
     checkConnectEnd == 18 (CampGet done)  -> setDrawInfoSet, effectSet, unitHpBar, fieldEffectCheck
     checkConnectEnd == 19 (CampEnd done)  -> changeScene 1242
     else if !fieldEffect()                -> connectCampGet
  connectCampGet @0x195E404 sends only when: connect type == 0, world and room set,
     now - RenewCampTime >= RaidWorldMst.getIntervalSec() (Rqc8pi0x = 4),
     myRoomInfo() exists.  Then setRenewCampTime.  => one poll per >= 4 s.
  touchBegan @0x195E854 on object 120 -> connectCampEnd (type 19)

1242 RaidFieldScene again (fresh instance, initConnect is EMPTY)
  initCheck @0x1988D28 reads the CACHED 81B4TvEK row:
     PlayerRest == 0 -> raidGameOverHeartCheck (heart shop / retire)
     CampFlg == 1    -> setRenewCampTime; changeScene 1256   <- straight back to camp
```

Consequences the server MUST honour:

1. The `RaidCampRestEnd` reply must carry the full `81B4TvEK` list with the
   caller's `MFz8YRS6 = 0`. Without it the rebuilt field reads the stale
   `1` and returns the player to the camp forever (initCheck above).
2. Any reply that carries `81B4TvEK` must contain the caller's row: the list
   is replaced from node 0 (see 3.3) and the CampStart handler calls
   `getCampFlg` on the lookup result without a null check (crash).
3. Do not send `g97TWISb` (RaidRoomMissionInfo) partially: its reader
   (@0x143FE4C) resets Result and RestTime before reading keys, so a node
   without `3tWuc7de`/`8n7oG4fS` reads as "mission running, 0 s left".

What the camp scene reads (OBSERVED, setDrawInfoSet @0x195CF6C and
unitHpDraw): `RaidUserPartyDeckInfoList` rows whose `h7eY3sAK` is the
caller, using `getMaxHp`, `getNowHp`, `getDisporder`. `fieldEffectCheck`
(@0x19DD324) also runs `resultCheck` (RaidRoomMissionInfo Result) and
`bossCheck` (boss data list vs the caller's point and BossEscapeLimit), so a
camp poll that carries the field's mission payload lets the camp react to the
mission ending or a boss walking into the camp.

### 3.2 Request bodies (OBSERVED, createBody @0x13BD880 / @0x13BD508 / @0x13BD190)

All three are identical in shape; `setParam(world, room, roomMission)` is fed
`RaidWorldMst::getWorldID`, `RaidRoomInfo::getRoomID` and
`RaidRoomUserInfo::getCurrentRoomMissionID` (the caller's `FV6MU5nQ` from
`uwH62EZy`). A param whose string is empty is omitted (each is guarded by a
length check). CampRestGet omits `createVersionTag`.

```json
{"IKqx1Cn9":[{...login...}],
 "8iBuZa3n":[{"43hMuY2I":"1","8VYd6xSX":"<room id>"}],
 "g97TWISb":[{"ja39HMLq":"<room mission id>"}]}
```

Treat `ja39HMLq` as optional: empty or `"0"` means "the caller's current live
room mission" (FV6MU5nQ is an int the MissionStart band fills; until it does
the client sends `"0"`).

### 3.3 Response groups used by the camp (OBSERVED readers)

There is no Camp*Response class. The replies are made of shared groups:

`wc2DiMY1` RaidUserPartyDeckInfo (reader @0x14452E8). Gated by
`RaidInfo::isResponsePartyDeck()` (byte +0x34, ctor @0x12F2F58 sets it to 1;
only RaidFieldItemUseScene clears it while it is open). List-replace: node 0
param 0 calls `removeAllObjects`; a row is committed on the node's last param,
so key order does not matter. One row per unit of every member:

| key | type | setter | value |
|---|---|---|---|
| `h7eY3sAK` | str | setUserID | owner user id |
| `edy7fq3L` | str | virtual setter (base unit info, user unit id) | user unit id |
| `pn16CNah` | str | virtual setter (unit id) | unit id |
| `4A6LzBxr` | int | virtual setter via vtable+0x50 (level) | unit level |
| `gr48vsdJ` | int | setMemberType | 0 own unit, helper type otherwise (field band decides) |
| `XuJL4pc5` | int | setDisporder | slot 0..5 (the camp draws 6) |
| `3WMz78t6` | int64 | setMaxHp | max HP |
| `e7DK0FQT` | int64 | setNowHp | current HP (camp heals this) |
| `gU2xtQ0V` | int | setBbGauge | BB gauge, unchanged by the camp |
| `nIGZ1X9C` | int | setSbGauge | SBB gauge, unchanged |
| `PWXu25cg` | int | setHel | as stored |
| `98WfKiyA` | str | setFriendID | helper's owner id or "" |
| `nBTx56W9` | str | setUnitTypeID | as stored |
| `2pAyFjmZ` | int | setUnitImgType | 0 |

The three keys round 1 could not separate are now resolved: `edy7fq3L` and
`pn16CNah` go to virtual string setters of the base unit class, `4A6LzBxr` to
the vtable+0x50 int setter; the key meanings match every other unit reader.

`81B4TvEK` RaidMissionUesrInfo (reader @0x143CCB8; list-replace; gated by
`isResponseMissionUser`, cleared only while RaidFieldScene animates a walk).
This band only owns two of its keys: `MFz8YRS6` CampFlg (int) and `fP1r39bd`
PlayerRest (int, lives; cap is `RaidMissionUesrInfo::getPlayerRestMax` @0x12F6110
= DefineMst raid_max_p `S92Hcor3` = 3). Every other key belongs to the field
band's builder.

### 3.4 Wire contracts

**RaidCampRestStart** (interactive). Preconditions: the caller is a member of
a live room mission that has no result yet; the caller is not already in a
battle; the caller's current point is a camp: RaidPointMst row with
`map_id == RaidMissionMst(mission).questMapId`, `point_id == ` the caller's
NowPointNum, `point_type == 1` (in the shipped data every map's camp is point
1). On success set camp_flg 1, camp_since = healed_until = now. Already
camping: succeed without resetting the timers.

Reply: `81B4TvEK` (full list, caller has `MFz8YRS6 = 1`) + `wc2DiMY1` (all
members). If the field band's `81B4TvEK` builder does not exist yet, send
`wc2DiMY1` only: the old `81B4TvEK` list stays, CampFlg reads 0, the client
fires one RaidMissionInfo (harmless if registered, a "not available" Home
refusal if not) and still enters the camp.

Refusal: `refuseToRetry("You can only rest at a camp.")` /
`refuseToRetry("This raid quest has ended.")` -> camp-less raid re-init (1201).

**RaidCampRestGet** (async poll, >= 4 s apart). Apply the lazy heal (3.5),
persist it, reply `wc2DiMY1` (all members) plus, when the field band's
builder exists, the same payload RaidMissionInfo returns (`81B4TvEK`,
`Ts2zJ6gj`, `g97TWISb`, ...), so the camp sees the mission end or a boss
arrive. Never refuse a live mission. Room mission gone or the caller not in
it: `refuseToRetry("This raid quest has ended.")`. Mission has a result: reply
normally with the mission payload (its `g97TWISb` Result makes
`fieldEffectCheck` play the result).

**RaidCampRestEnd** (interactive). Apply the final heal, set camp_flg 0.
Reply MUST contain `81B4TvEK` with the caller's `MFz8YRS6 = 0` (see 3.1), plus
`wc2DiMY1`. If the field band's builder does not exist yet this request cannot
be answered correctly: refuse with `refuseToRetry` rather than send a reply
that traps the player in the camp. Not camping: succeed with the same reply.

### 3.5 Heal rule (server design; no client-side formula exists)

The client only displays what `wc2DiMY1` says; there is no heal constant in
DefineMst or RaidPointMst (`X7g8cNAI` is the harvest pick time). Lazy, no tick
thread (round-1 rule):

```
tick      = RaidWorldMst.interval (Rqc8pi0x, 4 s)
ticks     = floor((now - healed_until) / tick)
per tick  = ceil(max_hp * kCampHealPercentPerTick / 100), kCampHealPercentPerTick = 10
now_hp    = min(max_hp, now_hp + ticks * per tick)  for every unit of the caller
healed_until += ticks * tick
```

Full heal in 40 s of rest; the 1800 s mission clock keeps running, which is
the cost of resting. Owner decisions, flagged: whether units at 0 HP are
revived by resting (default here: yes, they heal like any unit), and the
percentage. BB/SBB gauges are untouched. Archive counter `b28wpmvq`
(CampRecover in RaidUserTeamArchiveInfo `6nqGvdY1`) may be bumped by the archive
band; not needed here.

## 4. Raid items

### 4.1 The three lists (OBSERVED)

| list | reply group | reader | per | rows | consumers |
|---|---|---|---|---|---|
| raid item set | `Fihk73ZR` RaidEquipItemInfo | @0x1438368 | player, persistent | slot `XuJL4pc5` 0..4 (RaidItemEditScene::setItemLayout @0x19F2D18 loops `getObjectWithDispOrder(0..4)`) | battles (`GameUtils::getBattleEquipItemInfo`), field item scenes |
| reserve | `GvV1w4yM` RaidRsvItemInfo | @0x1441EB4 | player, persistent | slot 0..4 | pre-mission room item screens only |
| bag | `UsDq1E6o` RaidUserBagInfo | @0x1443A84 | player x room mission | stacks; cap = DefineMst raid_bag_cnt `09EbcDmX` = 50 stacks with num > 0 (`getActiveCount` @0x12FE168) | field item scenes, mix, harvest |

All three readers are list-replace on node 0 param 0 and commit a row on its
last param. The response classes have only ctor/dtor/readParam (no group-start
hook), so **an empty array cannot clear a list**: the client keeps whatever it
had. This is harmless after an edit (the client already removed the rows it
moved) but means a stale bag from an earlier mission survives an empty-bag
reply; see unknown 6.

Set / reserve row: `kixHbe54` item id (str), `XuJL4pc5` slot (int),
`wgV86x1q` count (int). The set and the reserve are taken OUT of the
warehouse: `RaidUtils::raidEqpItemFull` @0x11CC728 tops both up from
`GameUtils::getWarehouseItemNum` and calls `GameUtils::decWarehouseItem`, the
same disjoint-store model as the normal item bar (`BattleItems.hpp`).

Bag row:

| key | type | setter | value |
|---|---|---|---|
| `n6E8iMf3` | int | setFrameID | stack id, unique per bag |
| `h7eY3sAK` | str | (string field) | owner user id |
| `kixHbe54` | str | (string field) | item id |
| `wgV86x1q` | int | setItemNum | count, 1..ItemMst max_stack (`m9gd5h1u`) |
| `DbMVG16I` | int | setDiffType | 0 (no consumer found) |
| `dJNpLc81` | int | setNewFlg | 0 (no consumer found); 1 for freshly gained stacks is fine |
| `Slkc395l` | str | (string field) | event token id, "" for normal items (non-empty switches the icon) |

Client bag arithmetic, which the server must mirror so both sides agree:

* `RaidUtils::incBagItem(id, n)` @0x11C6D40: tops up existing stacks of the
  same item to `ItemMst::getMaxNum` (max_stack) in list order, then appends
  new stacks of up to max_stack with a TEMPORARY frame id `0x7fffffff - id`.
  No bag-cap check.
* `RaidUtils::decBagItem(id, n)` @0x11C70EC: collects the item's stacks,
  sorts them by (count, pointer) ascending and drains the smallest first.
  Emptied stacks stay in the client list at count 0 until the next bag reply.

The server keeps real frame ids (`max(frame_id) + 1` per bag); every bag reply
replaces the temporary ones.

### 4.2 RaidItemEdit (queued) - move items between bag and set, in the field

Callers (OBSERVED; all enqueue with `ConnectRequestList::addObject` after
mutating the local lists): RaidItemEditScene (1283) `eqpItemFull` @0x19F38D0
(top every set slot up to ItemMst max_equipped `t1i2vIbT` from the bag) and
`eqpItemReset` @0x19F3AF8 (whole set back to the bag); RaidItemEditNumScene
`equipItem` @0x19F225C (slider: put n of a bag item into a slot, old slot
contents back to the bag); RaidItemEditSelectScene `removeEquipItem`
@0x19F549C (one slot back to the bag); RaidFieldItemScene (1243)
`replaceSetItem` @0x19658CC / `overEqpSet` / `bagEquipSet`.

Flush points (`changeStepScene`, OBSERVED): leaving RaidFieldItemScene to the
field, RaidItemEditScene -> 1243, RaidFieldItemScene::changeItemUse (so edits
land before a RaidMissionUseItem), the mix list's back button, and
`touchFieldTypeBack` to the camp. `fieldEffectCheck(bool,bool)` empties the
queue when a result or boss encounter fires, so an edit can be lost; the
server state then simply wins.

Body (createBody @0x13BF54C; every value goes out even when empty):

```json
{"IKqx1Cn9":[{...}],
 "8iBuZa3n":[{"43hMuY2I":"1","8VYd6xSX":"<room>"}],
 "g97TWISb":[{"ja39HMLq":"<room mission>"}],
 "71U5wzhI":[{"kixHbe54":"20000","XuJL4pc5":0,"wgV86x1q":3}, ...]}
```

`71U5wzhI` holds the WHOLE new set (one node per row of RaidEquipItemInfoList);
the bag is not sent. The ints are written with `addParam(const char*, int)`;
read them with the existing `UserEquipItemInfo` struct (`u32::str` accepts
both, as the normal ItemEdit already does).

Server rule (all or nothing, one transaction):

1. Resolve the room mission (`ja39HMLq`, fallback: the caller's live one).
   None, or the caller is not in it, or it has a result: change nothing.
2. Validate the new set: slots 0..4, no repeats, rows with item 0 or count 0
   dropped, ItemMst row exists, count <= max_equipped.
3. For each item id: `delta = new_set[id] - old_set[id]`. delta > 0 must be
   covered by the bag (drain smallest stacks first); delta < 0 goes back into
   the bag (incBagItem rule, no cap). Nothing touches user_items: in the field
   the set only trades with the bag.
4. Any violation: no write, log the reason.
5. Reply `{"Fihk73ZR":[set rows], "UsDq1E6o":[bag rows]}` in both cases.

### 4.3 RaidItemMix (queued) - craft from bag materials

Client flow (OBSERVED): 1280 RaidItemMixListScene builds its list in
`setRecipeList` @0x19F9DC4: for every ItemMst of item_type 1 or 2, the FIRST
`PermitRecipeInfoList` recipe whose RecipeMst item_id is that item (the town's
permitted recipes, sent by UserInfo as `51yQrDBR`). 1281
RaidItemMixDetailScene: `continueMix` @0x19F8B74 refuses with
`RAID_ITEMMIX_NG` when the bag already has >= raid_bag_cnt active stacks and
with `MYTOWN_ITEM_SHORTAGE_ELEM` when `RaidUtils::isMixPossible` (bag only)
fails; `procMix` @0x19F9034 decBagItem's each material and incBagItem's the
product with a hard-coded count of **1** (`mov w1, #1` @0x19F9110), and
appends an ItemMixResultInfo. No karma or zel is read or spent. Leaving either
mix scene runs `RaidUtils::createItemMixConnectList(world, room, roomMission)`,
which turns the accumulated results into ONE queued request and clears them.

Body (createBody @0x13BFAFC, itemCsvData @0x13BFEA4 is the same routine as the
normal ItemMix):

```json
{"IKqx1Cn9":[{...}],
 "8iBuZa3n":[{"43hMuY2I":"1","8VYd6xSX":"<room>"}],
 "g97TWISb":[{"ja39HMLq":"<room mission>"}],
 "JTf2jY5o":[{"4HqhTf3a":"1037:2,5000:1"}]}
```

Server rule: parse with the normal ItemMix `parseOrders` (move it to a shared
header). For each order, for each craft, in order: stop the whole order when
the recipe is unknown, not in `Town::permittedRecipes`, its product's
item_type is not 1 or 2, the bag's active stack count is already >= 50, or the
bag lacks a material (`materials` is per craft). Otherwise drain materials
(smallest stacks first) and add exactly 1 product (incBagItem rule). Respect
the RecipeMst craft cap `required_material_amount` the same way the normal
ItemMix does. No karma, no user_items. Reply `{"UsDq1E6o":[bag rows]}`.

Shipped raid recipes (F_RECIPE_MST, all karma 0, all output 1): 1037 flare
70000, 1038 smoke bomb 70100, 1039 transfer potion 70200, 5000-5003 crystals
70300/70400/70500/70600 (`11016` refining stone + a material + one more);
plus the normal potion/stone recipes 1009-1070. They are unlocked by
town_facility_lv `release_receipe`, so a fresh account can craft only what its
synthesis facility allows (same as the client's list).

### 4.4 RaidItemDelete (dead code)

createBody @0x13BF278: `{"qa3iN58C":[{"n6E8iMf3":"<frame id>","wgV86x1q":<n>}]}`,
no world/room/mission. No call site constructs the request (no
`RaidItemDeleteRequestC1Ev@plt` exists; the only xref is createBody itself).
Register it anyway (cheap): remove up to n from that frame of the caller's
bag in their live mission, reply `UsDq1E6o`; never refuse.

### 4.5 Prerequisite: RoonItemEdit must start storing

Round 1 acks `bv9xt3m6` (RaidRoonItemEdit: `Fihk73ZR` set rows +
`GvV1w4yM` reserve rows, full lists) and stores nothing. The in-field
RaidItemEdit above conserves items against the STORED set; with nothing stored
every set the player brought from the warehouse looks like items conjured from
the bag, gets rejected, and the reply overwrites the client's set. So in the
same slice, RoonItemEdit becomes: validate both lists (slots 0..4, item exists,
set counts <= max_equipped), then move the per-item difference
`(new set + new reserve) - (old set + old reserve)` out of / back into
`user_items`, exactly like `ItemEdit.cpp` does for the normal bar, all or
nothing; ack `{}` as today. Also attach `Fihk73ZR` and `GvV1w4yM` to the
RaidUserInfo reply so the room screens show the stored set after a restart
(INFERRED safe: global readers; empty arrays change nothing).

What the reserve does inside a mission is not visible in the field code (no
field scene reads RaidUserRsvItemInfoList). Leave it untouched here; the
MissionStart band decides whether reserve items seed the bag (unknown 5).

## 5. RaidShopUse (gem purchases)

### 5.1 Client (OBSERVED)

`RaidSceneBase::shopConnect(type, cost)` @0x19CE378:

* type 100 "Recover Raid Orb" (from `roomConfirmAnswerYes` @0x19DBC44 after
  `raidAubeCheck` @0x19DC63C found aube 0; reachable from RaidRoomMenuScene,
  RaidFriendListScene, RaidRoomSearchListScene and every scene whose
  confirmAnswerYes forwards to roomConfirmAnswerYes, including
  RaidQuickCheckScene): world = first RaidWorldMst row; room and room mission
  are empty and therefore omitted; connect type 21. The scenes that check 21
  (RaidRoomMenuScene @0x19BC338, RaidQuickCheckScene @0x19ABE18, friend list,
  search list, auto enter) only call `updateHeader`.
* type 101 "Recover Lives" (from `friendConfirmAnswerYes` @0x19DE6EC, confirm
  id 0x207; reached from `raidHeartShopCheck` @0x19DE7D4 in RaidUnitScene and
  from RaidFieldScene's game-over check `raidGameOverHeartCheck` @0x19DEFD4
  when PlayerRest == 0): world, room and the caller's room mission are sent;
  connect type 22. RaidFieldScene type 22 (-> 0x198AB38): CampFlg 1 -> camp,
  else rebuild the field (1242), whose `initCheck` re-reads PlayerRest from
  the cached `81B4TvEK` and asks again if it is still 0.
* type 102: accepted by shopConnect, no caller found.
* Insufficient gems is decided on the client against `UserTeamInfo::getBraveCoin`;
  the "buy gems" branch opens ShopBuyCoinScene (confirm id 0x206), i.e. the
  IAP store, which this server does not serve (BundlePurchaseIap etc. are
  unregistered). The server never sells gems.

Body (createBody @0x13C7594):

```json
{"IKqx1Cn9":[{...}],
 "8iBuZa3n":[{"43hMuY2I":"1","8VYd6xSX":"<room>"}],   // type 100: [{"43hMuY2I":"1"}]
 "g97TWISb":[{"ja39HMLq":"<room mission>"}],         // type 100: [{}]
 "32ibWjFG":[{"60IsqxDt":100,"03UGMHxF":<cost>}]}
```

`60IsqxDt` is the shop type, `03UGMHxF` the price the client showed
(`RaidSceneBase` field +0x338 / +0x33c; where that field is set was not found,
see unknown 2). Same keys as the normal ShopUse node, different group id.

### 5.2 Server rule

Gems are an in-game currency here (earned, never bought), so spending owned
gems is allowed; nothing else is ever charged. Price is server-authoritative
(the ShopUse.cpp pattern: log a mismatching claim, charge the server price):
`kRaidAubeRefillGems = 1`, `kRaidLivesRefillGems = 1` (assumption matching
DefineMst ContinuDiaCnt = 1; compare with the first device log of
`03UGMHxF`).

* type 100: aube already >= raid_max_p -> no charge. Else in one statement
  `gems -= price WHERE gems >= price`, then `raid_user.aube = 3, aube_ts = 0`.
  Not enough gems: `refuseToRetry("You do not have enough Gems.")`.
  Reply: `fEi17cnx` (`gme::getTeamInfo`) + the round-1 RaidUserInfo reply
  (`1ry6BKoe` full node, `2WqoMm4d`, `8LF2ohCY`; the 1ry6BKoe reader resets
  every field first, so send the whole node).
* type 101: needs the field band's lives column. Caller must be in a live
  room mission with lives < raid_max_p (else no charge). Charge, set lives to
  raid_max_p (INFERRED: "Recover Lives", and the MAX message fires only at the
  cap). Reply `fEi17cnx` + the full `81B4TvEK` (REQUIRED: the field's initCheck
  re-reads PlayerRest from it). Before the field band exists:
  `refuseToRetry("Lives cannot be recovered on this server yet.")`.
* type 102 / anything else: reply `fEi17cnx` only, no charge, log.

## 6. Master data this band reads

| table | server cache | used for |
|---|---|---|
| RaidWorldMst `Rqc8pi0x` | `raidWorldMst()` | camp tick (4 s) |
| RaidMissionMst `questMapId` | `raidMissionMst()` | map of the mission (camp check) |
| RaidPointMst `map_id/point_id/point_type` (`AZFM14In`, 1 = camp, 50 rows) | `raidPointMst()` | camp check |
| ItemMst `max_stack m9gd5h1u`, `max_equipped t1i2vIbT`, `item_type`, `is_raid`, `raid_usage_type` | `itemMst()` | bag stacks, set limits, mix filter |
| ReceipeMst `id/item_id/materials/required_material_amount` | `initializeResp().receipe` | raid mix |
| TownFacilityLv `release_receipe` | via `gme::Town::permittedRecipes` | which recipes may be mixed |
| DefineMst `S92Hcor3` raid_max_p = 3, `09EbcDmX` raid_bag_cnt = 50, `zkD98Hfy` recover_time_raid = 3600 | constants in `Raids.hpp` (round 1 already hard-codes the first and last) | caps |

No F_RAID_* table carries a camp heal rate, shop price or recipe list; raid
crafting is the normal RecipeMst filtered to consumable/material products.

## 7. Server design

### 7.1 Storage (append to RegisterMigrations: `16102026_RaidCampItems`)

```sql
-- Raid item set (kind 0, Fihk73ZR) and reserve (kind 1, GvV1w4yM).
-- Disjoint from user_items, like user_equip_items: what sits here has left the warehouse.
CREATE TABLE IF NOT EXISTS raid_equip_items(
  user_id TEXT NOT NULL,
  kind INTEGER NOT NULL,
  disp_order INTEGER NOT NULL,
  item_id INTEGER NOT NULL,
  item_num INTEGER NOT NULL,
  PRIMARY KEY(user_id, kind, disp_order));
-- The mission bag (UsDq1E6o): one per player per room mission.
CREATE TABLE IF NOT EXISTS raid_bag(
  room_mission_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  frame_id INTEGER NOT NULL,
  item_id INTEGER NOT NULL,
  item_num INTEGER NOT NULL,
  new_flg INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(room_mission_id, user_id, frame_id));
-- Camp state; its own table so this band does not ALTER the field band's tables.
CREATE TABLE IF NOT EXISTS raid_camp(
  room_mission_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  camp_flg INTEGER NOT NULL DEFAULT 0,
  camp_since INTEGER NOT NULL DEFAULT 0,
  healed_until INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(room_mission_id, user_id));
```

Reused from other bands (coordinate names before coding): the room mission
row (`room_mission_id`, mission id, result, start time) and per-member state
(now point, lives) from the MissionStart/field band; the per-unit party HP
table that `wc2DiMY1` is built from (max_hp, now_hp, bb, sb, ...) from the
same band. The bag is credited by RaidMissionHarvest and paid out (or
discarded) by RaidMissionEnd; both live in other bands and must use the bag
helpers below. Remember the round-1 gotcha: number `$N` placeholders strictly
left to right.

### 7.2 Code layout

* `packet-generator/assets/net/raid.kdl` (already imported after user.kdl,
  items.kdl and friends.kdl):
  * `RaidMissionKeyNode { room_mission_id "ja39HMLq" str }`
  * `RaidCampReq { login_info IKqx1Cn9, room [RaidRoomKeyNode] 8iBuZa3n, mission [RaidMissionKeyNode] g97TWISb }`
    (also used by ItemEdit/ItemMix/ShopUse via the fields below)
  * `RaidItemEditReq` = RaidCampReq + `set [UserEquipItemInfo] 71U5wzhI`
  * `RaidItemMixReq` = RaidCampReq + `mix [ItemMixEntry] JTf2jY5o`
  * `RaidItemDeleteNode { frame_id n6E8iMf3 i32::str, item_num wgV86x1q i32::str }`,
    `RaidItemDeleteReq { login_info, del [RaidItemDeleteNode] qa3iN58C }`
  * `RaidShopUseEntry { shop_type 60IsqxDt i32::str, cost 03UGMHxF i32::str }`
    (do not reuse ShopUseEntry: it is defined in handlers.kdl's body, emitted
    after raid.kdl), `RaidShopUseReq` = RaidCampReq + `shop [RaidShopUseEntry] 32ibWjFG`
  * `RaidBagInfo` (7 keys of 4.1), `RaidEquipItemInfo` (reuse `UserEquipItemInfo`),
    `RaidUserPartyDeckInfo` (14 keys of 3.3) unless the field band already
    defines it
  * responses: `RaidItemsResp { set [UserEquipItemInfo] Fihk73ZR optional, bag [RaidBagInfo] UsDq1E6o optional }`,
    `RaidCampResp { party [RaidUserPartyDeckInfo] wc2DiMY1 }` + the field
    band's mission payload fields (optional),
    `RaidShopUseResp { team_info [UserTeamInfo]::size(1) fEi17cnx, ...RaidUserInfoResp fields optional, mission users 81B4TvEK optional }`
* `gimuserver/gme/common/RaidItems.hpp/.cpp`: `loadSet(kind)`, `storeSet`,
  `loadBag`, `bagAdd(id, n)` (incBagItem rule), `bagTake(id, n)` (decBagItem
  rule, returns false when short), `bagActiveStacks`, `bagRows` (wire rows),
  `applySetEdit(oldSet, newSet, bag)` returning the new bag or a reason,
  `mixInBag(orders, bag)`; pure functions over vectors so they are unit-testable
  without SQL.
* `gimuserver/gme/common/RaidCamp.hpp/.cpp`: `isCampPoint(missionId, pointNum)`,
  `campHeal(units, healedUntil, now, tick)` (pure), `startCamp`, `endCamp`.
* `gimuserver/gme/handlers/RaidCampItems.cpp`: one HANDLEF per request,
  each: read leniently, identify, resolve the room mission, validate, mutate in
  one transaction (`gme/common/Transactions.hpp`), build, write.
* `gme/handlers/ItemMix.cpp`: lift `splitOn`/`toInt`/`parseOrders` into
  `gme/common/ItemMixOrders.hpp` and use it from both handlers.
* `Handlers.hpp` declarations; `GmeControllerHandlers.cpp`:

```cpp
REGISTER("I7nVUv0h", RaidCampRestStart, "qy27SYu9");
REGISTER("diIe8ux7", RaidCampRestGet,   "G5uind2r");
REGISTER("f9Dh4djx", RaidCampRestEnd,   "0TCX6S4R");
REGISTER("DZLI83hx", RaidItemEdit,      "3Lxpf5KE");
REGISTER("K17JUYR2", RaidItemMix,       "c6wBM9sH");
REGISTER("N43JmIf2", RaidItemDelete,    "jvm3q24f");
REGISTER("4zJ0WmTX", RaidShopUse,       "RSvU7dh6");
```

### 7.3 Order of work / what can ship before the field band

| piece | depends on | ships alone? |
|---|---|---|
| RoonItemEdit storage + Fihk73ZR/GvV1w4yM on RaidUserInfo | nothing | yes, testable on device now (room item screens) |
| RaidShopUse 100 | round-1 raid_user (aube is never spent until MissionStart exists) | wire yes; on device only after aube can reach 0 |
| RaidItemEdit, RaidItemMix, RaidItemDelete | room mission id + bag table (bag filled by Harvest) | wire yes (seed raid_bag in the test); device after MissionStart+Harvest |
| Camp Start/Get | room mission, now point, party HP table | wire with seeded rows |
| Camp End, Shop 101 | field band's 81B4TvEK builder and lives column | no: refuse cleanly until then |

## 8. Wire test plan (`tools-local/raid_camp_items_test.py`)

Modelled on `raid_entry_test.py` (two guest players A and B) and
`test_arena.py` (`--db /deploy/gme.sqlite` to seed rows; back the database up
first). Steps:

1. RoonItemEdit: A owns 5x 20000 and 3x 20200 in user_items; send set
   `[20000 x3 @0]`, reserve `[20200 x2 @0]` -> user_items 2 and 1 left;
   raid_equip_items rows exist; a second edit returning the set to the
   warehouse restores the counts; an over-count (more than owned) changes
   nothing.
2. RaidUserInfo now carries Fihk73ZR/GvV1w4yM.
3. Seed a live room mission for A (field band's tables or a fixture) with a
   bag: 11016 x2, 10610 x1, 10309 x1, 20000 x5 (two stacks: 3 and 2).
4. RaidItemEdit set `[20000 x5 @0]` -> bag loses 2 more 20000 from the
   SMALLEST stack first; reply carries Fihk73ZR and UsDq1E6o; set row 0 = 5.
5. RaidItemEdit set `[]` -> 5 back into the bag (top-up then new frame).
6. RaidItemEdit asking for 9x 20200 not in the bag -> nothing changes, reply
   is the unchanged server state, status is success (never an error).
7. RaidItemMix `"1037:1,1038:1"` -> bag: 11016 0, 10610 0, 10309 0, +1 70000,
   +1 70100; karma and user_items untouched. `"1037:5"` with materials for
   one -> exactly one crafted. Unknown recipe -> no change, success.
8. Bag at 50 active stacks -> mix crafts nothing.
9. RaidItemDelete frame of 70000 -> stack gone; unknown frame -> success no-op.
10. Seed A at a camp point (point 1 of the mission map) with party HP 100/1000:
    CampRestStart -> camp_flg 1, reply has wc2DiMY1; move healed_until back 12 s
    in the DB; CampRestGet -> now_hp 400 (3 ticks x 10 %); CampRestEnd -> camp_flg
    0 and (when the builder exists) A's 81B4TvEK row has MFz8YRS6 "0".
11. A at a non-camp point: CampRestStart -> refusal (Retry command), no row.
12. B polls CampRestGet for A's mission -> refusal; A's state untouched.
13. ShopUse 100 with aube 0 and 3 gems -> aube 3, gems 2, reply has fEi17cnx
    and 1ry6BKoe; again with aube 3 -> no charge; with 0 gems and aube 0 ->
    refusal, nothing changed; claimed cost 99 -> charged the server price.
14. ShopUse 101 before the field band: refusal, no charge.

Device checks once the field exists: the camp HP bars climb every 4 s, leaving
the camp lands on the field (not back in the camp), crafting a flare at the
camp shows it in the bag after returning to the field, and a lives refill from
the game-over prompt does not re-prompt.

## 9. Unknowns

1. Camp heal rate and whether resting revives KO'd units: no client or master
   data value exists; the 10 %/tick and "revives" defaults are owner choices.
2. The price the client shows for both shop types (`RaidSceneBase` +0x338/
   +0x33c): no writer was found in raid code; it may even be 0. The server
   price (1 gem each) is authoritative; read `03UGMHxF` from the first device
   log and adjust the constant.
3. Whether type 101 restores one life or all of them (the client only blocks at
   the cap); default: all.
4. Where point buttons 112 (Mix) and 115 (Items) are drawn; their handlers exist
   and lead to 1280 / 1243.
5. What the reserve list (`GvV1w4yM`) does inside a mission: no field scene
   reads it. MissionStart band to decide (likely: stays as a pre-mission spare
   set, or seeds the bag).
6. An empty `UsDq1E6o` cannot clear the client's bag (no group-start hook), so a
   previous mission's bag could survive into a new mission whose bag starts
   empty. Check on device; the fallback is one placeholder row at count 0
   (`getActiveCount` ignores it, but `getList` @0x12FDBE8 would still list it).
7. What happens to the bag and the set at mission end / retire / failure
   (credited to user_items, lost, kept): MissionEnd band.
8. Which reply first hands the client its persisted raid set before any room
   screen (attaching it to RaidUserInfo is the proposal, unverified).
9. Whether the client shows an error dialog for a failed async CampRestGet the
   same way as for a foreground request (INFERRED yes: the camp's noticeOK
   handles -4000 either way).

## 10. As built (2026-10-17)

* Emptied bag stacks are stored and sent at count 0 (the core's rule): the
  bag list is replaced only by a non-empty reply. `RaidItemsResp` omits a
  list only while it never held anything.
* The bag lives in the core's `raid_mission_bag` (+ `new_flg`); camp state
  is `camp_since` / `healed_until` on `raid_mission_user` (no `raid_camp`
  table). Harvest and item use go through `bagAdd` / `bagTake`.
* Camp: Rest one route step from a camp takes the pending walk (the button
  is drawn before the poll reports the move); the heal claims its ticks with
  a compare-and-swap on `healed_until` and heals nothing while a battle is
  open. CampRestStart only counts CampRecover when it actually starts.
* Queued item requests open their transaction first and re-read the run
  and member inside it.
* Battle item use: client event type 2 (`0:itemId:useNum`) takes the items
  off the raid set once (`spendSetItems`); a field use from the set
  (Y5k8LJs9 0) takes one.
* Shop 100/101 decide "already full" inside the charge's transaction (101
  claims the hearts with a conditional UPDATE first), so a double tap pays
  once. Unknowns 2 and 3 are answered by the device: the game-over prompt
  reads "You can use 1 Gem to recover 1 Life", so 101 costs 1 gem and adds
  ONE life per purchase (the orb refill is assumed to cost 1 gem too).
* Not done: the RecipeMst craft cap (`required_material_amount`) is not
  enforced for raid mixing (the normal ItemMix does not enforce it either).

Device (emulator-5558, 2026-10-17): Manage Items stored the set
(RoonItemEdit) and it came back after an app restart (RaidUserInfo
Fihk73ZR); Rest / Stop Resting at the camp returned to the field; at 0
lives the Recover Lives prompt charged 1 gem and play went on.
