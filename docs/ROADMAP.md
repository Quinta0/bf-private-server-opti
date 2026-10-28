# Brave Frontier community server roadmap

Last updated 2026-10-15. Generated from the area audits (raid, arena, social, monetization, other, production, transport) with `re/work/roadmap/gen.py`, which also checks the coverage table against `re/notes/missing_requests.json`. Evidence labels follow the audits: OBSERVED means read from the binary, the server source or a live test; INFERRED means reasoned but not yet confirmed on a device.

## Where we are

The server is Seltraeh's fork of decompfrontier/server (C++23, Drogon, SQLite, KDL packet codegen). It builds in Docker on Linux arm64 and runs at `http://127.0.0.1:9960`. A patched Android v2.19.6 APK (plain HTTP, boot host baked into libgame.so) gets through login, Initialize, asset download, tutorial skip, the home screen and missions, and most single-player systems have real handlers (see `server/docs/GAMEPLAY_STATUS_2026-10-03.md`).

The server is still built as a single-player save editor, though. Every guest login maps to one shared account: AccountController reuses the first `gumi_live_users` row, returns the literal token `test_token`, and no handler ever reads the `gumi_live_token` the client sends. Friends, helpers, rankings and guild members are simulated NPC data. 124 client request classes are not registered, and any unregistered id closes the client session (GmeErrorCommand::Close). The most urgent problem is a path traversal on the asset routes that serves the save database, config and source tree without authentication; `curl --path-as-is http://127.0.0.1:9960/content/../../config.docker.json` still returned 200 on 2026-10-15 (OBSERVED).

## Product rules (decided by the owner)

1. **No purchases, ever.** No real-money path is reachable or implemented. Gems are earned in play only, and every purchase request gets a refusal handler that never grants anything.
2. **Ads become one-tap rewards.** Wherever the game offered "watch a video", the player taps once and gets the reward. The server enforces daily caps and cooldowns so free rewards stay bounded.
3. **Real multiplayer.** Each player has their own account, and players see and interact with each other: friends and helpers, arena and colosseum, rankings, guilds, raids.
4. **Fully functional.** The target is every reachable screen working. Until a feature is built, it is gated or answers with a friendly message, never a session close.
5. **Tokyo first.** One server in Tokyo behind a domain, then more regions. Android first, then iOS, then desktop.
6. **Anti-cheat later.** With nothing for sale, cheating mostly affects leaderboards and co-op fairness. The server still validates rewards and scores (Phase 2 and 3) so that a later "play for real" or boosted-reward mode is possible.

## How to read the estimates

Effort sizes are focused work with this RE setup: S = under half a day (counted as 0.25), M = 0.5–2 days (1), L = 2–5 days (3), XL = more than 5 days (8). Priorities come from the area audits: P0 blocks the phase, P1 is needed for the phase's exit, P2 should land in the phase, P3 is optional or can slip.

| Phase | Goal | Backlog items | Missing request ids closed | Nominal agent-days |
|---|---|---|---|---|
| 1 | Multi-user foundation, hardening, HTTPS, Tokyo test server | 20 | 3 | 14.5 |
| 2 | Remove payments and ads, finish single-player gaps | 25 | 13 | 17 |
| 3 | Async multiplayer: friends, helpers, arena, colosseum, rankings, guilds | 41 | 31 | 38.5 |
| 4 | Raids and remaining P2/P3 features | 79 | 72 | 85.25 |
| 5 | More regions and CDN, iOS, desktop | 7 | 0 | 14.5 |
| — | Dead client code, never registered | — | 5 | 0 |
| **Total** | | | **124** | **169.75** |

**Honest overall estimate.** The nominal sum is 169.75 agent-days. Not one response shape for arena, raid, colosseum or transfer has ever been captured from the live game; each one is rebuilt from `*Response::readParam` key sets, so most screens will need several rounds of on-device testing. Plan for 30–50% on top, which gives about 220–255 agent-days for everything. That figure also needs a human with a phone for every QA round, and it does not include the separate gameplay-fidelity track in `server/docs/GAMEPLAY_WORK_QUEUE.md` (boss AI, trials). The first public-ready milestone is the end of Phase 2: 14.5 + 17 nominal days, realistically 40–45 agent-days. Phases 3 and 4 can partly run in parallel once the Phase 3 friend graph exists.

## Phase 1: multi-user foundation, hardening, HTTPS, first server

**Goal.** A small closed group (5–20 testers) plays on a Tokyo server over HTTPS, each with their own account, without the server exposing data or falling over.

The work starts with the two P0 security problems. The path traversal is a small fix. Real identity is the foundation for everything after it: a guest account per device, a server-minted token checked on every request, a unique transfer ID, and a unique public friend ID. Guest login runs on every boot and is keyed by the device id stored in app data (INFERRED from the boot spec), so a reinstall loses the account. The client already has Transfer Code screens for recovery, which is why the three transfer requests are in this phase rather than later. Hardening turns the debug build into a service: quiet logs, a slim non-root image, a reverse proxy with rate limits, safer SQLite settings and tested backups.

For HTTPS, the transport audit recommends native HTTPS with a replaced trust anchor (OBSERVED: the client's curl verifies peer and host against exactly one embedded root, Go Daddy Class 2, in a 1445-byte PEM slot at 0x2318f2f that nothing else references). That slot fits a private ECDSA root or ISRG Root X2, but not ISRG Root X1. Assets stay on plain HTTP through the Java loader, which is acceptable for public files. This has not been tested on a device yet. Plain HTTP for the API is not acceptable once accounts are real, because the GME AES keys are static and anyone on shared Wi-Fi could take over a session.

**Exit criteria.**
- The traversal regression test passes, and nothing under `/src` or the DB is reachable over HTTP.
- Two devices get two separate accounts; a request with a wrong or missing token is rejected.
- A tester can reinstall and get their account back with a transfer code.
- The HTTPS build works on at least two Android versions, and the API is not reachable over plain HTTP.
- Backups run hourly and one restore has been rehearsed.
- One week with the test group with no data loss and no session closes from the fallback-covered paths.

### Security and identity (6 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | BfWebController::handle (asset routes) | Close the path traversal: resolve `weakly_canonical(root/rel)` and reject anything outside content/ and mst/; reject `..`, `%2e`, `\\` and NUL; compile out ENABLE_FILE_GENERATOR; regression test. OBSERVED: `curl --path-as-is /content/../../config.docker.json` still returned 200 on 2026-10-15. | S | P0 |
| — | AccountController::HandleGuest, gme::getUserIdentity | Per-device guest accounts. Key gumi_live_users by device_id; mint a 128-bit CSPRNG token, store sha256, rotate on login; check it in getUserIdentity on every GME request (today iN7buP1i is never read). UNIQUE(user_info.gumi_user_id). Replace mt19937 RandomId with CSPRNG plus retry. Two-device wire test with bf_testkit.py. | M | P0 |
| — | Initialize login_info.account_id, UserInfo 98WfKiyA | Unique per-user account_id (the transfer ID; Initialize.cpp:28 hardcodes "12345678") and an immutable 9-digit public friend_id. Migrate the existing single save. | S | P0 |
| `LK9e0WyX` | TransferCodeIssueRequest | Issue a transfer password: store an argon2id/bcrypt hash with expiry, return LZ2PfNm4/4WSu1irc/iyJH5k6p in UserInfo (fields already in user.kdl). | S | P0 |
| `Fn1UzTo3` | TransferCodeCheckRequest | Verify account_id + password in constant time with lockout (about 5 failures per hour per account and IP); return u3JU7LzQ ModelChangeUserInfo for the confirm screen. Needs a new KDL schema. | M | P0 |
| `I9DyF2aU` | TransferRequest | One transaction: re-verify, mark code used, rebind the device's gumi user to the target user_id, bump model_change_cnt, revoke the old token. Tests for replay, expiry, concurrency. | L | P0 |
| — | CreateUser, UserSummonerInfoEdit, NgwordCheck (TA4MnZX8) | Handle-name validation (length, character classes) and a real ng-word filter; NgwordCheck currently accepts every name. | S | P1 |

### Session safety (0.5 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | GmeController::Handle | Graceful fallback: unregistered group ids and plain handler errors answer refuseToHome/refuseToRetry with a 'not available on this server yet' message instead of GmeErrorCommand::Close. INFERRED safe for menu screens; must be verified per path on device, and boot-time requests (ArenaRetire in InitializeScene) need real stubs instead. | S | P1 |
| — | UnitOmniEvo (4Dk4spf9) | Return refuseToRetry instead of error() so the Omni+ Boost button stops closing the session (OBSERVED: error() maps to Close). | S | P1 |

### Production hardening (4.5 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | GimuServer logging, deploy/config*.json | INFO log level, extra_log off; if dumps stay, allowlist by user, sanitize reqId, redact tokens, add ms+counter to names; logs to stdout; localtime_r. | S | P1 |
| — | docker/ (Release image) | Multi-stage image: RelWithDebInfo, GIMU_ENABLE_DEBUG_CLI=OFF, slim runtime, non-root, read-only rootfs and assets, DB on a named volume, never mount /src; /healthz running SELECT 1; enable_session false; body size cap. | M | P1 |
| — | Reverse proxy (Caddy or nginx) | TLS termination, static /content and /mst, per-IP and per-user rate limits (guest/login, action.php), new-account cap per IP; build the content index once at boot (no rescan on 404). | M | P1 |
| — | SQLite (DatabaseInterface, MigrationManager) | PRAGMA WAL, synchronous=NORMAL, busy_timeout=5000, foreign_keys=ON; wrap each migration in BEGIN/COMMIT; user_id indexes on hot tables; hourly online backup to offsite storage and a tested restore. | M | P1 |
| — | GameClock and day-key helpers | One configured game day per server (JST or UTC); route Gifts, Achievements, MysteryChest, PermitPlace, Present day keys through GameClock; pin TZ in the container. | S | P2 |
| — | Admin tooling (replaces Windows-only DebugCli) | Localhost-bound admin CLI: find player by user_id/handle/account_id, grant/revoke, ban (banned_until), move a save between gumi ids, broadcast via NoticeList; append-only admin_audit table. | M | P1 |

### HTTPS, client build and deployment (3.5 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | tools/patch_apk.py --https | Keep the discovery-host patch, skip the 7 http-downgrade patches, overwrite the 1445-byte PEM slot at 0x2318f2f (NUL-padded) with the chosen root: private ECDSA P-256 root or ISRG Root X2 (ISRG Root X1 is 1939 bytes and does not fit). Leaf cert served by the proxy, TLS 1.2 (bundled OpenSSL 1.1.0c). | M | P0 |
| — | GuestLogin servers.live_api_server, discovery host | Return https://<domain> as live_api_server so HOST1 is not `https://host:80`; pick a discovery hostname of at most 19 characters on port 443. | S | P1 |
| — | Community APK build | Rename the package (manifest, four provider authorities, C2D_MESSAGE); new offline release key, v1+v2+v3 signing; server accepts the new GCLIENTID; keep signing/ out of the AGPL source. | M | P1 |
| — | AppVersionupScene + /dls force flag | Patch the version-up URL (at most 46 bytes at 0x23690c1, length immediate at 0x1b5af00) to the download page; serve force="1" to outdated builds; publish a release feed. | S | P1 |
| — | Tokyo deployment | Provision the VM, DNS, firewall (80/443 only), compose with restart policy, backups, uptime check; onboard a closed group of 5-20 testers with a bug-report channel. | M | P0 |


## Phase 2: remove payments and ads, finish single-player gaps

**Goal.** The client never shows a way to pay, every "watch an ad" button becomes a capped one-tap reward, and the reachable single-player screens that still close the session work.

No real-money purchase can start today, but only because the server never sends a product catalogue (OBSERVED: PurchaseMst `vW75Pgpw`, bundle categories `7itHlDro`, `MultiGachaIapMst` `3b6aDakz` and the subscription info are never emitted). This phase makes that permanent: refusal handlers for the four purchase requests, `ret` patches on the client's payment entry points, a startup assertion that fails if any catalogue key is ever emitted, and removal of the third-party ad and analytics SDKs. Ads are hidden today because the IronSource SDK never initialises. A small libgame patch makes the mediator report "ready" and fire the completion callback immediately, after which the server's VideoAdComplete handler grants the reward. The order matters: turning on `video_ads` before `r234ydi1` is registered makes the home screen send an unregistered request on a timer and close the session (OBSERVED path in HomeScene2::updateEvent).

On the single-player side, the Trial Zone and Battle Hall squad screens and Summoner boost items close the session today, a wrong leader skill on archive-granted units affects every PvP snapshot, and MissionEnd still trusts client-reported zel, karma and drops. Fixing MissionEnd here, before any leaderboard exists, is cheaper than fixing it afterwards.

**Exit criteria.**
- No purchase UI is reachable, the client contacts no store or ad SDK, and the startup assertion is in CI.
- Free energy, the extra daily spin and the video slot machine each give their reward in one tap and stop at the daily cap.
- Trial Zone, Battle Hall and boost items work from a fresh account.
- MissionEnd rejects rewards outside the run's server-rolled table.

### Remove payments (3 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `9o3q7SjN` | BuyCoinRequest | Refusal handler (refuseToHome, never grants, logs the attempt). Covers stray receipt restarts. | S | P1 |
| `kJs6CQq1` | BuyCoinStartRequest | Refusal handler; unreachable while PurchaseMst (vW75Pgpw) is never sent. | S | P1 |
| `kD298bt6` | BundlePurchaseIapRequest | Refusal handler; never send a BundlePacksInfo row with a product id (wXTxs50z). | S | P1 |
| `jk17biw9` | MultiGachaIapPurchaseIapRequest | Refusal handler; never send MultiGachaIapMst (3b6aDakz). | S | P1 |
| `u06y2UE4` | SetPurchaseInfoRequest | Benign success echoing 4W6EhXLS (age entry for the spending-cap dialog). | S | P3 |
| `swarOb4u` | BundleCategoryRefreshRequest | Benign `{}` stub; only fires when a bundle category expires, and none are sent. | S | P3 |
| — | libgame.so IAP neutralization, features.json | `ret` at PaymentUtil::paymentRequest (0x1ab502c), syncItemPricesAndPurchases (0x1ab50f4), storeOpening (0x1ab4f3c), GumiLiveManager::marketGoogle (0xf68028); set sg_target_bundle_flag=0 and keep supersonic/amazon/arx flags 0. | S | P1 |
| — | Server startup assertion | Fail startup if any response or cache would emit vW75Pgpw, R5E9odPZ, ABg6Lr3p, 7itHlDro, K72eC4nz, yT8fs6jL, 3b6aDakz or sT4ph0fe. | S | P1 |
| — | Third-party SDK removal | Strip or stub IronSource, Tapjoy, AppsFlyer, Facebook, Firebase/Crashlytics and Vungle initialisers (manifest providers + init calls) so the build sends no telemetry under Gumi's keys. | M | P2 |

### One-tap rewards instead of ads (6.75 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | MarketingMediator (libgame.so patch) | isMediatorInitialised -> `mov w0,#1`; isAdAvailable -> true; invokeAd stores id/scene and tail-calls adInvokeCompletedCallback (0xee351c) so completeViewVideoAds fires instantly; invokeOfferWall -> ret; optionally setupIornSourceSdk -> ret. | S | P1 |
| — | VideoAdInfo rows (j129kD6r), features.json video_ads | Rows for id 1 (energy) and 2000 (daily spin), 998 disabled, 999 only with slots; region 0; oohpPLCt as seconds-left. Ship in the same release as VideoAdComplete. | S | P1 |
| `r234ydi1` | VideoAdCompleteRequest | Server-authoritative free reward with per-day cap and cooldown (user_video_ads table): id 1 refills energy (reuse ShopUse restoreEnergy), id 999 bumps slot stand info. Required before video_ads=1, or HomeScene2's timer closes the session. | M | P1 |
| — | DailyLogin (4aClzokO), DailySpin.hpp | Raise kDailySpinLimit to 2 (free spin + one-tap 'ad' spin, button 1102 -> invokeAd(2000)); guaranteed gem stays on the first spin. | S | P2 |
| `AjKpMC9a` | VideoAdsSlotActionRequest | Video-slot spin: reuse the BraveSlots prize roll with an authored prize table (gem shards, small items) and a daily cap (max_ads_count). | L | P2 |
| `jwgJVpUP` | VideoAdSlotsStandInfoRefreshRequest | Return stand info (ads_count, max, bonus, next-day timer) with day rollover. | S | P2 |
| `ifL3TZim` | VideoAdSlotsCompleteRequest | Benign `{}` stub; OBSERVED no constructor call in full.s. | S | P3 |
| — | UserGemShardInfo (29slks49) | Return real s9d3lksS rows from a new gem_shard column instead of `{}`. | S | P2 |
| — | VideoAdSlotsClaimBonus (29s22s49) | Guarded shard-to-gem conversion at an authored threshold instead of `{}`. | S | P2 |
| — | Gem economy config | Move gem grants from constexpr to a config economy block; add daily-task gem, per-area first-clear gems; server-side only. Basis for a later 'boosted rewards' flag. | M | P1 |

### Single-player gaps (P0/P1) (7.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `gY63Zvy7` | TrialDeckGetRequest | user_trial_decks table, PnjQ49wK rows (same keys as DeckEdit rows), seed Trial Zone decks 0-2 from normal decks, also send PnjQ49wK in UserInfo. Today the first Trial Zone/Battle Hall entry closes the session (INFERRED; request path OBSERVED). | M | P1 |
| `MbdL5D4K` | TrialDeckEditRequest | Clone DeckEdit's transactional updateDecks for trial deck numbers, with ownership checks. | S | P1 |
| `4eCLR4rq` | ItemUseTimeLimitRequest | Debit boost items, extend user_item_time_limits, return YmaEqtA8 (also in UserInfo), apply the Summoner EXP multiplier in MissionEnd. Queued via ConnectRequestList, so today it closes the session on a later screen. | M | P2 |
| — | Leader skill on archive-granted units | Fix leader_skill_id=0 stored for archive-granted units (GAMEPLAY_STATUS_2026-10-03 item 2); arena, colosseum and campaign paths read the stored value. Prerequisite for PvP snapshots. | M | P1 |
| — | CampaignReceipt (5Imq3wC0) | OBSERVED mismatch: the client's only caller (CampaignCodeRequestScene) sends a serial code in 4MCxgS5p/pCIRMw04, but the handler models a Grand Mission claim (j28VNcUW). Confirm which flow the Grand Mission claim really uses and split them. | M | P2 |
| — | MissionEnd validation (Mission.cpp) | Store the server-rolled drop table per run at MissionStart; at MissionEnd accept only a subset, cap zel/karma (today req.battle_result.zel is added unbounded), clamp trophy counters, minimum run time, missions-per-hour limit, anomaly log. | L | P1 |


## Phase 3: async multiplayer

**Goal.** Players find each other, become friends, borrow each other's real units as helpers, fight each other's saved teams, and see real rankings and guilds.

None of the PvP modes in v2.19.6 is real-time: in every arena variant the client simulates the fight against a snapshot of the opponent's team and reports the result (OBSERVED: `RivalParty::entry` parses the opponent as NPC `P_*` strings; `ArenaBattleEndRequest::createBody` sends the client-computed result). So "multiplayer" here means server-held snapshots, matchmaking, seasons and leaderboards over ordinary HTTP. The friend graph comes first because helpers, friend lists, guild invites and the friends tabs of every ranking depend on it. Classic Arena unlocks Colosseum (`isColosseumOpen` checks the arena max rank). Challenge Arena is the largest piece because all its server master data has to be authored; nothing for it exists in any dump. Each mode's requests must ship together: `ArenaRetire` runs during InitializeScene and `ColosseumDeckEdit` is sent automatically, so a half-shipped mode can block login or close the session.

**Exit criteria.**
- Two real players can find, friend, accept or refuse, favorite and unfriend each other, and borrow each other's leader unit with Honor credited to the lender.
- Guild invites and applications between real players work, and the master can hand over without the guild dissolving.
- The Frontier Gate and Frontier Hunter boards show real players.
- Classic Arena, Colosseum and Challenge Arena each complete a full season cycle including rewards, with bots filling empty brackets.

### Friends and helpers (9 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Friend graph foundation (FriendGet/Apply/Delete) | user_relations with states 1 friend / 2 sent / 3 received; FriendApply creates a pending request instead of auto-accepting; two-sided delete; friend cap; FriendInfo and ReinforcementInfo rows built from the other player's real leader unit and spheres; lender Honor credit; NPC friends in a separate id namespace; identity from the session only. | L | P0 |
| `aVqJrR08` | FriendGetAgreeRequest | Pending list: tojMy68W rows with friend_type 2/3 and req_time; include established friends to avoid wiping the shared cache. | M | P0 |
| `NYKBH4V6` | FriendAgreeRequest | Accept (BPz1e5tU/h7eY3sAK): flip both sides to friend in one transaction; always reply success with the roster. | S | P0 |
| `N5SQd7oA` | FriendRefuseRequest | Refuse one or many (7f3WL6Mx comma list); delete pending_in rows; reply with refreshed pending list. | S | P0 |
| `umNt8M7i` | FriendSearchRequest | Search by public friend_id (98WfKiyA); return one FriendInfo row plus PAd9aS1H; rate-limited; design the not-found path (refuseToRetry). | M | P0 |
| `mE5gUQOp` | FriendRandomSearchRequest | Up to 20 recently active real players near the caller's level, excluding self/friends/pending/blocked; PAd9aS1H when the mode param is 1. | M | P0 |
| `ps6Vgx4R` | FriendFavoriteRequest | Per-side favorite toggle; can ship early against the synthetic table. | S | P1 |
| `38bSeq81` | GuildMemberGuildInfoRequest | Return 8bD18LPe guildInfoBlock for the target player's guild; check the no-guild path for a null deref. | S | P1 |
| — | Friend gifts (GetGiftInfo, FixGiftInfo) | Real daily gifting between friends instead of a simulated friend's drop. | M | P2 |
| — | Moderation | Report and block, friend-message and name filtering, search and random-search rate limits, admin review queue. | M | P1 |

### Guild completion (3 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Guild.cpp (registered stubs and simulated paths) | Real invites via GuildInviteManage + InboxMessageManage, applications (GuildJoin type 1), GuildSearch, master hand-over without dissolving, GuildUpdate, guild board (J93ki3Bw/X89bDai1), BCP contribution in GuildRanking. | L | P1 |

### Rankings (1.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `26zW90oG` | FrontierGateRankingRequest | Real board from user_frontier_gates.score: top 100 plus self, friends tab, indexes on (frogate_id, score). Frontier Gate is playable, so this button closes the session today. | M | P1 |
| — | ChallengeRanking (2Kxi7rIB) | Replace the five synthetic rivals with the same real-board query. | S | P2 |

### Classic Arena (async PvP against saved decks) (6.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Arena foundation | Trace NpcUnitParse/P_* keys (spheres, ES, omni, FE) and build one opponent-snapshot serializer; arena_snapshot/arena_battle tables with server battle_id and single settlement; bot fill from npc_mst. | M | P1 |
| — | Season and reset scheduler | Idempotent daily/weekly/season jobs (arena points, Colosseum seasons, Challenge Arena cooling and weekly reset, ChallengeArenaResetInfo real values). | M | P1 |
| `zu6ifpy0` | ArenaStartRequest | user_arena table; return one-row 8jBJ7uKR, PQ56vbkI and orbs; reuse the builder in ArenaInfo, UserInfo, GetPlayerInfo. | S | P1 |
| `82ztj6Iy` | ArenaMatchingRequest | About 3 opponents within a points band, snapshotted at match time; bots fill gaps. | M | P1 |
| `7f1rg92L` | ArenaBattleStartRequest | Validate offered opponent, spend an orb, create arena_battle, serialize both sides as P_* strings. | M | P1 |
| `oQNtKU51` | ArenaBattleEndRequest | Validate battle_id/owner/duration once; Elo-like points, rank-up rewards from arena_rank_mst, archive and daily tasks, defender counters. | M | P1 |
| `eyg8sA32` | ArenaRetireRequest | Settle the open battle; must ship with BattleStart because it runs during InitializeScene. | S | P1 |
| `NPyXDgV9` | ArenaBattleRestartRequest | Return the stored snapshot without spending an orb. | S | P1 |
| `xkd9H50w` | ArenaFriendListRequest | Friends' standings (BYaF62TE), shared row builder with ColosseumFriendList. | S | P2 |
| — | ArenaInfo (oim9TU1D) and gates | Fill ArenaInfo with 8jBJ7uKR/PQ56vbkI/FfBebG0R/HgYvs3am/Q5jXpri7; persist arena rows instead of zeroedArenaInfo; lift FeatureGates id 5; enable arena daily tasks. | S | P1 |

### Colosseum (7.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `rdZbaKBG` | ColosseumDeckEditRequest | Per-class atk/def decks with formation/support and cost cap; these are the defense snapshots. Auto-sent by ColosseumTopScene. | M | P2 |
| `ysP6qYcy` | ColosseumBattleStartRequest | Same-class opponent, snapshot both decks, spend a ticket. | M | P2 |
| `fGDPoQI3` | ColosseumBattleEndRequest | Per-stage CBP, all-win bonus, per-season standings and archive. | M | P2 |
| `ohH9lfDE` | ColosseumBattleRestartRequest | Return the stored snapshot; set battle num only while a battle is open. | S | P2 |
| `AoBvG2YN` | ColosseumFriendListRequest | Friends' standings per season and class, past rankings, duration list. | M | P2 |
| `jv0dI88o` | ColosseumGetRewardInfoRequest | Authored stage/category/reward lists with receive status. | M | P2 |
| `PN5CVaWy` | ColosseumRewardReceiptRequest | Transactional CBP spend and grant. | M | P2 |
| — | Colosseum data and ShopUse type 8 | Author seasons, CBP economy, reward tables (none exist in any dump); ticket refill (ShopUse type 8); send UserReleaseInfo 8. | M | P2 |

### Challenge Arena (11.75 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `z44LyLr2` | ChallengeArenaStartRequest | Ladder of opponents from other players' CA squads plus chest nodes; run state with HP/BB carry-over; KDL for about 15 response groups. | L | P2 |
| `rbOMSnba` | ChallengeArenaBattleStartRequest | Record the squad sent, validate HP, issue a battle token. | M | P2 |
| `syX20UG8` | ChallengeArenaBattleEndRequest | HP carry-over, points, coins, rewards, promotion, refreshed ladder. | L | P2 |
| `sXXX6Ec7` | ChallengeArenaTeamEditRequest | Swap/heal/revive/reorder; potions or free gems. | M | P2 |
| `q\x1b1\x02` | ChallengeArenaRankingRequest | Register all six real ids (eEmfIyG6/8roLLok3, L5LN9ytu/6gTFuTF9, Yc8M04Zm/j7Y8EHj4, RXOBSG1p/F8r7mIeQ, sCbqVnTn/6yc1ruKN, MfNpV6ZH/kjnUkn2R) to one handler; the JSON entry is a mis-read pointer table. | M | P2 |
| `tct1Jogs` | ChallengeArenaRankingRewardRequest | Static versioned reward table. | S | P3 |
| `xmzmjcxQ` | ChallengeArenaTChestReceivedRequest | Validate the current chest node, grant, remove it. | S | P3 |
| `xfmNcJVl` | ChallengeArenaShopInfoRequest | Authored rainbow-coin catalogue plus per-user stock. | S | P3 |
| `jtzuog2S` | ChallengeArenaShopAddItemRequest | Transactional coin spend; never a real-money path. | M | P3 |
| — | Challenge Arena data, flags | Author CA MSTs (potions, leagues, PvMatching tiers, gem costs, shop, chest loot, ranking rewards); then challenge_arena=1 and lift gate 15. | M | P2 |


## Phase 4: raids and remaining P2/P3 features

**Goal.** Co-op raids work for 1–4 players, and the remaining modes, events and shops are either built or deliberately gated.

**Decision: classic Raid is async over HTTP, Guild Raid is realtime.** The raid audit found that classic Raid never touches Pusher (OBSERVED: every PusherInterface caller belongs to guild raid or SG chat). Classic Raid lobbies and fields poll over HTTP every 4 seconds (`RaidWorldMst` interval `Rqc8pi0x="4"`), and shared boss HP is synced once per battle turn through `RaidUpdateProgress`. That fits the existing Drogon stack: a RaidService keeps room state in memory under per-room locks, persists to SQLite, and computes boss movement, regen and camp healing lazily from timestamps, so no tick thread is needed. A solo player is a room of one through `RaidQuickStart`. Guild Raid is the only mode that genuinely needs realtime. It uses the Pusher protocol through a Java bridge, so it needs a self-hosted Pusher-compatible server (for example Soketi), the presence-auth endpoint, and a smali patch. Build classic Raid first and treat Guild Raid as a separate epic that can be cut.

The rest of the phase is the long tail: Vortex Arena (hidden until its master data is sent), the Frontier Hunter event calendar and rank rewards, the Randall minigames (reachable today), SP Challenge (no master data dumped; keep hidden unless content is authored), summon variants, the remaining gem sinks, and optional promo features.

**Exit criteria.**
- Four players can create, join, fight and clear a raid with shared boss HP; a solo quick start works; disconnects time out cleanly.
- The Home Raid padlock (`FeatureGates {14,"Raid"}`) is lifted.
- Every reachable screen in the coverage table either works or answers with a message (no session closes). Guild Raid, SP Challenge and optional promos are either done or explicitly gated.

### Classic Raid (async HTTP co-op, 1-4 players) (32 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `jJZ01VQo` | RaidUserInfoRequest | RaidService skeleton, raid_* schema, raid_user row, rename RaidUserInfo KDL fields (RC, Agreement, LoginWorldID, LoginRoomID, OpenFlg, ...), shared builder with UserInfo. | M | P1 |
| `H7GRp01L` | RaidWorloInfoRequest | One row per RaidWorldMst with crowding from active rooms. | S | P1 |
| `Cu94xU8j` | RaidAgreementRequest | Set agreement=1, return RaidUserInfo. | S | P1 |
| `YF9UQpn6` | RaidRoomListRequest | Filtered room search; shared RaidRoomInfo serializer; confirm the ambiguous key->setter pairing by hand first. | M | P1 |
| `52H0dkjv` | RaidUpDataRoomInfoRequest | Create/edit room; RaidError (command 7) for invalid edits. | M | P1 |
| `yZReG75i` | RaidEnterRequest | Atomic join with capacity, password, kick and blacklist checks; party snapshot. | M | P1 |
| `jqi71nWu` | RaidExitRequest | Leave, promote master or dissolve. | S | P1 |
| `0VxE6kTW` | RaidRoomInfoRequest | Lobby heartbeat every 4 s: snapshot under the room mutex, presence timeout, chat delta. | L | P1 |
| `vmdA2GD8` | RaidUpDataUserReadyRequest | Ready toggle. | S | P1 |
| `H8N1R0TE` | RaidRoomDissolveRequest | Master-only dissolve; also stale-room cleanup on re-entry. | S | P1 |
| `Xbgen5J8` | RaidRoomKickRequest | Master kick with re-entry block. | S | P2 |
| `Xe2UB3A8` | RaidRoomFriendGetRequest | Reuse the FriendGet helper builder under ft4zY6Uv. | S | P1 |
| `u59zdKCw` | RaidFrlendSelectRequest | Store chosen helper; re-read the unit from the DB. | S | P1 |
| `u59zdKCi` | RaidFriendRemoveRequest | Clear helper (EMPTY_URL is harmless). | S | P2 |
| `CWeTY8BQ` | RaidQuickStartRequest | Solo/quick room of one with mission and helper; the solo path. | M | P1 |
| `8iQL3IKA` | RaidMissionSelectRequest | Master picks quest; RC check. | S | P1 |
| `2qDW9NBU` | RaidMissionStartRequest | Create room-mission, boss states, per-member state; deduct aube. | M | P1 |
| `BqF2x3EC` | RaidMissionInfoRequest | Field heartbeat: lazy boss movement/regen from RaidBossMst and routes, timeout, items, chat. | L | P1 |
| `Argz1U6B` | RaidMissionInfoLightRequest | Light variant of the same builder. | S | P1 |
| `8UiQj0uF` | RaidMissionBattleStartRequest | Mission serial via MissionRuns; live shared boss HP; weighted battle group (L if the client expects monster data). | M | P1 |
| `7geDL5Vb` | RaidUpdateProgressRequest | Per-turn shared HP sync; clamp implausible damage. | M | P1 |
| `5Ko2BjPA` | RaidMissionBattleEndRequest | Final damage, boss defeat for the room, persistent party HP, drops into raid_bag, EXP. | L | P1 |
| `La24KGAp` | RaidMissionBattleRestartRequest | Resume snapshot (or set raid_battle_restart=0 to defer). | M | P2 |
| `vgin9Mf1` | RaidMissionEndRequest | Once-per-member claim: clear check, Zel/Karma/FP, RC-up, raid medals (BraveSlots source), bag to warehouse, archive. | L | P1 |
| `a12v0RcA` | RaidMissionExitRequest | Retire without rewards; close when all retired. | S | P1 |
| `9QdRVwB1` | RaidMissionHarvestRequest | Point drop tables, shared depletion. | M | P1 |
| `I7nVUv0h` | RaidCampRestStartRequest | Camp start on camp points. | S | P2 |
| `diIe8ux7` | RaidCampRestGetRequest | Lazy heal (tuned rate). | S | P2 |
| `f9Dh4djx` | RaidCampRestEndRequest | Persist healed HP. | S | P2 |
| `o03kCtb1` | RaidMissionUseItemRequest | Field consumables applied server-side. | M | P2 |
| `SaN7v13E` | RaidLimitedUseItemRequest | Shares the UseItem handler. | S | P2 |
| `DZLI83hx` | RaidItemEditRequest | Port ItemEdit conservation to raid_bag/raid_equip. | M | P2 |
| `bv9xt3m6` | RaidRoonItemEditRequest | Pre-mission loadout from the warehouse; fires automatically, must not fail. | S | P2 |
| `K17JUYR2` | RaidItemMixRequest | ItemMix recipes against raid_bag. | M | P2 |
| `N43JmIf2` | RaidItemDeleteRequest | Discard from the raid bag. | S | P3 |
| `4zJ0WmTX` | RaidShopUseRequest | Aube refill/revive with earned gems (or free per policy); reuse ShopUse. | S | P2 |
| `XTban70i` | RaidUpDateCashRequest | Re-snapshot party after deck edit; ship with Enter. | S | P1 |
| `k5pMN68B` | RaidGetChatLogRequest | Room chat delta (dungeon-event user tag). | S | P1 |
| `dYIy6Eu8` | RaidSendChatRequest | Insert with filter and rate limit; RaidChatTempMst missing. | S | P2 |
| `4igfcyIm` | RaidUpdateBlackListRequest | Blacklist add/remove honoured by list/enter. | S | P3 |
| `81zkMjfN` | RaidGetPlayerInfoRequest | Serialize raid_archive counters. | S | P3 |
| — | Raid enablement | Load or author missing RaidChatTempMst/RaidTargetMst/RaidMissionElementTermMst if scenes index them; lift FeatureGates {14,'Raid'} only after the critical path works; per-screen device QA. | M | P1 |

### Guild Raid (realtime via Pusher protocol) (30.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Pusher-compatible server | Self-host Soketi (or similar), implement /bf/pusher/pres-auth.php HMAC presence auth, patch the Java BraveFrontierPusher host/key (smali + dex rebuild). | XL | P3 |
| — | Guild Raid core (22 registered GuildUnimplemented ids) | Rooms, outposts, deploy, battle start/end/progress, logs, ranking; author guild-raid MSTs (none dumped); shared boss state with optimistic concurrency. | XL | P3 |
| `jVRl14IS` | SGChatLogInfoListRequest | chat_messages table and backlog response (em4noAR5, N3d1supA); persist from a Soketi webhook. | M | P3 |
| `5710Dkla` | GRGuardianDetail | Guardian status poll from shared per-guild boss state. | L | P3 |
| `8b6DKiqz` | GuilldBattleMoveRequest | Map movement validated against route MST adjacency. | L | P3 |
| `dsRW32K` | GuildQuestClaimRequest | Already registered (GuildUnimplemented, GmeControllerHandlers.cpp:146); real claims need authored GuildQuestMst/GuildRewardQuestMst. | L | P3 |
| `fd54Sey` | GuildRaidQuestObjectiveRequest | Already registered (GuildUnimplemented, line 159); keep the stub until the guild-quest work above. | S | P3 |
| `7umuchUg` | FGPlusGuildRankingRequest | Frontier Gate+ guild ranking; no FG+ guild MST exists, must be authored. | L | P3 |
| `Sp7umiaY` | FGPlusGuildRewardClaimRequest | Idempotent FG+ guild reward via present box. | M | P3 |

### Vortex Arena (5.25 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Vortex data and entry | Author VortexArenaMst/rank/reward/daily/fixed settings; trace which response carries d2b8d79J so the tile appears. | M | P3 |
| `lp3a7bvD` | VortexArenaTopRequest | Season standing and MSTs. | M | P3 |
| `b5TunM93` | VortexArenaMatchingRequest | Reuse classic matching scoped to the season and constraint. | S | P3 |
| `j3s0iNM1` | VortexArenaBattleStartRequest | Reuse arena BattleStart with V-orbs. | S | P3 |
| `89HrE13b` | VortexArenaBattleEndRequest | Season points, daily leaderboard, rank-up rewards. | M | P3 |
| `290aD3iY` | VortexArenaLeaderboardRequest | Top-N plus self per season and day. | M | P3 |
| `iwo032Yj` | VortexArenaClaimRequest | Idempotent end-of-season claim. | S | P3 |
| `j3cyKi8o` | VortexArenaClaimDailyRequest | Daily claim after rollover. | S | P3 |
| `R34vyqw7` | VortexArenaRetireRequest | Benign stub; OBSERVED no constructor caller. | S | P3 |

### Events, minigames and SP Challenge (9 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | Rotating event calendar | Server-defined Frontier Hunter/event seasons with frohun_stat transitions (today always 1). | M | P2 |
| `m3fr7vxo` | ChallengeRankRewardRequest | Grant challenge_rank_reward_mst rows once per user/event when frohun_stat=3. | M | P2 |
| `SR7tf9pa` | MinigameStartRequest | Server season + play token; capture one real body first. Reachable today. | M | P2 |
| `jJKzi0v2` | MinigameCardEndRequest | Serves Card and Smash results (same group id); bounded scores, best per season, rank. | M | P2 |
| `oT8OP9Bp` | MinigameNameEntryRequest | Opt into the public board after the name filter. | S | P3 |
| `Hjk84giw` | MinigameFriendListRequest | Friends plus self per minigame and season. | M | P3 |
| `XrfDHUJo` | MinigameRankingInfoRequest | Benign stub only; scene never constructed and no parser key (OBSERVED). | S | P3 |
| `G3J9Domt` | SPChallengeUserInfoRequest | Only if SP Challenge content is authored (no MST dumped); keep type-3 single-mission dungeons hidden until then. | L | P3 |
| `QZy7cSW1` | SPChallengeMissionStartRequest | Start flag and start score with the above. | S | P3 |
| `27rGx1rL` | SPChallengeRankingRequest | Ordered best-score query reusing the FG ranking builder. | S | P3 |

### Summons, shops and promos (8.75 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| `z3Di1k0e` | GachaActionRequest_SG | Re-summon on top of the GachaAction core; becomes P0 the moment a banner uses a resummon id with sg_resummon_gacha_enable=1. | M | P2 |
| `MFLFP9f2` | GachaFixActionRequest | Selector summon, server-side pricing and validation. | M | P2 |
| — | ShopUse types 6, 11, 12, 103 | Implement remaining gem sinks (stamps, summoner items from shop_item_mst, summoner unit make, V-orbs). | M | P2 |
| — | GuildShopUse (cXi7b58e) | Guild currency shop from GuildExchangeMst. | M | P2 |
| `D3gyT3b3` | BundlePurchaseRequest | Optional free or gem-priced supply packs to the gift box, empty product id, server-validated. | L | P3 |
| `7Zxr9PzB` | InvitationReceiptRequest | Optional referral gift, one claim per account, rate-limited. | M | P3 |
| `4aL5upiU` | CollabRewardRequest | Optional deep-link promo claim table. | S | P3 |
| `nWBLmp53` | TieupCampaignInfoRequest | Benign stub; scene INFERRED unreachable. | S | P3 |
| `T_VIDEO_MST` | VideoTutorialRequest | Check reachability; return an empty list if a menu reaches it. | S | P3 |


## Phase 5: more regions and platforms

**Goal.** Grow beyond one Tokyo server and one platform without breaking accounts.

Each region is a separate server with its own leaderboards; the discovery host (`/dls`) can route players by GeoIP, and transfer codes can move an account between regions. SQLite with one writer is enough for one region with a few hundred concurrent players (INFERRED; no load test has been run), so the PostgreSQL port belongs here, when it is actually needed. Assets can move to a CDN on a separate resource host. iOS is a separate project: it needs a decrypted IPA of a matching build, the same patch classes found again in a probably stripped Mach-O, and a sideloading channel that works within Apple's limits. For Windows and Mac, the first step is finding out whether a compatible desktop client exists at all.

**Exit criteria.**
- A second region serves players with no shared-state bugs, and an account can move between regions.
- A go/no-go decision on iOS, backed by a working proof of concept or a documented blocker.
- A documented desktop route.

### Scale and regions (5.5 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | PostgreSQL port | Dialect layer in DatabaseInterface (INSERT OR, MIN/MAX, AUTOINCREMENT, strftime), atomic UPDATE ... RETURNING instead of read-modify-write, re-test single-connection assumptions. | L | P2 |
| — | Metrics and alerting | Prometheus /metrics (requests per handler, errors, DB latency), graphs, alerts. | M | P2 |
| — | Second region | Another server behind the same discovery host; GeoIP routing in /dls; per-region leaderboards; cross-region move via transfer codes. | M | P3 |
| — | Asset CDN | Separate resource_host on plain HTTP behind a CDN; optional https asset patch at 0x1ce9658/0x1ce9664 later. | S | P2 |
| — | Android compatibility | 16 KB-page devices (0x1000 LOAD alignment), Play Protect/developer-verification impact on sideloading. | S | P3 |

### Other platforms (9 days)

| Group id | Class / component | Work | Effort | Priority |
|---|---|---|---|---|
| — | iOS investigation | Obtain a decrypted IPA of a matching build, re-derive patch points (discovery host, https, BFCertificateProvider PEM, version-up URL) in Mach-O, choose a sideload channel. | XL | P3 |
| — | Windows/Mac investigation | Determine whether a desktop client of a compatible version exists; otherwise document an Android-emulator route; macOS on Apple Silicon could reuse the iOS build. | M | P3 |


## Coverage of the 124 missing request ids

Every entry in `re/notes/missing_requests.json` appears exactly once below; `gen.py` asserts this. Unclassified entries: None. All 124 entries are assigned.

Corrections to the list, from the audits (OBSERVED):
- `dsRW32K` GuildQuestClaimRequest and `fd54Sey` GuildRaidQuestObjectiveRequest are already registered as `GuildUnimplemented` (GmeControllerHandlers.cpp:146 and :159). The list's extractor missed 7-character ids. Regenerate the list before trusting it for other areas.
- The ChallengeArenaRankingRequest entry (`q\x1b1\x02`) is a mis-read pointer table. The real request uses six group id and AES key pairs chosen by ranking type (tables at 0x26f6a78 and 0x26f6aa0), and all six must be registered.
- `jJKzi0v2` is shared by MinigameCardEnd and MinigameSmashEnd, so one handler serves both.
- Several requests are listed by two audits (Colosseum reward claim, Challenge Arena shop, Raid shop, BuyCoin). Each is assigned to one phase here.

"Audit area" is the area audit that covers the request; "Phase" is where it gets built.

| # | Group id | Client class | Audit area | Phase | Disposition | Effort | Priority |
|---|---|---|---|---|---|---|---|
| 1 | `Fn1UzTo3` | TransferCodeCheckRequest | other | Phase 1 | implement | M | P0 |
| 2 | `LK9e0WyX` | TransferCodeIssueRequest | other | Phase 1 | implement | S | P0 |
| 3 | `I9DyF2aU` | TransferRequest | other | Phase 1 | implement | L | P0 |
| 4 | `swarOb4u` | BundleCategoryRefreshRequest | monetization | Phase 2 | benign stub | S | P3 |
| 5 | `kD298bt6` | BundlePurchaseIapRequest | monetization | Phase 2 | refuse (never grants) | S | P1 |
| 6 | `9o3q7SjN` | BuyCoinRequest | monetization | Phase 2 | refuse (never grants) | S | P1 |
| 7 | `kJs6CQq1` | BuyCoinStartRequest | monetization | Phase 2 | refuse (never grants) | S | P1 |
| 8 | `jk17biw9` | MultiGachaIapPurchaseIapRequest | monetization | Phase 2 | refuse (never grants) | S | P1 |
| 9 | `u06y2UE4` | SetPurchaseInfoRequest | monetization | Phase 2 | benign stub | S | P3 |
| 10 | `r234ydi1` | VideoAdCompleteRequest | monetization | Phase 2 | implement | M | P1 |
| 11 | `ifL3TZim` | VideoAdSlotsCompleteRequest | monetization | Phase 2 | benign stub | S | P3 |
| 12 | `jwgJVpUP` | VideoAdSlotsStandInfoRefreshRequest | monetization | Phase 2 | implement | S | P2 |
| 13 | `AjKpMC9a` | VideoAdsSlotActionRequest | monetization | Phase 2 | implement | L | P2 |
| 14 | `4eCLR4rq` | ItemUseTimeLimitRequest | other | Phase 2 | implement | M | P2 |
| 15 | `MbdL5D4K` | TrialDeckEditRequest | other | Phase 2 | implement | S | P1 |
| 16 | `gY63Zvy7` | TrialDeckGetRequest | other | Phase 2 | implement | M | P1 |
| 17 | `oQNtKU51` | ArenaBattleEndRequest | arena | Phase 3 | implement | M | P1 |
| 18 | `NPyXDgV9` | ArenaBattleRestartRequest | arena | Phase 3 | implement | S | P1 |
| 19 | `7f1rg92L` | ArenaBattleStartRequest | arena | Phase 3 | implement | M | P1 |
| 20 | `xkd9H50w` | ArenaFriendListRequest | arena | Phase 3 | implement | S | P2 |
| 21 | `82ztj6Iy` | ArenaMatchingRequest | arena | Phase 3 | implement | M | P1 |
| 22 | `eyg8sA32` | ArenaRetireRequest | arena | Phase 3 | implement | S | P1 |
| 23 | `zu6ifpy0` | ArenaStartRequest | arena | Phase 3 | implement | S | P1 |
| 24 | `syX20UG8` | ChallengeArenaBattleEndRequest | arena | Phase 3 | implement | L | P2 |
| 25 | `rbOMSnba` | ChallengeArenaBattleStartRequest | arena | Phase 3 | implement | M | P2 |
| 26 | `q\x1b1\x02` | ChallengeArenaRankingRequest | arena | Phase 3 | implement | M | P2 |
| 27 | `tct1Jogs` | ChallengeArenaRankingRewardRequest | arena | Phase 3 | implement | S | P3 |
| 28 | `jtzuog2S` | ChallengeArenaShopAddItemRequest | arena | Phase 3 | implement | M | P3 |
| 29 | `xfmNcJVl` | ChallengeArenaShopInfoRequest | arena | Phase 3 | implement | S | P3 |
| 30 | `z44LyLr2` | ChallengeArenaStartRequest | arena | Phase 3 | implement | L | P2 |
| 31 | `xmzmjcxQ` | ChallengeArenaTChestReceivedRequest | arena | Phase 3 | implement | S | P3 |
| 32 | `sXXX6Ec7` | ChallengeArenaTeamEditRequest | arena | Phase 3 | implement | M | P2 |
| 33 | `fGDPoQI3` | ColosseumBattleEndRequest | arena | Phase 3 | implement | M | P2 |
| 34 | `ohH9lfDE` | ColosseumBattleRestartRequest | arena | Phase 3 | implement | S | P2 |
| 35 | `ysP6qYcy` | ColosseumBattleStartRequest | arena | Phase 3 | implement | M | P2 |
| 36 | `rdZbaKBG` | ColosseumDeckEditRequest | arena | Phase 3 | implement | M | P2 |
| 37 | `AoBvG2YN` | ColosseumFriendListRequest | arena | Phase 3 | implement | M | P2 |
| 38 | `jv0dI88o` | ColosseumGetRewardInfoRequest | arena | Phase 3 | implement | M | P2 |
| 39 | `PN5CVaWy` | ColosseumRewardReceiptRequest | arena | Phase 3 | implement | M | P2 |
| 40 | `NYKBH4V6` | FriendAgreeRequest | social | Phase 3 | implement | S | P0 |
| 41 | `ps6Vgx4R` | FriendFavoriteRequest | social | Phase 3 | implement | S | P1 |
| 42 | `aVqJrR08` | FriendGetAgreeRequest | social | Phase 3 | implement | M | P0 |
| 43 | `mE5gUQOp` | FriendRandomSearchRequest | social | Phase 3 | implement | M | P0 |
| 44 | `N5SQd7oA` | FriendRefuseRequest | social | Phase 3 | implement | S | P0 |
| 45 | `umNt8M7i` | FriendSearchRequest | social | Phase 3 | implement | M | P0 |
| 46 | `26zW90oG` | FrontierGateRankingRequest | social | Phase 3 | implement | M | P1 |
| 47 | `38bSeq81` | GuildMemberGuildInfoRequest | social | Phase 3 | implement | S | P1 |
| 48 | `89HrE13b` | VortexArenaBattleEndRequest | arena | Phase 4 | implement | M | P3 |
| 49 | `j3s0iNM1` | VortexArenaBattleStartRequest | arena | Phase 4 | implement | S | P3 |
| 50 | `j3cyKi8o` | VortexArenaClaimDailyRequest | arena | Phase 4 | implement | S | P3 |
| 51 | `iwo032Yj` | VortexArenaClaimRequest | arena | Phase 4 | implement | S | P3 |
| 52 | `290aD3iY` | VortexArenaLeaderboardRequest | arena | Phase 4 | implement | M | P3 |
| 53 | `b5TunM93` | VortexArenaMatchingRequest | arena | Phase 4 | implement | S | P3 |
| 54 | `R34vyqw7` | VortexArenaRetireRequest | arena | Phase 4 | benign stub | S | P3 |
| 55 | `lp3a7bvD` | VortexArenaTopRequest | arena | Phase 4 | implement | M | P3 |
| 56 | `D3gyT3b3` | BundlePurchaseRequest | monetization | Phase 4 | implement | L | P3 |
| 57 | `z3Di1k0e` | GachaActionRequest_SG | monetization | Phase 4 | implement | M | P2 |
| 58 | `MFLFP9f2` | GachaFixActionRequest | monetization | Phase 4 | implement | M | P2 |
| 59 | `7Zxr9PzB` | InvitationReceiptRequest | monetization | Phase 4 | implement | M | P3 |
| 60 | `nWBLmp53` | TieupCampaignInfoRequest | monetization | Phase 4 | benign stub | S | P3 |
| 61 | `T_VIDEO_MST` | VideoTutorialRequest | monetization | Phase 4 | implement | S | P3 |
| 62 | `m3fr7vxo` | ChallengeRankRewardRequest | other | Phase 4 | implement | M | P2 |
| 63 | `4aL5upiU` | CollabRewardRequest | other | Phase 4 | implement | S | P3 |
| 64 | `5710Dkla` | GRGuardianDetail | other | Phase 4 | implement | L | P3 |
| 65 | `8b6DKiqz` | GuilldBattleMoveRequest | other | Phase 4 | implement | L | P3 |
| 66 | `jJKzi0v2` | MinigameCardEndRequest | other | Phase 4 | implement | M | P2 |
| 67 | `oT8OP9Bp` | MinigameNameEntryRequest | other | Phase 4 | implement | S | P3 |
| 68 | `SR7tf9pa` | MinigameStartRequest | other | Phase 4 | implement | M | P2 |
| 69 | `QZy7cSW1` | SPChallengeMissionStartRequest | other | Phase 4 | implement | S | P3 |
| 70 | `G3J9Domt` | SPChallengeUserInfoRequest | other | Phase 4 | implement | L | P3 |
| 71 | `fd54Sey` | GuildRaidQuestObjectiveRequest | raid | Phase 4 | already registered stub; list is stale | S | P3 |
| 72 | `Cu94xU8j` | RaidAgreementRequest | raid | Phase 4 | implement | S | P1 |
| 73 | `f9Dh4djx` | RaidCampRestEndRequest | raid | Phase 4 | implement | S | P2 |
| 74 | `diIe8ux7` | RaidCampRestGetRequest | raid | Phase 4 | implement | S | P2 |
| 75 | `I7nVUv0h` | RaidCampRestStartRequest | raid | Phase 4 | implement | S | P2 |
| 76 | `yZReG75i` | RaidEnterRequest | raid | Phase 4 | implement | M | P1 |
| 77 | `jqi71nWu` | RaidExitRequest | raid | Phase 4 | implement | S | P1 |
| 78 | `u59zdKCi` | RaidFriendRemoveRequest | raid | Phase 4 | implement | S | P2 |
| 79 | `u59zdKCw` | RaidFrlendSelectRequest | raid | Phase 4 | implement | S | P1 |
| 80 | `k5pMN68B` | RaidGetChatLogRequest | raid | Phase 4 | implement | S | P1 |
| 81 | `81zkMjfN` | RaidGetPlayerInfoRequest | raid | Phase 4 | implement | S | P3 |
| 82 | `N43JmIf2` | RaidItemDeleteRequest | raid | Phase 4 | implement | S | P3 |
| 83 | `DZLI83hx` | RaidItemEditRequest | raid | Phase 4 | implement | M | P2 |
| 84 | `K17JUYR2` | RaidItemMixRequest | raid | Phase 4 | implement | M | P2 |
| 85 | `SaN7v13E` | RaidLimitedUseItemRequest | raid | Phase 4 | implement | S | P2 |
| 86 | `5Ko2BjPA` | RaidMissionBattleEndRequest | raid | Phase 4 | implement | L | P1 |
| 87 | `La24KGAp` | RaidMissionBattleRestartRequest | raid | Phase 4 | implement | M | P2 |
| 88 | `8UiQj0uF` | RaidMissionBattleStartRequest | raid | Phase 4 | implement | M | P1 |
| 89 | `vgin9Mf1` | RaidMissionEndRequest | raid | Phase 4 | implement | L | P1 |
| 90 | `a12v0RcA` | RaidMissionExitRequest | raid | Phase 4 | implement | S | P1 |
| 91 | `9QdRVwB1` | RaidMissionHarvestRequest | raid | Phase 4 | implement | M | P1 |
| 92 | `Argz1U6B` | RaidMissionInfoLightRequest | raid | Phase 4 | implement | S | P1 |
| 93 | `BqF2x3EC` | RaidMissionInfoRequest | raid | Phase 4 | implement | L | P1 |
| 94 | `8iQL3IKA` | RaidMissionSelectRequest | raid | Phase 4 | implement | S | P1 |
| 95 | `2qDW9NBU` | RaidMissionStartRequest | raid | Phase 4 | implement | M | P1 |
| 96 | `o03kCtb1` | RaidMissionUseItemRequest | raid | Phase 4 | implement | M | P2 |
| 97 | `CWeTY8BQ` | RaidQuickStartRequest | raid | Phase 4 | implement | M | P1 |
| 98 | `H8N1R0TE` | RaidRoomDissolveRequest | raid | Phase 4 | implement | S | P1 |
| 99 | `Xe2UB3A8` | RaidRoomFriendGetRequest | raid | Phase 4 | implement | S | P1 |
| 100 | `0VxE6kTW` | RaidRoomInfoRequest | raid | Phase 4 | implement | L | P1 |
| 101 | `Xbgen5J8` | RaidRoomKickRequest | raid | Phase 4 | implement | S | P2 |
| 102 | `YF9UQpn6` | RaidRoomListRequest | raid | Phase 4 | implement | M | P1 |
| 103 | `bv9xt3m6` | RaidRoonItemEditRequest | raid | Phase 4 | implement | S | P2 |
| 104 | `dYIy6Eu8` | RaidSendChatRequest | raid | Phase 4 | implement | S | P2 |
| 105 | `4zJ0WmTX` | RaidShopUseRequest | raid | Phase 4 | implement | S | P2 |
| 106 | `52H0dkjv` | RaidUpDataRoomInfoRequest | raid | Phase 4 | implement | M | P1 |
| 107 | `vmdA2GD8` | RaidUpDataUserReadyRequest | raid | Phase 4 | implement | S | P1 |
| 108 | `XTban70i` | RaidUpDateCashRequest | raid | Phase 4 | implement | S | P1 |
| 109 | `4igfcyIm` | RaidUpdateBlackListRequest | raid | Phase 4 | implement | S | P3 |
| 110 | `7geDL5Vb` | RaidUpdateProgressRequest | raid | Phase 4 | implement | M | P1 |
| 111 | `jJZ01VQo` | RaidUserInfoRequest | raid | Phase 4 | implement | M | P1 |
| 112 | `H7GRp01L` | RaidWorloInfoRequest | raid | Phase 4 | implement | S | P1 |
| 113 | `7umuchUg` | FGPlusGuildRankingRequest | social | Phase 4 | implement | L | P3 |
| 114 | `Sp7umiaY` | FGPlusGuildRewardClaimRequest | social | Phase 4 | implement | M | P3 |
| 115 | `dsRW32K` | GuildQuestClaimRequest | social | Phase 4 | already registered stub; list is stale | L | P3 |
| 116 | `Hjk84giw` | MinigameFriendListRequest | social | Phase 4 | implement | M | P3 |
| 117 | `XrfDHUJo` | MinigameRankingInfoRequest | social | Phase 4 | benign stub | S | P3 |
| 118 | `jVRl14IS` | SGChatLogInfoListRequest | social | Phase 4 | implement | M | P3 |
| 119 | `27rGx1rL` | SPChallengeRankingRequest | social | Phase 4 | implement | S | P3 |
| 120 | `t5wfpZ32` | ChallengeMissionStartRequest | other | none | do not register (dead code) | - | - |
| 121 | `sQfU18kH` | ChallengeStartRequest | other | none | do not register (dead code) | - | - |
| 122 | `Zr37LHNE` | DebugUpdateRequest | other | none | do not register (dead code) | - | - |
| 123 | `6AqUE0xj` | ReinforcementRequest | other | none | do not register (dead code) | - | - |
| 124 | (empty) | VersionCheckRequest | other | none | do not register (dead code) | - | - |

## Biggest risks and unknowns

1. **No captured traffic.** Every arena, raid, colosseum, transfer and minigame response shape is rebuilt from readParam key sets. The key sets are OBSERVED, but value types, required groups and key-to-setter pairings are often INFERRED (for example the RaidRoomInfo pairing is off by one). A missing group can make the client dereference null, as it already did once on GuildRaidRankingResultScene. Budget device QA for every screen, and capture one real request body with a logging probe before building state on an inferred contract.
2. **Identity is the critical path.** Nothing social, competitive or co-op means anything until per-device accounts, token checks and transfer codes are done. Any delay in Phase 1 delays everything else.
3. **HTTPS is untested on a device.** The PEM swap is solid on paper, but it has not been run against the bundled OpenSSL 1.1.0c and curl 7.52.1. If it fails, the fallback (turning off verification) protects only against passive sniffing.
4. **Client-authoritative combat.** Damage, battle results and scores come from the client, and the patched APK is public. Server-held run tables, single settlement, minimum durations and caps limit the damage but do not stop a determined cheater. That is acceptable while nothing is for sale. A "play for real" or boosted-reward mode would need a separate, verified leaderboard.
5. **A large amount of authored data.** Colosseum seasons and the CBP economy, all Challenge Arena and Vortex Arena master data, Guild Raid, SP Challenge, guild quests, FG+ guild, video-slot prizes and shard thresholds have no source in any dump. They will be faithful in spirit, not in numbers. Keep them in versioned `deploy/system` JSON.
6. **Close-on-error behaviour.** An unregistered id or an unhandled error ends the session. Queued requests (TrialDeckEdit, ItemUseTimeLimit) fail later on an unrelated screen, which makes bug reports misleading. The Phase 1 graceful fallback reduces this, but it needs checking on each path.
7. **Single SQLite writer.** Cross-player queries (leaderboards, random search, matchmaking) compete with gameplay writes on one connection. Indexes, cached boards and WAL help. Postgres is the escape hatch, and it is a large port.
8. **Realtime Guild Raid needs client Java patching** and a Pusher-compatible server, which the current libgame-only patch pipeline does not cover yet.
9. **Distribution.** A sideloaded, renamed APK can be flagged by Play Protect, Google's developer-verification rollout may restrict sideloading in some countries, 32-bit-only phones are excluded, and the bundled third-party SDKs still send telemetry until Phase 2 removes them.
10. **Known client-side limits** from the gameplay status doc: the client's asset cache ignores server file changes, and scene entry can race incomplete downloads. That affects how quickly server data changes reach players.

## Decisions needed from the owner

1. **Hosting provider and region.** A Tokyo VM with arm64 or x86. The server already builds on arm64, so ARM instances are an option. Needed before the end of Phase 1.
2. **Domain.** The discovery hostname must be at most 19 characters (patched into a fixed slot in libgame.so). Game, asset and region hosts are not length-limited.
3. **Budget.** A monthly ceiling for VM, backups, domain and later a CDN and a second region. This decides when Postgres and more regions are worth it.
4. **HTTPS approach.** Option A is a private root CA pinned in the client: no dependency on outside CAs, any number of hostnames, but in-game web pages (terms, notices) will not load. Option B is pinning ISRG Root X2 with Let's Encrypt ECDSA certificates: web pages work, but it depends on Let's Encrypt's chain. The audit recommends A unless the in-game web pages matter.
5. **Account recovery UX.** Transfer code only (built into the client, Phase 1), or also username and password login through the client's Gumi Live account screens (more work, more robust). Also decide how long transfer codes stay valid and whether support may move saves by hand.
6. **Anti-cheat stance.** How strict reward and score validation should be, whether suspected cheaters are flagged or banned, and whether a "play for real" or boosted-reward mode is planned (it changes how strict Phase 3 leaderboards must be).
7. **Game-day time zone.** JST or UTC for daily resets on the Tokyo server.
8. **NPC friends.** Keep the simulated friends and developer encounters as bonus helpers alongside real players, or remove them once real players exist.
9. **Chat and moderation.** Raid room chat and guild chat let strangers talk to each other. Who moderates, and with which tools (filter, report, mute, ban)?
10. **Optional features.** Whether to build free supply packs, the referral gift, collab promo links, SP Challenge and Guild Raid, or leave them gated.
11. **Distribution channel.** Package name, where the APK is hosted, and how players are told about updates.
