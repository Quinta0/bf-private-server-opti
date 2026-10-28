# Classic Raid round 2: multiplayer band (rooms, lobby, chat, blacklist, helpers)

Spec stage for `feat/raids-2`, band "multiplayer" (2026-10-16). Extends
`RAIDS_SPEC.md` (round 1: entry screens, world list, helper pick, solo
quick-start room); read that first, its tables are not repeated here. Scope:
real co-op between up to 4 players over the 4 s HTTP poll: browse/search/auto
join, make/edit a room, lobby polling, ready/start, kick, member card, room
chat (stamps), blacklist, helper select/remove, own raid records, plus the
concurrency contract for shared boss HP that the mission band must follow.
Guild Raid (Pusher) stays out of scope.

Requests in this band:

| id | aes | class | sent by (scene id) |
|---|---|---|---|
| `YF9UQpn6` | `c0Qf94Ir` | RaidRoomListRequest | 1207 world (friend/guild shortcut), 1210 menu (auto), 1214 search, 1215 search list (refresh), 1208 friend list (refresh), 1224 auto-join |
| `52H0dkjv` | `9KdeuMX8` | RaidUpDataRoomInfoRequest | 1211 RaidRoomMakeScene, 1212 RaidRoomEditScene |
| `vmdA2GD8` | `esR7NZL9` | RaidUpDataUserReadyRequest | any room scene via `RaidSceneBase::connectReadyUp` |
| `Xbgen5J8` | `FrX0T2QJ` | RaidRoomKickRequest | 1231/1232 RaidUnitScene (member card) |
| `81zkMjfN` | `V7M6Bph4` | RaidGetPlayerInfoRequest | 1209 RaidPlayerInfoResultScene |
| `dYIy6Eu8` | `x2Fm1LSt` | RaidSendChatRequest | 1233/1253/1267 RaidChatScene |
| `k5pMN68B` | `cy0oLna7` | RaidGetChatLogRequest | RaidChatScene in the field (every 5 s), MissionScene (battle) |
| `4igfcyIm` | `XIgQ4UHm` | RaidUpdateBlackListRequest | RaidUnitScene (add), 1226 RaidBlackListDetailScene (remove) |
| `u59zdKCi` | `0HJ9ShKp` | RaidFriendRemoveRequest | 1251/1252/1295 RaidFriendSelectScene, PartyEditTopScene2, PartyEditListScene2 |
| `u59zdKCw` | `0HJ9ShKg` | RaidFrlendSelectRequest (sic) | RaidFriendSelectScene, RaidUnitSelectionScene |
| `yZReG75i` | `N0Azw6ob` | RaidEnterRequest | 1210, 1208, 1224, 1215 (`connectRoomEnter`) |

RaidEnter is not in this band's list, but "browse and join" cannot work
without it, so it is fully specified here (section 4.3); whoever owns it
should take this contract. RaidRoomInfo (`0VxE6kTW`) and RaidExit
(`jqi71nWu`) were registered in round 1 and need the changes in 4.1/4.2.
`RaidFriendRemove` has `EMPTY_URL` in `req_table.tsv`; that is irrelevant:
every raid request goes to `CommonUtils::getPhpUrl()` (round 1, section 1) and
the server routes by group id only.

Evidence labels as in round 1: **OBSERVED** = read from `libgame.so` (address
given) or the shipped data; **INFERRED** = reasoned, not yet seen on a device.
Nothing in this document has been seen on a device yet.

Tools (re-runnable, scratch, not committed): an annotated function dumper
(`fd.py <regex>`), a one-pass call-graph index of `re/full.s` (`cg.py` ->
`cg.tsv`, 305k edges, 5 s) with a `callers.sh` query, a connect-type mapper
(`ct.py`, every `setConnectType(n)` paired with the request built in the same
function), vtable reads via `llvm-readelf -r`. The English UI strings came from
`assets/content/sgtext/sgtext_en_338.zip` (base64 CSV, `KEY^English^...`):
191 `RAID_*` keys, quoted below where they pin down a meaning.

---

## 1. Client facts that every handler must respect

### 1.1 How a raid scene sends and finishes a request

Every room scene sends through `GameScene::accessPhpRaid` (or plain
`accessPhp`) and stores a **connect type** in the `RaidInfo` singleton
(`RaidSceneBase::setConnectType`). Its `updateEvent` later calls
`RaidSceneBase::checkConnectEnd` @0x19CEF08, which returns the stored type and
zeroes it; the scene's `connectEndCheck` then dispatches on it, and the shared
part is `RaidSceneBase::roomConnectEndPlay(int)` @0x19DC248 (jump table at
0x2360469). OBSERVED connect types:

| type | request | what success does |
|---|---|---|
| 2 | RaidRoomList | scene-specific list handling (section 4.4) |
| 3 | RaidExit | `roomExit` |
| 4 / 5 | RaidUpDataRoomInfo make / edit | `roomSceneChange` -> lobby 1216 (or 1286) |
| 6 | RaidEnter | `setRoomSearchBack(1)`, `roomSceneChange` -> lobby |
| 7 | RaidMissionSelect | (mission band) |
| 8 | RaidFrlendSelect | `setSelFriendInfo(local copy)`, scene 1231 or 1216 |
| 9 / 10 / 27 | RaidUpDataUserReady 1 / 0 / 2 | 9: system window `RAID_ROOM_START_OK_TEXT`; 10, 27: redraw |
| 11 | RaidRoomInfo (heartbeat) | kicked check, `roomInfoUpDate`, master-change notice |
| 12 | FriendApply (from member card) | `FRIEND_REQUEST_FINISH` |
| 13 | none (set by the client itself) | `changeSceneWithSceneID(1239)` RaidFieldReady + `setFieldInitFlg(1)` |
| 26 | RaidRoomKick | back to lobby 1216 |
| 30 | RaidUpdateBlackList (add) | `RAID_BLACKLIST_APPEND_FINISH_ROOM` |
| 50 | RaidSendChat | `checkSendSuccess` (section 4.8) |
| 51 | RaidGetChatLog | append to the log |
| 1000 | RaidFriendRemove | clear `SelFriendInfo`, scene 1231 or 1216 |

### 1.2 Error handling: use `refuseToRetry` (-4000) inside raid scenes

`GameScene::checkResponseMessage` @0x1615E4C maps GmeErrorCommand to a notice
id (OBSERVED, exact csel chain): Retry(2) -> -4000, Continue(3) -> -3998,
Close(4) -> -3999, Close2(5) -> -3996, ReturnToGame(6) -> -3992, RaidError(7)
-> -3991, Continue2(8) -> -3997, LogoutFacebook(9) -> -3995, Close3(10) ->
-3994. `GameScene::noticeOK` @0x16098AC (table at 0x234903A, -3999..-3989):

* **-3991 (RaidError)**: if `RaidInfo::getRaidEnterFlg` and agreement == 1,
  blanks LoginWorldID/LoginRoomID and builds `RaidInitScene(1202)` (world
  list); otherwise HomeScene2. While in a raid the server's message is
  REPLACED by `RAID_GETINFO_ERROR_MOVEWORLD` ("Unable to load Raid data. You
  will be returned to the World Select screen."). The controller also sets
  Connection: close for this command (harmless).
* **-3992 (ReturnToGame, `refuseToHome`)**: HomeScene2. Leaves the raid
  entirely; do not use it in this band.
* **-4000 (Retry, `refuseToRetry`)**: ignored by GameScene; every raid scene's
  own `noticeOK` handles it (OBSERVED, `cmn w20, #0xfa0` in each):

| scene | reaction to -4000 |
|---|---|
| RaidRoomSearchListScene 1215 @0x19BFD8C | `connectNull` + `connectSearchCon` (re-runs the search, stays on the list) |
| RaidFriendListScene 1208 | `connectNull`, stays |
| RaidRoomTopScene, RaidRoomMenuScene, RaidRoomAutoEnterScene, RaidRoomMakeScene, RaidRoomEditScene, RaidUnitScene, RaidFriendSelectScene, RaidChatScene, RaidWorldScene | `RaidSceneBase::connectErrorInit` @0x19CF898 -> `changeSceneWithSceneID(1201)` RaidInitConnect, i.e. a full raid re-sync through RaidUserInfo (resume the login room if it is live, else the world list) |

So the refusal for this band is **`HandleResult::refuseToRetry(message)`**:
the player reads the server's message verbatim, then either stays on the
search list (refreshed) or is re-synced from RaidUserInfo. That makes the
server's `raid_user.login_*` the single source of truth: before refusing,
make sure RaidUserInfo will tell the truth (for example blank the login ids
of a player who was dropped from a room).

Exception, by design: requests whose failure must be invisible (chat send,
chat log, friend remove from the party editor, player info) never refuse;
see their sections.

### 1.3 Lists are only replaced by a non-empty reply (affects round 1)

`RaidRoomInfoResponse::readParam` and `RaidRoomUserInfoResponse::readParam`
clear their list when they read the first param of node 0; nothing else does
(`ResponseData::onReadFinished` is the empty base for both, vtables
0x274D948/0x274DB28). An empty JSON array never reaches `readParam`, so **an
empty `8iBuZa3n` or `uwH62EZy` leaves the previous rows in place** (OBSERVED;
`RaidRoomInfoList::removeAllObjects` is otherwise called only by
RaidRoomSearchScene2::connectSearch, RaidRoomSearchListScene::connectSearchCon
and RaidFriendListScene::connectSearchCon, before they send).

Consequences:

* Round 1's RaidRoomInfo answer for a vanished room (empty `8iBuZa3n`) does
  NOT produce RAID_ROOMDATA_NULL as `RAIDS_SPEC.md` 3.7 assumed: the lobby
  keeps the stale room and polls forever. Fix in this band: answer
  `refuseToRetry` (4.1).
* A RaidEnter / room-make reply MUST carry the joined room as the first (and
  only) `8iBuZa3n` row: `roomSceneChange` -> `RaidUtils::roomInfoUpDate`
  @0x11CC208 takes `RaidRoomInfoList::getObjectTop(false)`, which would
  otherwise be the top row of the search results (another room).
* "No rooms" for the world friend shortcut and for auto-join needs a
  placeholder row (4.4), because those paths do not clear the list first.
* The chat list (`WCA9VhL3`) is append-only (4.8), never replaced.

### 1.4 Polling cadence (OBSERVED)

* Lobby-side scenes (RaidRoomTopScene 1216/1290, RaidUnitScene,
  RaidQuestTopScene, RaidRoomEditScene, RaidQuestInfoScene,
  RaidQuestListScene, RaidFriendSelectScene) call
  `RaidSceneBase::roomConnectReadyUp` @0x19CDE40 every frame; it sends
  `RaidRoomInfoRequest(true)` when no request is pending and `now -
  RaidInfo::renewTime >= RaidWorldMst.IntervalSec` (`Rqc8pi0x` = 4), with the
  newest chat log id (`UserChatLogInfoList::getLastLogId`), connect type 11.
* RaidChatScene: in the room (scene 1233) the same RaidRoomInfo routine
  (`connectRoutineRoom`); in the field it polls RaidGetChatLog every >= 5 s
  (`RaidSceneBase::RoutineRequestChat` @0x19CE6A4, connect type 51).
* Scenes that do NOT poll while the player is still in the room: party edit
  (PartyEditTopScene2), item edit, help, shops, the quick-check/field-ready
  scenes. A member can legitimately be silent for minutes; see the absence
  rule in 3.3.
* RaidRoomList, RaidEnter, RaidUpDataRoomInfo, ReadyUp, Kick, Blacklist,
  SendChat, FriendSelect/Remove and PlayerInfo are one-shot (user action).

### 1.5 The lobby state machine (OBSERVED unless marked)

Wire state that drives it: room row `j28VNcUW` MissionID (selected quest, ""
= none), `3tWuc7de` MissionStartFlg (1 = the room's quest is running),
`1Txpza2u` MidwayJoinableFlg, `4Qf8RePm` RoomMissionJoinableRestTime; member
row `uX87tAZB` ReadyInfo (0/1/2), `j3g5P4cq` MissionStatus, `D2ZKtGR9`
KickedFlg; room master `8ZK3w0Np`.

* `RaidUtils::roomInfoUpDate` @0x11CC208: room = top row; draw slot 1 to the
  master, 2..4 to the others in list order, **skipping kicked rows**; returns
  true when the master user id changed. `RaidRoomUserInfoResponse::readParam`
  keeps a member's slot across refreshes by matching user ids, so member order
  only matters for newcomers (send master first, then join order).
* `roomConnectEndPlay(11)`: if MY row has KickedFlg == 1 -> notice 162
  `RAID_ROOM_FORCEKICK_EXIT` ("You are being expelled from this Room."); its
  OK (`RaidSceneBase::roomNoticeOK` @0x19DE6C4) sends **RaidExit**. Else
  `roomInfoUpDate`; if the master changed and I am the new master
  (`roomMasterCheck` @0x19CA92C = my id == room master) -> window
  `RAID_ROOM_MASTER_CHANGE`. So master promotion is a supported, visible
  event; the server must promote instead of closing (3.3).
* `RaidSceneBase::questStartStand` @0x19DC530, every frame when idle: MY row
  MissionStatus == 1 and I am not master -> connect type 13 -> RaidFieldReady
  1239 (which then sends RaidMissionStart itself). MissionStatus 2..4 ->
  scene 1261 (quest result path, mission band).
* Ready button, non-master (`roomTagTouchPlay` @0x19DA600 and dialog 96 in
  `roomConfirmAnswerYes` @0x19DBC44): if the master's row MissionStatus == 0
  -> `connectReadyUp(1)` (shows `RAID_ROOM_START_OK_TEXT`: "Once the Room
  Master taps Mobilize, you will proceed automatically."); if the master is
  already in the quest and the room is midway-joinable -> dialog 87
  (`windowQuestCheckMidway`) -> `connectReadyUp(2)` then connect type 13 ->
  1239 (mid-quest join). Tapping again un-readies (`connectReadyUp(0)`,
  type 10).
* Start, master: `questStartCheck` (aube/gem check) -> item check ->
  `windowQuestCheck` (`RAID_ROOMTOP_WINDOW_START2/3/4`) -> dialog 63 -> connect
  type 13 with **no lobby request** -> 1239 -> RaidMissionStart. The server
  therefore learns "the master mobilized" only from the master's
  RaidMissionStart (mission band; its required lobby side effects are in 5.2).
* `connectReadyUp(int)` @0x19CE900: body uses `RaidInfo::getInfoUserID`,
  which is `UserTeamInfo::getUserID` (the caller). Connect type: 2 -> 27,
  1 -> 9, else 10.

Member status labels (`RaidRoomTopScene::strUserStatus`, texts
`RAID_ROOMTOP_STATUS0..6`): Standby, Ready, On a Quest (ready 2 + status 1),
Cannot join, Loading, Camp, Not qualified (`permitQuestCheck` fails).

---

## 2. Master data that drives this band

| table | rows | used for |
|---|---|---|
| RaidWorldMst (`S9zI8iPU`, deploy `raid_world_mst.json`) | 1 (world "1", `Rqc8pi0x` = 4) | poll interval; world id validation |
| RaidPlaystyleMst (`n2kNWdq5`) | 6 (`NQR1Vd3t` 1..6, 1 = "Not Specified") | validate room/search playstyle |
| RaidMissionMst (`8J0dh4YZ`) | 154 | `KR6JacN8` member limit (4 x142, 1 x11, 2 x1; the client's `roomInMemberCheck` @0x19CFE00 compares it with `rPIya39u`), `7x3pPB2C` RC, `qY49LBjw` time limit |
| RaidRcMst (`Yr5PCpo3`) | 6 | RC range 1..7 for min RC |
| StampMst (`F_STAMP_MST` Ver8, in the client manifest; NOT in deploy/mst yet) | 100, id `FfZ30yBt` 1..101, 8000..8003; sets 1..6, sets 4..6 cost 2 gems (`F_STAMP_SET_MST`) | the only thing chat can carry (4.8) |
| RaidChatTempMst (`20EeL8T1`), RaidTargetMst | no shipped data | not needed: chat is stamps; target is never sent by the client (4.2) |

Copy `F_STAMP_MST_Ver8.json` from `mst-decoded/3-mst-translated` into
`deploy/mst/stamp_mst.json` (id list only is enough) and load the id set in
ServerCache, or embed the 100 ids as a constant with a comment naming the
source.

---

## 3. Server state

### 3.1 Migration `16102026_RaidMultiplayer` (append at the end of RegisterMigrations)

```sql
ALTER TABLE raid_room ADD COLUMN mission_started_at INTEGER NOT NULL DEFAULT 0;
ALTER TABLE raid_room ADD COLUMN password_flg INTEGER NOT NULL DEFAULT 0;
ALTER TABLE raid_room_member ADD COLUMN helper_json TEXT NOT NULL DEFAULT '';
CREATE TABLE IF NOT EXISTS raid_chat(
  log_id INTEGER PRIMARY KEY AUTOINCREMENT,  -- global, so ids rise per room too
  room_id INTEGER NOT NULL,
  user_id TEXT NOT NULL,
  chat_type TEXT NOT NULL,
  content TEXT NOT NULL,
  created_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS raid_chat_room ON raid_chat(room_id, log_id);
CREATE TABLE IF NOT EXISTS raid_blacklist(
  user_id TEXT NOT NULL,
  target_user_id TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY(user_id, target_user_id));
CREATE TABLE IF NOT EXISTS raid_user_stat(
  user_id TEXT NOT NULL,
  stat TEXT NOT NULL,          -- RaidUserTeamArchiveInfo key, e.g. 'T7BvL1kh'
  value INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(user_id, stat));
```

`raid_room.status` keeps round 1's values (0 lobby, 3 closed) and adds 2 = in
the field (set by RaidMissionStart, mission band). `raid_room_member.kicked`
stays; a kicked row is kept (so the same player cannot re-enter) until the
room closes, and is excluded from member counts and from every other
viewer's `uwH62EZy`.

### 3.2 Constants (one header, each with its reason)

| name | value | why |
|---|---|---|
| `kLobbyAbsentSec` | 180 | a member with no raid request naming the room for this long is dropped (3.3). Party/item edit do not poll; 3 minutes covers them without leaving ghosts for long. Tunable. |
| `kRoomIdleSec` | 600 (round 1) | room closed when nobody touched it this long |
| `kRoomListMax` | 30 | rows per RaidRoomList reply (client has no cap; it sorts by member count itself) |
| `kChatDeltaMax` | 50 | newest rows sent when the client asks from log id 0 |
| `kChatKeepPerRoom` | 200 | the client keeps 200 (`UserChatLogInfoList::checkNum` @0x1288074 trims to 200) |
| `kChatMinGapMs` | 1500, and 20 per rolling minute | rate limit (4.8) |
| `kBlacklistMax` | 100 | INFERRED; the client shows no cap |
| `kMidwayJoinWindowSec` | 300 | INFERRED from `infoMsgSet` comparing `RoomMissionJoinableRestTime` with 300 (0x12C) |
| `kCommentMaxChars` | 40 code points | INFERRED (edit box limit not recovered); the room list row is one short line |

### 3.3 Membership rules (apply lazily on every read that touches a room)

1. Any raid request naming a room the caller is a member of refreshes the
   caller's `last_seen` (RoomInfo, ReadyUp, Kick, Chat, ChatLog,
   FriendSelect/Remove, RoonItemEdit, UpDateCash).
2. On any room read: delete non-kicked members with `last_seen < now -
   kLobbyAbsentSec` whose `mission_status = 0` (members in the field are owned
   by the mission band's timeouts); blank their `raid_user.login_*` if they
   still name this room.
3. Master promotion: if the master left (RaidExit, absence, or RaidRoomDissolve
   by a non-master which is just a leave) and other non-kicked members remain,
   set `master_user_id` to the earliest-joined remaining member, reset that
   member's `ready` to 0, and keep the room. Close the room (status 3, blank
   every member's login ids) only when no non-kicked member remains, or when a
   master sends RaidRoomDissolve. This REPLACES round 1's "master leaving
   closes the room" in `leaveRoom`.
4. Capacity = RaidMissionMst `KR6JacN8` of the room's mission (4 when no
   mission is selected). Counted members exclude kicked rows. Quick-start rooms
   (`quick_start = 1`) are never listed and never enterable.
5. Every multi-statement change runs in one transaction
   (`gme::newTransaction` + `gme::CommitTransaction`); the decisive check is in
   the same statement as the write (4.3, 5.1). Drogon's SQLite client binds
   `$N` in order of appearance (round 1, 7.1): number placeholders left to
   right and never repeat one; pass a value twice as two parameters.

---

## 4. Wire contracts

All requests carry the standard identity group `IKqx1Cn9` (createUserInfoTag;
SendChat and GetChatLog use `createDungeonEventUserInfoTag` @0x13A0B8C, same
group key, with user id, contact id and the gumi-live params, but WITHOUT
`236dItKo`/`fKSzGDFb` and WITH `N4XVE1uA`/`9yVsu21R`, so `LoginInfoReq` must
keep those optional). Read with `glz::read<glz::opts{.error_on_unknown_keys =
false}>`; resolve with `gme::getUserIdentity`. String params are sent only
when non-empty; int params are always sent unless noted. Every value goes out
as a string.

Room reply shorthand used below:

* **ROOM** = `8iBuZa3n` with exactly one row (the room, round-1 `RaidRoomInfo`
  struct) + `uwH62EZy` members (round-1 `RaidRoomUserInfo`), master first, then
  join order. For a viewer who is not the master, send `4WSu1irc` password as
  "" (the client reads it only in `RaidSceneBase::setSelRoomData`, the master's
  edit form). `XqMsnE89` PasswordFlg = 1 when a password is set. `rPIya39u` =
  non-kicked member count. `C1vG0iKh` EnterableFlg = 1 when someone could join
  now (not full, not quick-start, lobby or midway window open). Kicked rows
  are included ONLY in the kicked player's own reply (4.1).
* **USER** = `1ry6BKoe` (round-1 RaidUserInfo) with login ids naming the room,
  so an app restart resumes it.

### 4.1 RaidRoomInfo `0VxE6kTW` (round 1, changes)

Request (round 1): `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`}, `pw38qdui`
{`NLKce95n` newest chat log id the client holds}, `T49LxzKr` {`wbo6qS5P`,
`c13QLNRf`}.

Changes:

1. Caller not a member, or room closed: blank the caller's login ids, then
   `refuseToRetry("This Room is no longer available.")` (was: empty lists;
   see 1.3). Lobby -> connectErrorInit -> 1201 -> RaidUserInfo -> world list.
2. Caller is a kicked member: reply ROOM where the caller's own row has
   `D2ZKtGR9` = 1 and the other rows are normal, on EVERY poll until the
   caller's RaidExit (idempotent: a second poll racing the notice must not
   refuse). The kick already blanked the caller's login ids, so a killed app
   does not resume the room. The client shows the expel notice (162) and its
   OK sends RaidExit.
3. Apply 3.3 (absence, promotion) before building the reply.
4. Append `WCA9VhL3`: chat rows with `log_id > NLKce95n` (all of them up to
   `kChatDeltaMax`, oldest first; when `NLKce95n` = 0, the newest
   `kChatDeltaMax`). Same row shape as 4.8.
5. Optional (recommended for the member card): `9PS8BLiC` RaidRoomPartyInfo,
   one row per unit of each member's raid deck (round-1 4 key list). Without
   it RaidUnitScene shows empty unit frames (`unitNull`), nothing breaks.

### 4.2 RaidUpDataRoomInfo `52H0dkjv` (make / edit)

Request (`RaidUpDataRoomInfoRequest::createBody` @0x13C7B2C, setParam
@0x13C7974, OBSERVED; args traced in RaidRoomMakeScene::connectUpDateMake
@0x19B53EC and RaidRoomEditScene::connectUpDateEdit @0x19B0D64), one node in
`8iBuZa3n`:

| key | type | meaning |
|---|---|---|
| `mnZ5K4Ii` | int | OpeType: **1 = create**, **2 = edit** |
| `43hMuY2I` | str | world id |
| `8VYd6xSX` | str | room id (edit); absent on create |
| `NQR1Vd3t` | str | playstyle id (RaidPlaystyleMst) |
| `WDUyI85C` | str | comment, free text, absent when empty |
| `4WSu1irc` | str | password, absent when empty |
| `wYmg52xp` | int | lowest RC; sent only when non-zero |
| `nXCf2FE9` | int | sent only when non-zero; 1 on create; on edit it comes from `RaidInfo::getSelPassFlg`. INFERRED: "apply the password field" (1) vs "keep the stored one" (absent) |
| `0TfXnP3m` | str | lowest team level, decimal ("0"/"1" = none) |
| `j28VNcUW` | str | mission id from the quest picker, absent when none |
| `j3sGA1zt` | int | Expel enabled (kick flag) |
| `1Txpza2u` | int | mid-quest joinable |
| `KQA1oWj7` | int | Auto-join On (QuickMatchingFlg; `getObjectTop(true)` needs it) |

The UI's "Target" (setParam 5th arg) is stored client-side but never sent;
`TdDHf59J` goes out as "".

Validation (refuse with `refuseToRetry`, message in quotes):

* world exists in RaidWorldMst; playstyle in RaidPlaystyleMst (else "1").
* `wYmg52xp` in 1..caller RC ("You can't select a class higher than yours";
  clamp missing/0 to 1).
* min level: integer 1..caller team level (`RAID_ROOM_LEVEL_WARNING`), else 1.
* password: "" or exactly 4 ASCII digits (`RAID_ROOM_MAKE_NAVI_PASSWORD`,
  `RAID_ROOM_MAKE_PASSWORD_SET_ERR`). Store hashed or plain (4 digits, low
  value); never echo it to non-masters.
* comment: UTF-8 valid, strip control characters and `<` `>` (the client
  renders `<color=...>`/`<br>` markup in labels; a member could otherwise inject
  markup into other players' room lists), cap `kCommentMaxChars` code points,
  trim. Never reject for content, just sanitize.
* mission: "" or a RaidMissionMst id with `7x3pPB2C <= caller RC`.
* create: if the caller is in a live room, leave it first (3.3 rules).
* edit: room exists, open, caller is master ("Changes can only be made by the
  Room Master."); the kick flag is NOT editable (`RAID_ROOMMAKE_WINDOW_CREATE2`:
  "These conditions can be edited later, except for the Expel feature") -
  ignore a changed value; a mission change while `status = 2` is refused.

Effect: create inserts `raid_room` (status 0, quick_start 0, master = caller,
`password_flg`) and the master's member row (ready 0) and points the master's
login ids at it, in one transaction. Edit updates the fields.

Reply: ROOM + USER. Client: type 4 -> lobby 1216 (or 1286 quest top), type 5
-> RaidRoomEditScene redraws (`setSelRoomData`, `menuDraw`).

### 4.3 RaidEnter `yZReG75i` (join)

Request (`RaidEnterRequest::createBody` @0x13BDBAC): `8iBuZa3n` {`43hMuY2I`,
`8VYd6xSX`, `4WSu1irc` typed password, absent when empty}.

Client pre-checks before sending (OBSERVED): `roomInMemberCheck` (members <
mission member limit, else `RAID_SEARCHLIST_INERR2`), `roomInQuestCheque`
(room mission in my RaidPermitMissionInfo, else `RAID_SEARCHLIST_INERR1`),
password window when `XqMsnE89` = 1 (RaidRoomEnterScene). The server repeats
all of them; the client's view is up to 4 s old.

Validation, in this order (all `refuseToRetry`):

1. Room exists, open, world matches, not quick-start: "This Room is no longer
   available."
2. Caller already a non-kicked member: success, idempotent (a retried request
   must not fail).
3. Caller has a kicked row in this room: "You cannot rejoin this Room."
4. Password set and the typed one differs: "The password is incorrect."
5. Blacklist, either direction, between the caller and any non-kicked member:
   "You cannot join this Room." (`RAID_BLACKLIST_APPEND_FINISH_ROOM`: "You will
   no longer be matched up with this user.")
6. Caller RC < `min_rc`, or team level < `min_lv`: "You do not meet this
   Room's requirements."
7. Room in the field (`status = 2`): require `midway_joinable = 1` and `now <
   mission_started_at + kMidwayJoinWindowSec` and the mission permitted for the
   caller; else "You cannot join that Room's current Quest."
8. Full: decided by the INSERT itself, not a prior SELECT (two players racing
   for the last slot):

```sql
INSERT INTO raid_room_member(room_id, user_id, ready, joined_at, last_seen)
SELECT $1, $2, 0, $3, $4
WHERE (SELECT COUNT(*) FROM raid_room_member
       WHERE room_id = $5 AND kicked = 0) < $6
RETURNING user_id;
```

   No row returned -> "This Room is full."

Leave any other live room first (3.3), set login ids, all in one transaction.

Reply: ROOM (the joined room, the ONLY `8iBuZa3n` row, 1.3) + USER. The
chat history arrives with the next RoomInfo poll (`NLKce95n` = 0: the
client cleared its chat list in RaidRoomMenuScene's constructor / roomExit).

Client reaction to -4000: from the search list it re-runs the search (good
UX); from auto-join, the menu and the world screen it re-syncs via 1201.

### 4.4 RaidRoomList `YF9UQpn6` (browse, search, auto-join, friends, guild)

Request (`RaidRoomListRequest::createBody` @0x13C6884, setParam @0x13C6768),
one node in `8iBuZa3n`:

| key | type | from setParam arg | notes |
|---|---|---|---|
| `ouU5H7qm` | int | search type | 1 search, 2 friends, 3 auto, 4 guild |
| `43hMuY2I` | str | world | sent only when non-empty; ABSENT for types 2/4 (RaidWorldScene::connectSearchCon passes an empty string) |
| `NQR1Vd3t` | str | playstyle | absent when none |
| `wYmg52xp` | int | RC filter | only when non-zero; auto-join from 1224 sends -1 |
| `j28VNcUW` | str | mission | absent when none |
| `j3sGA1zt` | int | kick filter | -1 = any (1224), 0 from menu auto |
| `1Txpza2u` | int | mid-quest joinable filter | |
| `0TfXnP3m` | str | level filter | "1" (menu/world), "0" (1224), saved value (search) |

Callers and their constant arguments (OBSERVED):

| scene, function | type | other args |
|---|---|---|
| RaidRoomSearchScene2::connectSearch @0x19C494C, RaidRoomSearchListScene::connectSearchCon @0x19BCB18 | 1 | saved style/quest/RC/kick/joinable/level (`RaidInfo::getSearchSave*`); list cleared client-side first |
| RaidWorldScene::connectSearchCon @0x19EF8B0, RaidFriendListScene::connectSearchCon | 2 | all empty/0, level "1"; FriendList clears first, World does not |
| RaidRoomMenuScene::connectRoomAuto @0x19BC828 | 3 | all empty/0, level "1"; not cleared |
| RaidRoomAutoEnterScene::connectRoomAuto @0x19AE5D8 | 3 | RC -1, kick -1, saved joinable and quest, level "0"; not cleared |
| RaidWorldScene::connectSearchGuildCon @0x19EFB10 | 4 | as type 2 |

Server filter (always): open rooms in the world (types 2/4: any world, each
row carries its own `43hMuY2I`, shown by `RaidFriendListScene::strWorld`),
`quick_start = 0`, caller not a member, caller not kicked from it, no
blacklist link either way, caller meets `min_rc`/`min_lv`, not full, and
either lobby or (field and midway window open). Then per type:

* **1 search**: playstyle (when given and != "1") exact; mission (when given)
  exact; RC filter when > 0: `min_rc >= value` (INFERRED from the "RC n Over"
  label `RAID_ROOMSEARCH_RC`); kick and joinable filters: -1 = any, else exact
  (INFERRED); level filter ignored when <= 1, else `min_lv >= value`
  (INFERRED). Newest first, `kRoomListMax`.
* **2 friends**: rooms with at least one non-kicked member in the caller's
  social roster (`gme::loadFriendRoster` / `gme::socialList`). Fill the
  row's friend columns with that friend: `gkYiqB40` user id, `H5pQK04c` name,
  `usM7W3DS` level, `1ctR6GHC` leader unit id, `8Ix0Soup` leader level,
  `JmLLHcDv` img type (RaidFriendListScene::listDraw reads
  `getFriendUserName/LeaderUnitID/UnitImgType`).
* **3 auto**: rooms with `quick_matching = 1`; mission filter when given;
  pick the best ONE (most members first, then oldest) and send only it:
  `RaidRoomInfoList::getObjectTop(true)` @0x12F8AFC returns row 0 only if its
  `KQA1oWj7` != 0, and the menu/auto scenes immediately RaidEnter it.
* **4 guild**: as 2 with guild mates (`Guilds.hpp` roster) instead of friends.

Rows: round-1 RaidRoomInfo shape without password (`4WSu1irc` ""), with
`ouU5H7qm` = the request's type, `C1vG0iKh` = 1, `3tWuc7de` = 1 when in the
field. No `uwH62EZy` in this reply (the caller is not in a room; it would
overwrite nothing useful).

Empty result (1.3):

* type 1: send `8iBuZa3n: []` (the scenes cleared the list) -> count 0 ->
  `RAID_ROOMSEARCH_LISTNULL`.
* type 3: send ONE placeholder row (`8VYd6xSX` "0", world, `KQA1oWj7` 0,
  `C1vG0iKh` 0, member num 0): `getObjectTop(true)` returns null ->
  `RAID_ROOMMENU_AUTONULL` ("Available Rooms not found."). Without it a stale
  room from an earlier list would be auto-joined.
* types 2/4 from the world screen: one placeholder row with `C1vG0iKh` 0 and
  `3tWuc7de` 1: RaidWorldScene::connectEndCheck @0x19EF540 counts
  `RaidRoomInfoList::getListMissionFlg` @0x12F8CF0, which keeps only rows with
  EnterableFlg == 1 and MissionStartFlg != 1 -> 0 -> `RAID_WORLD_FRIEND_LISTNULL`.
  UNVERIFIED: whether RaidFriendListScene (refresh) draws via `getList` or
  `getListMissionFlg`; if a placeholder ever shows there, send `[]` for the
  refresh instead (that scene clears first).

### 4.5 RaidUpDataUserReady `vmdA2GD8`

Request (@ connectReadyUp 0x19CE900): `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`},
`uwH62EZy` {`h7eY3sAK` = the caller's own id, `uX87tAZB` ready 0/1/2}.

Validation: caller is a non-kicked member (else 4.1 rule 1 refusal);
`h7eY3sAK` must equal the caller (never set someone else's ready); value in
0..2.

* 0: `ready = 0` (lobby only).
* 1: `ready = 1`. The master may send it too (dialog 96 path) - accept.
* 2: mobilize now. Allowed when the room is in the field and either the caller
  was ready when the master started or midway join is open and permitted;
  then `ready = 2, mission_status = 1` so the caller's RaidMissionStart is
  accepted. Otherwise "You cannot join this Quest." (the client re-syncs).

Reply: ROOM. Client: 9 -> START_OK window; 10/27 redraw; for ready 2 the
client already set connect type 13, so a success goes to 1239.

### 4.6 RaidRoomKick `Xbgen5J8`

Request (`RaidRoomKickRequest::createBody`, sent by
RaidUnitScene::connectMemberKick @0x19E694C after `RAID_MEMBER_KICK_WINDOW2`):
`8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`}, `uwH62EZy` {`h7eY3sAK` target}.

Validation: caller is the master ("Changes can only be made by the Room
Master."); room `kick_flg = 1` ("Kick function unavailable." =
`RAID_KICK_NG_TEXT`); target is a non-kicked member and not the caller;
target `mission_status = 0` (do not pull a player out of a running quest;
"That player is on a Quest.").

Effect (one transaction): target `kicked = 1, ready = 0`; blank the target's
login ids (so their RaidUserInfo no longer resumes this room).

Reply: ROOM (without the kicked row). Client: back to lobby 1216. The target
sees the expel notice on their next poll (4.1 rule 2) and sends RaidExit,
which must succeed for a kicked member (4.10).

### 4.7 RaidGetPlayerInfo `81zkMjfN`

Request: identity only (`RaidGetPlayerInfoRequest::createBody` adds nothing).
Sent by RaidPlayerInfoResultScene 1209 (the player's OWN raid records; the
scene reads only the `RaidUserTeamArchiveInfo` singleton and the trophy list).

Reply `6nqGvdY1` RaidUserTeamArchiveInfo, one node, every value an int string
(kmap of `RaidUserTeamArchiveInfoResponse::readParam`, OBSERVED):
`h7eY3sAK` user id (the caller), `T7BvL1kh` Challenge, `8CEu9Kcm` Win,
`58rGsYvL`/`2dVBPNQ5`/`ai0gh9Vo`/`UNycbmV0`/`x0s78E4G`/`y6fVdTch` Rc1win..Rc6win,
`Js8Amd2N` ItemHarvest, `0LwvAF3H` CrystalBMax, `rQ3TAy6I` CrystalHMax,
`hoG2ieT5` CrystalB, `6PLsn8xo` CrystalH, `5NRJQ1LU` BattleTurnMaxDamage,
`XP06YWdT` BattleTurnMaxSpark, `Wnc6A31E` OverkillCnt, `kqI45VU1`
BattleDamage, `kHbYZ9w1` SparkCnt, `4vx39EIB` SkillCnt, `c3Bo97kI`
ItemMixCnt, `1VbdgX76` RaidItemUse, `E5S7XgR6` EscapeCnt, `bprW7k3f`
BossFinishCnt, `D8EZfi4p` SingleWin, `nLb68MSq` CooperationWin, `54imIPa2`
FastestPlaytime, `nbu1dyP5` Playtime, `b28wpmvq` CampRecover; `b5LZj7fy`
unresolved (send "0").

Source: `raid_user_stat` (missing rows = 0). This band only reads; the mission
band increments (Challenge at MissionStart, Win/RcNwin/Single/Cooperation at
clear, BossFinishCnt for the kill credit of 5.1). Never refuses.

### 4.8 Room chat: RaidSendChat `dYIy6Eu8`, RaidGetChatLog `k5pMN68B`

**Chat is stamps only** (OBSERVED). RaidChatScene has no text input; the only
`sendChat(type, content)` callers are `touchStamp` @0x1956900 (type "2",
content = `CommonUtils::IntToString(StampMst::getStampId())`),
`touchUserStamp` @0x1956CE4 (type "5", a stamp from the per-member palette)
and the resend in `updateEvent` (same type/content again). Rendering
(`setChatLogLayout` @0x1952A3C, `drawBalloon`, `drawStamp`, `drawLogText`):
"1" balloon text, "2"/"5" stamp looked up in StampMstList by content, "3"/"4"
plain log text drawn verbatim (system lines; the server may emit "3", e.g. "X
joined", INFERRED, not required).

SendChat request (`RaidSendChatRequest::createBody`): `IKqx1Cn9`
(dungeon-event tag), `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`}, `JQ6hW0Ma`
{`8Fj06n3K` chat type, `3kQUaGx0` content}, `pw38qdui` {`NLKce95n` newest log
id held}.

GetChatLog request: `IKqx1Cn9` (dungeon-event tag), `8iBuZa3n` {`43hMuY2I`,
`8VYd6xSX`}, `pw38qdui` {`NLKce95n`}.

Reply rows `WCA9VhL3` (`RaidGetChatLogResponse::readParam` @0x1438570), one
per message: `NLKce95n` log id (int), `ToSxkU34` date (string, drawn verbatim
by `drawTime`; send "HH:MM" server time, INFERRED width), `h7eY3sAK` sender id,
`B5JQyV8j` sender name, `pn16CNah` sender leader unit id, `8Fj06n3K` type,
`3kQUaGx0` content, `2pAyFjmZ` unit img type.

Client list semantics (OBSERVED): `addObject` appends with NO de-duplication;
after every reply GameResponseParser sorts by log id and trims to 200
(`UserChatLogInfoList::sort`/`checkNum`). So **send only rows with `log_id >
NLKce95n`**, oldest first, or the player sees duplicates.

Send confirmation (`RaidChatScene::checkSendSuccess` @0x1957E7C, OBSERVED): the
client shows a provisional bubble and waits until a reply carries a row from
itself with the same type and content and an id >= the id it held at send;
otherwise it resends after >= 1 s, up to 3 attempts, then deletes the
provisional bubble. Therefore SendChat's reply MUST include the delta
including the caller's own new row.

Server rules for SendChat ("never trust client text"):

* caller is a non-kicked member of the room (field or lobby); if not, reply
  success with no rows (do NOT refuse: -4000 in RaidChatScene throws the
  player to 1201).
* `8Fj06n3K` must be "2" or "5"; `3kQUaGx0` must be the decimal id of a
  StampMst row (2). Anything else: drop silently.
* paid stamp sets (4..6): accept (the server has no stamp ownership yet);
  note it as a known gap.
* rate limit per user: one message per `kChatMinGapMs`, 20 per rolling minute;
  over the limit: drop silently. A legitimate client resend (same type+content
  within 10 s of a stored identical row from this user) is not stored again;
  the delta already contains the stored row, which ends the client's retry.
* store, then prune the room to the newest `kChatKeepPerRoom` rows.
* reply: `WCA9VhL3` delta `> NLKce95n` (cap `kChatDeltaMax`).

GetChatLog: same delta rules, never refuses (non-member: no rows). Refreshes
`last_seen`.

### 4.9 RaidUpdateBlackList `4igfcyIm` and the blacklist list

Request (`RaidUpdateBlackListRequest::createBody` @0x13C86D0): `kKpAdqAu`
{`KpTZlmkh` target user id, `mnZ5K4Ii` op}. Op values OBSERVED: **1 = add**
(RaidUnitScene::connectBlock @0x19E668C, connect type 30), **3 = remove**
(RaidBlackListDetailScene::confirmAnswerYes @0x19490E0, confirm id 200). 2 is
never sent; treat unknown ops as a no-op success.

Validation: target exists in `user_info`, target != caller; add when the list
has `kBlacklistMax` rows: `refuseToRetry("Your Black List is full.")`
(RaidUnitScene re-syncs; acceptable for a rare case). Add/remove are
idempotent.

Reply `Qazc0Qv8` RaidUserBlackListInfo: the caller's full list, one row per
target, the FriendInfo-like key set of the reader (`h7eY3sAK B5JQyV8j
2Fh3J7ng 96Nxs2WQ 0CAQ6wUe pn16CNah 4A6LzBxr ... 2pAyFjmZ Fnxab5CN`, the
same keys FriendInfo uses, see kmap output in the work notes); build each row
with the existing stranger/FriendInfo builder (`gme::playerPeak`,
`gme::friendUnitFor`), friend type 0.

Where the list comes from: the World screen's blacklist button goes straight to
scene 1225 (RaidWorldScene::touchBegan @0x19EF6EC) with no request, so the list
must already be loaded. Attach `Qazc0Qv8` to RaidUserInfo and RaidWorloInfo
replies too (the reader clears on node 0, so a non-empty list replaces;
an empty one leaves the client's copy, which is fine because the detail scene
clears the list itself after a remove, `exitSceneConnect` @0x1948F08).

Effect elsewhere: room lists, auto-join and RaidEnter exclude blacklist links
in either direction (4.3, 4.4). Existing rooms are not changed (the lobby
warns on its own: `roomMemberBlackListCheck`, `withBlackListMemberWindow`).

### 4.10 RaidExit `jqi71nWu` (round 1, changes)

Must succeed for a kicked member (delete the kicked row only if the room is
closed; otherwise keep it with `kicked = 1` so rejoining stays blocked, just
blank login ids). Apply master promotion (3.3) instead of closing. Reply
unchanged (USER).

### 4.11 RaidFrlendSelect `u59zdKCw` / RaidFriendRemove `u59zdKCi`

FrlendSelect request (`RaidFrlendSelectRequest::createBody`): `8iBuZa3n`
{`43hMuY2I`, `8VYd6xSX`}, `ZX7gzQ6s` {the full FriendInfo of the choice, same
keys as round-1 QuickStart's `ZX7gzQ6s`, including `edy7fq3L` user unit id,
`pn16CNah` unit id, `zsiAn9P1` deck no}. Two sources (OBSERVED):
RaidFriendSelectScene (a friend's helper) and RaidUnitSelectionScene (one of the
caller's OWN units not in the party, `UserUnitInfoList::getNotParty`, wrapped
in a FriendInfo with the caller's user id - used for elemental-lock missions,
`RAID_NO_FRIEND` / `RAID_NO_UNIT`).

Validation and effect (never trust the client's FriendInfo stats):

* caller is a non-kicked member with `mission_status = 0`, else
  `refuseToRetry` (RaidFriendSelectScene re-syncs).
* `h7eY3sAK` == caller: `edy7fq3L` must be a caller unit not in the current
  party; else the user must be in the caller's social roster
  (`gme::loadFriendRoster`), and the unit is the friend's real helper unit
  (`gme::friendUnitFor`), ignoring the client's numbers.
* store the SERVER-built FriendInfo JSON in `raid_room_member.helper_json`
  (the mission band reads it for the party at MissionStart).

Reply: `ZX7gzQ6s` with the server-built FriendInfo (lands in
`RaidInfo::setSelFriendInfo`; the client also sets its own copy on success)
+ ROOM. Client: scene 1231 or 1216.

FriendRemove request: `8iBuZa3n` {`43hMuY2I`, `8VYd6xSX`}. Clear
`helper_json` if the caller is a member. ALWAYS succeed with `{}` (+ ROOM when
in a room): it is also sent from PartyEditTopScene2/PartyEditListScene2,
generic party screens whose -4000 behaviour is not raid-aware.

### 4.12 Requests this band does not change but must keep working

RaidRoonItemEdit `bv9xt3m6` and RaidUpDateCash `XTban70i` (acks, round 1) are
sent from room scenes (`RaidRoomTopScene::updateEvent` drains item edits); they
refresh `last_seen` (3.3) and must keep succeeding. RaidUpDateCash's
`X6WBc9kJ` (int) is the party deck the player switched to (INFERRED); storing
it as `raid_user.active_deck` feeds `9PS8BLiC` (4.1 item 5).

---

## 5. Shared mission state and concurrency (contract for the mission band)

The lobby hands over to the field here; the mission band (BattleStart/End,
MissionInfo, MissionEnd) owns the handlers, this section fixes the rules both
bands rely on.

### 5.1 Boss HP shared by all members

```sql
CREATE TABLE raid_room_mission(
  room_mission_id INTEGER PRIMARY KEY AUTOINCREMENT,
  room_id INTEGER NOT NULL, mission_id TEXT NOT NULL,
  started_at INTEGER NOT NULL, ends_at INTEGER NOT NULL,
  status INTEGER NOT NULL DEFAULT 0);            -- 0 running, 1 cleared, 2 failed
CREATE TABLE raid_mission_boss(
  room_mission_id INTEGER NOT NULL, boss_enc_id TEXT NOT NULL,  -- SP29fLtH
  mission_boss_id TEXT NOT NULL, hp_max INTEGER NOT NULL, hp_now INTEGER NOT NULL,
  defeated_by TEXT NOT NULL DEFAULT '', defeated_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(room_mission_id, boss_enc_id));
CREATE TABLE raid_battle_log(
  room_mission_id INTEGER NOT NULL, user_id TEXT NOT NULL,
  battle_serial TEXT NOT NULL,                   -- yL3Yrf1M.1kisc6IF
  boss_enc_id TEXT NOT NULL, damage INTEGER NOT NULL, created_at INTEGER NOT NULL,
  PRIMARY KEY(room_mission_id, user_id, battle_serial));
```

RaidMissionBattleEnd for a boss battle, all in ONE transaction:

1. Membership: caller row `mission_status = 1` in this room's running
   `room_mission_id`; otherwise refuse (mission band decides the command).
2. Damage = client total clamped to `[0, hp_max]` (and to any per-battle cap
   the mission band can derive).
3. Idempotency: `INSERT OR IGNORE INTO raid_battle_log(...)`; zero rows
   inserted means this exact battle was already applied (client retry after a
   lost reply) -> skip to step 6 with the current state.
4. Atomic decrement, read-modify-write in one statement (SQLite holds the
   write lock for the statement; no await between read and write):

```sql
UPDATE raid_mission_boss
SET defeated_by = CASE WHEN hp_now > 0 AND hp_now <= $1 THEN $2 ELSE defeated_by END,
    defeated_at = CASE WHEN hp_now > 0 AND hp_now <= $3 THEN $4 ELSE defeated_at END,
    hp_now = MAX(0, hp_now - $5)
WHERE room_mission_id = $6 AND boss_enc_id = $7
RETURNING hp_now, defeated_by;
```

   (damage passed three times as `$1/$3/$5`: never reuse a placeholder.) SET
   expressions see the OLD row, so exactly one request - the one whose damage
   crossed zero - is credited with the kill (BossFinishCnt).
5. Mission clear: when no boss of the mission has `hp_now > 0`:
   `UPDATE raid_room_mission SET status = 1 WHERE room_mission_id = $1 AND
   status = 0` - only the request that gets one changed row runs the clear
   side effects (member `mission_status` -> 2, stats, rewards), so they happen
   once even when 4 players finish in the same second.
6. Reply from the RETURNING row (`Ts2zJ6gj` BossNowHp), never from a value read
   before the transaction.

Every other member sees the new HP on their next MissionInfo poll (4 s), read
from the same row. Boss movement and regen stay derived from timestamps on
read (round 1, 5.1), so no tick thread writes concurrently.

### 5.2 Lobby side effects of RaidMissionStart (mission band implements)

* Master's MissionStart (room `status = 0`): create `raid_room_mission`;
  room `status = 2`, `mission_started_at = now`; master `mission_status = 1`;
  every non-kicked member with `ready = 1` -> `mission_status = 1` (their next
  RoomInfo makes `questStartStand` move them to 1239); members with `ready = 0`
  stay in the lobby ("On a Quest" shows for the others) and may join mid-quest
  via ReadyUp 2 when allowed.
* A member's MissionStart: accepted only with `mission_status = 1` and the room
  in the field.
* Room row while in the field: `3tWuc7de` = 1, `4Qf8RePm` = remaining midway
  window seconds (0 when not joinable).
* Mission end: members' `mission_status` 2 (clear) / 3 (fail) / 4 (retire),
  INFERRED from `questStartStand` sending 2..4 to scene 1261 and
  `strUserStatus` comparing 2/3/4; after the result screens the room returns to
  `status = 0`, members `ready = 0, mission_status = 0`.

---

## 6. Code layout (follow round 1)

* `packet-generator/assets/net/raid.kdl`: request nodes `RaidRoomListNode`,
  `RaidRoomEditNode`, `RaidEnterNode`, `RaidReadyNode` (`h7eY3sAK`,
  `uX87tAZB`), `RaidKickNode`, `RaidChatNode` (`8Fj06n3K`, `3kQUaGx0`),
  `RaidChatCursorNode` (`NLKce95n`), `RaidBlackListNode` (`KpTZlmkh`,
  `mnZ5K4Ii`); rows `RaidChatLog` (WCA9VhL3), `RaidUserTeamArchiveInfo`
  (6nqGvdY1), reuse `FriendInfo` for `Qazc0Qv8`/`ZX7gzQ6s`; replies
  `RaidRoomResp` (+ optional `chat`, `party`), `RaidChatResp`,
  `RaidBlackListResp`, `RaidPlayerInfoResp`. Every request list field is a
  list so a missing group parses; all optional keys `optional #true`. Fix the
  `RaidRoomResp` doc ("EMPTY rooms list means gone" is wrong, 1.3).
* `gimuserver/gme/common/Raids.cpp` grows past 400 lines: split into
  `RaidRooms.cpp` (membership, promotion, absence, ROOM builder),
  `RaidRoomSearch.cpp` (4.4 filters), `RaidChat.cpp` (4.8),
  `RaidBlackList.cpp` (4.9); keep functions under ~50 lines.
* `gimuserver/gme/handlers/RaidRoom.cpp` (List/Edit/Enter/Ready/Kick/Exit
  changes), `RaidSocial.cpp` (Chat, ChatLog, Blacklist, PlayerInfo,
  FriendSelect/Remove).
* `Handlers.hpp` `HANDLE(...)`; `GmeControllerHandlers.cpp`:
  `REGISTER("YF9UQpn6", RaidRoomList, "c0Qf94Ir")`, `("52H0dkjv",
  RaidUpDataRoomInfo, "9KdeuMX8")`, `("yZReG75i", RaidEnter, "N0Azw6ob")`,
  `("vmdA2GD8", RaidUpDataUserReady, "esR7NZL9")`, `("Xbgen5J8",
  RaidRoomKick, "FrX0T2QJ")`, `("81zkMjfN", RaidGetPlayerInfo, "V7M6Bph4")`,
  `("dYIy6Eu8", RaidSendChat, "x2Fm1LSt")`, `("k5pMN68B", RaidGetChatLog,
  "cy0oLna7")`, `("4igfcyIm", RaidUpdateBlackList, "XIgQ4UHm")`,
  `("u59zdKCi", RaidFriendRemove, "0HJ9ShKp")`, `("u59zdKCw",
  RaidFrlendSelect, "0HJ9ShKg")`.
* Migration `16102026_RaidMultiplayer` (3.1), closed `migrate(...)` block.

## 7. Tests

### 7.1 Wire (`tools-local/raid_multi_test.py`, players A..E via round-1 `Player`)

1. A creates a room (op 1, mission 2000, password "1234", kick on, auto-join
   on): ROOM row master A, `XqMsnE89` 1, A sees password, USER login ids set.
2. B RaidRoomList type 1: A's room listed, password "" for B; type 3: exactly
   A's room; C with no rooms in a fresh world state: type 3 returns the single
   placeholder (`KQA1oWj7` 0).
3. B enters with "0000": refused (Retry); with "1234": ROOM with A, B; B
   enters again: success, still 2 members (idempotent).
4. A RaidRoomInfo with `NLKce95n` 0: both members, no chat rows.
5. Chat: B sends ("2", "1") -> reply has B's row; A polls with the old cursor
   -> gets it once; A polls with the new cursor -> no rows; B sends ("1",
   "<color=255:0:0>hi") and ("2", "999999") -> dropped, no rows stored; B sends
   25 stamps fast -> at most the rate allowance stored; resend of the same
   stamp within 10 s is not stored twice.
6. Ready: B ready 1 -> B row `uX87tAZB` 1; B sets C's id -> refused.
7. Kick: B kicks A -> refused (not master); A kicks B -> ROOM without B; B's
   RaidUserInfo login ids are blank; B polls twice -> both times its own row
   with `D2ZKtGR9` 1; B RaidExit -> success; B RaidEnter A's room -> refused
   (kicked); A's polls never show B.
8. Race: C, D, E enter A's 2-slot-left room (mission member limit 4)
   concurrently 20 rounds -> never more than 4 members (`raid_race_test.py`
   style barrier).
9. Promotion: A RaidExit -> C becomes master (`8ZK3w0Np`), room open; last
   member leaves -> room closed, RaidRoomInfo for a stale member -> Retry.
10. Absence: set D's `last_seen` back 200 s in the DB copy, any RoomInfo -> D
    dropped and D's login ids blank.
11. Blacklist: C adds E (op 1) -> `Qazc0Qv8` has E; E's type 1 search does not
    list C's room; C removes E (op 3) -> list without E; RaidUserInfo carries
    the list.
12. Edit: C changes comment to `<b>x</b>\x01` -> stored without `<`, `>` and
    control chars; tries kick flag change -> unchanged; non-master edit ->
    refused.
13. FrlendSelect with a non-friend helper -> refused; with an own non-party
    unit -> stored, `ZX7gzQ6s` server-built; FriendRemove outside a room ->
    `{}` success.
14. PlayerInfo -> one `6nqGvdY1` node, all "0", `h7eY3sAK` caller.
15. Concurrency (5.1, once the mission band lands): 4 players BattleEnd the
    same boss with damage that each alone kills it -> exactly one
    `defeated_by`, hp 0, clear side effects once; replay one BattleEnd -> no
    double damage.

Back up `deploy/gme.sqlite` first (creates accounts).

### 7.2 Device (emulator-5554 + emulator-5556)

Raise both test accounts' RC/level in the worktree DB if quest lists gate
them. A: Raid -> world -> Open Room (password, auto-join on); B: Search ->
see the room -> join with the password; check lobby shows both, ready toggles
on both screens within ~4 s, a stamp appears once on both, kick B (expel
notice, B lands on the world list after RaidExit), B auto-join (Available
Rooms not found while A's room is full/kicked), master leaves -> master-change
window on the other device. Record what -4000 actually does in each scene
(1.2 table is static analysis).

## 8. Unknowns

1. `nXCf2FE9` meaning (INFERRED "apply password").
2. Search filter semantics for RC/kick/joinable/level (4.4, INFERRED).
3. Whether RaidFriendListScene draws placeholder rows (4.4).
4. Chat date format/width (`ToSxkU34` drawn verbatim) and whether type "3"/"4"
   system lines need anything beyond plain text.
5. Edit-box limits for comment and password (not recovered; server limits are
   chosen, 3.2).
6. MissionStatus values 2/3/4 exact meanings and the midway join window length
   (300 s INFERRED).
7. `X6WBc9kJ`/`sQDIZ8Pj` in RaidUpDateCash (deck number INFERRED).
8. `b5LZj7fy` in RaidUserTeamArchiveInfo.
9. Paid stamp set ownership is not tracked by the server.
10. Nothing here is device-verified; the -4000/RaidError reactions, the
    placeholder-row trick and the kicked-row flow need the 7.2 run.

## 9. As built (2026-10-17)

Where the implementation (RaidRooms/RaidRoomSearch/RaidChat/RaidBlackList/
RaidStats, handlers RaidRoomHandlers.cpp, RaidSocialHandlers.cpp, Raid.cpp)
differs from or adds to the sections above:

* **Room in the field** is `raid_room.status = 2` plus the core's
  `mission_start_flg`; `startRun` sets both and `mission_started_at`.
  The master's MissionStart (5.2) also flips every ready member to
  MissionStatus 1 (`sendReadyMembers`); anyone else's MissionStart into a
  shared room is refused unless their row says 1. A master cannot start a
  new run while members still sit on the last run's results; the room goes
  back to the lobby (`settleRoomAfterRun`) once every recently seen member
  collected it or left, and members it did not wait for are reset to 0.
* **RaidRoomInfo** refuses (Retry) a caller with no member row at all; a
  kicked member keeps getting the room with their own `D2ZKtGR9` row.
  The chat delta (`WCA9VhL3`) rides on it from `pw38qdui.NLKce95n`.
* **Auto-join never picks a password room** (its RaidEnter sends none and
  would always be refused); test step 2f checks both cases.
* Friend (2) and guild (4) searches decide "has a friend / guild mate in it"
  in SQL, so the 100-row scan cap cannot hide older rooms.
* A listed room's mission must be at or below the searcher's RC whether the
  room is in the lobby or the field (RaidEnter refuses otherwise).
* RaidRoomKick, RaidExit and RaidRoomDissolve run in one transaction; the
  kick repeats its checks in the UPDATE itself. A master's Dissolve closes
  the room, anyone else's is a leave.
* Edit: a mission change is refused when a member's RC is too low or the
  quest has fewer slots than members, and resets the others' ready.
* Comments: shortest-form UTF-8 only; C0/C1 controls, U+2028/9, `<`, `>`
  dropped; 40 code points.
* Own-unit helper (RaidUnitSelectionScene): the card is rebuilt from
  user_units (`gme::loadUnitCard`), and the unit must not be in the active
  raid squad (deck type 3).
* Records (`raid_user_stat`): Challenge at the first MissionStart, Win /
  RcNwin / Single or Cooperation / Playtime / FastestPlaytime at a won
  MissionEnd, BossFinishCnt for the report that took a boss body to 0,
  battle records from BattleEnd `zS9s5bEI` (clamped), ItemHarvest,
  RaidItemUse, ItemMixCnt, CampRecover, EscapeCnt (retire).
* Boss HP stays the core's parts JSON; damage is applied inside the battle
  report's transaction, which holds the only SQLite connection, so
  concurrent members' reports serialize (test 15: four killing blows at
  once, one kill credit, clear side effects once).

Device (emulator-5558, 2026-10-17): Records screen, the world friend
search placeholder ("Friends not found in any existing Rooms.") seen.
Not yet seen on a device: a second real player in a room (lobby, chat,
kick, promotion); the wire test covers them.
