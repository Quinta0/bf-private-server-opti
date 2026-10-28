#pragma once

#include <gimuserver/db/Types.h>

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// FRONTIER HUNTER SCORING AND REWARDS (ticket #8).
//
// "Challenge" is the client's name for Frontier Hunter.  The decoded client
// behaviour and the decisions below are written up in
// docs/features/FRONTIER_HUNTER.md; the short version:
//
// * A run is an ordinary MissionStart / MissionEnd on a quest of the running
//   event's dungeon.  MissionEnd carries the client-computed run score
//   (group ZYSWU9V1, TPR79fyI) and a counter note (7cev0fzQ).  The score turns
//   into FRONTIER SERVICE at the quest's conversion rate (field 17 of
//   chlng_mission_mst score_rate, .1 / .2 / 1 / 2 % for event 97), and the
//   service of every cleared run accumulates.  The event ranking is the total
//   service; the per-quest point rewards (chlng_mission_reward_mst) are paid
//   once per survey when the quest's BEST score reaches them.
// * ONE SURVEY PER WEEK.  This server keeps a single event (the newest row of
//   challenge_mst) open for good, so "the end of the event" is a weekly cycle:
//   Monday 00:00 UTC to the next Monday.  A finished week's ranking is final,
//   and its rank rewards (challenge_rank_reward_mst) are claimable from the
//   next week on, until claimed.
// * HUNTER RANK only rises through reward type 10 ("HR +N"): the rank tiers,
//   the top-10 / lucky-rank bonuses and two point rewards.  Each level reached
//   pays that level's challenge_hr_mst row.  The client never computes HR; it
//   draws kN2i7qds Sv80kL5r.
// * THE CLAIM is the lobby's blue Reward button (frohun_stat 3), which sends
//   ChallengeRankReward (m3fr7vxo).  Its reply lists one receipt card per HR
//   level and reward; everything is paid into the present box in the same
//   transaction, and the reply puts the lobby back on the running week.

namespace gme
{
struct UserIdentity;
}

namespace gme::frohun
{

// ---------------------------------------------------------------------------
// The weekly survey.
// ---------------------------------------------------------------------------

inline constexpr int64_t kCycleSeconds = 7 * 24 * 3600;
// Monday 1970-01-05 00:00 UTC: weeks start on Monday.
inline constexpr int64_t kCycleEpoch = 4 * 24 * 3600;

/*! The survey week a unix time falls in. */
int64_t cycleAt(int64_t unixSeconds);
/*! The running survey week (GameClock, so the debug console can move it). */
int64_t cycleNow();
/*! Unix seconds the week starts at. */
int64_t cycleStartsAt(int64_t cycle);

// ChallengeInfo::frohun_stat (s7A3bGLe j0Uszek2), the lobby phase:
// 1 runs the event (red Entry button), 3 offers the rewards (blue Reward
// button, ChallengeLobbyScene::btnSetSt @0x155B904).
inline constexpr int32_t kStatRunning = 1;
inline constexpr int32_t kStatRewards = 3;

/*! Every account starts at HR 1 (the ladder pays from HR 2). */
inline constexpr int32_t kBaseHunterRank = 1;

/*! The highest Hunter Rank challenge_hr_mst has a row for (240). */
int32_t maxHunterRank();

/*! The running event (the newest challenge_mst row), 0 when there is none. */
int32_t activeEventId();

/*! A quest of the running event, from its chlng_mission_mst row. */
struct QuestRules
{
	int32_t eventId = 0;
	int32_t missionId = 0;
	// Percent of the run score that becomes Frontier Service (score_rate [17]).
	double servicePercent = 0.0;
	// Highest run score accepted (the score is the client's own count).
	int64_t scoreCap = 0;
};

/*! The rules of a quest of the RUNNING event; nullopt for anything else. */
std::optional<QuestRules> questRules(int32_t missionId);

/*! The quests of the running event, in mission id order. */
std::vector<int32_t> activeQuests();

/*! The player's stored Hunter Rank (user_info.hunter_rank). */
drogon::Task<int32_t> loadHunterRank(db::Database database, std::string userId);

// ---------------------------------------------------------------------------
// What the Survey Office shows.
// ---------------------------------------------------------------------------

struct LobbyState
{
	int32_t stat = kStatRunning;
	// The survey week the lobby is about: the running one, or the finished
	// week whose rewards are waiting.
	int64_t cycle = 0;
	std::string term;      // s2gM3deu, drawn after "Event Period" / "Ranking Result"
	std::string comment;   // WDUyI85C, the navigator's line
	int64_t search = 0;    // TPR79fyI, the week's Frontier Service
	int32_t order = 0;     // vV4P53m7, the week's position (0 = not ranked)
};

drogon::Task<LobbyState> loadLobbyState(db::Database database, std::string userId);

/*! The s7A3bGLe object (ChallengeUserInfoResponse) for a lobby state. */
std::string userInfoJson(const LobbyState& state, int32_t eventId);

/*! cv9rC2KN rows (ChallengeMisUserInfoResponse): the week's best per quest. */
drogon::Task<std::string> missionBestJson(db::Database database, std::string userId, int64_t cycle);

/*! Appends `"key":value` members to a serialised JSON object. */
void appendMembers(std::string& body, const std::string& members);

// ---------------------------------------------------------------------------
// Runs.
// ---------------------------------------------------------------------------

/*!
* MissionStart extras for a Frontier Hunter quest: the quest's best so far
* (7B5Ue9kH, the NEW RECORD test) and the per-quest bests.  Never throws.
*/
drogon::Task<void> appendMissionStart(db::Database database, std::string userId,
	int32_t missionId, std::string& buffer);

/*!
* Records a settled Frontier Hunter run and returns the reply members to
* splice into MissionEnd ("" when the quest is not one of the running event).
* Runs inside MissionEnd's transaction, after its once-per-battle guard.
*
* @param requestJson The MissionEnd body (ZYSWU9V1 is read loosely from it).
* @param won mission_status 2.  A loss counts as an entry and scores nothing.
* @param zelGained, karmaGained What MissionEnd paid, for the result screen.
*/
drogon::Task<std::string> settleRun(db::Database transaction, UserIdentity identity,
	std::string requestJson, int32_t missionId, bool won, int64_t zelGained, int64_t karmaGained);

/*!
* The Frontier Hunter reply members for a MissionEnd that settled nothing (a
* retry of a result already paid): nothing gained, and the result screen's
* reward lists cleared, so it cannot show the last run's rewards again.
* Read-only; "" when the quest is not one of the running event.
*/
drogon::Task<std::string> replayRun(db::Database transaction, UserIdentity identity, int32_t missionId);

// ---------------------------------------------------------------------------
// The ranking and the claim.
// ---------------------------------------------------------------------------

/*! The Hmiwj75u array: friends, total and MVP boards of the lobby's week. */
drogon::Task<std::string> rankingJson(db::Database database, std::string userId);

/*!
* Pays every finished week's rank rewards the player has not claimed, in one
* transaction, and returns the whole ChallengeRankReward reply.
*/
drogon::Task<std::string> claimRankRewards(UserIdentity identity);

} // namespace gme::frohun
