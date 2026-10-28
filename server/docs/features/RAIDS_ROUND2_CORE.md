# Classic Raid round 2, band "core": the solo raid run

Stage: SPEC (2026-10-16), branch `feat/raids-2`. Extends
[RAIDS_SPEC.md](RAIDS_SPEC.md) (round 1: entry screens, world list, helper
pick, solo quick-start room). Read that first; this document does not repeat
its room/user tables or the entry flow.

Goal of the band: a solo quick-start raid that plays start to finish. Enter
the field, walk the map, fight route battles, fight the boss whose HP persists
per room, harvest, use items, finish with rewards and Raid Class (RC)
progression, exit. The same handlers must later serve shared rooms, so every
piece of state is keyed by room mission, not by player.

Band requests (group id / aes key from `re/notes/req_table.tsv`):

| request | group | aes | connect type | sent by |
|---|---|---|---|---|
| RaidMissionStart (round 1 stub, rewritten here) | `2qDW9NBU` | `kY5y1zBj` | (scene state) | RaidFieldReady::connectQuestStart |
| RaidMissionInfo | `BqF2x3EC` | `6SyFs0Du` | 14 | RaidSceneBase::questConnectReadyUpSet, RaidChatScene::connectRoutineField |
| RaidMissionInfoLight | `Argz1U6B` | `1hJrYxs7` | 14 | RaidSceneBase::questConnectReadyUpLight (item/team/unit/quest-info overlays) |
| RaidMissionBattleStart | `8UiQj0uF` | `1Jhavqb7` | (scene state) | RaidBattleStartScene::connectBattleStart |
| RaidMissionBattleEnd | `5Ko2BjPA` | `gi6B0m2p` | (scene state) | RaidBattleEndScene::connectBattleEnd |
| RaidMissionBattleRestart | `La24KGAp` | `ecJRzX16` | (scene state) | RaidBattleRestartScene::initConnect |
| RaidUpdateProgress | `7geDL5Vb` | `7DkVMG8Y` | 25 (field) / none (battle) | RaidSceneBase::connectRaidUpdateProgress, MissionScene::requestRaidUpdateProgress |
| RaidMissionHarvest | `9QdRVwB1` | `b1ScyFv9` | 20 | RaidFieldPickScene::connectPick |
| RaidMissionUseItem | `o03kCtb1` | `Ys8ro9Sq` | 23 | RaidFieldItemUseScene::connectItemUse |
| RaidLimitedUseItem | `SaN7v13E` | `btR9Jkw7` | 24 | RaidSceneBase::connectRaidItemUse |
| RaidMissionEnd | `vgin9Mf1` | `95ULkowE` | (scene state) | RaidFieldQuestResultConScene::connectResult |
| RaidMissionExit | `a12v0RcA` | `14GMJFIN` | 15 | RaidFieldScene / RaidQuestInfoScene / RaidQuestGiveUpScene ::questRetireRequest |
| RaidMissionSelect | `8iQL3IKA` | `ZBex5ju9` | 7 | RaidQuestInfoScene::questSelectRequest (room path only) |
| RaidEnter | `yZReG75i` | `N0Azw6ob` | 6 | RaidRoomMenuScene / RaidRoomAutoEnterScene / RaidRoomSearchListScene / RaidFriendListScene ::connectRoomEnter (room path only) |

Evidence labels as in round 1: **OBSERVED** = read from `libgame.so` (address
given), the server source or the shipped master data; **INFERRED** = reasoned
from those; **UNKNOWN** = not resolved, with the safe default to use. Nothing
in this document has been seen on a device: no live raid traffic was ever
captured, every shape is rebuilt from `createBody` / `readParam`.

Tools used (re-runnable, written this stage, kept in the session scratchpad and
easy to recreate): an annotated dumper over `re/full.s` that resolves
`adrp+add` strings and demangles calls; a one-pass call graph
(`LC_ALL=C awk` over `full.s`, 11 s for the whole binary); a jump-table decoder
(`target = base + entry*4`, byte or halfword tables). `re/work/raid/kmap.py`
produced the key->setter maps; where it guessed wrong (virtual setters) the
reader was read by hand and the table below says so.

---

## 1. How in-raid scenes react to replies and refusals

### 1.1 Connection model (OBSERVED)

* Every raid request goes through `GameScene::accessPhpRaid` to the normal game
  URL (round 1, 1.1).
* Field-side requests record a **connect type** in `RaidInfo` (offset 0x1c)
  when they are sent; the scene's `connectEndCheck` reads it back once the
  reply has been parsed (`RaidSceneBase::checkConnectEnd`, which also zeroes
  it). The type, not the reply content, decides the reaction. Decoded types:
  2 RoomList, 3 RaidExit, 4/5 room make/edit, 6 RaidEnter, 7 MissionSelect,
  8 FrlendSelect, 9/10/27 ready, 11 RoomInfo, 12 friend apply, 13 room
  mission start, 14 MissionInfo/MissionInfoLight, 15 MissionExit,
  17 CampRestStart, 18 CampRestGet, 19 CampRestEnd, 20 Harvest, 21/22 shop,
  23 MissionUseItem, 24 LimitedUseItem, 25 UpdateProgress (field),
  26 kick, 28 QuickStart, 29 deck edit, 30 block, 50 send chat, 51 chat poll.
  (collected from every `setConnectType` call site.)
* Battle-side scenes (BattleStart/End/Restart, QuestResultCon) do not use the
  connect type; they advance a scene state machine at `+0x48c` when
  `HttpConnector::isFinished`.
* A new poll is only sent while the connect type is 0, so the client never has
  two field requests in flight.

### 1.2 Refusals (OBSERVED, consequences INFERRED)

`GameScene::checkResponseMessage` @0x1615E4C maps the server's error command
to a notice id: cmd 2 (Retry) -> -4000, 6 (ReturnToGame) -> -3992,
7 (RaidError) -> -3991, 10 -> -3994, others to -3995..-3999.
`GameScene::noticeOK` @0x16098AC: -3992 rebuilds HomeScene2, -3991 opens the
GUILD raid top (so RaidError is wrong for classic raid), -4000 only opens a URL
if the message carries one.

Every classic raid scene's own `noticeOK` (RaidFieldReady, RaidFieldScene,
RaidBattleStart/End, RaidFieldQuestResultCon, RaidFieldPick ...) calls
`GameScene::noticeOK` and then **`RaidSceneBase::connectErrorInit`
@0x19CF898, which changes to scene 1201** (RaidInitConnect). 1201 sends
RaidUserInfo and RaidInitScene resumes the login room (`caseGetRoom`, 2.5).

Rules for the server:

1. Never throw and never use `HandleResult::error` in a raid handler: that is
   Close, and `noticeOK` exits the app.
2. **Use `HandleResult::refuseToRetry` for every in-raid refusal.** The player
   sees the message and the client re-enters the raid through 1201, which
   resumes the room if it is still live and drops to the world list if the
   server has blanked `raid_user.login_*`. That is the recovery path the
   client was built around.
3. `refuseToHome` (-3992) is ambiguous inside a raid scene: GameScene builds
   HomeScene2 and the raid scene then requests 1201. Which wins is UNKNOWN;
   keep `refuseToHome` for the entry screens round 1 already uses it on and
   test it on device before relying on it anywhere else.
4. A refusal must leave the server state consistent with what the next
   RaidUserInfo will say: either the room is still live (resume) or the login
   ids are blank (world list).

### 1.3 Shared parsing facts

* Bodies are parsed into a `picojson` map: keys inside a node arrive in byte
  order (digits, then upper case, then lower case). Readers that finalize a row
  "on the last param" are order independent; no reader below depends on order.
* `readParam(nodeIdx, paramIdx, key, value, isLast)`: a list reader that calls
  `removeAllObjects` at node 0 / param 0 is a **full replace**; a zero-row array
  never reaches the reader, so such a list cannot be emptied by sending `[]`.
* Every number in a request arrives as a string; model request numbers as
  `i32::str` (handbook rule).
* Two lists are gated by client flags, both default true (`RaidInfo` ctor,
  `strh 0x101` at +0x34):
  * `wc2DiMY1` RaidUserPartyDeckInfo is skipped while
    `RaidInfo::isResponsePartyDeck` is false (RaidFieldItemUseScene clears it
    while an item is being applied).
  * `81B4TvEK` RaidMissionUesrInfo is skipped while
    `isResponseMissionUser` is false (`RaidFieldScene::walkingSet` clears it
    while the avatar walks; the field scene's destructor restores it). So a
    reply that lands mid-walk cannot teleport the player.

---

## 2. Client flow of a solo run

### 2.1 State machine (scene ids decimal)

```
1239 RaidFieldReady   connectQuestStart -> RaidMissionStart (2qDW9NBU)
     reply: roomInfoUpDate; questStartInfoCheck (2.2) else "RAID_QUEST_START_NG" (notice 501)
     my RaidMissionUesrInfo.BattleNum != 0 and the suspend files exist -> 1270 RaidBattleRestartScene
     else                                                              -> 1240 RaidFieldScene
1240/1241/1242 RaidFieldScene (1241 = back from a boss battle, 1242 = arrived at a point)
     every RaidWorldMst.IntervalSec (4 s): RaidMissionInfo (type 14)
     tap a neighbour point (RaidMapRouteMst start = my NowPointNum, end = target):
        walk animation; on arrival: NowPointNum = DstPointNum = target,
        SendPointNum = target, new RaidFieldScene(1242)
     next poll carries 81B4TvEK{pjX27bKB = target} once (2.4)
     on every reply (type 14/25/24): resultCheck -> 1261 when RaidRoomMissionInfo.Result is 2 or 4
                                     bossCheck   -> boss encounter effect -> 1260
     point buttons (current point only, RaidFieldScene::touchPointBtn @0x198D5DC, byte table @0x235EA76):
        111 camp (RaidCampRestStart, out of band)   112 -> 1280 item mix
        113 harvest -> 1257 RaidFieldPickScene      114 battle -> 1260
        115 -> 1243 RaidFieldItemScene (item use)
     my PlayerRest == 0 at scene init -> raidGameOverHeartCheck (buy hearts or retire)
     retire -> RaidMissionExit (type 15) -> 1268 RaidQuestGiveUpScene
1260 RaidBattleStartScene  RaidMissionBattleStart; reply ->
        RaidRoomMissionInfo.Result in {2,3,4} -> UserState.setMissionState(2) -> 1259
        resultCheck true -> 1240 (1241 if a boss was engaged)
        else saveBattleMstFiles(serial) -> scene 82 (MissionScene, the battle)
  82 MissionScene (raid mode)  per turn: RaidUpdateProgress (in-battle form, 4.5)
        end of battle -> 1259
1259 RaidBattleEndScene    RaidMissionBattleEnd; reply -> 1240, or 1241 when BossEncID is set
1270 RaidBattleRestartScene (resume after a crash) RaidMissionBattleRestart -> battle
1261 RaidFieldQuestResultConScene  RaidMissionEnd, then RaidRoomInfo (0VxE6kTW), then
        1271 (FUNC_RAID_EXT, 1 in .data and never written, so always 1271)
1271 RaidFieldQuestResultRewardScene  boss rewards from LApo1M3n; with no rows -> 1262
1262 RaidFieldQuestResultScene  zel/karma/friend point count-up (RewardType 1 rows)
1285 RaidFieldQuestResultScene  materials page, only when a RewardType 2/5/6 row exists
        (a guild member whose GuildInfo ExperienceObtained >= 1 first gets
        MissionResultGuildScene, as after every other mode's result)
1266 Coin (medals: RewardType 1/2, PresentType 12) -> 1265 Clear (any RewardType 4)
        -> 1263 Bonus (RcUPFlag; lists RewardType 3) -> 1264 RC scenario, each skipped
        when it has nothing; then checkSpecialScript: 1288/1287 story scripts, else
        quickStartExit for a quick room -> 1206 RaidWorldScene (no request), else 1216
1268 RaidQuestGiveUpScene  quick room: quickStartExit -> 1206; room: -> 1216 room top
```

Evidence: RaidFieldReady::updateEvent @0x1987568, RaidFieldScene::updateEvent
@0x198A7B4 and connectEndCheck @0x198A8DC (jump table @0x235EA2A over types
14..51: 14/24 field refresh, 15 retire -> 1268, 17 camp, 22 shop -> camp,
25 progress, 51 footer), walkingSet @0x1991238, unitWalking arrival
@0x198BEA4, battleScene/resultScene, RaidBattleStartScene::nextScene
@0x19474C0, RaidBattleEndScene::fieldScene @0x1946218,
RaidFieldQuestResultConScene::updateEvent @0x1978BB0 (states 1 MissionEnd,
2 RoomInfo, 3 next scene), RaidQuestGiveUpScene::updateEvent @0x19A40C4,
quickStartExit @0x19CF064.

### 2.2 What MissionStart must satisfy (OBSERVED, `questStartInfoCheck` @0x19CABD0)

All of these, or the player gets RAID_QUEST_START_NG:

* `8iBuZa3n` RaidRoomInfo row with `3tWuc7de` MissionStartFlg = 1
  (`RaidInfo::getRoom`, set by roomInfoUpDate from the top row);
* `uwH62EZy` row for the player with `j3g5P4cq` MissionStatus = 1 and a
  non-empty `FV6MU5nQ` CurrentRoomMissionID (read as a string);
* `g97TWISb` RaidRoomMissionInfo whose `j28VNcUW` equals the room's `j28VNcUW`.

The CurrentRoomMissionID is what every later request sends back as
`ja39HMLq` (the room-mission id). Make it the decimal id of the
`raid_room_mission` row.

### 2.3 Field reply needs (OBSERVED, RaidFieldScene::initialize @0x1987D10)

* `g97TWISb` mission id must resolve in RaidMissionMstList, else
  RAID_QUESTDATA_NULL (`setQuestData`).
* The player's `81B4TvEK` row: NowPointNum must be a point of the mission's
  map (`setMiMapPoint` -> RaidMapPointMstList by map id + point num); the
  camera, the buttons (only at the current point) and the walk routes all hang
  off it. A missing row leaves the map without the player.
* `8n7oG4fS` RestTime: seconds left. The client stamps the receive time and
  counts down locally (`questTimeSet`, `RaidUtils::questTimeCount`).
* `Ts2zJ6gj` boss rows drive the boss sprite and the encounter check.

### 2.4 Movement (OBSERVED)

* `RaidMissionInfoRequest::createBody` adds `81B4TvEK{pjX27bKB}` only when
  `RaidInfo::getSendPointNum() != -1`, and then sets SendPointFlg. The reader
  of `81B4TvEK` overwrites the player's own Now/Dst with SendPointNum while it
  is pending and resets it to -1 once the flag is set
  (RaidMissionUesrInfoResponse @0x143CCB8 tail). So the server only has to
  accept the point and echo it.
* Validate the move against RaidUserRouteMst (`raid_user_route_mst.json`,
  `j28VNcUW` mission, `4vSjr6zw` start, `Qf67k1ut` end): start = stored
  NowPointNum. On an invalid move keep the stored point and reply normally;
  the next reply snaps the client back (do not refuse a poll).

### 2.5 Resume after an app kill (OBSERVED, `RaidInitScene::caseGetRoom` @0x19A06C0)

With login ids set, RaidInitScene asks RaidRoomInfo and branches on the
player's `j3g5P4cq` MissionStatus:

| MissionStatus | client goes to | server must |
|---|---|---|
| 1 (in mission) | 1238 RaidFieldReady, which re-sends **RaidMissionStart** | treat MissionStart for a room whose mission is running as "resume": return the running state, do not start a new mission or charge anything |
| 2, 3, 4 (mission over, result not collected) | 1261, which sends RaidMissionEnd | keep the finished mission until MissionEnd collects it |
| anything else | quick room: `quickStartExit` (world list); room: 1216 room top | for a quick room, blank `login_*` as soon as its mission is over or exited, so a restart never lands on the room top of a solo room |

After a restart `RaidInfo` is fresh: QuickQuestFlg is false until a
RaidRoomInfo with `GWfpjED4` = 1 is read by RaidFieldReady.

---

## 3. Response groups used by the band

Key -> setter, `(i)` = parsed with StrToInt, otherwise kept as a string.
"replace" = full replace at row 0; "single" = one object, fields reset at the
start.

| group | class | semantics | keys |
|---|---|---|---|
| `8iBuZa3n` | RaidRoomInfo | replace; the top row is the room | round 1, 3.5. Field use: `3tWuc7de` MissionStartFlg(i) = 1 while a mission runs, `j28VNcUW` mission, `GWfpjED4` QuickStartFlg(i) |
| `uwH62EZy` | RaidRoomUserInfo | merged by user (round 1) | round 1, 3.5. `j3g5P4cq` MissionStatus(i): 0 lobby, 1 in mission, 2/3/4 over (= Result); `FV6MU5nQ` CurrentRoomMissionID (string); `C45mRxnZ` aube |
| `g97TWISb` | RaidRoomMissionInfo | single (@0x143FE4C) | `j28VNcUW` MissionID, `3tWuc7de` Result(i) 0/1 running, 2 won, 3 lost, 4 time up (strings in `resultEffEndStand` @0x19DDCA4: "RAID WIN", "RAID LOST BY 3-LOSS, 4-TIME"); `8n7oG4fS` RestTime(i) seconds left; `d8IPTUp6` Attalnment (clear-condition progress string, UNKNOWN format, send ""); `de9f4qXb` PointSetItem (UNKNOWN, send "") |
| `81B4TvEK` | RaidMissionUesrInfo | replace, gated (1.3) | `h7eY3sAK` UserID, `B5JQyV8j` UserName, `k3I8AWLs` LeaderUnitID, `pjX27bKB` NowPointNum(i), `qbJcuC60` DstPointNum(i), `fP1r39bd` PlayerRest(i) hearts left, `MFz8YRS6` CampFlg(i), `zHs17TE2` SearchInfo "point:count,..." harvests done, `EqgF1H8M` BattleNum(i) = serial of the open battle, 0 = none (also sets BattleFlg = value != 0), `53NMFRL9` BattleBossFlg(i) 1 when that battle is a boss battle, `4rcdJRS6` BossEscapeLimit(i) must be 0 to meet the boss, `5soj5vGI` BossEncountInfo "missionBossId:count,..." read by boss AI conditions |
| `Ts2zJ6gj` | RaidMissionBossInfo | each row `setObject` by mission boss id after `validOff` of all: send every boss every time (@0x143AD20) | `SP29fLtH` MissionBossID, `4x7IS9Vg` BossNowHp(i), `7Ex5mKRP` BossCurrentPoint(i), `5NAb7ds3`/`9P1MbHx5` BossPosX/Y(i) (0/0 = standing on its point; the encounter needs 0/0), `hsjMq7i6`/`g57HTBUa` goal pos(i), `7z6dqvuc` VisibleFlg(i) |
| `wc2DiMY1` | RaidUserPartyDeckInfo | replace, gated (1.3), all players' rows | `h7eY3sAK` owner, `edy7fq3L` user unit id (virtual setter, by hand), `pn16CNah` unit id (virtual), `4A6LzBxr` unit lv (i, virtual), `gr48vsdJ` MemberType(i) 0 leader / 1 member, `XuJL4pc5` Disporder(i), `3WMz78t6` MaxHp(i64), `e7DK0FQT` NowHp(i64), `gU2xtQ0V` BbGauge(i), `nIGZ1X9C` SbGauge(i), `PWXu25cg` Hel(i), `98WfKiyA` FriendID ("" or "0" = own unit, else the helper; `isFriend` @0x1301B28), `nBTx56W9` UnitTypeID, `2pAyFjmZ` UnitImgType(i) |
| `UsDq1E6o` | RaidUserBagInfo | replace | `n6E8iMf3` FrameID(i), `h7eY3sAK`, `kixHbe54` ItemID, `wgV86x1q` ItemNum(i), `DbMVG16I` DiffType(i), `dJNpLc81` NewFlg(i), `Slkc395l` EventTokenID |
| `Fihk73ZR` / `GvV1w4yM` | RaidEquipItemInfo / RaidRsvItemInfo | replace | `kixHbe54`, `XuJL4pc5` DispOrder(i), `wgV86x1q` ItemNum(i) |
| `S2fpqc7K` | RaidUseItemInfo | replace | `bL9t7ABR` MissionUserItemID, `kixHbe54` ItemID |
| `8c47oISW` | RaidRoomMissionItemInfo | replace | `h7eY3sAK`, `kixHbe54`, `PV3gH1Yf` PointNum(i): items set on points (boost items) |
| `2cBk5WH8` | RaidMissionBattleInfo | single | `j28VNcUW` RoomMisID, `qbJcuC60` PointNum(i), `tA4T0GXB` BossID |
| `yL3Yrf1M` | RaidBattleStatus | single (@0x1436E60) | `1kisc6IF` -> `MissionInfo::setMissionSerialID`, `Ez52fI60` -> `RaidMissionClientEventList::setEventID` (next client event id) |
| `QP4cwt7n` | RaidUserProgressInfo | acks (@0x1445958) | `o96QPNZn` event id(s), comma list allowed; the client removes those events from its pending list |
| `Fx60pCaU` | RaidBattleMonsterGroupInfo | replace | `o49dYfpH` MonsterID, `7Hf3DnVj` BodyFlg(i), `jXEdk5J4` TotalRecover(i), `e7DK0FQT` NowHp(i). `MissionScene::updateRaidBossMonsterStatus` @0x18120A4 sets each enemy's HP from the row with its monster id at battle start |
| `HQtqJGor` / `x7YLyM7H` | RaidMissionBossBuffInfo / BossStatusInfo | replace | buffs/ailments shared between players (`PlayerParty::setSharingBoss`); multiplayer only, omit |
| `UC8S1AiX` | RaidMissionResult | single | `3fHunNe4` FirstClearFlg(i), `6dkEYp04` BossFinished (string; `RaidFieldQuestResultScene::bossLast` @0x1981844 splits it on ',' and compares each part with a member's user id to draw `raid_boss_defeat_mark.png`, so it is the finishers' user ids; the server sends the boss name, which marks nobody), `oFQ3mbS6` FriendCnt(i) |
| `4sbU8kdW` | RaidMissionRewardInfo | replace | `30Kw4WBa` PresentType(i), `TdDHf59J` TargetID, `wJsB35iH` TargetCnt(i), `37moriMq` TargetParam, `IkmC8gG2` RewardType(i) |
| `awQFCW03` | RaidMissionRCUpInfo | single | `UJTR1h5r` RcUPFlag(i), `3Y9cKkuV` RcQuestBefore(i) |
| `LApo1M3n` | RaidServeInfo | replace | per-member boss rewards for 1271 (`h7eY3sAK` user id, `gcTR4MV0` boss points, `V4Qc7mL8` bonus points, `vIf2wG43` stars (float), `7eHKS3Nn` "itemId:count,..."); the client keeps the list until a reply carries the group. No data exists; omit, and 1271 skips itself |
| `4FVXsAw8` | RaidRoomMissionUserPerformanceInfo | replace | `h7eY3sAK`, `5V6Mrt8A` PorposeType, `SP29fLtH`, `TdDHf59J`, `wJsB35iH`; no consumer found, omit |

Reward vocabulary (OBSERVED in the result scenes, ticket #154):
PresentType 2 friend points, 3 zel, 4/5/7 item, 6 unit, 8 gem, 9 dungeon key,
11 karma, 12 medal (TargetID = medal id), 8004 event token. RewardType is how
the scenes sort rows (`RaidFieldQuestResultScene` ctor @0x197C940):
1 = the 1262 count-up (only PresentType 3/11/2; any other type-1 row, items
included, is dropped) and, with PresentType 12, the medal scene 1266;
2, 5 = the materials page 1285 unless PresentType 12 (2 + 12 counts as medals on
1266, 5 + 12 shows nowhere); 6 = page 1285 whatever the PresentType;
3 = the Raid Class-up scene 1263 (`BonusScene::updateItemInit` @0x1972C6C);
4 = the clear-reward scene 1265 (`getOneResultCheck` @0x19855E0). The old
reading (3 first clear, 4 RC up) came from the dev dummy and was wrong:
RC-up rows sent as 4 showed on the clear scene, and bag items sent as 1 were
paid but never shown. An empty list does not clear the client's, so a run
that pays nothing sends one blank row (RewardType 0).

### 3.1 Client events (OBSERVED)

The battle reports boss damage and item use as **events** with client-side ids
(`RaidMissionClientEventList::getNewEvent` @0x10DC754: id = counter at +0x20,
then counter++). Events stay queued and are re-sent with every in-battle
RaidUpdateProgress and every RaidMissionBattleEnd until the server acks them in
`QP4cwt7n`. Request node `QP4cwt7n`: `NH65Wj0f` type, `o96QPNZn` event id,
`xIB0C8PN` params (string).

| type | added by | params |
|---|---|---|
| 1 boss damage | `addBossDamage` from `RaidBossDamageLogList::getCsv` | `bossEncId:monsterId:damage,...` (damage is int64) |
| 2 item use | `addUseItem` from `BattleUseItemLogList::getCsvForRaid` | `0:itemId:useNum,...` |
| 3 boss recover | `addBossRecover` | UNKNOWN csv |
| 4 sharing boss buff | `addSharingBossBuff` | multiplayer |
| 5 sharing boss status | `addSharingBossStatus` | multiplayer |

Server contract: apply each `(user, room_mission, event id)` at most once, ack
every event id received (applied now or before) in `QP4cwt7n`, and send
`yL3Yrf1M.Ez52fI60` = (highest event id seen for this user and room mission) + 1
in BattleStart/BattleRestart replies so ids never repeat after an app restart.
Boss HP is driven by type-1 events only; the BattleEnd total `g5L3bKPu` is the
same damage again (do not apply it twice).

---

## 4. Request contracts

All requests also carry the identity groups (`IKqx1Cn9`, `6FrKacq7`, version
tag). "room node" = `8iBuZa3n{43hMuY2I world, 8VYd6xSX room}`; "rm node" =
`g97TWISb{ja39HMLq room-mission id}`. Resolve the player with
`gme::getUserIdentity`, then check that the room is live, the player is a
member and `ja39HMLq` is that room's current mission; on mismatch
`refuseToRetry` (1.2), which re-enters through 1201 and resyncs.

The common **field state block** used by several replies is:

`g97TWISb` + `81B4TvEK` (every member of the room mission) + `Ts2zJ6gj`
(every boss) + `wc2DiMY1` (every member's party) + `UsDq1E6o`/`Fihk73ZR`/
`GvV1w4yM` (the caller's bag and equipped/reserve items, round 1 does not store
them yet: send the empty-safe versions, see 6.6) + `8iBuZa3n` + `uwH62EZy`.

### 4.1 RaidMissionStart `2qDW9NBU` (rewritten)

Request (createBody @0x13C3B90, OBSERVED): room node plus `8ZK3w0Np` master id;
`wc2DiMY1` one row per party unit: `edy7fq3L` user unit id, `pn16CNah` unit
id, `e7DK0FQT` HP (int64, the unit's current max HP after spheres/extra
passives, computed client side), `PWXu25cg` heal stat, `XuJL4pc5` disp order,
`nBTx56W9` unit type, `gr48vsdJ` member type, `4A6LzBxr` lv, `2pAyFjmZ` img
type; plus the helper row from `FriendDataSet` (`98WfKiyA` helper user id,
same unit keys).

Server:
1. Room must be live and the caller a member (master for the start).
2. If the room has a running mission (status 1): **resume** (2.5). Reply with
   the field state; do not touch HP, hearts or timers.
3. Else create `raid_room_mission` (mission id = room's, started_at = now,
   time limit = RaidMissionMst `qY49LBjw`), one `raid_mission_user` per member
   (NowPointNum = DstPointNum = the mission's start point, 5.1;
   PlayerRest = 3, CampFlg 0, SearchInfo "", BattleNum 0), the party rows from
   the request (MaxHp = NowHp = `e7DK0FQT`, gauges 0, owner = caller), one
   `raid_mission_boss` per RaidMissionBossMst row of the mission (5.3).
   Set room `mission_start_flg` 1, members' `mission_status` 1 and
   `current_room_mission_id`.
4. Aube: the quick path never checks aube client side; the room path does
   (`raidAubeCheck` on room menu/search entry). Keep aube unspent in this band
   (round-1 decision stands) and note it as a design choice.

Reply: field state block, with `8iBuZa3n.3tWuc7de` = 1. Optional
`8c47oISW`/`S2fpqc7K` empty lists are not needed (never send `[]` for a
replace list, it does nothing).

### 4.2 RaidMissionInfo `BqF2x3EC` (the field heartbeat)

Request (createBody @0x13C340C): room node, rm node, `pw38qdui{NLKce95n last
chat log id}` (int), and `81B4TvEK{pjX27bKB}` only when a move is pending
(2.4). Sent every IntervalSec (4 s) from the field and the chat overlay.

Server: refresh the caller's `last_seen`; apply the pending point move (2.4);
derive time-out (if running and now >= started_at + limit: Result 4, members'
MissionStatus 4); reply with the field state block. Never refuse a poll for a
stale move; refuse (retry) only when the room or mission is gone.

Client reaction (type 14): RaidFieldScene::connectEndCheck -> roomInfoUpDate,
`setMiUesrInfo`, redraw units/boss/points, `resultCheck`/`bossCheck`
(fieldEffectCheck @0x19DD324).

### 4.3 RaidMissionInfoLight `Argz1U6B`

Request (createBody @0x13C3084): room node, rm node only. Sent on the same
4 s cadence by overlays opened from the field (item, team, unit, quest info,
item mix ...). Reply: same field state block (cheap enough at one player).
RaidFieldItemUseScene treats a type-14 reply with a pending boss/result effect
as "go back to the field".

### 4.4 RaidMissionBattleStart `8UiQj0uF`

Request (createBody @0x13C2134; values from connectBattleStart @0x194720C):
`9Q1Lq5FS{h7eY3sAK user id, j28VNcUW mission id}`, room node,
`2cBk5WH8{ja39HMLq room-mission id, SP29fLtH boss encounter id ("" for a route
battle), qbJcuC60 point num}`. The point is `RaidInfo::getMovePoint`, set to
the player's current point by the battle button (114) or the boss encounter
(`bossCheck` @0x19CAFA0 sets BossEncID to the boss standing there).

Server:
1. Mission must be running; the point must equal the stored NowPointNum.
2. Boss battle when `SP29fLtH` is non-empty: that boss must be alive and at the
   point (BossCurrentPoint == point, pos 0/0). Otherwise route battle: the point
   must not be a camp (RaidPointMst `AZFM14In` = 1).
3. Issue a serial (`gme::issueMissionSerial`, the user_mission_runs table, with
   the raid mission id) and store it as the member's BattleNum, BattleBossFlg,
   battle point and boss id.
4. Build battle content (5.4): BattleGroupMst/BattleMonsterGroupMst/MonsterMst/
   AIMst (+ MonsterCgsMst for boss parts) with `MissionArchiver::populatePacket`
   on an in-memory MissionRecord whose id is the raid mission id.

Reply (INFERRED from the battle scene's consumers; the raid branch of
MissionScene uses the normal battle lists): `pj41dy9g` battle groups,
`75t0sx9z` monster groups, `U0v5IeJo` monsters, `89ausgc4` AIs, `8hoyIF9Q`
monster cgs (boss only), `Kz7qfSs5` MissionNumInfo with the serial, `9Q1Lq5FS`
echo, `yL3Yrf1M{1kisc6IF serial, Ez52fI60 next event id}`, `2cBk5WH8` echo,
`Fx60pCaU` one row per boss part with its persisted HP (boss battle only),
plus `g97TWISb`, `81B4TvEK`, `wc2DiMY1`, `Ts2zJ6gj`.

Client reaction (nextScene @0x19474C0): Result in {2,3,4} -> battle skipped,
1259 (send BattleEnd); `resultCheck` -> back to field; else battle (scene 82).
`PlayerParty::entryRaid` @0x10C7024 builds the party from the caller's
`wc2DiMY1` rows (own units looked up in UserUnitInfoList by `edy7fq3L`) and
the helper from `RaidInfo::getSelFriendInfo` (QuickStart's `ZX7gzQ6s`);
`MissionScene::updateRaidUnitStatus` then applies NowHp and BB/SBB gauges, so
**HP and gauges carry over between raid battles through `wc2DiMY1`**.

### 4.5 RaidUpdateProgress `7geDL5Vb`

Request (createBody @0x13C8998), two forms chosen by the flag at +0x68:

* in battle (MissionScene::requestRaidUpdateProgress @0x1820AD0, from
  `EnemyParty::exitTurnChild` and `PlayerParty::updateChild`, so every turn):
  `yL3Yrf1M{1kisc6IF serial, SP29fLtH boss enc id}`, room node, rm node,
  `QP4cwt7n` events (3.1), `wc2DiMY1` the caller's party rows (`e7DK0FQT`
  NowHp, `gU2xtQ0V`, `nIGZ1X9C`, `XuJL4pc5` disporder, `gr48vsdJ`,
  `4A6LzBxr`, `2pAyFjmZ`, `h7eY3sAK`, `edy7fq3L`, `98WfKiyA`, `pn16CNah`),
  `S2fpqc7K{bL9t7ABR mission user item id, kixHbe54 item id}` for used
  mission items not yet logged, `pw38qdui{NLKce95n}`.
* in the field (connectRaidUpdateProgress @0x19CED90, after a field item effect,
  connect type 25): the same minus `yL3Yrf1M` and `QP4cwt7n`.

Server: apply events (boss HP, 5.3), store the party rows (NowHp, gauges),
consume logged items; if a boss part HP reaches 0 on the main body, Result 2.
Reply: `QP4cwt7n` acks, `g97TWISb` (the battle checks Result: at 2 every
enemy is set to 0 HP at the end of the turn, `EnemyParty::exitTurnChild`
@0x10BF560), `Ts2zJ6gj`, `81B4TvEK`, and `wc2DiMY1` with every member's rows
(it is a full replace, so a partial list would drop the other players; in
battle the client only applies its own rows again at the next battle start).

### 4.6 RaidMissionBattleEnd `5Ko2BjPA`

Request (createBody @0x13C0E78 and helpers):
* `yL3Yrf1M{1kisc6IF serial, SP29fLtH boss enc id, x69GYheC dead unit count}`
* room node
* `zS9s5bEI{M4NAYyc6 UserState mission state, 6RxkwBv7 defeated
  "monsterId:count,...", g5L3bKPu boss total damage "monsterId:damage,...",
  4T0Q2Bh5 drops (BattleRewardList item csv), hoG2ieT5 battle crystals,
  6PLsn8xo heart crystals, 5NRJQ1LU max turn damage, XP06YWdT max spark,
  U8uZLA34 spark count, rZQJF5G9 skill uses, kqI45VU1 sum damage (int64),
  E5S7XgR6 escapes, Wnc6A31E overkills, pJqG1B7b cheat log}`
* `QP4cwt7n` events (the last turn's damage and item logs are converted to
  events here)
* `wc2DiMY1` party rows (as in 4.5, disporder from `getSendDisporder`).

`M4NAYyc6` values: 1 is set by `RaidUtils::initRaidBattle` at battle start;
the battle end path passes 2 or 3 through `changeMissionResultScene`; which of
2/3 is win vs wipe is UNKNOWN (both exitMissionClear and the retreat menu pass 2,
exitBattle passes 3). **Do not branch on it.** Decide a wipe from the party:
all own rows NowHp == 0.

Server: serial must be the member's open BattleNum (else ack and reply the
state: a duplicate BattleEnd after a retry must be harmless); apply events;
store party rows; clear BattleNum/BattleBossFlg; on wipe: PlayerRest -= 1
(floor 0) and restore own party rows to MaxHp (INFERRED design: the original
returned the party to camp); credit battle drops/zel/karma only through
MissionEnd (5.6). Reply: `QP4cwt7n` acks + field state block.

Client reaction: RaidBattleEndScene::fieldScene -> 1240, or 1241 when a boss was
engaged.

### 4.7 RaidMissionBattleRestart `La24KGAp`

Request (createBody @0x13C1BCC): room node, rm node, `yL3Yrf1M{1kisc6IF serial}`.
Sent by 1270 after MissionStart reported BattleNum != 0. initConnect
@0x19463F0 first jumps to 1259 when Result is already 2..4, then uses the
player's NowPointNum (map point), BattleBossFlg (re-finds the boss at that point
and sets BossEncID) and DstPointNum (move point).

Server: serial must be the open BattleNum (else `refuseToRetry`, which lands on
1201 and then the field). Reply: `yL3Yrf1M{serial, Ez52fI60}`, `Fx60pCaU` for a
boss battle, field state block. The battle itself resumes from the client's
suspend files (same as MissionRestart).

### 4.8 RaidMissionHarvest `9QdRVwB1`

Request (createBody @0x13C2BB8, connectPick @0x196FE8C): room node, rm node,
`81B4TvEK{zHs17TE2 "<point>:1"}`.

Client: the pick button checks the player's SearchInfo first
(`pickCheck` @0x198DFDC): "point:count" pairs; count >= RaidMissionPointMst
`H6k1LIxC` (Count) shows RAID_FIELD_PICK_CHECK. The pick scene saves the bag
before sending and, on the reply (type 20), `getItemStrSet` diffs the new
`UsDq1E6o` against it: the difference is what the player "found"; no difference
shows RAID_FIELDPICK_MIS.

Server: point must be the player's current point and not exhausted; roll the
RaidMissionPointMst `8fjZN6Mx` ItemInfo list "weight:itemId:num,..."
(weights out of 10000, INFERRED; mission 2000 point 2 = "5000:10610:1,5000:10300:1");
add to the mission bag (`raid_mission_bag`, capacity DefineMst `raid_bag_count`
= 50); bump SearchInfo for that point. Reply: `UsDq1E6o` complete bag + field
state block.

### 4.9 RaidMissionUseItem `o03kCtb1` / RaidLimitedUseItem `SaN7v13E`

Request (both, createBody @0x13C4C90 / @0x13C0840): room node, rm node,
`igAhC0R5{kixHbe54 item id, sx9S4gZF user id (the caller, from
UserTeamInfo::getUserID), H6k1LIxC int, Y5k8LJs9 int}`, `wc2DiMY1` the caller's
party rows. Field mapping by setParam offsets (OBSERVED for UseItem):
`Y5k8LJs9` = 1 when the item came from the bag (RaidUserBagInfo), 0 when from
the equipped list; `H6k1LIxC` = scene field +0x770, which must be >= 1 to send
(target/amount, exact meaning UNKNOWN). LimitedUseItem is the field-effect
item path (`RaidFieldScene::itemUseEff`, ItemMst process ids 902..907:
902/905/907 have point effects, 905 teleports via SendPointNum, 903 plays a
field effect).

UseItem (type 23) is the unit-target item screen: it clears
`isResponsePartyDeck` while sending, so the reply's `wc2DiMY1` is ignored and
the scene applies the heal itself (`RaidFieldItemUseScene::useItem`); the reply
must just succeed and reflect the decremented bag/equip counts.

Server (band minimum): decrement the item from the mission bag or equipped
list (refuse with retry when the count is 0), store the party rows as sent,
reply with `UsDq1E6o`/`Fihk73ZR` and the field state block. Item effects on
the server (heal values, teleports) are client-computed; the server stores the
resulting party rows and, for 905, accepts the move on the next poll.

### 4.10 RaidMissionEnd `vgin9Mf1`

Request (createBody @0x13C24A0): room node, rm node, `GWfpjED4` QuickStartFlg.
Sent by 1261 when the field saw Result 2/4 or the resume found MissionStatus 2..4.
Then the scene sends RaidRoomInfo and only then moves on (1262).

Server, once per member (idempotent: a second MissionEnd returns the stored
result):
* won (Result 2): zel `Najhr8m6`, karma `HTVh8a65`, friend points `J3stQ7jd`
  of RaidMissionMst; mission drops banked from battles (4T0Q2Bh5 rows, 5.6);
  first clear -> `raid_clear` row and FirstClearFlg 1; RC up (5.5).
* lost/time up: no rewards, FirstClearFlg 0.
* Set member MissionStatus 0, room `mission_start_flg` 0 and the mission
  closed. Quick room (`GWfpjED4` = 1): blank `login_*` now and mark the room
  closed, but **keep answering RaidRoomInfo for it to that member for 120 s**
  so 1261's follow-up RaidRoomInfo gets the room (it does not check, but an
  empty reply would trip `errorRoom` if a later build does).

Reply: `UC8S1AiX`, `4sbU8kdW` (one row per credited reward), `awQFCW03`,
`8iBuZa3n` + `uwH62EZy`, the updated `1ry6BKoe` (RC, login ids), and the
player's updated team info group so header zel/karma refresh (same group the
normal MissionEnd handler sends).

### 4.11 RaidMissionExit `a12v0RcA`

Request (createBody @0x13C282C): room node, rm node, `GWfpjED4` QuickStartFlg.
Sent by retire buttons in the field, the quest info overlay and 1268, and by
`initCheck` when PlayerRest is 0 and the player declines hearts.

Server: mark the caller's mission over (MissionStatus 0, no rewards); quick
room: close the room and blank `login_*`; room: if every member has exited,
close the mission (Result 3). Reply: quick: `1ry6BKoe` only (the field skips the
room check for quick quests); room: `8iBuZa3n` + `uwH62EZy`.

Client: field type 15 -> 1268 RaidQuestGiveUpScene; quick -> quickStartExit
(1206 world list, no request); room -> 1216.

### 4.12 RaidMissionSelect `8iQL3IKA` (room path)

Request (createBody @0x13C3810): `8iBuZa3n{43hMuY2I, j28VNcUW mission,
8VYd6xSX room, 8ZK3w0Np master}`. Master only; mission must be permitted for
every member's RC. Reply: `8iBuZa3n` with the new `j28VNcUW` + `uwH62EZy`.
Client (type 7, RaidQuestInfoScene::connectEndCheck @0x19A5158): roomInfoUpDate,
errorRoom, the room mission must resolve in RaidMissionMstList (else
RAID_ROOMQUEST_SEL_NULL), then 1216.

### 4.13 RaidEnter `yZReG75i` (room path)

Request (createBody @0x13BDBAC): `8iBuZa3n{43hMuY2I, 8VYd6xSX, 4WSu1irc
password}`. Reply: `8iBuZa3n` + `uwH62EZy` + `1ry6BKoe` with login ids. Client
(type 6): `roomSceneChange` -> room top. RoomList auto-enter (type 2) picks
`getObjectTop(true)` and sends RaidEnter. Out of the solo slice; register it
only with the room band.

---

## 5. Master data and server rules

### 5.1 Tables that drive the run

| file (deploy/mst) | client class | used for |
|---|---|---|
| `raid_mission_mst.json` (`8J0dh4YZ`) | RaidMissionMst | `j28VNcUW` id, `7x3pPB2C` RC, `H9sx5D08` map, `qY49LBjw` time limit s, `KR6JacN8` member limit, `HSRhkf70` needed mission, `UJTR1h5r` RCUpFlg, `Najhr8m6` zel, `HTVh8a65` karma, `J3stQ7jd` friend points |
| `raid_point_mst.json` (`4XA9m7of`) | RaidMapPointMst | per map: `PV3gH1Yf` point num, `AZFM14In` type (1 camp, 0 field), `f3L8HeKn` name, `X7g8cNAI` pick time, `8f4NYKxb` battle effect list, battle bg/bgm |
| `raid_user_route_mst.json` (`maU6riN7`) | RaidMapRouteMst | per mission: walkable edges start->end, `69vnphig` battle count (1 on every edge of mission 2000; self loops 2->2 etc. exist). Server-only key `E0VbaLr7` = route id, which `raid_battle_group_mst.json` references |
| `raid_mission_point_mst.json` (`6hLn3bBr`) | RaidMissionPointMst | per mission point: `H6k1LIxC` harvest count, `8fjZN6Mx` harvest table |
| `raid_mission_boss_mst.json` (`7pJD5tNd`) | RaidMissionBossMst | `SP29fLtH` mission boss id, `tA4T0GXB` boss, `i5r7Vjkq` init point, `E0yfb8eK` rest point, `g9EcMPR6` visible |
| `raid_boss_mst.json` (`0qK4FXpC`) | RaidBossMst | name, bgm, move/interval/recover timers, `ogFx8s2Y` escape HP % (client never reads it: server-side rule), `Qzhp8B40` monster group id |
| `raid_boss_route_mst.json` (`VyDP51hx`) | RaidBossRouteMst | boss walking routes (later band) |
| F_RAID_BOSS_PARTS_MST (`Ver78`, not yet in deploy/mst) | RaidBossPartsMst | `tA4T0GXB` boss, `o49dYfpH` part monster id, `7Hf3DnVj` main body, `2gZIRQ7D` z order, sprite/cgg/cgs files. Needed to build boss MonsterMst rows: import it from `mst-decoded/2-mst-decoded/F_RAID_BOSS_PARTS_MST_Ver78.json` as `raid_boss_parts_mst.json` and cache it in ServerCache |
| `raid_battle_group_mst.json` (`QEv8VG6C`) | RaidBattleGroupMst | server data: route id -> weighted monster group ids (`ug9xV4Fz` rate, `Qzhp8B40` group). **The monster groups it names (102000, 104000, 20000000 ...) exist nowhere in deploy/archive**: raid battle content must be authored (5.4) |
| `raid_mission_clear_cnd_mst.json` (`9EmPTqn3`) | RaidMisCondClearMst | `3l52Vq2h` "targetType:targetId:count"; mission 2000 = "1:1:0" (defeat mission boss 1) |
| `raid_rc_mst.json` (`Yr5PCpo3`) | RaidRCMst | RC 2..7 rows: `JQ23rIvk` rewards "presentType:targetId:count:?:?", `N4XVE1uA` intro scenario |
| DefineMst | | `raid_max_p` = 3 (= `RaidMissionUesrInfo::getPlayerRestMax` @0x12F6110, the heart cap), `raid_bag_count` = 50 |

Worked example, mission 2000 "Fire Eater" (RC 1, map 1, 1800 s): points 1
Camp, 2 Gentle River Bank, 3 Sunlit Woods, 4 Luminous Forest; edges 1-2, 1-3,
2-4, 3-4 both ways and self loops 2, 3, 4; boss 10000 Edelma, one part
(monster 210001, sprite `monster_anime_raid02.png`, assets present), starts
and rests at point 4, visible.

Start point: the first camp point (`AZFM14In` = 1) of the mission's map,
falling back to the lowest point num (INFERRED: the map's camp is where the
party starts).

### 5.2 Hearts, wipes, time

* PlayerRest starts at `raid_max_p` (3). A wipe (4.6) costs one and restores
  the party. At 0 the field offers the heart shop or retire (`initCheck`,
  `raidGameOverHeartCheck` @0x19DEFD4); RaidShopUse is out of this band, so
  the player retires (MissionExit).
* Time: Result 4 when the limit passes, evaluated lazily on every read.

### 5.3 Boss HP

* `raid_mission_boss` row per mission boss: max/now HP per part (JSON map
  monster id -> HP), current point = `i5r7Vjkq`, pos 0/0, visible =
  `g9EcMPR6`.
* `Ts2zJ6gj.4x7IS9Vg` = main-body part HP (INFERRED: the field only tests
  `> 0`).
* Type-1 events subtract from the named part (clamp 0, ignore unknown
  monsters, ignore events for a boss that is not this room's). Main body at 0
  -> boss defeated; when every boss in RaidMisCondClearMst target type 1 is
  defeated -> Result 2, all members' MissionStatus 2.
* Boss movement along RaidBossRouteMst and the escape rule (`ogFx8s2Y` %) are
  a later band. The client's escape sequence is AI driven, so authored boss AIs
  without an escape action keep the boss on its point.

### 5.4 Battle content (design decision, no source data exists)

The original server picked monster groups from RaidBattleGroupMst; their
contents were never shipped to the client and are not in our archive. The
server therefore authors raid battles:

* New archive file `deploy/archive/raid_battle.json` with the same stage
  schema as `mission.json` (`battle_monsters` with unit_id, hp, atk, def,
  ai_id, position ...), keyed by monster group id (route battles) and by raid
  boss id (boss battles, one monster per RaidBossPartsMst part, using the
  part's monster id and sprites).
* Fallback when a group is missing: route battle = one stage cloned from a
  fixed archive mission chosen by mission RC (RC n -> a story mission of
  matching difficulty, table in code), labelled with the raid mission id so
  `populatePacket` stays internally consistent (handbook 3.1 mismatch crash);
  boss = main body part only, HP = 300000 x RC, atk/def scaled by RC, AI 1.
  Log a warning naming the missing group, like MissionStart's template
  fallback.
* Route battle choice: RaidBattleGroupMst rows whose route id is the
  RaidUserRoute edge the player last walked (or the self loop at the current
  point), weighted by `ug9xV4Fz`.

### 5.5 Raid Class

Rule (INFERRED, `JQ23rIvk` grammar UNKNOWN beyond the first three fields):
clearing a mission whose `UJTR1h5r` = 1 at the player's current RC raises RC by
one (max 7). Pay that RC row's `JQ23rIvk` rewards to the present box
(`gme::addUserPresent`, presentType:targetId:count), send `awQFCW03{UJTR1h5r 1,
3Y9cKkuV old RC}` and RewardType 4 rows, and update `raid_user.rc` so the next
RaidUserInfo permits the next tier. The RC intro scenario (`N4XVE1uA`) is
already handled by RaidUpScenarioInfo.

### 5.6 Rewards

Credit at MissionEnd only (a retire forfeits): RaidMissionMst zel/karma to the
user immediately (RewardType 1); Brave Medals from RaidMissionMst `b5yeVr61`
("medalId:count": 0-1 at RC 1 up to 11 at RC 7, 5-28 on Medal Rush) to the
present box (PresentType 12, RewardType 1: the MissionEnd reply has no
`6C0kzwM5` medal group, so only a present claim refreshes the client's count,
which Brave Slots spends against); battle drops reported in
BattleEnd `4T0Q2Bh5` (banked per member in `raid_mission_user.drops`) and
harvested bag items to the present box (PresentType 4, RewardType 2); RC-up
rewards as RewardType 3. Every credited row is echoed in `4sbU8kdW`.
RaidMissionMst has no friend point column (`J3stQ7jd` is absent), so none.

Data that does not exist (never shipped to the client, not in any archive or
public datamine): boss/battle drop tables (the RaidBattleGroupMst monster
groups and per-part boss drops), per-mission clear or first-clear rewards
beyond RaidRcMst, and the LApo1M3n boss points. The client only rolls raid
battle drops from BattleMonsterGroupMst `i30R8TAs` ("dropPer:itemId:dropRest",
`MonsterParty::setMaterialDrop` @0x10C2354); it wipes LE6JkUp7 drop info
in raid battles (`MonsterParty::entry` @0x10C0FF0), so story-mission drops
cannot be reused, and monster zel/karma would show in battle but are never
reported. Fandom (Raid:Class 1-6) lists each boss's items without rates;
the r/bravefrontier "raredrops" page has per-part rates for some rare items.

---

## 6. Server design

### 6.1 Migration `16102026_RaidField` (append to RegisterMigrations)

```sql
CREATE TABLE raid_room_mission(
  room_mission_id INTEGER PRIMARY KEY AUTOINCREMENT,
  room_id INTEGER NOT NULL,
  mission_id TEXT NOT NULL,
  result INTEGER NOT NULL DEFAULT 1,      -- 1 running, 2 won, 3 lost, 4 time up
  started_at INTEGER NOT NULL,
  time_limit INTEGER NOT NULL,
  ended_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE raid_mission_user(
  room_mission_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  now_point INTEGER NOT NULL, dst_point INTEGER NOT NULL,
  player_rest INTEGER NOT NULL DEFAULT 3,
  camp_flg INTEGER NOT NULL DEFAULT 0,
  search_info TEXT NOT NULL DEFAULT '',
  battle_serial INTEGER NOT NULL DEFAULT 0,
  battle_boss INTEGER NOT NULL DEFAULT 0,
  battle_boss_id TEXT NOT NULL DEFAULT '',
  last_route_id TEXT NOT NULL DEFAULT '',
  status INTEGER NOT NULL DEFAULT 1,      -- mirrors j3g5P4cq
  ended INTEGER NOT NULL DEFAULT 0,       -- MissionEnd collected / exited
  drops TEXT NOT NULL DEFAULT '',
  result_json TEXT NOT NULL DEFAULT '',   -- stored MissionEnd reply, for idempotency
  PRIMARY KEY(room_mission_id, user_id));
CREATE TABLE raid_mission_party(
  room_mission_id INTEGER NOT NULL, user_id TEXT NOT NULL,
  disp_order INTEGER NOT NULL, user_unit_id TEXT NOT NULL, unit_id TEXT NOT NULL,
  unit_lv INTEGER NOT NULL, member_type INTEGER NOT NULL, friend_id TEXT NOT NULL DEFAULT '',
  unit_type_id TEXT NOT NULL DEFAULT '', img_type INTEGER NOT NULL DEFAULT 0,
  max_hp INTEGER NOT NULL, now_hp INTEGER NOT NULL, hel INTEGER NOT NULL DEFAULT 0,
  bb_gauge INTEGER NOT NULL DEFAULT 0, sb_gauge INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(room_mission_id, user_id, disp_order));
CREATE TABLE raid_mission_boss(
  room_mission_id INTEGER NOT NULL, mission_boss_id TEXT NOT NULL, boss_id TEXT NOT NULL,
  current_point INTEGER NOT NULL, visible INTEGER NOT NULL DEFAULT 1,
  parts_json TEXT NOT NULL,               -- {"210001":{"max":N,"now":N,"body":1}}
  PRIMARY KEY(room_mission_id, mission_boss_id));
CREATE TABLE raid_mission_event(
  room_mission_id INTEGER NOT NULL, user_id TEXT NOT NULL, event_id INTEGER NOT NULL,
  type INTEGER NOT NULL, applied_at INTEGER NOT NULL,
  PRIMARY KEY(room_mission_id, user_id, event_id));
CREATE TABLE raid_mission_bag(
  room_mission_id INTEGER NOT NULL, user_id TEXT NOT NULL, frame_id INTEGER NOT NULL,
  item_id TEXT NOT NULL, num INTEGER NOT NULL,
  PRIMARY KEY(room_mission_id, user_id, frame_id));
ALTER TABLE raid_room ADD COLUMN mission_start_flg INTEGER NOT NULL DEFAULT 0;
ALTER TABLE raid_room ADD COLUMN current_room_mission_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE raid_room ADD COLUMN closed_at INTEGER NOT NULL DEFAULT 0;
```

`raid_room_member.mission_status` (round 1) carries `j3g5P4cq`. Rooms in
mission are live while the mission runs (time limit + 120 s), regardless of
`kRoomIdleSec`; the field polls keep `last_seen` fresh anyway. Remember the
round-1 discovery: Drogon's SQLite binds `$N` in order of appearance, number
placeholders left to right.

Every handler that mutates more than one table runs in one transaction
(`gme/common/Transactions.hpp`); round 1's "single statements" excuse no
longer holds (BattleEnd touches events, boss, party and user).

### 6.2 Code layout

* `packet-generator/assets/net/raid.kdl`: `RaidRoomMissionInfo`,
  `RaidMissionUserInfo` (keep the client's "Uesr" only in docs),
  `RaidMissionBossInfo`, `RaidUserPartyDeckInfo`, `RaidUserBagInfo`,
  `RaidItemCountInfo` (Fihk73ZR/GvV1w4yM), `RaidUseItemInfo`,
  `RaidBattleStatus`, `RaidEventAck`, `RaidBattleMonsterGroupInfo`,
  `RaidMissionBattleInfo`, `RaidMissionResult`, `RaidMissionRewardInfo`,
  `RaidRcUpInfo`; request nodes `RaidRoomMissionNode` (ja39HMLq),
  `RaidPointNode` (pjX27bKB, zHs17TE2), `RaidClientEvent` (NH65Wj0f,
  o96QPNZn, xIB0C8PN), `RaidBattleNumNode` (1kisc6IF, SP29fLtH, x69GYheC),
  `RaidBattleResultNode` (zS9s5bEI keys, all `str`), `RaidItemUseNode`
  (igAhC0R5); envelopes per request. MissionStart's battle-content reply reuses
  `BattleGroupMst`, `BattleMonsterGroupMst`, `MonsterMst`, `MonsterCgsMst`,
  `AIMst`, `MissionNumInfo`, `MissionStartInfo`, so `raid.kdl` must stay
  imported after `mission.kdl` (embedded types, see handlers.kdl ordering note).
  Party HP fields are int64 on the client (`setNowHpEx`): use `i64::str`.
* `gimuserver/gme/common/RaidField.hpp/.cpp`: load/save mission, members,
  party, bosses; `applyEvents`; `deriveTimeout`; `fieldStateResp`;
  `startPoint`; `routeAllowed`.
* `gimuserver/gme/common/RaidBattles.hpp/.cpp`: battle content (5.4), boss part
  rows, `raid_battle.json` loader (strict JSON, no `_doc` keys: handbook).
* `gimuserver/gme/common/RaidRewards.hpp/.cpp`: MissionEnd payout and RC up.
* `gimuserver/gme/handlers/RaidField.cpp` (MissionStart/Info/InfoLight/
  UpdateProgress/Harvest/UseItem/LimitedUseItem/Exit) and
  `RaidBattle.cpp` (BattleStart/End/Restart/MissionEnd), keeping files
  under ~400 lines.
* Registrations (GmeControllerHandlers.cpp), replacing the round-1
  MissionStart stub:

```
REGISTER("2qDW9NBU", RaidMissionStart,         "kY5y1zBj");
REGISTER("BqF2x3EC", RaidMissionInfo,          "6SyFs0Du");
REGISTER("Argz1U6B", RaidMissionInfoLight,     "1hJrYxs7");
REGISTER("8UiQj0uF", RaidMissionBattleStart,   "1Jhavqb7");
REGISTER("5Ko2BjPA", RaidMissionBattleEnd,     "gi6B0m2p");
REGISTER("La24KGAp", RaidMissionBattleRestart, "ecJRzX16");
REGISTER("7geDL5Vb", RaidUpdateProgress,       "7DkVMG8Y");
REGISTER("9QdRVwB1", RaidMissionHarvest,       "b1ScyFv9");
REGISTER("o03kCtb1", RaidMissionUseItem,       "Ys8ro9Sq");
REGISTER("SaN7v13E", RaidLimitedUseItem,       "btR9Jkw7");
REGISTER("vgin9Mf1", RaidMissionEnd,           "95ULkowE");
REGISTER("a12v0RcA", RaidMissionExit,          "14GMJFIN");
// room band: REGISTER("8iQL3IKA", RaidMissionSelect, "ZBex5ju9"); REGISTER("yZReG75i", RaidEnter, "N0Azw6ob");
```

### 6.3 What the band deliberately leaves out

Boss movement and escape, camp rest (RaidCampRest*), the heart shop
(RaidShopUse), item mix/edit persistence (RaidItemMix/ItemEdit, still the
round-1 ack), shared-boss buffs/status (event types 4/5), chat, multi-player
rooms (MissionSelect/RaidEnter are specified but belong to the room band).

### 6.4 Implementation order (each step testable)

1. Migration + KDL + MissionStart (new/resume) + MissionInfo/Light + Exit.
   Device check: quick start lands in the field, the timer runs, walking works,
   retire returns to the world list.
2. BattleStart/End/UpdateProgress with authored route battles; party HP carries
   over; wipe costs a heart.
3. Boss battles with persisted HP, Result 2, MissionEnd rewards, RC up.
4. Harvest, UseItem/LimitedUseItem, BattleRestart.

### 6.5 Wire tests (`tools-local/raid_field_test.py`, two fake devices)

1. A: QuickStart 2000, MissionStart -> `3tWuc7de` 1, own `j3g5P4cq` 1,
   `FV6MU5nQ` non-empty, `g97TWISb.j28VNcUW` 2000, `81B4TvEK` own row at point
   1, `Ts2zJ6gj` boss at 4, RestTime ~1800.
2. A: MissionStart again -> same room mission id (resume), RestTime decreasing.
3. A: MissionInfo with `pjX27bKB` 2 -> now 2; with `pjX27bKB` 4 from 2 -> 4;
   illegal 1->4 (after reset) -> point unchanged, no error.
4. A: BattleStart at point 2 -> serial, battle groups present, `yL3Yrf1M`;
   B (not a member) -> retry refusal.
5. A: UpdateProgress (battle form) with events 1,2 -> acks 1,2; resend 1,2 ->
   acks, HP unchanged; BattleEnd with party HP -> `wc2DiMY1` echoes HP,
   BattleNum 0.
6. A: wipe BattleEnd (all NowHp 0) -> PlayerRest 2, HP restored.
7. A: walk to 4, boss BattleStart -> `Fx60pCaU` with full HP; damage events
   for half the HP; BattleEnd; second BattleStart -> `Fx60pCaU` half HP;
   damage the rest -> `g97TWISb.3tWuc7de` 2, `j3g5P4cq` 2.
8. A: MissionEnd -> rewards rows, first clear 1, RC 2 when `UJTR1h5r` = 1
   (pick such a mission), login ids blank, RaidRoomInfo still answers for
   120 s; MissionEnd again -> same reply, no double credit.
9. A: new run, MissionExit quick -> login ids blank, room closed.
10. Time-out: set started_at back by the limit -> MissionInfo reports 4.
11. Harvest at point 2 three times -> bag grows by the roll, fourth refused
    with retry; SearchInfo "2:3".
12. All requests with numbers as strings (handbook rule).

### 6.6 Device checks (emulator-5554)

Quick start -> field renders (map 1, avatar at the camp, timer); walk 1->2;
battle button fights the authored battle and returns with the HP left; walk to
4, boss sprite visible, encounter effect, boss battle starts at persisted HP;
kill the boss -> win effect -> result scenes -> world list; kill the app mid
battle and relaunch -> 1270 restart path; retire -> world list. Bag and
equipped item lists: round 1 never stored the quick-check item edits, so send
`Fihk73ZR`/`GvV1w4yM` only once the item band stores them; until then the
field item screen shows what the client cached from QuickCheck.

---

## 7. Open questions

1. `M4NAYyc6` (BattleEnd mission state) 2 vs 3 meaning; the design avoids
   depending on it.
2. The exact battle reply the raid MissionScene requires. The design sends the
   normal MissionStart battle groups plus raid groups; the first device test of
   step 2 decides. If the battle does not load, compare with GuildRaid's
   `GuildRaidBattleStartScene::nextScene` (the only other user of
   BattleGroupMstList in a raid) before changing shapes.
3. Boss part MonsterMst rows: whether `MonsterUnit::setPartsEnemy` takes the
   sprite from RaidBossPartsMst or from MonsterMst `img_a`/`anm_cgg`. Send both
   consistently.
4. `igAhC0R5.H6k1LIxC` meaning (target index or amount), `d8IPTUp6`
   Attalnment and `de9f4qXb` PointSetItem formats, RewardType 2, PresentType 2.
5. `JQ23rIvk` fields 4 and 5; whether RC up needs the mission's own RC to equal
   the player's RC.
6. Which of HomeScene2 (-3992) and 1201 (connectErrorInit) wins after a
   `refuseToHome` inside a raid scene.
7. Scene 1271 vs 1262 after MissionEnd (a global client flag).
8. Whether the original charged aube at MissionStart; this band does not.

---

## 8. As built (2026-10-16)

Where the implementation differs from or adds to the sections above:

* **Raid squads are deck type 3.** `UserPartyDeckList` keeps raid squads apart
  from quest decks and its copy routine (`deckCopyRaid` @0x129B91C) is never
  called, so a fresh account entered the quick-check with an empty squad and
  MissionStart's `wc2DiMY1` carried only the helper. UserInfo now seeds type-3
  decks from the quest decks once (`gme::raid::seedRaidDecks`); the campaign
  fallbacks that read `user_decks` unfiltered are pinned to type 1.
* **The client acts before it reports a walk.** A boss met on arrival
  (`bossCheck` in `initCheck`) and the pick button both fire before the next
  MissionInfo carries the move, so BattleStart and Harvest take the named point
  as the pending move when a route allows it (`catchUpMove`).
* **BossEscapeLimit** (`4rcdJRS6`) is 1 after a boss battle the boss survived,
  until the player moves; with 0 the next poll dragged them back into the fight.
* **No heart check at BattleStart** (spec 4.4 has none): with 0 hearts on the
  boss's point the encounter fires before the heart/retire dialog, and a refusal
  looped.
* **Teleport (ItemMst skill 905)** from the bag arms one route-free move
  (`warp_pending`) for the next poll.
* **Battle content**: route battles take a wave of a story mission named by the
  point's `8f4NYKxb`; boss parts get `kBossHpPerRc` = 40 000 HP per RC (a solo
  design number, not 300 000). `deploy/archive/raid_battle.json` (5.4) is not
  used.
* **Idempotency**: a battle report only updates the party while its serial is
  the open battle; BattleEnd closes with a serial guard; MissionEnd claims the
  member row (`ended = 0 -> 1`) inside the payout transaction; harvest claims
  its count with a conditional update. Drops are sanitized ("id:count", 20 rows)
  and paid at MissionEnd for real items only. Emptied bag stacks stay at 0 so the
  full-replace bag list overwrites the client's copy.
* Registered early from the room band: MissionSelect and a minimal RaidEnter
  (the multiplayer band replaces RaidEnter).

Open after this band (device checks of 6.6 still to do: battle, boss, result
scenes, BattleRestart, retire):

1. Missions whose only clear condition is type 2 (e.g. 2003 "2:213101:5", also
   3000, 3003, 4002, 5003, 80008, 80102, 80109) have no bosses and cannot be won
   yet: type-2 kills (BattleEnd `6RxkwBv7`) are not tracked.
2. A run that starts with an empty bag sends no `UsDq1E6o`, so the client may
   still show an earlier run's bag (an empty list cannot clear it); needs a
   device check and possibly a 0-count placeholder row (CAMP_ITEMS unknown 6).
3. A finished quick run resumed after an app kill reaches MissionEnd through
   1261 without QuickQuestFlg, so the result scenes end on the room top (1216)
   of the closed solo room instead of the world list.
4. `S2fpqc7K` (used mission items) and type-2 item events are acked but not
   applied; the raid item set is the camp/items band's.

Update 2026-10-17 (round 2 complete on `feat/raids-2`):

* Item 1 is closed: BattleEnd adds `6RxkwBv7` to a per-run tally
  (`raid_mission_kill`, migration `16102026_RaidKills`) once per closed
  battle, and `runIsWon` checks type-2 targets. No shipped target names a
  monster the server can field (213101.., 87001000 are raid field monsters
  with no MonsterMst row), so such a target counts every defeated enemy;
  mission 2003 plays as "defeat 5 enemies" (`tools-local/raid_kills_test.py`).
* Item 4 is closed: type-2 item events take the items off the raid set.
* Item 3: a gone room is now a Retry refusal on RaidRoomInfo (MULTIPLAYER
  4.1), which re-inits through 1201 to the world list. The resumed quick
  room's room top does not poll, though: the player leaves it with Leave
  Room (seen on emulator-5558).
* Raid battles crashed at load like the Research Lab boss (SIGSEGV 0x48 in
  `GameScene::requestMonsterSummonFiles`, seen on emulator-5558 at the first
  route battle): the battle reply now carries the monsters' temporary
  F_SKILL_MST rows under `8aiBoHg5` (`battleSkillRows`), as MissionStart does.
* Raid battles also crashed in `PlayerParty::entryRaid` (SIGSEGV in
  `BattleUnit::initializeBaseStatus`): while `RaidInfo::isQuest` is set,
  `UserUnitInfoList::shared` @0x12B8B44 returns `RaidUserUnitInfoList`,
  which nothing filled. MissionStart and BattleStart/Restart now send the
  caller's own party units as `CEVQ4p7A` (RaidUserUnitInfoResponse, the
  login read's UserUnitInfo rows, `raidUnitsGroup`).
* The helper lives only in client memory (`RaidInfo::getSelFriendInfo`), so
  after an app restart a resumed run fought without it; MissionStart now
  re-sends the stored pick as `ZX7gzQ6s`.
* Device (emulator-5558, 2026-10-17): route battle and boss battle (Edelma)
  load and play, per-turn boss damage persists (40,000 -> 29,018), BattleEnd
  wipes cost a heart and return to the field without re-engaging the boss,
  the time-up result path lands on the world list, and a resumed run after
  an app kill keeps its helper. Not seen on a device: a boss kill and the
  win result scenes (the Lv1 test squad could not win; covered by
  raid_field_test.py).
* Item 2 (empty-bag display) is unchanged.


Update 2026-10-23: open items 2 and 3 are fixed. An empty bag goes out as one
row with an empty item id (the bag screens skip it, @0x1961ACC), so a new run no
longer shows the last run's items. A closed quick room answers its members for
20 s instead of 120 s: long enough for 1261's own RaidRoomInfo right after
MissionEnd, short enough that a resumed run's result scenes end with the room
refused (Retry, then the world list) instead of the dead solo room's top.
`5soj5vGI` BossEncountInfo now counts each member's boss battles; `d8IPTUp6`
Attalnment stays empty on purpose (RaidRoomMissionInfo::getAttalnment has no
caller in this client).
