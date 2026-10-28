# Frontier Hunter: scoring, Hunter Rank and claimable rewards (ticket #8)

"Challenge" is the client's internal name for Frontier Hunter. Code:
`gimuserver/gme/common/FrontierHunter.hpp` (API) with `FrontierHunter.cpp`
(week, lobby, runs), `FrontierHunterData.cpp` (the event's MST index),
`FrontierHunterBoards.cpp` (ranking), `FrontierHunterClaim.cpp` (Hunter Rank,
claim) and `FrontierHunterDetail.hpp` (their shared internals); the Challenge
handlers; two hooks in `handlers/Mission.cpp`. Migration `18102026_FrontierHunterScores`.
Wire test: `tools/test_frontier_hunter.py` in the parent repository.

## How the original worked (decoded from libgame.so v2.19.6 + the shipped data)

**A run** is an ordinary MissionStart / MissionEnd on a quest of the event's
dungeon (DungeonMst type 1). `ChallengeMissionStartRequest` (t5wfpZ32) and
`ChallengeStartRequest` are dead code: their only builders are never
constructed. MissionStart carries `cvg8hzp9 {c5yZnpB4: event}`; MissionEnd
carries it too, plus `ZYSWU9V1`:

| key | source | meaning |
|---|---|---|
| `TPR79fyI` | `LongLongToString(ChallengeBattleResultList::getTotalScore)` @0x13A845C | the run's score (Esteem points), plain decimal |
| `7cev0fzQ` | `getNote` @0x1034EC4 | `battle:N,spark:N,overkill:N,multikill:N,bbkill:N,...,oneturn:N,...` summed over the run |
| `pG2n1A28` | `max(getClearBattleOrder, 0)` | last battle cleared |
| `gUvjqu39` | `SaveData::getItemSetID` | the item set picked |

(The old comment in `net/handlers.kdl` has TPR79fyI and 7cev0fzQ swapped.)
The score is computed by the client from `chlng_mission_mst` score_rate
[0..16] (sparks, overkills, multi-kills, BB kills, weak hits, damage, turn
bonuses, crystals), score_limit (spark / weak caps) and bonus_param.
**score_rate [17] is never read by the client**: it is the Esteem -> Frontier
Service conversion % (.1 / .2 / 1 / 2 for event 97, matching the wiki), so the
conversion was always the server's.

**The result screen** (ChallengeResultScene, scene 0x453 -> 0x456) reads from
the MissionEnd reply: `ZYSWU9V1` (score shown), `21BqKn8b` (grade "1".."7" =
D..SSS, Zel, Karma, ranks, `TPR79fyI` = service gained, `5Z1LNoyH` bonus
rate), `ZSC3t4Fv` (rewards obtained; row 0 clears the lists, kind 1/2 add),
`cv9rC2KN` (best per quest), and NEW RECORD compares with MissionStart's
`7B5Ue9kH {TPR79fyI: previous best}`. Without any of these the screen still
completes, showing zeros.

**Point rewards** (`chlng_mission_reward_mst`): a row is "got" once the
quest's BEST score reaches point_start (ChallengeRewardScene::setList; that
scene is browse-only). Types 3/6/7/8/9/11 are presents, type 10 is HR +N.

**Ranking** (`ChallengeRanking`, one request per lobby visit, body = event id
only): one `Hmiwj75u` array holds every board, told apart by `NH65Wj0f`
(0 friends, 1 total, 2 MVP category with `9w5e3ZJB` = challenge_mvp_mst
rank_type and `j28VNcUW` = its mission id). The client sorts by `U4pMNjy0`
(int64), keeps RankOrder <= 30 (friends <= 1000) and appends the viewer's own
row last. MVP 101-104 = best run on quest 1-4, 201-207 = event totals of
sparks, overkills, multi-kills, BB kills, one-turn kills, entries, helps.

**Hunter Rank**: the client never computes it and never reads
`challenge_hr_mst`; it draws `kN2i7qds Sv80kL5r`. HR rises only through reward
type 10: the rank tiers of `challenge_rank_reward_mst` (event 97: 1-100 +8,
101-500 +6, 501-5000 +5, 5001-20000 +4, 20001+ +3), +1 for places 1-10 and for
the "lucky" repdigit places, and two point rewards (1009702 at 1.1M, 1009703
at 4.5M). The ladder pays one present per HR level reached.

**The lobby** gets its phase only from `s7A3bGLe` (`j0Uszek2`): 1 = event
running (red Entry), 2 = calculating, 3 = rewards waiting (blue Reward button),
4 = results (button off). The client never looks at the challenge_mst dates.
The period text (`s2gM3deu`) and Randall's line (`WDUyI85C`) are drawn
verbatim. Before this change the Survey Office reply carried no `s7A3bGLe`
at all.

**The claim**: at stat 3 the blue button sends `ChallengeRankReward`
(m3fr7vxo / 4BuRdgr2, body `cvg8hzp9 {c5yZnpB4}`), the server decides what was
earned, and the reply's `p73SJiWv` rows are the receipt cards
(ChallengeReceiptScene): `NH65Wj0f` 1 (or 3) = an HR card "HR n-1 -> n" when
`Sv80kL5r` > 1, a plain reward card when it is 1; 2 = an MVP card. Each card
shows one reward (`30Kw4WBa`/`TdDHf59J`/`wJsB35iH`). The client pays nothing
itself. Crash rules honoured: no type 10 card (it takes the item branch and
dereferences a null ItemMst), MVP pairs from challenge_mvp_mst only, only
info types 1 and 2. An empty or absent list does not clear the last claim's
cards, so the reply always puts the lobby back on stat 1.

## Decisions for this server

* **One survey per week.** The server keeps one event (the newest
  challenge_mst row, 97) open for good, so "the end of the event" is a weekly
  cycle: Monday 00:00 UTC to the next Monday 00:00 UTC
  (`cycle = floor((unix - 345600) / 604800)`, GameClock time). Scores, bests
  and point rewards start fresh each week.
* **Frontier Service** = round(score x score_rate[17] %), for CLEARED runs
  only (mission_status 2). A loss or give-up counts as an entry (MVP 206) and
  scores nothing. The client-computed score is capped at 9,999,999 (the
  battle's seven-digit score counter) or 3 x the quest's top point reward or
  grade when that is more (13.5M on the Terminus). The 2022 tables are far
  below today's units: a real Entrance run with a Lv 999 helper scored 1.9M
  (top reward 400k), so every point reward falls in one or two good runs.
* **The ranking** is the week's total service. Only players with service that
  week hold a place on any board (they are the ones who can claim), so an MVP
  place is never held by someone it cannot pay. Ties go to whoever reached the
  value first (each ranked value keeps its own `*_at` time), then the user id,
  so places are unique. Real players only: the emulated rivals are gone. A
  viewer with no place gets no total-board row of their own (the scene copies
  that row's RankOrder into the header). MVP 207 ("times helped") has no
  counter and stays empty.
* Only a battle MissionStart issued a serial for is scored (the start that
  charged the Hunter Orb). A lost run echoes score 0 (the client raises its
  cached best from the echo), and a retried MissionEnd that settles nothing
  gets "nothing gained" blocks so the result screen cannot replay the last
  run's rewards. A run that ends after Monday 00:00 counts for the new week.
* **Point rewards** are paid at MissionEnd, once per week each, into the
  present box, and listed on the result screen. An HR +1 point reward raises
  the rank at once (and pays that level's ladder reward).
* **Rank rewards** of a finished week are claimable from the next week on,
  with no expiry (the original removed unclaimed rewards at the next survey;
  a weekly cycle makes that too harsh). While a finished week with service is
  unclaimed the lobby shows stat 3, the period of that week and Randall's
  closing line; the tap pays every unclaimed week (at most 8 per claim): one HR
  card per level gained (each with its challenge_hr_mst reward), the tier's
  items (Elementum Tomes, the God Hunter sphere for #1), the MVP spheres. All
  into the present box, HR applied directly, in one transaction recorded in
  `user_frohun_claims`. HR is capped at 240 (the ladder's top); over-cap HR is
  dropped.
* While the week runs, Randall's line is the rank tier's comment
  ("At this rate your Hunter Rank will increase by 5 or more!"), and the
  ranking board shows the lobby's week (the finished one while stat 3).

## Tables

`user_frohun_scores (user_id, cycle, mission_id)`: best_score, service, runs,
clears, sparks, overkills, multikills, bbkills, oneturns, event_id,
updated_at, and a `*_at` per ranked value (best_at, service_at, runs_at,
sparks_at, ...) for the tie-break. `user_frohun_rewards (user_id, cycle, reward_id)`: point rewards
paid. `user_frohun_claims (user_id, cycle)`: final_rank, service, hr_before,
hr_after, claimed_at. `user_info.hunter_rank` (default 1). The three tables are player-owned (an account reset or
delete must remove their rows).

## Not done

* The lobby's Reward List button opens a web page
  (`ChallengeRewardListScene::viewerOpen`: getPhpUrl + ChallengeMst url
  `/bf/web/frontier_hunter/html/index.php`); no page is served for it yet.
* The rank-reward economy is the original's: with a few hundred players
  everyone who scores lands in the top 100 (+8 HR, +1 for the top 10, 40
  Elementum Tomes a week). Point rewards (gems, Zel, frogs, imps) repeat
  weekly. Tune `challenge_rank_reward_mst` / the cycle length if that is too
  generous.
* `chlng_mission_mst` bonus_param rows carry the next row's groups too (a data
  bug in the dump); the client takes the max, which inflates its bonuses
  slightly. Left as shipped.
