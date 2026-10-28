# Classic Raid: reverse-engineering spec and first slice

Stage 1 deliverable for the `feat/raids` track (2026-10-16). Scope is classic
Raid Battle only: async HTTP co-op, 1 to 4 players, lobby and field polled
every 4 s. Guild Raid (Pusher, realtime) is out of scope.

Evidence labels: **OBSERVED** = read from `libgame.so` (address given), the
server source, or the shipped master data. **INFERRED** = reasoned from those
but not yet confirmed by a live client. Nothing here has been seen on a device
yet: no raid response from the live game was ever captured, every shape below
is rebuilt from `*Response::readParam` and `*Request::createBody`.

Tools used (re-runnable): `re/work/raid/summ.py`, `re/work/raid/kmap.py`, plus
two scratch helpers written this stage (an annotated function dumper and a
branch-following key->setter mapper, `km2.py`); their output is what the key
tables below quote. Scene ids were decoded from the jump table in
`GameScene::getGameScene` @0x1612C4C (table @0x2349046, `target =
0x1612CB8 + hword*4`).

---

## 1. Transport facts that shape the design

| Fact | Evidence |
|---|---|
| Every classic Raid request goes to the normal game server. `GameScene::accessPhpRaid(RequestData*, RaidWorldMst*)` ignores the world's host column (`PIa6T3RK` = `ios125.bravefrontier.gumi.sg`) and calls `CommonUtils::getPhpUrl()` + `accessPhp`. | OBSERVED @0x16081F0 |
| Raid is enabled client-side by the featureCheck key `raid` (FUNC_RAID); `deploy/system/features.json` already sends `"raid": 1`. `raid_beta` is a separate key (sent 0). | OBSERVED `FeatureManager::onFeatureCheckResS` @0x1C77890 |
| The Home Raid padlock is FeatureGates id 14 (`gme/common/FeatureGates.hpp`). The server currently sends it locked. | OBSERVED server source |
| Raid master data (F_RAID_WORLD/MAP/POINT/USER_ROUTE/BOSS_ROUTE/BOSS/MISSION/BATTLE_GROUP/PLAYSTYLE/RC/MISSION_POINT/MISSION_CLEAR_CND/BOSS_PARTS/MISSION_BOSS/ELEMENTAL_LOCK) is already in the Initialize version manifest (`KeC10fuL`, `deploy/mst/version_info_mst.json`) and on disk as `deploy/mst/raid_*.json`; `ServerCache` loads 13 of them (`raidWorldMst()` ... `raidBattleGroupMst()`). The client reads them from its own MST cache, the server never re-sends them. | OBSERVED |
| `RaidWorldMst` has exactly one world: id `"1"`, name `WORLD1`, poll interval `Rqc8pi0x="4"`. | OBSERVED `deploy/mst/raid_world_mst.json` |
| Response groups are dispatched by group key to a `*Response` class no matter which request was sent, so the server may add any raid group to any reply and the client stores it into the shared list. | OBSERVED (`GameResponseParser::parseBodyTag`), consistent with existing handlers |
| The parser holds a body as a `picojson` object (`std::map`), so keys inside a node reach `readParam` in **alphabetical** order, not in the order the server wrote them. None of the readers below depend on order except through the "last param of node" tail, which is order-independent. | OBSERVED (parseBodyTag signature @0x139B008) |
| Unregistered group ids already answer "This feature isn't available on this server yet" and return Home, so every raid request we do not implement degrades without closing the session. Note `HandleResult::refuseToHome` documents Home-rebuild "outside a raid": raid scenes have their own `noticeOK`; confirm on device that ReturnToGame from a raid scene lands Home. | OBSERVED server; device behaviour INFERRED |

## 2. Entry flow (what tapping Raid does)

Scene ids are decimal; the constructor argument equals the scene id.

```
Home (HomeScene2::touchEnded2 @0x16F5404)
  -> GameScene::changeRaidScene @0x1615CE0: StepScene -> 1200, play bf047_raid_valden.mp3,
     RaidInfo::setRaidEnterFlg(1). First visit also sends UserEnteredFeature(14) (already handled).
1200 RaidInitConnect(1200)            updateEvent @0x199FB9C
     FUNC_RAID == 0                  -> 1204 RaidCloseScene
     RaidWorldMstList empty           -> scene 41 = HomeScene2        [MST cache missing]
     else send RaidUserInfoRequest (jJZ01VQo), on success -> 1202 RaidInitScene
1202 RaidInitScene(1202)              initialize @0x199FE50, caseInit @0x19A0480
     special scenario "raid" not finished -> SpecialEventScene("raid_op.txt") then back to 1202
     agreement != 1                   -> 1205 RaidTopScene (terms) -> RaidAgreementRequest -> 1202
     loginWorldID empty OR loginRoomID empty
                                      -> RaidWorloInfoRequest (H7GRp01L) -> 1207 RaidWorldScene
     else OpenFlg == 1                -> setWorld(loginWorldID), RaidRoomInfoRequest -> caseGetRoom
     else                             -> "RAID_WORLD_OPEN_NULL"
     (1203 variant: RaidInfo errorCount >= 2 with a login room -> RaidRoomDissolveRequest)
1207 RaidWorldScene(1207)             setList @0x19EE6DC, touchEnded @0x19EFCB8
     list = synthetic "-1000" QUICK row (RAID_WORID_QUICK_START, when FUNC_RAID_QUICK, default 1,
            and getTopWorld() finds a RaidWorldInfo row) + one row per RaidWorldMst
     tap QUICK  -> setItemAutoMaxFlg(1), setWorld(topWorld) -> 1293
     tap world  -> RaidWorldInfo(world).raidOpenFlg == 1 ? setWorld -> 1210 : "RAID_WORLD_SEL_NG_WINDOW"
```

Solo (quick start) path, the target of the first playable slice:

```
1293 RaidQuestListScene(1293)  choose RC tier            (no request)
1294 RaidQuestListScene(1294)  choose mission            (no request)
     list = RaidMissionMstList::getList(selRC) filtered by RaidPermitMissionInfoList::isMission(id)
            setList @0x19A5F54; isMission @0x12F6FE0 is pure id membership.
     => WITHOUT RaidPermitMissionInfo rows THE MISSION LIST IS EMPTY.
1295 RaidFriendSelectScene(1295) helper pick; initConnect sends RaidRoomFriendGetRequest (Xe2UB3A8),
     "no helper" button exists (RAID_NO_FRIEND when the list is empty)
1296 RaidQuickCheckScene        party/item check; updateEvent @0x19ABA60 first flushes queued
     ConnectRequestList entries (ItemMix K17JUYR2, ItemEdit DZLI83hx, UpDateCash XTban70i,
     RoonItemEdit bv9xt3m6), then RaidQuickStartRequest (CWeTY8BQ).
     On success connectEndCheck @0x19ABE3C: roomInfoUpDate, setWorld(RaidRoomInfo.worldID),
     errorAll (needs world AND room), changeField -> 1239
1239 RaidFieldReady(1239)       connectQuestStart: RaidMissionStartRequest (2qDW9NBU) -> field 1240+
```

Room (multiplayer) path for later slices: `1210 RaidRoomMenuScene` -> `1211`
make (RaidUpDataRoomInfo) / `1214` search and `1224` auto (RaidRoomList, then
RaidEnter) -> `1216/1290 RaidRoomTopScene` lobby (RaidRoomInfo heartbeat,
ready, chat) -> `1217/1218` quest list -> `1219 RaidQuestInfoScene`
(RaidMissionSelect) -> `1238/1239 RaidFieldReady` (RaidMissionStart).

Full raid scene id table (decoded): 1200/1201 RaidInitConnect, 1202/1203
RaidInitScene, 1204 RaidCloseScene, 1205 RaidTopScene, 1206/1207
RaidWorldScene, 1208 RaidFriendListScene, 1209 RaidPlayerInfoResultScene, 1210
RaidRoomMenuScene, 1211 RaidRoomMakeScene, 1212 RaidRoomEditScene, 1213
RaidConditionScene, 1214 RaidRoomSearchScene2, 1215 RaidRoomSearchListScene,
1216/1290 RaidRoomTopScene, 1217/1218/1293/1294 RaidQuestListScene,
1219/1220/1255 RaidQuestInfoScene, 1221-1223 RaidRoomItemEdit*, 1224
RaidRoomAutoEnterScene, 1225/1226 RaidBlackList*, 1229 RaidItemScene, 1230
RaidFriendScene, 1231/1232/1247/1248/1250 RaidUnitScene, 1233/1253/1267
RaidChatScene, 1234/1254 RaidTeamScene, 1238/1239 RaidFieldReady, 1240-1242
RaidFieldScene, 1243 RaidFieldItemScene, 1244 RaidFieldItemUseScene,
1245/1246 RaidFieldCarryItemScene, 1251/1252/1295 RaidFriendSelectScene, 1256
RaidFieldCampScene, 1257/1258 RaidFieldPickScene, 1259 RaidBattleEndScene,
1260 RaidBattleStartScene, 1261-1266/1271/1287/1288 RaidFieldQuestResult*,
1268/1269 RaidQuestGiveUpScene, 1270 RaidBattleRestartScene, 1280-1284
RaidItemMix/Detail/Edit*, 1286/1291 RaidQuestTopScene, 1296
RaidQuickCheckScene.

## 3. Wire contracts for the first slice

Every request also carries the standard identity groups (`IKqx1Cn9`
LoginInfo, `6FrKacq7` signal key, version tag); read with
`glz::read<glz::opts{.error_on_unknown_keys = false}>` and resolve the player
with `gme::getUserIdentity(theDb(), req.login_info)`. All values go out as
strings (`i32::str` / `str`), matching every other handler.

### 3.1 RaidUserInfo `jJZ01VQo` / aes `SsQ26dVY`

Request: bare (`RaidUserInfoRequest::createBody` adds identity tags only).
OBSERVED.

Response group `1ry6BKoe` = `RaidUserInfoResponse` (single node). Reader
@0x1444ED0. Before reading keys it resets OpenFlg=0, RC=1, Agreement=0,
LoginWorldID="", LoginRoomID="", RaidAube=0, RaidRestTimer=0, so every key
should be sent every time.

| key | type | setter | meaning / value to send |
|---|---|---|---|
| `7x3pPB2C` | int | setRC | Raid Class 1..7 (RaidRcMst ids 2..7 are the class-up rows). From `raid_user.rc`. |
| `dziN1Ha9` | int | setAgreement | 1 once the terms screen was accepted, else 0 -> scene 1205. |
| `43hMuY2I` | str | setLoginWorldID | world of the room the player is still in, else "". |
| `8VYd6xSX` | str | setLoginRoomID | that room id, else "". Both non-empty makes RaidInitScene resume the room via RaidRoomInfo, so send them ONLY for a live room. |
| `iFA0fd6Y` | int | setOpenFlg | 1 = raid open. Must be 1 for the resume branch. |
| `Z0Y4RoD7` | int | setActiveDeckNum + setServeDeckNum | party deck index (0-based, like the main party). |
| `C45mRxnZ` | int | setRaidAube | current raid stamina ("aube"). |
| `S92Hcor3` | int | setRaidAubeMax | cap; DefineMst `raid_max_p` = 3. |
| `6d2XBDUu` | int | setRaidRestTimer | seconds until the next aube; DefineMst `recover_time_raid` = 3600. 0 when full. |

The existing `net/raid.kdl` `RaidUserInfo` struct has these keys as
`unk1..unk11` plus `3wp7j5MI` and `N4XVE1uA`, which this reader never reads;
rename the fields and drop or mark those two.

Also attach to this reply (INFERRED necessary, safe to send):

* `2WqoMm4d` RaidPermitMissionInfoResponse, list: `j28VNcUW` mission id (str),
  `qY49LBjw` time limit (int, seconds; use RaidMissionMst `qY49LBjw`). One row
  per mission the player may start. Policy for this slice: every RaidMissionMst
  row with `rc <= raid_user.rc`. Without it the quest list is empty.
* `8LF2ohCY` RaidClearMissionInfoResponse, list: `h7eY3sAK` user id, `j28VNcUW`
  mission id. Drives the `quest_clear_img` stamp. Empty list is valid.

### 3.2 RaidAgreement `Cu94xU8j` / aes `Eh5a3yr9`

Request: bare. OBSERVED. Set `raid_user.agreement = 1` and reply with the same
body as 3.1. RaidTopScene::updateEvent @0x19E3170 then returns to 1202.

### 3.3 RaidWorloInfo `H7GRp01L` / aes `5JxFp0tD` (sic, "Worlo")

Request: bare. OBSERVED.

Response group `taWh0Pr7` RaidWorldInfoResponse, list, one row per
RaidWorldMst:

| key | type | setter | value |
|---|---|---|---|
| `43hMuY2I` | str | setWorldID | RaidWorldMst id ("1") |
| `SyL0XFc6` | int | setCrowdedPercent | 0..100, from live member count (rooms*4 capacity, any monotone formula is fine) |
| `iFA0fd6Y` | int | setRaidOpenFlg | 1, else the world is refused and QUICK disappears (getTopWorld) |

Also re-send `2WqoMm4d` and `8LF2ohCY` here (the world screen is where a
returning player lands). Optional `8iBuZa3n` room list for the friend
shortcut (`RaidWorldScene` reads `RaidRoomInfoList::getListMissionFlg`): send
none in this slice.

### 3.4 RaidRoomFriendGet `Xe2UB3A8` / aes `9WN8oYBx`

Request group `8iBuZa3n`: `43hMuY2I` world id, `8VYd6xSX` room id (empty in
the quick path, INFERRED).

Response group `ft4zY6Uv` RaidRoomFriendGetResponse, list of FriendInfo. Its
key set is a strict subset of `FriendInfoResponse` (`tojMy68W`): `h7eY3sAK
B5JQyV8j 2Fh3J7ng 96Nxs2WQ 0CAQ6wUe pn16CNah 4A6LzBxr e7DK0FQT 67CApcti
q08xLEsy PWXu25cg cuIWp89g TokWs1B3 RT4CtH5d t4m1RH6Y GcMD0hy6 e6mY8Z0k
C1HZr3pb X6jf8DUw a1Jp3TVb s2WnRw9N 5JbjC3Pp Ge8Yo32T mZA7fH2v 98WfKiyA
bM7RLu5K JmFn3g9t U4pMNjy0 nBTx56W9 nj9Lw7mV 3NbeC8AB iEFZ6H19 RQ5GnFE2
cP83zNsv LjY4DfRg 2pAyFjmZ Fnxab5CN 3InKeya4 zsiAn9P1 yu18xScw jkldTrhL
9i2xhMaJ iNy0ZU5M btZizNep RVVgyuor U8FCB2Wj NqVAPbLC mgNdrCEe`. Reuse the
existing `::FriendInfo` struct and the FriendGet builder
(`gme::playerPeak`, `gme::loadFriendRoster`, `gme::friendUnitFor` in
`FriendGet.cpp`) under the new group key. An empty list is valid (the scene
shows RAID_NO_FRIEND and the "no helper" button).

### 3.5 RaidQuickStart `CWeTY8BQ` / aes `HsVofOJJ`

Request (createBody @ RaidQuickStartRequest, OBSERVED):

* `8iBuZa3n` node: `j28VNcUW` mission id.
* `ZX7gzQ6s` node: the chosen helper as FriendInfo (`h7eY3sAK` user id,
  `B5JQyV8j` name, `2Fh3J7ng` team lv, `96Nxs2WQ` friend type, `0CAQ6wUe`,
  `pn16CNah` unit id, `4A6LzBxr` lv, `e7DK0FQT` hp, ... `98WfKiyA` friend id,
  `2pAyFjmZ`, `edy7fq3L`, `cP83zNsv`, `LjY4DfRg`, `Fnxab5CN`). Whether the
  group is omitted or empty for "no helper" is UNKNOWN: accept both.

Validation: mission exists in RaidMissionMst and is permitted (3.1 policy);
player is not already in a live room (if they are, dissolve the stale one
first; the client cannot resolve it on its own); aube is NOT spent here
(roadmap: aube is deducted at MissionStart).

Response (INFERRED minimum for `connectEndCheck` -> `errorAll` -> 1239):

* `8iBuZa3n` RaidRoomInfoResponse, list with ONE row (the client takes
  `RaidRoomInfoList::getObjectTop(false)` in `RaidUtils::roomInfoUpDate`
  @0x11CC23C):

| key | type | setter | value for a solo room |
|---|---|---|---|
| `8VYd6xSX` | str | setRoomID | new room id |
| `43hMuY2I` | str | setWorldID | "1" (must exist in RaidWorldMst or errorAll fires RAID_WORLDDATA_NULL) |
| `8ZK3w0Np` | str | setMasterUserID | player's user id |
| `IAfv98Wd` | str | setMasterUserName | handle name |
| `6WKvM4o8` | int | setMasterUserLv | team level |
| `5bBwEo9f` | str | setMasterUserLeaderUnitID | leader unit id |
| `fZgEP96L` | int | setMasterUserLeaderUnitLv | leader lv |
| `i9L42Gx0` | int | setMasterUnitImgType | 0 |
| `NQR1Vd3t` | str | setPlaystyleID | "1" (RaidPlaystyleMst "Not Specified") |
| `TdDHf59J` | str | setTargetID | "" |
| `j28VNcUW` | str | setMissionID | the mission |
| `3tWuc7de` | int | setMissionStartFlg | 0 |
| `rPIya39u` | int | setRoomMemberNum | 1 |
| `GWfpjED4` | int | setQuickStartFlg | 1 (RaidFieldReady reads it to set QuickQuestFlg) |
| `KQA1oWj7` | int | setQuickMatchingFlg | 0 |
| `1Txpza2u` | int | setMidwayJoinableFlg | 0 |
| `j3sGA1zt` | int | setKickFlg | 0 |
| `wYmg52xp` | int | setMinRC | 1 |
| `0TfXnP3m` | str | setMinLv | "1" |
| `WDUyI85C` | str | setComment | "" |
| `4WSu1irc` | str | setPassword | "" |
| `XqMsnE89` | int | setPasswordFlg | 0 |
| `nJ2Qe3wM` | int | setDissolveTime | seconds left before auto-dissolve (unit INFERRED) |
| `4Qf8RePm` | int | setRoomMissionJoinableRestTime | 0 |
| `ouU5H7qm` | int | setSearchType | 0 |
| `mnZ5K4Ii` | int | setOpeType | 0 |
| `C1vG0iKh` | int | setEnterableFlg | 0 for a solo room |
| `gkYiqB40` `H5pQK04c` `usM7W3DS` `1ctR6GHC` `8Ix0Soup` `JmLLHcDv` | str/str/int/str/int/int | friend user id / name / lv / leader unit / leader lv / img type | the helper from `ZX7gzQ6s`, or empty/0 |
| `jkldTrhL` `9i2xhMaJ` `iNy0ZU5M` `btZizNep` `RVVgyuor` | int/int/int/str/str | Ep3Flg, Sex, Element, HairID, ArmID (summoner avatar) | copy from the player's summoner info, 0/"" if absent |

* `uwH62EZy` RaidRoomUserInfoResponse, list; must contain the player
  (`RaidSceneBase::myRoomInfo` looks the player up by user id, and
  `roomConnectEndPlay` @0x19DC268 kicks the player out when `D2ZKtGR9`=1):

| key | type | setter | value |
|---|---|---|---|
| `8VYd6xSX` | str | setRoomID | room id |
| `h7eY3sAK` | str | setUserID | user id |
| `B5JQyV8j` | str | setUserName | handle |
| `F6bPj50Q` | str | setLeaderUnitID | leader unit id |
| `7x3pPB2C` | int | setUserRC | raid class |
| `D9wXQI2V` | int | setUserLv | team lv |
| `C45mRxnZ` | int | setUserAubeNum | aube |
| `uX87tAZB` | int | setReadyInfo | 1 (solo is always ready) |
| `j3g5P4cq` | int | setMissionStatus | 0 = in lobby, not in the field (INFERRED; RaidInitScene::caseGetRoom branches on it) |
| `D2ZKtGR9` | int | setKickedFlg | 0 |
| `BY8fZ7M1` | int | setActiveStatus | 1 |
| `Tq86EG0N` | int | setSortieFlg | 0 |
| `FV6MU5nQ` | int | setCurrentRoomMissionID | 0 |
| `j28VNcUW` | str | setMissionID | mission id |
| `2pAyFjmZ` | int | setUnitImgType | 0 |
| `sD73jd20` | int | setGuildID | guild id or 0 |

* `ZX7gzQ6s` RaidSelFriendInfoResponse: echo the chosen helper (same FriendInfo
  keys; it lands in `RaidInfo::setSelFriendInfo`). Omit when there is none.
* `1ry6BKoe` RaidUserInfo (3.1) with `43hMuY2I`/`8VYd6xSX` now set, so a
  crash or app restart resumes the room through RaidInitScene.

### 3.6 Requests the quick path fires automatically (must not fail)

`RaidQuickCheckScene::updateEvent` drains `ConnectRequestList` before
QuickStart. These are sent whenever the player touched items or the deck:

| id | class | slice-1 answer |
|---|---|---|
| `bv9xt3m6` | RaidRoonItemEdit (aes `9mKud8kc`) | ack `{}` (roadmap: "fires automatically, must not fail"). Body: `Fihk73ZR` equip rows {`kixHbe54` item, `XuJL4pc5` order, `wgV86x1q` num}, `GvV1w4yM` reserve rows (same keys). |
| `XTban70i` | RaidUpDateCash (aes `H5d6B2pi`) | ack `{}`. Body: `8iBuZa3n` {`43hMuY2I`,`8VYd6xSX`,`X6WBc9kJ`}, `79PaHFBh` {`sQDIZ8Pj`}. |
| `DZLI83hx` | RaidItemEdit (aes `3Lxpf5KE`) | not in slice (leave unregistered unless a test hits it). |
| `K17JUYR2` | RaidItemMix (aes `c6wBM9sH`) | not in slice. |

An ack must still be a valid body; if any of these returns an error the
queue aborts and QuickStart never goes out (INFERRED from the loop shape).

### 3.7 Where slice 1 stops

`RaidMissionStart` (`2qDW9NBU`) is the next request after 1239. Until the
field (RaidMissionInfo heartbeat, boss state, battle start/end) exists it must
refuse cleanly AND clear the room, otherwise the next Raid tap resumes a dead
room through RaidInitScene: refuse with `HandleResult::refuseToHome("Raid
quests are not available yet.")`, after deleting the room and blanking
`raid_user.login_*`. Request body for later: `8iBuZa3n` {`43hMuY2I`,
`8VYd6xSX`, `8ZK3w0Np` master id}, `wc2DiMY1` party rows {`edy7fq3L` user
unit id, `pn16CNah` unit id, `e7DK0FQT` hp, `PWXu25cg`, `XuJL4pc5` order,
`nBTx56W9` unit type, `gr48vsdJ` member type, `4A6LzBxr` lv, `2pAyFjmZ` img
type, ...}.

Also register in slice 1 (cheap, keeps resume/back-out paths clean):

* `jqi71nWu` RaidExit (aes `PD4ke3Ii`): body `8iBuZa3n` {`43hMuY2I`,
  `8VYd6xSX`}, `uwH62EZy` {`h7eY3sAK`}. Remove member; dissolve when empty;
  blank `login_*`; reply RaidUserInfo.
* `H8N1R0TE` RaidRoomDissolve (aes `q7dbw29J`): body `8iBuZa3n` {`43hMuY2I`,
  `8VYd6xSX`}. Master only (non-master: just leave). Reply RaidUserInfo.
* `0VxE6kTW` RaidRoomInfo (aes `cUrw7K6W`), minimal: body `8iBuZa3n`
  {`43hMuY2I`,`8VYd6xSX`}, `pw38qdui` {`NLKce95n` last chat log id},
  `T49LxzKr` {`wbo6qS5P` first-connect flag, `c13QLNRf` renew count}. Reply
  `8iBuZa3n` + `uwH62EZy` (+ `9PS8BLiC` party, `g97TWISb` room mission when
  they exist); a missing room replies with an empty `8iBuZa3n`, which the
  client turns into RAID_ROOMDATA_NULL (`RaidSceneBase::errorRoom`), then
  blank `login_*`.

## 4. Later-slice request bodies (already decoded, for planning)

| id | class | request groups/keys |
|---|---|---|
| `YF9UQpn6` RaidRoomList | `8iBuZa3n` {`wYmg52xp` minRC, `43hMuY2I` world, `NQR1Vd3t` playstyle, `j3sGA1zt` kick, `1Txpza2u` midway, `0TfXnP3m` minLv, `j28VNcUW` mission, `ouU5H7qm` search type} |
| `52H0dkjv` RaidUpDataRoomInfo | `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX` ("" = create), `NQR1Vd3t`, `WDUyI85C` comment, `4WSu1irc` password, `wYmg52xp`, `nXCf2FE9` (unknown), `0TfXnP3m`, `j28VNcUW`, `j3sGA1zt`, `1Txpza2u`, `KQA1oWj7`, `mnZ5K4Ii`} |
| `yZReG75i` RaidEnter | `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`, `4WSu1irc` password} |
| `vmdA2GD8` RaidUpDataUserReady | `8iBuZa3n` {`43hMuY2I`,`8VYd6xSX`}, `uwH62EZy` {`h7eY3sAK`, `uX87tAZB` ready} |
| `8iQL3IKA` RaidMissionSelect | `8iBuZa3n` {`43hMuY2I`, `j28VNcUW`, `8VYd6xSX`, `8ZK3w0Np` master} |
| `BqF2x3EC` RaidMissionInfo | `8iBuZa3n` {`43hMuY2I`,`8VYd6xSX`}, `g97TWISb` {`ja39HMLq` room-mission id}, `pw38qdui` {`NLKce95n`}, `81B4TvEK` {`pjX27bKB` now point} (only when a move is pending) |
| `k5pMN68B` RaidGetChatLog | dungeon-event user tag, `8iBuZa3n` {`43hMuY2I`,`8VYd6xSX`}, `pw38qdui` {`NLKce95n`} |
| `5Ko2BjPA` RaidMissionBattleEnd | `yL3Yrf1M` {mission serial `1kisc6IF`, boss enc `SP29fLtH`, `x69GYheC` dead count}, `8iBuZa3n`, `zS9s5bEI` battle result (csvs of defeated monsters, damage logs, `QP4cwt7n` client events), `wc2DiMY1` party HP/BB/SB |

Later-slice response readers (key -> setter, `(i)` = parsed as int):

* `9PS8BLiC` RaidRoomPartyInfo: `h7eY3sAK` UserID, `edy7fq3L` UserUnitID,
  `pn16CNah` UnitID, `2pAyFjmZ`(i), `zsiAn9P1` DeckNo(i), `gr48vsdJ`
  MemberType(i), `XuJL4pc5` Disporder(i), `4A6LzBxr` UnitLv(i), `e7DK0FQT`
  UnitHp(i), `cuIWp89g` UnitAddHp(i), `TokWs1B3`(i), `Ge8Yo32T` EqpItemID,
  `mZA7fH2v` EqpItemID2, `cP83zNsv`/`LjY4DfRg` ExtraPassiveSkillID 1/2,
  `nBTx56W9` UnitTypeID(i), `98WfKiyA` FriendUserID, `Fnxab5CN` FeSkillInfo,
  `jkldTrhL 9i2xhMaJ iNy0ZU5M`(i) `btZizNep RVVgyuor`, `U8FCB2Wj` ArmLv(i),
  `NqVAPbLC` Arm(i), `mgNdrCEe` AbilityInfo, `utP1c0CD` SummonerName.
* `g97TWISb` RaidRoomMissionInfo (single): `j28VNcUW` MissionID, `3tWuc7de`
  Result(i), `8n7oG4fS` RestTime(i), `d8IPTUp6` Attalnment, `de9f4qXb`
  PointSetItem.
* `81B4TvEK` RaidMissionUesrInfo: `h7eY3sAK`, `pjX27bKB` NowPointNum(i),
  `qbJcuC60` DstPointNum(i), `fP1r39bd` PlayerRest(i), `B5JQyV8j`,
  `k3I8AWLs` LeaderUnitID, `MFz8YRS6` CampFlg(i), `zHs17TE2` SearchInfo,
  `EqgF1H8M` BattleNum(i), `53NMFRL9` BattleBossFlg(i), `4rcdJRS6`/`5soj5vGI`
  Boss.
* `Ts2zJ6gj` RaidMissionBossInfo: `SP29fLtH` MissionBossID, `4x7IS9Vg`
  BossNowHp(i), `7Ex5mKRP` BossCurrentPoint(i), `5NAb7ds3`/`9P1MbHx5` pos(i),
  `hsjMq7i6`/`g57HTBUa` goal pos(i), `7z6dqvuc` VisibleFlg(i).
* `2cBk5WH8` RaidMissionBattleInfo: `j28VNcUW` RoomMisID, `qbJcuC60`
  PointNum(i), `tA4T0GXB` BossID.
* `UC8S1AiX` RaidMissionResult: `3fHunNe4` FirstClearFlg(i), `6dkEYp04`
  BossFinished, `oFQ3mbS6` FriendCnt(i).
* `awQFCW03` RaidMissionRCUpInfo: `UJTR1h5r` RcUPFlag(i), `3Y9cKkuV`
  RcQuestBefore(i).
* `UsDq1E6o` RaidUserBagInfo: `n6E8iMf3` FrameID(i), `h7eY3sAK`, `kixHbe54`
  ItemID, `wgV86x1q` ItemNum(i), `DbMVG16I` DiffType(i), `dJNpLc81` NewFlg(i),
  `Slkc395l` EventTokenID.
* `Fihk73ZR` RaidEquipItemInfo / `GvV1w4yM` RaidRsvItemInfo: `kixHbe54`,
  `XuJL4pc5` DispOrder(i), `wgV86x1q` ItemNum(i).
* `6nqGvdY1` RaidUserTeamArchiveInfo (player-info screen): 30 int counters,
  `T7BvL1kh` Challenge ... `b28wpmvq` CampRecover (full list in `km2.py`
  output; `b5LZj7fy` unresolved).
* `WCA9VhL3` RaidGetChatLog: `NLKce95n` LogId(i), `ToSxkU34` LogDate,
  `h7eY3sAK`, `B5JQyV8j`, `pn16CNah` UnitId, `8Fj06n3K` ChatType, `3kQUaGx0`
  Content, `2pAyFjmZ`.
* `wc2DiMY1` RaidUserPartyDeckInfo: `h7eY3sAK`, `gr48vsdJ` MemberType,
  `XuJL4pc5` Disporder, `3WMz78t6` MaxHp, `e7DK0FQT` NowHp, `gU2xtQ0V`
  BbGauge, `nIGZ1X9C` SbGauge, `PWXu25cg` Hel, `98WfKiyA` FriendID,
  `nBTx56W9` UnitTypeID, `2pAyFjmZ`; `edy7fq3L`/`pn16CNah`/`4A6LzBxr` share
  code paths the mapper could not separate: resolve by hand before use.
* `LApo1M3n` RaidServeInfo: `h7eY3sAK gcTR4MV0(i) V4Qc7mL8(i) w4KCZqs2(i)
  vIf2wG43 7eHKS3Nn`, setters unresolved.

## 5. Server design

### 5.1 State and storage

SQLite is the source of truth; four players polling every 4 s is about 1
request/s, well inside what the single Drogon SQLite client handles, so the
in-memory RaidService cache from the roadmap is a later optimisation, not a
slice-1 requirement. Boss movement, regen, aube and camp healing are all
derived lazily from timestamps when read (the `HunterOrbs.hpp`
`deriveHunterOrbs` pattern), so no tick thread is needed.

Migration `16102026_RaidSchema` (append to `db/MigrationManager.cpp`):

```sql
CREATE TABLE raid_user(
  user_id TEXT PRIMARY KEY NOT NULL,
  rc INTEGER NOT NULL DEFAULT 1,
  agreement INTEGER NOT NULL DEFAULT 0,
  login_world_id TEXT NOT NULL DEFAULT '',
  login_room_id TEXT NOT NULL DEFAULT '',
  aube INTEGER NOT NULL DEFAULT 3,
  aube_rest_ts INTEGER NOT NULL DEFAULT 0,   -- epoch of the next +1, 0 = full
  active_deck INTEGER NOT NULL DEFAULT 0);
CREATE TABLE raid_room(
  room_id INTEGER PRIMARY KEY AUTOINCREMENT,
  world_id TEXT NOT NULL,
  master_user_id TEXT NOT NULL,
  mission_id TEXT NOT NULL DEFAULT '',
  quick_start INTEGER NOT NULL DEFAULT 0,
  playstyle_id TEXT NOT NULL DEFAULT '1',
  comment TEXT NOT NULL DEFAULT '',
  password TEXT NOT NULL DEFAULT '',
  min_rc INTEGER NOT NULL DEFAULT 1,
  min_lv INTEGER NOT NULL DEFAULT 1,
  kick_flg INTEGER NOT NULL DEFAULT 0,
  midway_joinable INTEGER NOT NULL DEFAULT 0,
  quick_matching INTEGER NOT NULL DEFAULT 0,
  status INTEGER NOT NULL DEFAULT 0,       -- 0 lobby, 1 mission selected, 2 in field, 3 closed
  helper_json TEXT NOT NULL DEFAULT '',    -- quick start helper FriendInfo as sent
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE TABLE raid_room_member(
  room_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  ready INTEGER NOT NULL DEFAULT 0,
  kicked INTEGER NOT NULL DEFAULT 0,
  mission_status INTEGER NOT NULL DEFAULT 0,
  joined_at INTEGER NOT NULL,
  last_seen INTEGER NOT NULL,
  PRIMARY KEY(room_id, user_id));
CREATE TABLE raid_clear(
  user_id TEXT NOT NULL,
  mission_id TEXT NOT NULL,
  cleared_at INTEGER NOT NULL,
  PRIMARY KEY(user_id, mission_id));
```

Room ids go on the wire as decimal strings. A room is stale when every
member's `last_seen` is older than 90 s (lobby) or the room is older than the
mission time limit plus a margin (field); stale rooms are closed lazily on the
next RaidUserInfo / QuickStart / RoomInfo that touches them, never by a timer.

### 5.2 Code layout (small files, functions under ~50 lines)

* `packet-generator/assets/net/raid.kdl`: rename `RaidUserInfo` fields; add
  `RaidWorldInfo`, `RaidPermitMissionInfo`, `RaidClearMissionInfo`,
  `RaidRoomInfo`, `RaidRoomUserInfo`, request/response envelopes
  (`RaidUserInfoReq/Resp`, `RaidAgreementReq`, `RaidWorloInfoReq/Resp`,
  `RaidRoomFriendGetReq/Resp`, `RaidQuickStartReq/Resp`, `RaidExitReq`,
  `RaidRoomDissolveReq`, `RaidRoomInfoReq/Resp`, `RaidAckReq`). `raid.kdl` is
  already imported by `handlers.kdl`; it must stay after `user.kdl` and
  `friends.kdl` if it embeds `LoginInfoReq`/`FriendInfo` (generator emits in
  import order, see the guild note in `handlers.kdl`).
* `gimuserver/gme/common/Raids.hpp/.cpp`: `loadRaidUser`, `deriveAube`,
  `permittedMissions(rc)`, `buildRaidUserInfo`, `buildWorldInfo`,
  `buildRoomInfo/RoomUserInfo`, `closeRoom`, `liveRoomFor(user)`.
* `gimuserver/gme/handlers/Raid.cpp`: one `HANDLEF` per request, each: read,
  identify, validate, mutate in one transaction (`gme/common/Transactions.hpp`),
  build, write. Refusals use `HandleResult::refuseToHome`, never throw.
* `Handlers.hpp`: `HANDLE(...)` declarations; `GmeControllerHandlers.cpp`:
  `REGISTER("jJZ01VQo", RaidUserInfo, "SsQ26dVY")`, `("Cu94xU8j",
  RaidAgreement, "Eh5a3yr9")`, `("H7GRp01L", RaidWorloInfo, "5JxFp0tD")`,
  `("Xe2UB3A8", RaidRoomFriendGet, "9WN8oYBx")`, `("CWeTY8BQ",
  RaidQuickStart, "HsVofOJJ")`, `("jqi71nWu", RaidExit, "PD4ke3Ii")`,
  `("H8N1R0TE", RaidRoomDissolve, "q7dbw29J")`, `("0VxE6kTW", RaidRoomInfo,
  "cUrw7K6W")`, `("2qDW9NBU", RaidMissionStart, "kY5y1zBj")` (refuse +
  cleanup), `("bv9xt3m6", RaidRoonItemEdit, "9mKud8kc")` and `("XTban70i",
  RaidUpDateCash, "H5d6B2pi")` (acks).
* `FeatureGates.hpp`: drop `{14, "Raid"}` in the same commit that registers
  the entry handlers, with a comment saying the quick path stops at
  MissionStart.

### 5.3 Existing code to reuse

| Need | Existing code |
|---|---|
| identity, sessions | `gme::getUserIdentity` (`gme/common/Common.hpp`) |
| raid master data | `ServerCache::raidWorldMst() / raidMissionMst() / raidRcMst() ...` (`drogon/ServerCache.hpp`), structs from `mst/raid.kdl` |
| aube cap/regen | DefineMst `raid_max_p` (S92Hcor3 = 3), `recover_time_raid` (zkD98Hfy = 3600), `raid_bag_count` (09EbcDmX = 50); regen pattern from `gme/common/HunterOrbs.hpp` |
| helper list | `FriendGet.cpp` builder (`gme::playerPeak`, `gme::loadFriendRoster`, `gme::friendUnitFor`) |
| transactions | `gme/common/Transactions.hpp` |
| player name/level/leader | UserInfo / deck loaders used by `FriendGet` and `UserInfo.cpp` |
| later: mission serials, drops, medals | `gme/common/MissionRuns.hpp`, `BattleItems.hpp`, `BraveSlots.cpp` (raid medals already named as the slot medal source), `Warehouse.cpp` |
| raid intro bookkeeping | `special_scenario_info` ("raid," marks raid_op.txt as seen) is already persisted by DeckEdit/DungeonEventUpdate/CampaignEdit |

### 5.4 Wire test (tools-local/raid_entry_test.py)

Modelled on `tools/smoke_test.py` (`gme_call(base, group, key, body)`) and
`tools/test_accounts.py` (`guest_login`), two fake devices A and B:

1. A: RaidUserInfo -> agreement 0, rc 1, login ids "", aube 3/3.
2. A: RaidAgreement -> agreement 1; RaidUserInfo again -> agreement 1
   persisted; permit list non-empty and only rc-1 missions.
3. A: RaidWorloInfo -> one row world "1", open 1.
4. A: RaidRoomFriendGet -> `ft4zY6Uv` list (may be empty), no error.
5. A: RaidRoonItemEdit, RaidUpDateCash -> success bodies.
6. A: RaidQuickStart mission 2000 -> room row (world "1", master A, quick 1,
   members 1), user row for A, RaidUserInfo login ids set.
7. B: RaidUserInfo -> B's login ids still "" (isolation).
8. A: RaidRoomInfo -> same room; A: RaidMissionStart -> refusal; A:
   RaidUserInfo -> login ids cleared, room gone.
9. A: QuickStart for a mission above its RC -> refusal, no room created.
10. A: QuickStart, then RaidExit -> login ids cleared.

Back up `deploy/gme.sqlite` before running (it creates accounts).

## 6. Open questions / unknowns

1. Whether the device's MST cache actually holds the F_RAID_* tables (they are
   in the manifest at the shipped versions). If `RaidWorldMstList` is empty,
   RaidInitConnect silently returns to Home (scene 41 = HomeScene2) and no raid request is sent.
2. `RaidChatTempMst` (`20EeL8T1`), `RaidTargetMst`, `RaidMissionElementTermMst`
   have readers but no dumped data; `F_RAID_ELEMENTAL_LOCK_MST` is in the
   manifest but not cached by the server. Needed for chat templates and the
   RAID_ELELOCK_MISSION_PROMPT check, not for slice 1.
3. Exact meaning/units of `nJ2Qe3wM` dissolve time, `8n7oG4fS` rest time,
   `SyL0XFc6` crowding, `j3g5P4cq` mission status values.
4. Whether QuickStart omits `ZX7gzQ6s` or sends it empty when no helper.
5. RaidUserPartyDeckInfo keys `edy7fq3L`/`pn16CNah`/`4A6LzBxr` and all six
   RaidServeInfo keys are not yet mapped to setters.
6. How a raid scene reacts to GmeErrorCommand ReturnToGame vs the dedicated
   `RaidError` command (the controller already closes the connection on
   RaidError). Use ReturnToGame (`refuseToHome`) until a device test says
   otherwise.
7. `RaidUpScenarioInfo` (`R38qvphm`) is already registered as a generic
   scenario reporter; its `N4XVE1uA` value may be an RC intro file name
   (RaidRcMst `N4XVE1uA` = `raid_01_2.txt`), not a numeric id. The handler
   logs and skips non-numeric values, which is harmless; revisit with RC-up.
8. RC progression rules (RaidRcMst `JQ23rIvk` conditions like
   `6:20302:1:1:1,8:0:3:0:0`) are not decoded.

## 7. Slice 1 implementation (stage 2)

Code: `packet-generator/assets/net/raid.kdl` (now imported after
`friends.kdl`), `gimuserver/gme/common/Raids.hpp/.cpp`,
`gimuserver/gme/handlers/Raid.cpp`, migration `16102026_RaidSchema`,
registrations in `GmeControllerHandlers.cpp`, padlock 14 removed from
`FeatureGates.hpp`. Wire test: `tools-local/raid_entry_test.py`.

| request | behaviour |
|---|---|
| RaidUserInfo `jJZ01VQo` | creates `raid_user` on first touch; sends `1ry6BKoe` + permit (`rc <= raid_user.rc`) + clear lists; forgets a login room that is no longer live |
| RaidAgreement `Cu94xU8j` | `agreement = 1`, same reply |
| RaidWorloInfo `H7GRp01L` | one row per RaidWorldMst, open 1, crowding = live members x 5 (cap 100) + permit/clear |
| RaidRoomFriendGet `Xe2UB3A8` | `gme::socialList` (the Social roster) under `ft4zY6Uv`; strangers are not offered |
| RaidQuickStart `CWeTY8BQ` | refuses unknown or above-RC missions; leaves any previous room; creates a solo room (world = first RaidWorldMst, quick 1, ready 1); echoes `ZX7gzQ6s` verbatim when a helper was sent |
| RaidRoonItemEdit `bv9xt3m6`, RaidUpDateCash `XTban70i` | acknowledge `{}`; nothing stored |
| RaidRoomInfo `0VxE6kTW` | room + members, refreshes the caller's `last_seen`; empty `8iBuZa3n` once the room is gone |
| RaidExit `jqi71nWu`, RaidRoomDissolve `H8N1R0TE` | leave; master leaving or last member closes the room and blanks every member's login ids |
| RaidMissionStart `2qDW9NBU` | closes the room, then `refuseToHome("Raid quests are not available on this server yet.")` |

Deliberate simplifications: no master promotion (no multiplayer rooms yet);
a room counts as live while a member polled within 600 s (`kRoomIdleSec`),
generous because the quick path's check and field-ready scenes do not poll;
aube is never spent (MissionStart is where it will be); RC is always 1 (RC-up
rules undecoded); the handlers do not wrap their writes in one transaction
(each write is a single statement and a partial failure leaves at worst a
closed room or blank login ids, both of which the next RaidUserInfo repairs).

Not yet verified on a device: whether the client's MST cache has the raid
tables, whether the screens accept these shapes, and how a raid scene shows
the MissionStart refusal.

### 7.1 Test status (2026-10-16)

Wire only, no device. `tools-local/raid_entry_test.py` against a local build
(port 9963, fresh save): two guest accounts (CreateUser + Initialize, then the
login node carries `h7eY3sAK`) run the 10-step plan from 5.4 plus extra checks
(unknown mission, empty `ZX7gzQ6s`, re-QuickStart replacing the old room,
Dissolve with no body falling back to the login room): 23/23 PASS.

Found on the way: Drogon's SQLite client binds `$N` placeholders in order of
appearance, so `SET a = $2 ... WHERE id = $1` or a repeated `$6` silently binds
the wrong values. All raid SQL numbers placeholders left to right (comment in
`Raids.cpp`).

Known gap: UserInfo's reply still embeds a default `1ry6BKoe` (rc/open "0",
empty login ids). Harmless because RaidInitConnect always sends RaidUserInfo
before any raid screen reads it, but it is not the player's real raid state.
