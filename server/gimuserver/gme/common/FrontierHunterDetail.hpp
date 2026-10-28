#pragma once

// Internals shared by the Frontier Hunter translation units
// (FrontierHunter.cpp, FrontierHunterData.cpp, FrontierHunterBoards.cpp,
// FrontierHunterClaim.cpp).  The public API is FrontierHunter.hpp.

#include "FrontierHunter.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gme::frohun::detail
{


// MVP boards (challenge_mvp_mst rank_type).  101-104 are the best single run
// on one quest; 201-206 are week totals of the counters the client reports in
// the run note (7cev0fzQ, ChallengeBattleResultList::getNote @0x1034EC4).
// 207 "times helped" has no counter on the wire, so that board stays empty.
constexpr int32_t kRankTotal = 1;
constexpr int32_t kRankQuestFirst = 101;
constexpr int32_t kRankQuestLast = 199;

// The receipt card kinds (ChallengeReceiptScene ctor @0x1563F74): 1 shows a
// Hunter Rank step when HRID > 1 and a plain reward card when HRID is 1, 2 is
// an MVP card whose banner is looked up in challenge_mvp_mst.
constexpr int32_t kCardRank = 1;
constexpr int32_t kCardMvp = 2;

// MST reward type 10 is "Hunter Rank +target_cnt", never a present.  The
// receipt card sends it down the item branch and crashes on a null ItemMst
// (setSlideInAction @0x15659A8), so it is applied to the stored rank instead.
constexpr int32_t kRewardHunterRank = 10;

// Board sizes the client draws: RankOrder <= 30 on the total and MVP boards,
// <= 1000 on the friend board (ChallengeNewRankingScene::setRankingList).
constexpr int32_t kBoardRows = 30;
constexpr int32_t kFriendRows = 100;
// RankOrder for the viewer's own row when they have no place: above 30 keeps
// it out of the main list, and the client appends it last anyway.
constexpr int32_t kUnplacedOrder = 9999;

// A ZSC3t4Fv row of a kind the client drops (only 1 and 2 are kept): sent as
// row 0 it clears the result screen's reward lists and adds nothing
// (ChallengeRewardResponse::readParam @0x13D3818).
constexpr const char* kNoRewardRow =
	"{\"IkmC8gG2\":\"0\",\"empaR60j\":\"0\",\"30Kw4WBa\":\"0\",\"TdDHf59J\":\"0\",\"wJsB35iH\":\"0\",\"37moriMq\":\"\"}";

// Unpaid weeks one claim settles at most (a returning player's backlog).
constexpr int32_t kMaxWeeksPerClaim = 8;

inline int64_t parseInt(const std::string& text, const int64_t fallback = 0)
{
	if (text.empty())
		return fallback;
	char* end = nullptr;
	const auto value = std::strtoll(text.c_str(), &end, 10);
	return end == text.c_str() ? fallback : value;
}

inline std::vector<std::string> split(const std::string& text, const char sep)
{
	std::vector<std::string> out;
	std::string part;
	std::istringstream in(text);
	while (std::getline(in, part, sep))
		out.push_back(part);
	return out;
}

inline std::string jsonText(const std::string& text)
{
	std::string out;
	if (glz::write_json(text, out))
		return "\"\"";
	return out;
}

inline std::string str(const int64_t value)
{
	return "\"" + std::to_string(value) + "\"";
}

inline bool safeId(const std::string& id)
{
	return !id.empty() && id.size() <= 32
		&& std::all_of(id.begin(), id.end(), [](const unsigned char c) { return std::isalnum(c) != 0; });
}

// ---------------------------------------------------------------------------
// The running event's data, indexed once.
// ---------------------------------------------------------------------------

struct PointReward
{
	int32_t rewardId = 0;
	int64_t pointStart = 0;
	int32_t type = 0;
	int32_t targetId = 0;
	int32_t count = 0;
	std::string param;
};

struct LadderRow
{
	int32_t type = 0;
	int32_t targetId = 0;
	int32_t count = 0;
};

struct RankRow
{
	int32_t rankType = 0;
	int32_t rankMin = 0;
	int32_t rankMax = 0;
	int32_t type = 0;
	int32_t targetId = 0;
	int32_t count = 0;
	std::string comment;
};

struct MvpBoard
{
	int32_t rankType = 0;
	std::string missionId;   // as challenge_mvp_mst has it: "0" for 2xx
};

struct EventData
{
	int32_t eventId = 0;
	std::string endComment;
	std::map<int32_t, QuestRules> quests;
	std::map<int32_t, std::vector<std::pair<int64_t, int32_t>>> grades;   // mission -> (score, grade)
	std::map<int32_t, std::vector<PointReward>> pointRewards;            // mission -> rows
	std::vector<RankRow> rankRows;
	std::vector<MvpBoard> mvpBoards;
	std::map<int32_t, LadderRow> ladder;                                  // HR level -> reward
	int32_t maxHr = kBaseHunterRank;
};

/*! The running event's data, indexed once (FrontierHunterData.cpp). */
const EventData& eventData();

// ---------------------------------------------------------------------------
// Boards (FrontierHunterBoards.cpp).
// ---------------------------------------------------------------------------

/*!
* One board in SQL over a user's rows of a week: the value it ranks and when
* that value last grew (the tie-break: whoever got there first).
*/
struct BoardSql
{
	std::string value;
	std::string reached;
	bool empty() const { return value.empty(); }
};

BoardSql boardExpression(int32_t rankType, const std::string& missionId);

struct Placing
{
	int64_t value = 0;
	int32_t rank = 0;   // 0 = no place on the board
};

/*!
* A player's value and place on one board of a week.  Ties go to whoever got
* there first, then to the user id, so places are unique.
*/
drogon::Task<Placing> placing(db::Database database, std::string userId, int64_t cycle, BoardSql expression);

struct BoardEntry
{
	std::string userId;
	int64_t value = 0;
	int64_t runs = 0;
	int32_t order = 0;
};

/*! The top of one board of a week, best first, among one pool (Sandbox.hpp). */
drogon::Task<std::vector<BoardEntry>> topOfBoard(db::Database database, int64_t cycle,
	BoardSql expression, int32_t limit, bool sandbox);

// ---------------------------------------------------------------------------
// Hunter Rank and receipt cards (FrontierHunterClaim.cpp).
// ---------------------------------------------------------------------------

/*! One receipt card (ChallengeRankRewardResponse, p73SJiWv). */
struct Card
{
	int32_t infoType = kCardRank;
	int32_t rankType = 0;
	std::string missionId = "0";
	int32_t hrId = kBaseHunterRank;
	int32_t type = 0;
	int32_t targetId = 0;
	int32_t count = 0;
};

/*!
* Raises the stored Hunter Rank by `gain` (capped at the ladder's top) and pays
* each level reached its challenge_hr_mst reward into the present box.
*
* @return The levels reached with their rewards, lowest first.
*/
drogon::Task<std::vector<Card>> raiseHunterRank(db::Database database, UserIdentity identity, int32_t gain);

/*! The kN2i7qds block with Hunter Rank and the orbs, leaving the FG score alone. */
drogon::Task<std::string> headerJson(db::Database database, UserIdentity identity);

} // namespace gme::frohun::detail
