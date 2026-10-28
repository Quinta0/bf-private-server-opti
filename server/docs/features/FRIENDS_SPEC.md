# Friends and helpers between real players (implementation spec)

Stage 1 of the friends track, written 2026-10-16. Stage 2 implemented most of it; see §9 for what landed and what did not. Each claim says where it comes from: **OBSERVED** means read in the v2.19.6 arm64 `libgame.so` (addresses are VAs, which equal file offsets) or in a captured request log. **AUTHORED** means a choice this server makes, with no recovered behaviour behind it. **UNVERIFIED** means it has to be confirmed on a device before anyone relies on it.

The server's friend system today is single-player. `user_friends` holds five seeded NPC "friends" per player, plus any developer encounters or `NEW########` strangers the player recruited. FriendApply recruits instantly on the other side's behalf, FriendDelete removes only NPC rows, and nobody can find another real player. This spec adds real players next to the NPC roster. The NPC roster is not replaced. It stays the fallback that keeps the helper picker full on a small server.

## 1. How the client's Social screens work

### 1.1 Three client caches, all fully replaced

| Key | Response class | Client singleton | Row-0 behaviour |
|---|---|---|---|
| `tojMy68W` | FriendInfoResponse::readParam @0x13D93C0 | FriendInfoList | `removeAllObjects` on row 0 field 0 (OBSERVED) |
| `xZH6EIQ7` | ReinforcementInfoResponse | ReinforcementInfoList | full replace (existing server doc) |
| `PAd9aS1H` | SummonerFriendInfoResponse::readParam @0x144C11C | SummonerFriendInfoList | `removeAllObjects` on row 0 (OBSERVED) |

**Every reply that carries `tojMy68W` has to carry the whole list**: friends, pending requests in both directions, and any transient search rows. A partial list silently deletes the rest on the client. An empty JSON array never reaches readParam, so it cannot clear anything (see 4.6).

### 1.2 `FriendInfo.friend_type` (`96Nxs2WQ`): one list, four meanings (OBSERVED)

| Type | Meaning | Evidence |
|---|---|---|
| 1 | Established friend | FriendList2Scene::setFriendInfoList @0x1682070 shows only type 1 with priority ≤ 1. `getCountAgreeOk` @0x126030C counts type 1 with priority ≤ 1. `existTypeOK` @0x12607CC accepts exactly 1, which also decides the friend Honor rate. |
| 2 | Request **sent** by me (pending outgoing) | FriendAgreeListScene::setFriendInfoList @0x1664E18 lists types 2 and 3. A type 3 row gets the label `FRIEND_AGREE_REC`; other rows get `FRIEND_AGREE_REQ`. |
| 3 | Request **received** (pending incoming) | `getCountAgreeRec` @0x1260144 counts distinct user ids of type 3. GameScene::updateFooter and FriendTop2Scene read it as the pending-request **badge**. Reject-all collects only type 3 rows (@0x16684DC). |
| 4 | Search result (transient) | FriendIdSearchFriendRequestScene::initialize @0x16555FC and FriendRandomSearchScene::setFriendInfoList @0x165C228 pick type 4 rows. |

`priority` (`yu18xScw`): rows with priority > 1 are skipped by every counter and by the friend and agree lists. Send **1** on every real row.

`jkldTrhL` on FriendInfo is the Ep3/"summoner mode" flag, member +0x250. It is read through vtable slot +0x78, which is `UserUnitSummonerInfoBase::isSummoner` @0xE68148 (`== 1`). FriendAgreeListScene drops rows where the flag does not match `UserState::getEp3ModeFlg`. **Send 0** for the normal game. Summoner-mode friends are out of scope (§7).

### 1.3 Navigation (OBSERVED from constructor call sites)

- **Friend list**: FriendList2Scene. In Ep3 mode it is SummonerFriendListScene instead. It sends no request and renders the cached FriendInfoList. **FriendGet runs at login (LoginScene::initConnect) and after missions (MissionResultScene, ChallengeResultScene, FrontierGateBattleEndScene, …).** No Social screen calls it.
- **Friend detail**: FriendDetailScene. Favorite goes through FriendDetailConnectScene and sends **FriendFavorite**. Delete is `confirmAnswerYes` and sends **FriendDelete**, then the scene goes back to FriendList2Scene. The detail scene finds its row again by `UserState::getSelectUserID` in `downloadFiles` (@0x1669A6C), so a full replace in the reply is safe for it.
- **Add friend, by ID**: FriendIdSearchScene. It shows your own ID from `UserInfo::getFriendID`, which is `98WfKiyA` in the login packet. The edit box is decimal input mode (5) with max length 128. Empty text shows `FRIEND_ID_SEARCH_NOT_ID` locally. Otherwise the scene stores the text and opens FriendIdSearchFriendRequestScene, which sends **FriendSearch**. Tapping Request sends **FriendApply**.
- **Add friend, random**: FriendRandomSearchScene sends **FriendRandomSearch**. Tapping a card opens FriendRandomSearchFriendRequestScene, which sends **FriendApply**. On success its `noticeOK` sends **FriendGet** to refresh.
- **Requests**: FriendAgreeListConnectScene sends **FriendGetAgree**, then FriendAgreeListScene renders types 2 and 3. The confirm codes are 5000 for **FriendAgree** (FriendAgreeConnectScene), 6550 to refuse or cancel one row (FriendRefuseConnectScene sends **FriendRefuse**), 5500 to reject all, which sends FriendRefuse with the comma list, and 0 when the list is full, which opens ShopFriendExtScene.
- **Post-battle "add this helper?"**: MissionResultFriendRequestScene (Arena, Colosseum, Vortex and Challenge Arena variants too). It is offered only when `FriendInfoList::exist(helperId)` is false, and that check ignores the type. So a helper who already has a pending row (type 2 or 3) is never offered again. That is correct, and it means type 2 and type 3 rows must be present at login.
- **Helper picker**: FriendListUtils::getReinforcementList @0x11689A0 reads **ReinforcementInfoList** (filled only by FriendGet). It drops your own user_id, groups favorites first, then rows where `existTypeOK` is true, then the rest, and sorts.

### 1.4 How refusals behave (OBSERVED: each scene's `noticeOK`)

`HandleResult::refuseToRetry` reaches the client as notice -4000 (`cmn w20, #0xfa0`):

| Scene (request) | Handles -4000? | What -4000 does |
|---|---|---|
| FriendAgreeConnectScene (Agree) @0x1663464 | yes | back to FriendAgreeListScene |
| FriendRefuseConnectScene (Refuse) @0x1662144 | yes | back to the agree list |
| FriendAgreeListConnectScene (GetAgree) @0x1664AB0 | yes | rebuilds a scene |
| FriendIdSearchFriendRequestScene (Search, Apply) @0x1657510 | yes | back to FriendIdSearchScene |
| FriendDetailConnectScene (Favorite) @0x1669810 | yes | back to the detail scene |
| FriendRandomSearchFriendRequestScene (Apply) @0x165BC0C | yes | sends FriendGet |
| **FriendDetailScene (Delete)** @0x166B7BC | **no** | stays put. **Never refuse Delete.** |
| **MissionResultFriendRequestScene (Apply)** @0x18C5934 | **no** | **Never refuse Apply** (the same handler serves both scenes). |
| **FriendRandomSearchScene (RandomSearch)** | **no own noticeOK** | **Never refuse RandomSearch.** |

A plain `HandleResult::error` or an exception closes the session or sends the player Home. No friend handler should ever produce one for a normal outcome.

## 2. Wire formats

All bodies also carry `IKqx1Cn9` (login info), `6FrKacq7` (signal key) and `KeC10fuL` (version table; OBSERVED in the FriendGet capture `server/deploy/log/http_log_2o4axPIC_20261015_072220.log`). Read them with `glz::opts{.error_on_unknown_keys=false}`. Integer params from `JsonNode::addParam(const char*, int)` travel as quoted strings: use `i32::str`.

| Request | Group / AES key | Body (createBody) | Status |
|---|---|---|---|
| FriendGet | `2o4axPIC` / `EoYuZ2nbImhCU1c0` | optional `StQIyohe:[{jkldTrhL:"<reinType>"}]`, gated by a global flag (@0x13A41E4). LoginScene and MissionResultSummonerTopScene call `setReinType`. | registered; extend |
| FriendApply | `WUNi08YL` / `gbpdAEZuv8IP40UF` | `BPz1e5tU:[{h7eY3sAK:<target user_id>}]` from `UserState::getSelectUserID` (@0x13A3DA0) | registered; change semantics |
| FriendDelete | `SfMN9w4p` / `yWXI80UKQNBZFozw` | `BPz1e5tU:[{h7eY3sAK}]` (@0x13A3EC0) | registered; extend |
| FriendGetAgree | `aVqJrR08` / `H4zJCK2jPeaskVdf` | nothing beyond the tags (@0x13A4114) | **missing** |
| FriendAgree | `NYKBH4V6` / `8PFVDiUIZyES2vzR` | `BPz1e5tU:[{h7eY3sAK:<requester user_id>}]` (@0x13A3C80) | **missing** |
| FriendRefuse | `N5SQd7oA` / `fk8YaVy9puJbnLWv` | `BPz1e5tU:[{h7eY3sAK:<selectUserID>, 7f3WL6Mx:"id1,id2,..."}]` (@0x13A43C4). The comma list is `UserState::getRejectReqFriendInfoList` and may be empty or stale (§4.4). | **missing** |
| FriendSearch | `umNt8M7i` / `qa3sHuDgKxV5nI1F` | `BPz1e5tU:[{98WfKiyA:"<typed text>"}]` from `UserState::getFriendID` (@0x13A4698). This is the **public** ID, not a user_id. | **missing** |
| FriendRandomSearch | `mE5gUQOp` / `oK4VaDGF` | `3fTYzqT6:[{3Ug2QynF:"0", lzpDckWv:"100"}]` (literals, likely offset and limit) + `StQIyohe:[{jkldTrhL:"<mode>"}]`. Mode is 0 from the classic scene (ctor default, @0x13A4250) and set by the Summoner scene through `setFriendSearchParam`. (@0x13A42B4) | **missing** |
| FriendFavorite | `ps6Vgx4R` / `XcE1CK6L` | `BPz1e5tU:[{h7eY3sAK, 5JbjC3Pp:"<0/1>"}]` (favorite from `UserState::getFavorite`, @0x13A3FDC) | **missing** |
| FixGiftInfo (send part) | `gLRIn74v` / `15gTE9ft` | `Gg5c3eM6:[{s2WnRw9N, 23Xi0jom, qD74LPXb:"toUserId:giftId,..."}]` (@0x13A37E4) | registered; sending ignored today |
| ReinforcementRequest | `6AqUE0xj` / (none) | dead: no constructor call site | leave unregistered |

New KDL (add to `packet-generator/assets/net/handlers.kdl` next to FriendApplyReq, or to a new `friends_social.kdl` imported there):

```kdl
json FriendSearchNode   { field friend_id type="str" { key "98WfKiyA" } }
json FriendSearchReq    { field loginInfo type="[LoginInfoReq]::size(1)" { key "IKqx1Cn9" }
                          field nodes type="[FriendSearchNode]" { key "BPz1e5tU" } }
json FriendRandomPage   { field offset type="i32::str" { key "3Ug2QynF" }
                          field limit  type="i32::str" { key "lzpDckWv" } }
json FriendModeNode     { field mode type="i32::str" { key "jkldTrhL" } }
json FriendRandomSearchReq { loginInfo IKqx1Cn9; page "[FriendRandomPage]" 3fTYzqT6; mode "[FriendModeNode]" StQIyohe }
json FriendRefuseNode   { field user_id type="str" { key "h7eY3sAK" }
                          field user_ids_csv type="str" { key "7f3WL6Mx" } }
json FriendRefuseReq    { loginInfo; nodes "[FriendRefuseNode]" BPz1e5tU }
json FriendFavoriteNode { field user_id type="str" { key "h7eY3sAK" }
                          field favorite type="i32::str" { key "5JbjC3Pp" } }
json FriendFavoriteReq  { loginInfo; nodes "[FriendFavoriteNode]" BPz1e5tU }
json FriendGetAgreeReq  { loginInfo }
json FriendAgreeReq     { loginInfo; nodes "[FriendApplyNode]" BPz1e5tU }   // reuse FriendApplyNode
json FriendSocialResp   { friend_info "[FriendInfo]" tojMy68W
                          friend_point_info "[FriendPointInfo]" 6e4b7sQt }  // one reply shape for all of them
```

(The shorthand above names fields as `name type key`. Write them in the real `field … { key … doc … }` form, with a `doc` on every field, as the existing KDL does.) Make every `str` field of the requests tolerant: read failures are logged and treated as "nothing named", never as an error.

## 3. Data model (AUTHORED)

Migrations append to `gimuserver/db/MigrationManager.cpp`:

```text
16102026_UserInfoPublicFriendId
  ALTER TABLE user_info ADD COLUMN friend_id TEXT;
  CREATE UNIQUE INDEX user_info_friend_id ON user_info(friend_id) WHERE friend_id IS NOT NULL;
  -- backfill in the C++ migration body: for each user_info row with NULL friend_id,
  -- draw a CSPRNG 9-digit id in [100000000, 999999999], retry on UNIQUE conflict.

16102026_CreateUserRelations
  CREATE TABLE user_relations (
    user_id    TEXT    NOT NULL,   -- the side this row belongs to
    other_id   TEXT    NOT NULL,   -- a REAL player's user_info.id (NPC ids never go here)
    state      INTEGER NOT NULL,   -- 1 friend, 2 request sent, 3 request received (= FriendInfo.friend_type)
    favorite   INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,   -- unix seconds; request time for 2/3, friendship time for 1
    PRIMARY KEY (user_id, other_id));
  CREATE INDEX user_relations_other ON user_relations(other_id, state);
```

One relation is always **two rows**, one per side, written in a single transaction. The state pairs are (1,1), (2,3) and (3,2). Favorite is per side. NPC friends stay in `user_friends` unchanged, so the two id namespaces never share a table.

- **Public friend ID.** Add `field<&::LoginInfoResp::friend_id>("friend_id", {.read = true})` to `PacketInterfaceSchemas.hpp`. `getLoginInfo` then emits `98WfKiyA` everywhere UserInfo goes out. Assign it in CreateUser (`Tutorial.cpp`, inside the existing transaction). The ID never changes. The 9-digit size is from ROADMAP and is AUTHORED, but the decimal edit box (OBSERVED) does force digits.
- **ID collisions.** `RandomId()` draws 8 characters from `[0-9A-Za-z]`, so a real user id could in theory equal an NPC id such as `DCF00001` or start with `DEV`/`NEW`. Make CreateUser re-roll ids that start with `DCF`, `DEV` or `NEW`. Resolve ids by table, real players first, rather than by prefix.
- **Last login** is `gumi_live_users.last_login_at`, joined through `user_info.gumi_user_id`. It feeds `0CAQ6wUe` and random-search ordering.
- **Rate limits** use an in-process token bucket per user (mutex plus map; resetting on restart is fine), unless the admin or hardening track adds a shared table first. Search: 20 per 10 minutes. Random search: 30 per 10 minutes. Apply: 30 per day. All of these are AUTHORED.
- **Optional (P2):** `user_info.friend_message TEXT DEFAULT ''`, stored from DeckEdit's `bM7RLu5K` (DeckEditRequest::createBody sends it, OBSERVED @0x13A2F6C) and length-capped. It fills `bM7RLu5K` on FriendInfo and ReinforcementInfo instead of the hardcoded "GG WP".

## 4. Server behaviour per request

Put the new logic in a new module `gimuserver/gme/common/Relations.{hpp,cpp}`. `Friends.cpp` is already 864 lines and should keep only the NPC roster. Functions should stay under 50 lines, each with a doc comment that explains why.

```cpp
namespace gme {
struct PlayerCard {            // what other players see of someone
  std::string user_id, handle_name, friend_id, friend_message;
  int32_t level = 1; int64_t last_login_at = 0;
  FriendUnit unit;             // leader of the active deck, real stats
  int32_t sphere_1 = 0, sphere_2 = 0;   // ITEM ids (resolved from warehouse rows)
};
struct Relation { std::string other_id; int32_t state; int32_t favorite; int64_t created_at; };

Task<std::string> ensurePublicFriendId(Database, const std::string& userId);
Task<std::optional<std::string>> userIdForPublicId(Database, const std::string& digits);
Task<std::vector<Relation>> loadRelations(Database, const std::string& userId);
Task<std::map<std::string, PlayerCard>> loadPlayerCards(Database, const std::vector<std::string>& ids);
Task<std::vector<PlayerCard>> randomCandidates(Database, const std::string& me, int32_t limit);
enum class ApplyOutcome { Sent, AlreadyPending, AlreadyFriends, AutoAccepted, SelfFull, TargetFull, NotFound };
Task<ApplyOutcome> sendRequest(Database tx, const std::string& me, const std::string& other);
enum class AgreeOutcome { Accepted, NoRequest, SelfFull, TargetFull };
Task<AgreeOutcome> acceptRequest(Database tx, const std::string& me, const std::string& other);
Task<int32_t> refuseOrCancel(Database tx, const std::string& me, const std::vector<std::string>& ids);
Task<bool> removeRelation(Database tx, const std::string& me, const std::string& other);
Task<bool> setFavorite(Database, const std::string& me, const std::string& other, bool on);
Task<int32_t> friendCapacity(Database, const std::string& userId);  // level mst + bought slots, as recruitFriend computes
// The ONE builder for tojMy68W: NPC roster (type 1) + relations (1/2/3) + extra type-4 rows.
std::vector<::FriendInfo> socialListWithRelations(const std::vector<FriendRow>& npcRoster,
    const std::vector<Relation>&, const std::map<std::string, PlayerCard>&,
    const PlayerPeak&, const std::vector<PlayerCard>& searchRows);
}
```

**Leader unit for a PlayerCard.** Take the `user_decks` row with `user_id = X`, `deck_type = 1`, `deck_num = user_info.active_deck` and `member_type = 0`. member_type 0 is the leader, per the comment in `Common.hpp addDefaultDecks`. Then read that `user_units` row: unit_id, unit_lvl, base_*, ext_*, bb_*, sbb_*, unit_type_id, extra_passive_skill_id(2) and fe_skill_info. Resolve `eqip_item_frame_id` and `eqip_item_frame_id2` (warehouse instance ids, see the `kNoSecondSphereSlot` note in Common.hpp) to `item_id` through `user_warehouse_rows(instance_id, user_id)`. A value of -1 or 0 means no sphere. If the active-deck leader is missing, fall back to the player's highest-rarity, then highest-level unit. If there are no units at all, skip the player, because a half-built card is worse than none. Check the `deck_type` value against DeckEdit before relying on it.

**A real FriendInfo row** (`tojMy68W`): user_id is the internal id. Fill handle_name, `team_lv = level`, friend_type (1/2/3/4), `last_login_date = last_login_at`, unit_id, `unit_lv`, base_* and ext_* (add_* 0), skill ids and levels as strings, `unit_type_id`, element, `equipitem_id`/`equipitem_id2` (item ids, the second only when the unit is 7★ or above, matching the NPC rule), `friend_id` = the 9-digit public ID, friend_message, `favorite` (from this side's row), `priority = 1`, `deck_no = 0`, `jkldTrhL = 0`, `guild_id = 0`. For pending rows (types 2 and 3), `req_time` is `created_at` formatted UTC `"%Y-%m-%d %H:%M:%S"`. The client prints it raw after `FRIEND_REQUEST_TIME<br>` (@0x1666918) and sorts on it with `sortFriendListByReqTime`, so the string must sort lexically (AUTHORED format). `elapsed_agree_time` is 0 (meaning UNVERIFIED).

**A real ReinforcementInfo row** (`xZH6EIQ7`) carries the same unit data. `q9Arz8Jv` (friend_type) is 1 for a friend and 0 otherwise. `friend_point` and `normal_friend_point` follow the existing rule. `user_unit_id` comes from a **reserved synthetic block** per picker position, never the lender's real `user_unit_id`. Real ids are a global autoincrement, so they will eventually run into the 990000–991xxx NPC blocks. Recommendation: move every reserved helper block to `2'000'000'000 + i`, which still fits `i32::str`. That value is UNVERIFIED on device and has to be tested before switching.

### 4.1 FriendGet (`2o4axPIC`), extended

`xZH6EIQ7` (the picker), in order:
1. Real friends (state 1) with a resolvable leader, friend_type 1.
2. The existing NPC roster friends that are not stale, friend_type 1, unchanged.
3. Up to `kRealStrangerCount = 10` (AUTHORED) real non-friends, ordered by `last_login_at` descending, active within 7 days, excluding self, friends and anyone with a pending row. friend_type 0, the stranger Honor rate.
4. The existing developer encounter and NPC strangers, so a small server still offers a full board. Keep the total at 50 or fewer (AUTHORED; the client's cap is unknown).

`tojMy68W` is `socialListWithRelations(npcRoster, relations, cards, peak, {})`, which includes the type 2 and type 3 rows. That makes the footer badge (`getCountAgreeRec`) light at login. `6e4b7sQt` stays as it is. Ignore `StQIyohe`; ROADMAP's mode-param idea applies only to Ep3.

### 4.2 FriendApply (`WUNi08YL`): **never refuse** (§1.4)

For each `h7eY3sAK` in the request:
- Empty, or the caller's own id: skip.
- An NPC-shaped id (`DEV…` or `NEW…`) with no `user_info` row: the existing `gme::recruitFriend` instant path, unchanged.
- A real user_id (exists in `user_info`, not banned): call `sendRequest`, which handles each case in one transaction.
  - Already friends, or my side already state 2: no-op.
  - My side is state 3, meaning they already asked me: **auto-accept**, both sides to 1, if both have capacity. Otherwise leave it pending.
  - Otherwise insert (me, them, 2) and (them, me, 3) with `created_at = now`, unless my pending-outgoing count is 30 or more or their pending-incoming count is 50 or more (AUTHORED). In that case log and skip.
  - Do **not** block on the target's capacity here. Capacity is checked at Agree, which can refuse cleanly.
- Reply with `FriendSocialResp{ tojMy68W: full list, 6e4b7sQt }`. The new type 2 row appears straight away in the Requests screen. A failed DB write still answers success with `"{}"`, as the code does today.

### 4.3 FriendAgree (`NYKBH4V6`)

`acceptRequest(me, other)`, in one transaction. It requires my row to be state 3.
- No request found (cancelled, already handled, or stale): reply success with the refreshed list. The row just disappears.
- My friend count (state 1 plus NPC roster, priority ≤ 1) is at or above my capacity: `refuseToRetry("Your friend list is full.")`. The client also checks this first, so this is a backstop.
- Their friend count is at or above their capacity: `refuseToRetry("That Summoner's friend list is full.")`. Leave the request pending.
- Otherwise set both rows to state 1, `created_at = now`, and reply `FriendSocialResp`. Optionally add `xZH6EIQ7` from the same builder as 4.1 so the new friend can be borrowed before the next FriendGet. No picker is open in the Social menu, so replacing it there is safe.

### 4.4 FriendRefuse (`N5SQd7oA`)

The id set is `h7eY3sAK` plus `7f3WL6Mx` split on `,`, with empties dropped and duplicates removed, capped at 100. For each id where my row is state 3 (refuse incoming) or state 2 (cancel outgoing, which is what the delete button on a type 2 row does), delete both rows. Ids in any other state are ignored. This makes a stale RejectReq list harmless. The client only clears that list when a new reject-all starts (@0x166845C), so a later single refuse may resend old ids. Reply with `FriendSocialResp`. It never needs to refuse.

### 4.5 FriendGetAgree (`aVqJrR08`)

Reply with `FriendSocialResp`, the full list including types 2 and 3. Lazily expire pending rows older than 14 days first (AUTHORED). This must always succeed.

### 4.6 FriendDelete (`SfMN9w4p`): **never refuse**

A real id deletes both sides' rows, whatever their state. An NPC id uses the existing `gme::removeFriend`. **Change the reply from `{}` to `FriendSocialResp`.** OBSERVED: FriendDetailScene does not remove the row locally and nothing on that path calls FriendGet, so with `{}` the deleted friend stays in FriendList2Scene until the next login. The detail scene's `+0x338` FriendInfo pointer is dereferenced only on the guild path in `updateEvent` (@0x166B198), so the full replace looks safe. That is UNVERIFIED and is the first thing to test on device.

**The empty-list case.** If the new list has zero rows, a `[]` cannot clear the client. Send one sentinel row with `friend_type = 0`, `priority = 2`, user_id `"0"` and handle_name `""`. Every list and counter skips priority > 1 or type 0 (OBSERVED, §1.2). This is UNVERIFIED on device. If it misbehaves, accept that the last friend lingers until the next login. This happens when a player deletes all their friends: the NPC roster is seeded once per account (see §6, decision 2).

### 4.7 FriendSearch (`umNt8M7i`)

1. Trim the typed text. If it is not 1–12 ASCII digits, or the rate limit is hit, the result is "not found". It is not an error.
2. `userIdForPublicId(digits)`. **Found, and it is me:** append my own card as type 4. The client compares the row's user_id to its own and shows `FRIEND_SEARCH_MYID` (@0x16556EC). **Found, someone else:** append their card as type 4 even if they are already a friend. The client's first pass matches a type 1 row by `98WfKiyA` and disables the Request button (`friend_type != 1`, @0x1656808), so the type 4 row is simply ignored.
3. Reply `FriendSocialResp{ tojMy68W: socialList + [type-4 row if found] }`. If nothing is found, the client shows `FRIEND_SEARCH_NOT_FOUND` by itself (@0x1655C10), so no error is needed.
4. Omit `PAd9aS1H`. The client looks it up only in Ep3 mode, and a null lookup takes the classic card path (@0x165574C).
5. Only a rate-limit refusal may use `refuseToRetry("Too many searches. Try again in a few minutes.")`, which returns the player to FriendIdSearchScene. A friendly "not found" is preferred.

The type 4 row stays in FriendInfoList until the next full reply. The only type-agnostic reader is `FriendInfoList::exist` (MissionResultFriendRequestScene), so the worst case is that the "add as friend?" prompt is skipped for someone you searched for. That is acceptable.

### 4.8 FriendRandomSearch (`mE5gUQOp`): **never refuse**

`randomCandidates(me, min(limit, 20))` (AUTHORED cap) returns real players active in the last 14 days. Exclude self, friends, both pending directions and players with no units. Order by `abs(level - myLevel)` and then `last_login_at` descending, with a small per-day shuffle. Ignore `3Ug2QynF` (offset 0). When there are fewer than 5 real candidates, top up with NPC strangers from `strangerSuggestions`, using their `NEW…` ids, which FriendApply already resolves. Reply `FriendSocialResp{ tojMy68W: socialList + candidates as type 4 }`. The client drops any candidate that `typeCheck` flags (type 3 for the same id, @0x165C488). If `mode == 1` (Summoner scene), return the same thing and no `PAd9aS1H` (§7). A rate-limited request returns the social list with no candidates.

### 4.9 FriendFavorite (`ps6Vgx4R`)

Set `favorite = (5JbjC3Pp == 1)` on my row: `user_relations` for a real friend, `user_friends.favorite` for an NPC. Reply `FriendSocialResp`. An unknown id is not an error. Reply with the unchanged list.

### 4.10 Missions: helper Honor, lender credit, leader skill

- `gme::isFriend` (used by MissionEnd's Honor rate) must also return true for `user_relations` state 1. Otherwise a real friend's card promises "Honor +50" (client `existTypeOK`) and MissionEnd pays 10.
- `gme::helperLeaderUnit` must resolve a real helper id to that player's **current** leader unit_id through `PlayerCard`, and keep the NPC path otherwise. The Player EXP Boost leader-skill logic in Mission.cpp already calls it.
- Lender credit (P2, AUTHORED): when MissionEnd pays Honor for a **real** helper, also credit the lender `kHonorPerNormalHelper` (`friend_points` and `friend_p_get`), at most 300 a day per lender, tracked in new columns `user_info.lend_honor_day` and `lend_honor_today`. The lender sees it the next time their UserTeamInfo is sent.

### 4.11 Friend gifts (P2, cheap)

In `FixGiftInfo`, parse `qD74LPXb` (`toUserId:giftId` pairs). For each recipient who is a **real friend** (state 1), whose gift id is in the gift catalogue, and who has had no gift from me today, insert into `user_gifts` (user_id = recipient, from_user_id = me, gift_id, day = `giftToday()`). Gifts are not deducted from the sender (existing Gifts.hpp note). GetGiftInfo needs to resolve the sender name from `from_user_id` for real senders; today it is a fixed name. Fill `FriendInfo.want_gift` (`s2WnRw9N`) from the friend's `user_info.want_gift` so GiftSendScene can highlight what each friend wants. How the client learns "already sent today" (`UserState::getUserGiftInfoListSend`) has not been traced: UNVERIFIED.

## 5. Existing code to reuse

| Need | Reuse |
|---|---|
| Identity | `gme::getUserIdentity(theDb(), req.login_info)` (Common.hpp:1464). Never trust ids in the body as "me". |
| NPC roster, picker strangers, dev encounter | `loadFriendRoster`, `strangerSuggestions`, `devEncounterFor`, `recruitFriend`, `removeFriend`, `friendUnitFor`, `playerPeak`, `chainTopRarity` (Friends.hpp) |
| Social list builder | `gme::socialList` (Friends.cpp:~750). Extend it into `socialListWithRelations` and keep one builder (the full-replace rule). |
| Capacity | the formula inside `recruitFriend` (Friends.cpp:605–617): level mst `friend_count` + min(`max_friend_count`, `add_friend_count`). Extract it into `friendCapacity`. |
| Honor | FriendPoints.hpp: `friendPointInfo()`, `honorForMission`, `kHonorPer*` |
| Gift inbox | Gifts.hpp (`giftToday`, `user_gifts`), FixGiftInfo.cpp |
| Transactions | `co_await gme::newTransaction(theDb())` (Transactions.hpp: takes the write lock first, ticket #72), as Tutorial.cpp does |
| Refusals | `HandleResult::refuseToRetry` only where §1.4 says the scene handles it |
| Wire tests | `tools/smoke_test.py`, `tools/test_accounts.py` (guest login per fake device gives separate accounts and tokens) |

## 6. Decisions for the owner (defaults chosen so work can start)

1. **NPC friends stay** alongside real friends in the Social list and the picker (default). The alternative is to hide NPC friends once a player has at least N real friends.
2. **Seed once (changed in review)**: the NPC roster is seeded once per account, tracked by `user_info.friends_seeded` (migration `16102026_FriendRosterSeedOnce`), and only into free slots. Re-seeding an empty list gave a player who deleted their last NPC five new ones in the same reply, past their cap now that real friends share the slots. Deletions are permanent and an empty list uses the §4.6 sentinel.
3. **Instant mutual accept** when both players have requested each other (default on).
4. **Real strangers in the picker** (default on, 10, active within 7 days). This leaks only what the original game already showed: name, level and leader unit.
5. Limits (AUTHORED, adjustable): 30 outgoing pending, 50 incoming pending, 14-day expiry, the rate limits in §3.

## 7. Out of scope, and other clients of the friend graph

These are out of scope here: Summoner (Ep3) friend screens (`PAd9aS1H`, SummonerFriend*Scene, FriendGet reinType); ArenaFriendList (`xkd9H50w`), ColosseumFriendList (`AoBvG2YN`), MinigameFriendList (`Hjk84giw`), TierTourneyFriendList; RaidRoomFriendGet (`Xe2UB3A8`), RaidFriendRemove and GuildRaidFriendSelect; GuildMemberGuildInfo (`38bSeq81`, which the detail screens call); and block/report moderation. Each can reuse `loadRelations` and `PlayerCard` once this lands.

## 8. Test plan

Run the wire tests from `tools-local/` in the toolchain image. Each test uses two or three fake devices, which means two or three accounts.

1. A logs in. `98WfKiyA` is 9 digits and stays the same over two logins. Account B gets a different ID.
2. A searches for B's ID: one type 4 row with B's user_id, plus A's roster. A searches for their own ID: a type 4 row with A's user_id. A searches for `"000000001"`: no type 4 row and no error.
3. A applies to B. A's reply has B as type 2. B's FriendGet and FriendGetAgree have A as type 3, and the badge count is 1.
4. B refuses: both rows are gone. A applies again and B agrees: both see each other as type 1. B's FriendGet `xZH6EIQ7` contains A with `q9Arz8Jv = 1` and A's real leader unit and sphere item ids.
5. B favorites A: `5JbjC3Pp` is 1 in B's list only.
6. B deletes A: both sides lose the relation. The reply list does not contain A. With an empty roster, the reply has the sentinel row.
7. A applies to B while B has applied to A: both become friends at once.
8. Capacity: set B's friend count to capacity, then A applies (success) and B agrees, which gives `refuseToRetry` (GmeErrorCommand::Retry) and leaves the request pending.
9. RandomSearch: B and C appear as type 4 for A. Friends and pending players are excluded. With no other players, NPC `NEW…` strangers fill in.
10. MissionStart with helper = B, then MissionEnd: Honor at the friend rate when A and B are friends, the stranger rate otherwise. The lender credit (if built) goes to B.

On device: open Social, then Friend List, Add Friend (own ID shown, search, request), Requests (accept and refuse), Friend detail (favorite, delete), and the quest helper picker with the real friend at the top. Device testing needs a second account. Use a second emulator profile, or make the second account on the wire and play the first one on the device.

## 9. Implementation status (stage 2, 2026-10-16)

Landed: migration `16102026_FriendRelations` (user_info.friend_id plus user_relations), public IDs assigned lazily by `gme::ensurePublicFriendId` from `getLoginInfo`, the Relations module (`gme/common/Relations.{hpp,cpp}`), the six new handlers in `gme/handlers/FriendSocial.cpp` (GetAgree, Agree, Refuse, Search, RandomSearch, Favorite), FriendApply real requests with mutual auto-accept, two-sided FriendDelete with a full-list reply, FriendGet with pending rows in `tojMy68W`, real friends first in the picker and up to 10 recently active real strangers after the NPC roster, and `isFriend` / `helperLeaderUnit` / roster capacity aware of real friends. The search and random-search rate limits (20 and 30 per 10 minutes) are in-process.

Verified on the wire by `tools-local/test_friends.py` (three fake devices): every step of §8 except 8 (capacity refusal) and 10 (MissionEnd Honor), which have no wire test yet.

On device (emulator-5554, local-9961 APK, one device player plus a scripted second player from `tools-local/device_peer.py`): the login FriendGet lit the Social and Friend Request badges for an incoming request; Friend Request listed it with leader and date; Accept worked; Friend List showed the real friend on top (6/10); Add Friend showed the 9-digit ID; search by ID found a wire-made player and Friend Request sent it (state 2 in the DB); Search From List listed real players; Add/Remove Favorite worked; Remove Friend with the full-list reply did not crash and the list dropped to 5/10 at once (§4.6 confirmed). Opening the Requests screen did NOT send FriendGetAgree on this build; it rendered the cached list from the login FriendGet. `0CAQ6wUe` turned out to be seconds since last login (a unix time drew "20741 days ago"), so real cards now send elapsed seconds; NPC rows still send a timestamp and read "999 days ago". Not checked on device: the quest helper picker, Refuse/Reject All, and MissionEnd Honor.

Not done: friend gifts (§4.11), lender Honor credit (§4.10, P2), the 2,000,000,000+ helper block (real helpers use 992000+i), DeckEdit friend message, a ban check on targets, Summoner/Ep3 flows. CreateUser does not re-roll ids that start with DCF/DEV/NEW (§3). Real strangers in the picker are ordered by last login with no 7-day cutoff.
