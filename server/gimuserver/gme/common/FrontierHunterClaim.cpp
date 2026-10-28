// Frontier Hunter: Hunter Rank and the end-of-week claim (ChallengeRankReward).
#include "FrontierHunterDetail.hpp"

#include "Common.hpp"
#include "GameClock.hpp"
#include "HunterOrbs.hpp"
#include "RaidRequest.hpp"
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <unordered_map>

namespace gme::frohun
{

using namespace detail;

namespace detail
{

// ---------------------------------------------------------------------------
// Hunter Rank.
// ---------------------------------------------------------------------------

std::string cardJson(const Card& card, const int32_t eventId)
{
	std::string row = "{";
	row += "\"c5yZnpB4\":" + str(eventId);
	row += ",\"NH65Wj0f\":" + str(card.infoType);
	row += ",\"9w5e3ZJB\":" + str(card.rankType);
	row += ",\"j28VNcUW\":" + jsonText(card.missionId);
	row += ",\"Sv80kL5r\":" + str(card.hrId);
	row += ",\"30Kw4WBa\":" + str(card.type);
	row += ",\"TdDHf59J\":" + str(card.targetId);
	row += ",\"wJsB35iH\":" + str(card.count);
	row += ",\"37moriMq\":\"0\"}";
	return row;
}

/*!
* Raises the stored Hunter Rank by `gain` (capped at the ladder's top) and pays
* each level reached its challenge_hr_mst reward into the present box.
*
* @return The levels reached with their rewards, lowest first.
*/
drogon::Task<std::vector<Card>> raiseHunterRank(db::Database database, const UserIdentity identity,
	const int32_t gain)
{
	std::vector<Card> steps;
	if (gain <= 0)
		co_return steps;
	const auto& data = eventData();
	const auto before = co_await loadHunterRank(database, identity.userId);
	const auto after = std::min(before + gain, data.maxHr);
	for (int32_t level = before + 1; level <= after; ++level)
	{
		Card card;
		card.hrId = level;
		const auto it = data.ladder.find(level);
		if (it != data.ladder.end() && it->second.type != kRewardHunterRank)
		{
			card.type = it->second.type;
			card.targetId = it->second.targetId;
			// HR 240's Omni Frog row carries count 0; a reward of nothing
			// would be no reward at all.
			card.count = std::max(it->second.count, 1);
			co_await addUserPresent(database, identity, card.type, std::to_string(card.targetId),
				card.count, 0, "Frontier Hunter: Hunter Rank " + std::to_string(level) + " reward");
		}
		steps.push_back(card);
	}
	if (after != before)
	{
		co_await database->execSqlCoro(
			"UPDATE user_info SET hunter_rank = $1 WHERE id = $2;", after, identity.userId);
		LOG_INFO << "FrontierHunter: " << identity.userId << " Hunter Rank " << before << " -> " << after;
	}
	co_return steps;
}

/*! The kN2i7qds block with Hunter Rank and the orbs, leaving the FG score alone. */
drogon::Task<std::string> headerJson(db::Database database, const UserIdentity identity)
{
	const auto hr = co_await loadHunterRank(database, identity.userId);
	const auto orbs = co_await loadHunterOrbRefresh(database, identity);
	co_return "{\"Sv80kL5r\":" + str(hr) + ",\"38sHatGk\":" + str(orbs.aube)
		+ ",\"6mh0jiKJ\":\"0\",\"RHKA30s5\":" + str(orbs.aube_rest_timer) + "}";
}

} // namespace detail

drogon::Task<std::string> claimRankRewards(const UserIdentity identity)
{
	const auto& data = eventData();
	std::vector<Card> cards;
	auto transaction = co_await gme::newTransaction(theDb());
	try
	{
		const auto now = cycleNow();
		const auto weeks = co_await transaction->execSqlCoro(
			"SELECT cycle, MAX(event_id) AS event_id, SUM(service) AS svc FROM user_frohun_scores s"
			" WHERE s.user_id = $1 AND s.cycle < $2"
			" AND NOT EXISTS (SELECT 1 FROM user_frohun_claims c WHERE c.user_id = s.user_id AND c.cycle = s.cycle)"
			" GROUP BY cycle HAVING SUM(service) > 0 ORDER BY cycle LIMIT " + std::to_string(kMaxWeeksPerClaim) + ";",
			identity.userId, now);

		for (const auto& week : weeks)
		{
			const auto cycle = week["cycle"].as<int64_t>();
			const auto eventId = week["event_id"].as<int32_t>();
			const auto hrBefore = co_await loadHunterRank(transaction, identity.userId);

			// The week's places: the total board, and every MVP board that has
			// a reward row.  Only the running event's tables are loaded; a week
			// scored under an older event is settled with them too.
			const auto total = co_await placing(transaction, identity.userId, cycle, boardExpression(kRankTotal, "0"));
			std::map<int32_t, Placing> mvpPlaces;
			std::map<int32_t, std::string> mvpMission;
			for (const auto& mvp : data.mvpBoards)
			{
				mvpMission[mvp.rankType] = mvp.missionId;
				mvpPlaces[mvp.rankType] = co_await placing(transaction, identity.userId, cycle,
					boardExpression(mvp.rankType, mvp.missionId));
			}

			int32_t hrGain = 0;
			std::vector<Card> rewards;
			for (const auto& row : data.rankRows)
			{
				if (row.rankMin <= 0)
					continue;   // the 0-0 row is Randall's line for the unranked
				const int32_t place = row.rankType == kRankTotal ? total.rank
					: (mvpPlaces.contains(row.rankType) ? mvpPlaces[row.rankType].rank : 0);
				if (place <= 0 || place < row.rankMin || (row.rankMax != 0 && place > row.rankMax))
					continue;
				if (row.type == kRewardHunterRank)
				{
					hrGain += std::max(row.count, 0);
					continue;
				}
				Card card;
				if (row.rankType == kRankTotal)
				{
					card.infoType = kCardRank;
					card.hrId = kBaseHunterRank;   // HRID 1: a plain reward card, no rank banner
				}
				else
				{
					card.infoType = kCardMvp;
					card.rankType = row.rankType;
					card.missionId = mvpMission[row.rankType];
				}
				card.type = row.type;
				card.targetId = row.targetId;
				card.count = std::max(row.count, 1);
				co_await addUserPresent(transaction, identity, card.type, std::to_string(card.targetId),
					card.count, 0, card.infoType == kCardMvp ? "Frontier Hunter: MVP reward"
						: "Frontier Hunter: ranking reward");
				rewards.push_back(card);
			}

			const auto steps = co_await raiseHunterRank(transaction, identity, hrGain);
			cards.insert(cards.end(), steps.begin(), steps.end());
			cards.insert(cards.end(), rewards.begin(), rewards.end());
			const auto hrAfter = co_await loadHunterRank(transaction, identity.userId);

			co_await transaction->execSqlCoro(
				"INSERT INTO user_frohun_claims (user_id, cycle, event_id, final_rank, service, hr_before, hr_after, claimed_at)"
				" VALUES ($1, $2, $3, $4, $5, $6, $7, $8);",
				identity.userId, cycle, static_cast<int64_t>(eventId), static_cast<int64_t>(total.rank),
				week["svc"].as<int64_t>(), static_cast<int64_t>(hrBefore), static_cast<int64_t>(hrAfter),
				static_cast<int64_t>(GameClock::nowSeconds()));
			LOG_INFO << "FrontierHunter: " << identity.userId << " claimed week " << cycle << ": rank "
				<< total.rank << " of " << week["svc"].as<int64_t>() << " service, HR " << hrBefore << " -> "
				<< hrAfter << ", " << rewards.size() << " reward(s)";
		}

		const auto lobby = co_await loadLobbyState(transaction, identity.userId);
		std::string reply = "{";
		// Absent rather than empty when nothing was paid: an empty array would
		// not clear an earlier claim's cards (readParam clears on row 0 only).
		if (!cards.empty())
		{
			reply += "\"p73SJiWv\":[";
			for (size_t i = 0; i < cards.size(); ++i)
				reply += (i ? "," : "") + cardJson(cards[i], data.eventId);
			reply += "],";
		}
		reply += "\"s7A3bGLe\":" + userInfoJson(lobby, data.eventId);
		// The running week's bests: point rewards are per week, and the reward
		// list marks them got against this cache (ChallengeRewardScene::setList).
		reply += ",\"cv9rC2KN\":" + co_await missionBestJson(transaction, identity.userId, cycleNow());
		reply += ",\"kN2i7qds\":" + co_await headerJson(transaction, identity);
		reply += "}";
		co_return reply;
	}
	catch (...)
	{
		transaction->rollback();
		throw;
	}
}

} // namespace gme::frohun
