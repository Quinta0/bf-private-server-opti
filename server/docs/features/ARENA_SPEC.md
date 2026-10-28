# Classic Arena: implementation spec (stage 1)

## Implementation status (stage 2)

Implemented in `gimuserver/gme/handlers/Arena.cpp` + `gme/common/Arena.{hpp,cpp}`,
schemas in `packet-generator/assets/net/arena.kdl`, tables from migration
`16102026_CreateArenaTables`, wire test `tools-local/test_arena.py`.

* ArenaInfo, ArenaStart, Matching, BattleStart, Restart, BattleEnd, Retire and
  FriendList are registered.  UserInfo / GetPlayerInfo send the real
  `user_arena` standing (rank "1" for new players) and archive.
* Feature gate 5 is unlocked; 15 (Challenge Arena) stays locked.
* DeckEdit persists `gKNfIZiA` into `user_info.active_arena_deck`, which
  UserTeamInfo now reports as `arena_deck_num`.
* Opponents: players with a party deck within max(1000, 25%) points, previous
  offer last; otherwise one of the two npc_mst teams re-levelled to the
  attacker's team level (bot ids `ARENABOT<npc id>`).  The enemy snapshot is
  taken at BattleStart, not at Matching (a defender who edits their deck in
  between is fought with the new deck).
* Arena Orbs regenerate server-side (`user_arena.orb_ts`, DefineMst
  recover_time_fight) so the server agrees with the client's local timer;
  `fight_rest_timer` is sent as seconds to the next orb (UNVERIFIED meaning).
* Arena Orb gem refill (ShopUse type 4) clears `orb_ts`, and any sync that
  finds the bar full clears a leftover clock, so a refill never refunds the
  next orb spent.
* Rank-up rewards: gems are paid straight into the wallet, items go to the
  present box (type 4).  Daily task AV advances on a win.
* Settling is one SQLite transaction (claim, payouts, standings, cached
  reply): a failure part-way rolls back to an open battle that a resent
  BattleEnd settles once.
* The battle report is the client's word.  Per battle the server caps
  defeated units at 6 and each crystal count at 9999; the running totals
  saturate at INT_MAX in SQL and are read back saturated, so a forged report
  cannot break other players' screens (it can still inflate the cheater's
  own archive within those caps).
* Unit AI (ticket #45): BattleStart and Restart also send `89ausgc4`
  AIMst rows for every UnitMst.ai_id (100-230).  Both sides act on AI
  (PlayerUnit::checkCmdInput -> BattleUnit::autoCmdInput -> setAiTargetList,
  which looks up UnitMst.ai_id in AIMstList).  AIMstResponse empties that list
  on its first row and MissionStart fills it with monster AI, so without these
  rows no unit finds its AI and every unit only attacks.  A `skill:0` row
  passes when the BB or SBB gauge is full and casts the SBB if ready, else the
  BB.  Rows 100-160 are the original Global F_AI_MST rows (public datamine,
  2017); 180-230 are later types missing from every dump and reuse the rows of
  100.  Data: `deploy/archive/ai.json` via `scripts/gen_arena_unit_ai.py`;
  wire test `tools-local/test_arena_unit_ai.py`.  npc_mst's Kevin team ships
  with empty P_SKILL_ID (its gauges read 0:0 in the battle report), so bot
  units missing a BB/SBB id get their own from the unit archive.  Device
  check 2026-10-19: player BBs (Heaven's Gate, Frozen Orchid) and Kevin's
  (Hell Twins Attack, Destructive Attack) all cast.
* NOT done: friend-request prompt after a battle (friend_req_state is always
  0), random extra rules, Colosseum (still answered by
  the "not available" fallback), event bonus points.

Original stage 1 status follows.

Status: spec only, nothing implemented yet. Written 2026-10-16 from the v2.19.6
client (`re/lib/libgame.so`, `re/full.s`) plus the earlier arena audit in
`re/work/arena/` (`req_annot.txt`, `resp_raw.txt`, `annot.py`, `scenecalls.py`).
Evidence labels: OBSERVED = read from the binary or the server source;
INFERRED = reasoned, must be confirmed on a device. No arena response has ever
been captured from the live game, so every response shape below is rebuilt
from `*Response::readParam`.

Scope: Classic Arena end to end (open from Home, rank screen, rankings, find an
opponent, fight, settle, rank-up rewards). Colosseum is summarised at the end
because Classic Arena progress opens its banner.

## 1. How the client reaches Arena (OBSERVED)

Scene ids come from `GameScene::getGameScene` (`re/work/bootnet/scid.py`):
400 ArenaTopConnectScene, 401 ArenaTopScene, 402 ArenaSuggestEnemyScene,
404 ArenaBattleScene, 405 ArenaBattleEndConnectScene, 406 ArenaResultScene2,
407 ArenaResultRewardScene, 408 ArenaBattleStartConnectScene(int),
409 ArenaResultFriendRequestScene, 410 ArenaFriendListScene, 411 ArenaRankScene,
412 ArenaArchiveScene, 415/416 arena tutorial, 1800 ArenaInfoConnectScene,
1801 ChallengeArenaSelectScene, 1805+ Colosseum.

Home tile (`HomeScene2::touchEnded2` @0x16F5464):

1. `isOverCost` (unit box) must pass.
2. `DefineMst::getArenaNeedMissionID` (`yFRYDj67`, currently "0") must be "0"
   or a cleared mission.
3. If global `FUNC_CHALLENGE_ARENA` is set, go to 1800. Else if
   `UserInfo::isArenaTutoPlaying()` go to 416 (tutorial). Else if
   `FUNC_COLOSSEUM` (features.json `colosseum_enable`, currently 1) go to 1800,
   otherwise 401.

`isArenaTutoPlaying()` @0x1295C7C is true exactly when
`UserArenaInfo.arena_rank_id == "0"` (or a debug flag). Today the server sends
`""`, which avoids the tutorial but makes ArenaTopScene look up rank "" in
ArenaRankMst. **New players must be sent rank "1"** (Rookie, 0 points). The
tutorial path (416 -> 415 -> 400 ArenaTopConnectScene -> ArenaStartRequest) is
then never used; ArenaStart still gets a real handler for safety.

With the current features.json the path is:

```
Home -> 1800 ArenaInfoConnectScene   [ArenaInfoRequest oim9TU1D]
     -> 1801 ChallengeArenaSelectScene [ChallengeArenaResetInfo Zw3WIoWu, already registered]
        "normal_arena_banner": gate 5 locked? -> standard_arena_banner_closed.png + locked notice
        tap -> UserEnteredFeature(5) -> 401 ArenaTopScene (416 if tutorial)
     -> 401 ArenaTopScene            [GetUserInfoRequest, existing UserInfo handler]
        Fight -> deck cost/unit box/item box checks, fight_point > 0
              -> 402 ArenaSuggestEnemyScene + ArenaMatchingRequest 82ztj6Iy
                 "search again" re-creates 402 -> another ArenaMatchingRequest
                 confirm -> setEnemyUserID -> 408
        no orbs -> confirm -> ShopUse type 4 (orb refill, already implemented)
        Rank button 411 (no request), Archive 412 (no request), Rankings 410 [ArenaFriendListRequest xkd9H50w]
     -> 408 ArenaBattleStartConnectScene [ArenaBattleStartRequest 7f1rg92L, or ArenaBattleRestartRequest NPyXDgV9]
     -> 404 ArenaBattleScene (client simulates the whole fight)
     -> 405 ArenaBattleEndConnectScene [ArenaBattleEndRequest oQNtKU51]
     -> 406 ArenaResultScene2 -> 407 ArenaResultRewardScene (one pass per rank gained)
     -> 409 ArenaResultFriendRequestScene (only if friend_req_state == 1) -> 401
Boot: InitializeScene sends ArenaRetireRequest eyg8sA32 when SaveData::isArenaRetire
      (set by CCDirector::pause while an arena battle runs, cleared on resume), i.e. the
      app was killed mid-battle.
```

Required gate change: `gme::featureGates()` must stop locking id 5 (keep 15,
Challenge Arena, locked; `challenge_arena` stays 0). ChallengeArenaSelectScene
reads `shouldGateLocked(5)` for the Standard Arena banner.

Colosseum warning (OBSERVED, `ColosseumCommonInfo::isColosseumOpen` @0x12D9214):
open when `FUNC_COLOSSEUM && (UserReleaseInfo 8 released || max_arena_rank_id > 3)`.
So once a player reaches rank 4 (2000 pts) the Colosseum banner on 1801 and the
shortcut on 401 become live, and ColosseumTopScene auto-sends
ColosseumDeckEditRequest. Until Colosseum ships, those requests hit the
unregistered-id fallback ("not available on this server yet", back to Home),
which is acceptable. The FeatureGates.hpp comment saying Colosseum is only
reachable through the arena battle path is wrong and should be corrected.

## 2. Wire format notes (OBSERVED)

All requests carry the usual `IKqx1Cn9` login block; read them with
`glz::opts{.error_on_unknown_keys = false}`. Every value the client sends is a
string. Response singletons (`8jBJ7uKR`, `PQ56vbkI`, `fQ3XImM2`, `9NX83YVe`,
`yTEvm85f`) run their setter per row, so they must carry exactly one row; an
empty array leaves stale client memory (see user.kdl UserArenaInfo doc).
The `60subGk3: [{EfbZ7mh9: "1"}]` group on start/end/retire/restart is an
operation header; ignore it.

## 3. Requests

### 3.1 ArenaInfoRequest `oim9TU1D` / key `oqQxdFfa` (registered, returns `{}`)

Body: empty (login only). Caller ArenaInfoConnectScene, which then always goes
to 1801. It reads `UserArenaInfo` (8jBJ7uKR, via dead `arenaBattleTheWay`),
`ColosseumClassInfoList` (HgYvs3am) and `ColosseumUserInfo` (FfBebG0R, cbp).

Response: `8jBJ7uKR` (one row, real standing). Colosseum groups optional until
Colosseum ships. `{}` already works; adding 8jBJ7uKR keeps the singleton fresh.

### 3.2 ArenaStartRequest `zu6ifpy0` / `bGjK4UH1` (unregistered)

Body: empty. Only reached from the arena tutorial (rank "0"). Handler: ensure
the user_arena row exists with rank 1, return `8jBJ7uKR`. ArenaTopConnectScene
then goes to 401.

### 3.3 ArenaMatchingRequest `82ztj6Iy` / `Q53tUIBE` (unregistered)

Body:
```
"fq2C03jh": [{ "Z0Y4RoD7": "<arena deck num>",   // UserTeamInfo arena deck, else active deck
               "ciDqCjAF": "<bonus type>",       // GeneralEventMst arena camp param, may be "0"/""
               "59b1eE6N": "<bonus param>" }]
```
Response `fQ3XImM2` ArenaSuggestEnemyResponse, ONE row (singleton
`ArenaSuggestEnemy`):

| key | setter | type | notes |
|---|---|---|---|
| h7eY3sAK | setUserID | str | opponent user id (bot ids too); echoed back in BattleStart |
| B5JQyV8j | setHandleName | str | becomes the enemy party name in battle |
| D9wXQI2V | setLv | int | team level |
| JmFn3g9t | setArenaRankID | int | must exist in ArenaRankMst |
| pn16CNah | setUnitID | str | leader unit_id (mst id); thumbnail + requestUnitFiles |
| 69vnphig | setBattleCnt | int | |
| 8CEu9Kcm | setWinCnt | int | |
| 20iEWRCV | setBattleCntAccept | int | |
| 0rAkzg7L | setWinCntAccept | int | |
| U4pMNjy0 | setRankingPoint | int | |
| 98WfKiyA | setFriendID | str | unused by the scene; send "" |
| 2pAyFjmZ | setUnitImgType | int | leader alt art, 0 default |

Server work: pick one opponent (players within a points band first, never
self, prefer not the previous offer; fall back to a bot). Snapshot the
opponent's defense deck NOW and store it as the user's pending offer, so the
fight matches what was shown.

### 3.4 ArenaBattleStartRequest `7f1rg92L` / `zaMR9o0d` (unregistered)

Body:
```
"9NX83YVe": [{ "h7eY3sAK": "<enemy user id from ArenaSuggestEnemy>",
               "Z7PXoxc2": "<0|1 random rule toggle>" }],
"fq2C03jh": [{ "Z0Y4RoD7": "<deck num>", "ciDqCjAF": "...", "59b1eE6N": "..." }],
"rj5fgJh0": [{ "t5R47iwj": "<drop item display config>" }]
```
Validate: enemy id equals the stored offer; fight_point >= 1; deck num 0..9 with
at least one unit. Spend one orb, create the battle row (server battle id,
status open), store both snapshots.

Response:

`9NX83YVe` ArenaBattleEnemyInfoResponse, ONE row:

| key | setter | type | notes |
|---|---|---|---|
| h7eY3sAK | setUserID | str | the scene copies this into UserState enemy id |
| xL0p2MkY | setBattleID | int | int32 numeric string; echoed in BattleEnd |
| INT1K9Pf | setUserTeamInfo | str (JSON text) | NpcTeamParse |
| nZc1Ip3N | setUserPartyInfo | str (JSON text) | NpcPartyParseList |
| U42YcFNu | setUserUnitInfo | str (JSON text) | NpcUnitParseList |
| bM7RLu5K | setFriendMessage | str | |
| IvuccUuD | UserArenaRuleInfo::setRule | int | "0" = no extra rule (INFERRED); random rule out of scope |

plus `fEi17cnx` UserTeamInfo with the decremented `fight_point` (YS2JG9no).

The three JSON strings use the same format as `npc_mst.json` (`g94bDiaS`,
`oPsmRC18`, `bS9s4GCp`) and are consumed by `RivalParty::entry(team, party,
units, message)` @0x10D28DC:

* Team (NpcTeamParse::parse): `{"P_USER_ID":"..","P_FRIEND_MESSAGE":"..","P_LV":"..","P_GUILD_ID":""}`.
* Party (NpcPartyParseList::parse): array of `{"P_USER_UNIT_ID","P_MEMBER_TYPE","P_DISPORDER"}`; member type "0" = leader (matches server's deck convention), disporder 0..5.
* Units (NpcUnitParseList::parse @0x10AFCDC), keys read: `P_USER_UNIT_ID, P_UNIT_ID, P_UNIT_TYPE_ID, P_LV, P_HP, P_ATK, P_DEF, P_HEL, P_SKILL_ID, P_SKILL_LV, P_EXTRA_SKILL_ID, P_EXTRA_SKILL_LV, P_EXTRA_PASSIVE_SKILL_ID, P_EXTRA_PASSIVE_SKILL_ID2, P_ADD_HP, P_EXT_HP, P_ADD_ATK, P_EXT_ATK, P_ADD_DEF, P_EXT_DEF, P_ADD_HEL, P_EXT_HEL, P_EQP_ITEM_ID, P_EQP_ITEM_ID2, P_IMG_TYPE, P_FE_INFO, P_OMNI_LV`. All values strings.
* Units whose unit_id is not in the unit mst are dropped (`Utils::unitMstExists`).

Mapping from the server's `UserUnitInfo` (user.kdl): P_HP/ATK/DEF/HEL = base_*,
P_ADD_* = add_* (imps), P_EXT_* = ext_* + limit_over_* (INFERRED, verify that a
rival unit's stats equal the same unit's stats on its owner's unit screen),
P_SKILL_ID/LV = bb_id/bb_lvl, P_EXTRA_SKILL_ID/LV = sbb_id/sbb_lvl,
P_EXTRA_PASSIVE_SKILL_ID(2) = extra_passive_skill_id(2), P_EQP_ITEM_ID(2) =
equipitem_id(2), P_IMG_TYPE = unit_img_type, P_OMNI_LV = omni_level,
P_FE_INFO = fe_skill_info (OBSERVED since #136: the parser calls UserUnitInfoBase::setFeSkillInfo, the same setter as UserUnitInfo Fnxab5CN; sent as-is so SP picks fight). Leader skill comes
from the unit mst on the client side. Fix the archive-granted leader_skill_id=0
bug (ROADMAP) before trusting snapshots.

The player's own side is NOT sent: `PlayerParty::entry` builds it from the
local arena deck unless `UserState::isArenaRestartFlg`.

### 3.5 ArenaBattleRestartRequest `NPyXDgV9` / `sWofKGMl` (unregistered)

Body: only the `60subGk3` header. ArenaBattleStartConnectScene(1) sends it and
sets the restart flag, so `PlayerParty::entry` builds the player side from
`5K5wsBoL` ArenaBattlePartyInfoResponse (`nZc1Ip3N` party JSON, `U42YcFNu` unit
JSON, same NPC format). Response: the stored `9NX83YVe` row for the open
battle plus `5K5wsBoL` built from the attacker snapshot; no orb spent. If no
open battle, refuseToHome. Reachability in 2.19.6 is unclear (the only caller
seen, `ArenaInfoConnectScene::arenaBattleTheWay`, has no callers); ship it but
do not spend device time on it.

### 3.6 ArenaBattleEndRequest `oQNtKU51` / `jkgWq1d0` (unregistered)

Body group `yTEvm85f` (one row; all strings):

| key | source | meaning |
|---|---|---|
| xL0p2MkY | ArenaBattleEnemyInfo battle id | must match the open battle |
| 3pAQY5KW | BattleState::getArenaResult | "1" win, "0" loss (incl. give-up; timeout -> HP judge) |
| hoG2ieT5 / 6PLsn8xo | BattleLog | BC / HC crystals picked up |
| 5NRJQ1LU / XP06YWdT | | max turn damage / max turn sparks |
| U8uZLA34 / rZQJF5G9 | | sparks / skill uses |
| mFID53JZ / sQ2aMk8B | | defeated unit count / defeated units info (string, format unread) |
| G8EebW0o / x3ZgJS9W / s92RgSwp | | all-knockdown / timeout / perfect flags |
| XRoD9Hg7 / GRCVD1i8 / Uy15wSJd | | BB knockdowns / BB KOs / SBB KOs |
| EfwspXXo | | unnamed BattleLog getter |
| w6IiLvN8 / xFH5i4o8 | | multi-kills / max multi-kill |
| hI68VzHu / F32xJzsH / Wnc6A31E | | sum damage / sum heal / overkills |
| Ljg1nUro / tZtlrUHI / IWh5sCSm / yY8snrWG / hnWoR2fE | ArenaBattleLog | HP-max cnt / gauge-max cnt / weak-element kills / skill uses / extra skill uses |
| gpDCY81t | | "lastTurnPlayerCnt,lastTurnRivalCnt" |
| WDYR11jJ / chy3HnIm / rIT7NvJ2 / V2Tb2CCj | | HP party rest / HP enemy rest / BB party rest / BB enemy rest (INFERRED order) |

Server: settle once (battle must be open and owned by caller, else a second
call returns the stored result unchanged). Apply points, counters, archive,
rank-ups and rewards, defender counters, daily task `AV` on a win.

Response groups:

`yTEvm85f` ArenaBattleResultResponse, ONE row:

| key | setter | type |
|---|---|---|
| xL0p2MkY | setBattleID | str |
| 3pAQY5KW | setBattleResult | str ("1"/"0") |
| s07yWuZR / C1w4H62i | setBefore/AfterArenaRankID | str |
| jPoM1UV8 / TJ9cQ8AY | setBefore/AfterArenaRankPoint | int |
| HTVh8a65 | setKarmaNum | int (karma awarded) |
| Sx5H0Ikn | setFriendReqState | int: 1 = show the friend-request prompt (`enableFriendRequest` @0x1252D50 is `== 1`); 0 for bots, self-friends, and when unsure |
| tvWWL9wI | setGetBonusPoint | int (event bonus points, 0) |
| ciDqCjAF / 59b1eE6N | setBonusType / setBonusParam | echo request |
| 69vnphig | setBattleCnt | int |

`okd0y3Ir` ArenaBattleRewardResponse, one row PER RANK GAINED, in order
(ArenaResultRewardScene walks them with `NowLvUpIndex`); `[]` when no rank-up:
`1ef7JNSC` dia (gems) int, `4T0Q2Bh5` items "itemId:num,itemId:num", `ZBwx5ct3`
first-rank-up flag int. From arena_rank_mst: reward type 1 -> dia = second
field of "0:N"; type 2 -> items "id:N".

Also send `8jBJ7uKR` (new standing, battle_id ""), `PQ56vbkI` (updated
archive: atk series wins, sums), `fEi17cnx` (karma, gems, colosseum tickets),
and the user item list group if items were granted (reuse whatever MissionEnd
sends for item drops).

### 3.7 ArenaRetireRequest `eyg8sA32` / `93i8EbyM` (unregistered, BOOT PATH)

Body: `"yTEvm85f": [{"xL0p2MkY": "<SaveData arena battle id>"}]`. Sent from
InitializeScene through ConnectRequestList. Settle the battle as a loss if
open; ALWAYS succeed (unknown or already-settled id -> `{}`), never refuse,
because a refusal here would break login. Return `8jBJ7uKR`.

### 3.8 ArenaFriendListRequest `xkd9H50w` / `L3TDhrn2` (unregistered)

Body: empty. Response `BYaF62TE` FriendUserArenaInfoResponse (list, full
replace). Keys: `h7eY3sAK` user id, `B5JQyV8j` handle, `2Fh3J7ng` team lv,
`pn16CNah` leader unit id, `4A6LzBxr` unit lv, `JmFn3g9t` rank id (int),
`U4pMNjy0` points, `69vnphig`/`8CEu9Kcm`/`20iEWRCV`/`0rAkzg7L` counters,
`NH65Wj0f` info type, `2pAyFjmZ` img type, `2rqxaZ6K` position, `jkldTrhL` ep3
flg, `9i2xhMaJ` sex, `iNy0ZU5M` element, `btZizNep` hair id, `RVVgyuor` arm id
(the last four only matter when the leader is the summoner).

The scene has two tabs that call `getList(type)` with type 0 and 1
(`ArenaFriendListScene+0x348`), captions arena_ranking_total / _friend; which
value is which is UNKNOWN. Plan: send each friend row with both types, and the
global top 50 with the other type, then fix on device. The self row is built
client-side from UserArenaInfo (`getMyInfo`), so the caller is not in the list.

### 3.9 Screens with no request

ArenaTopScene (401) and ArenaRankScene (411) read `UserArenaInfo`,
`ArenaRankMst` (already sent by Initialize, 101 ranks), `DefineMst`
(`tzCfGX83` ArenaMaxRank 101, `sy9G24Su` FightPointHealCnt 1, `0BPn68DG`
orb recover 3600 s, `YR4HI56k` battle time limit 99, `yFRYDj67` need mission 0),
`UserTeamInfo` (`YS2JG9no` fight_point, `9m5FWR8q` max, `gKNfIZiA` arena deck)
and `UserTeamArenaArchive.atk_series_win_cnt`. ArenaArchiveScene (412) reads
PQ56vbkI. All of these arrive through GetUserInfo / Initialize.

## 4. Existing server code to reuse

* `ArenaInfo.cpp` (registered stub), `ChallengeArenaResetInfo.cpp` (hub request, already fine).
* `gme::zeroedArenaInfo` / `zeroedArenaArchive` (Common.hpp ~1897-1946), used by UserInfo.cpp:203 and GetPlayerInfo.cpp:72: replace with a loader that reads user_arena; keep the one-row guarantee. Default rank "1", max rank "1".
* `FeatureGates.hpp`: drop id 5 from the locked list (keep 15).
* `ShopUse.cpp` type 4 already refills Arena Orbs (`fight_point`, price `fight_point_heal_count`).
* `user_info.fight_point / max_fight_point` (migration 400) and `active_arena_deck` (unused so far); `getTeamInfo` + PacketInterfaceSchemas UserTeamInfo mapping (add `arena_deck_num` <- `active_arena_deck`).
* `DeckEdit.cpp`: also persist `Ti62XfZK[0].gKNfIZiA` (DeckEditRequest sends it; DeckEditTeamInfo in user.kdl lacks the field) into `active_arena_deck`. Without this the arena deck resets to 0 on every login.
* `user_decks` (deck_type 1, deck_num, member_type 0 = leader, disp_order) + `readUserUnits()` give everything a snapshot needs.
* `fromArchivedUnit()` + `scaleUnitBaseStats()` (Common.hpp 174/1691) to build bot units from the unit mst.
* `addUserItem()` (Common.hpp:390), gem/karma updates via user_info, `advanceDailyTask(db, id, "AV")` and flip `dailyTaskCountable` to allow AV.
* `npc_mst.json` (2 NPCs, Seria/Kevin) as bot templates; `arena_rank_mst.json` for thresholds and rewards (ServerCache already loads it as `arena_ranks`).
* KDL: UserArenaInfo / UserTeamArenaArchive exist in user.kdl; ArenaRankMst exists. New: requests above, ArenaSuggestEnemy, ArenaBattleEnemyInfo, ArenaBattlePartyInfo, ArenaBattleResult, ArenaBattleReward, FriendUserArenaInfo (new `arena.kdl`, imported in handlers.kdl).

## 5. Server data model (proposed)

Migration `16102026_CreateArenaTables`:

```
user_arena(user_id PK, rank_id INT DEFAULT 1, max_rank_id INT DEFAULT 1, points INT DEFAULT 0,
           battle_cnt, win_cnt, battle_cnt_accept, win_cnt_accept, atk_series_win, max_atk_series_win,
           def_series_win, max_def_series_win, offer_user_id TEXT, offer_snapshot TEXT, updated_at)
arena_battle(battle_id INTEGER PK AUTOINCREMENT, attacker_id, defender_id, is_bot,
             enemy_team TEXT, enemy_party TEXT, enemy_units TEXT, enemy_message TEXT,
             my_party TEXT, my_units TEXT, before_points, before_rank,
             status INT (0 open,1 won,2 lost,3 retired), result_json TEXT, created_at, settled_at)
user_arena_archive(user_id PK, one column per PQ56vbkI counter)   -- or columns on user_arena
```
Only one open battle per attacker: starting a new one retires the old one as a loss.

Rules (INFERRED, original formula unknown; keep in one function):
win = clamp(100 + (opp_points - my_points)/50, 50, 300) points, loss = 0 (ranks never
go down; the result screens only animate rank-ups). Rank = row of arena_rank_mst whose
[start,end] contains points. Rank-up rewards granted once per rank (when it exceeds
max_rank_id). Karma per win: 100 * rank (tunable). Defender: battle_cnt_accept++, and
win_cnt_accept++ when the attacker loses; defenders never lose points.
Matching: real players with a deck and |points diff| <= max(1000, 25%), excluding self
and the last offer; otherwise a bot whose level/stat scale follows the attacker's rank.
Bot ids: fixed 8-char ids that can never be real (e.g. "ARENABOT" + n kept out of
user_info), friend_req_state 0.

## 6. Test plan

Wire (tools-local/test_arena.py, modelled on tools/test_accounts.py): two fake devices
A and B finish the tutorial; A: UserInfo shows rank "1"; Matching returns B (or a bot);
BattleStart spends an orb and returns parseable JSON strings (json.loads each); BattleEnd
win raises points/rank, returns one reward row per rank crossed; second BattleEnd with the
same id changes nothing; Retire with junk id succeeds; B's UserInfo shows
battle_cnt_accept 1; FriendList lists B for A; orbs at 0 -> BattleStart refuses.
Device (emulator-5556): Home -> Arena hub -> Standard Arena opens 401 with rank Rookie,
Fight shows an opponent, a full battle plays with the enemy team visible, result and
rank-up screens finish and return to 401; Rankings tab opens; kill the app mid-battle and
relaunch to confirm login survives ArenaRetire.

## 7. Open questions

* Which FriendUserArenaInfo info type (0/1) is the Friends tab vs the Total tab.
* P_EXT_* meaning (ext only, or ext + limit over).
* Whether rule id "0" is safe for `UserArenaRuleInfo::setRule` (no ArenaExtraRuleMst group key exists in resp_keys.tsv; the rule table is a downloaded MST).
* Exact original point formula and karma amounts (none dumped).
* Whether `UserArenaInfo.battle_id` "" vs "0" matters anywhere live (only the dead `arenaBattleTheWay` reads it).
* What ArenaBattleStartConnectScene(1) (restart) is reached from in 2.19.6.
