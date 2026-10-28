#include "FrontierHunter.hpp"
#include "FrontierHunterDetail.hpp"

#include "Common.hpp"
#include "GameClock.hpp"
#include "HunterOrbs.hpp"
#include "RaidRequest.hpp"

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

namespace
{

std::string weekTerm(const int64_t cycle)
{
	const auto format = [](const int64_t when)
	{
		const std::time_t t = static_cast<std::time_t>(when);
		std::tm tm{};
		gmtime_r(&t, &tm);
		char buffer[16];
		std::strftime(buffer, sizeof(buffer), "%b %d", &tm);
		return std::string(buffer);
	};
	const auto start = cycleStartsAt(cycle);
	return format(start) + " - " + format(start + kCycleSeconds - 1) + " (UTC)";
}

/*! Randall's line for a place on the running week's ranking. */
std::string tierComment(const int32_t rank)
{
	for (const auto& row : eventData().rankRows)
	{
		if (row.rankType != kRankTotal || row.type != kRewardHunterRank)
			continue;
		if (rank == 0 ? (row.rankMin == 0 && row.rankMax == 0)
			: (row.rankMin > 0 && rank >= row.rankMin && (row.rankMax == 0 || rank <= row.rankMax)))
			return row.comment;
	}
	return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

int64_t cycleAt(const int64_t unixSeconds)
{
	const auto offset = unixSeconds - kCycleEpoch;
	return offset >= 0 ? offset / kCycleSeconds : -((-offset + kCycleSeconds - 1) / kCycleSeconds);
}

int64_t cycleNow()
{
	return cycleAt(static_cast<int64_t>(GameClock::nowSeconds()));
}

int64_t cycleStartsAt(const int64_t cycle)
{
	return kCycleEpoch + cycle * kCycleSeconds;
}

int32_t maxHunterRank()
{
	return eventData().maxHr;
}

int32_t activeEventId()
{
	return eventData().eventId;
}

std::optional<QuestRules> questRules(const int32_t missionId)
{
	const auto& quests = eventData().quests;
	const auto it = quests.find(missionId);
	if (it == quests.end())
		return std::nullopt;
	return it->second;
}

std::vector<int32_t> activeQuests()
{
	std::vector<int32_t> out;
	for (const auto& [mission, rules] : eventData().quests)
		out.push_back(mission);
	return out;
}

drogon::Task<int32_t> loadHunterRank(db::Database database, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT hunter_rank FROM user_info WHERE id = $1;", userId);
	if (rows.empty() || rows[0]["hunter_rank"].isNull())
		co_return kBaseHunterRank;
	co_return std::clamp(rows[0]["hunter_rank"].as<int32_t>(), kBaseHunterRank, maxHunterRank());
}

drogon::Task<LobbyState> loadLobbyState(db::Database database, const std::string userId)
{
	LobbyState state;
	const auto now = cycleNow();

	// A finished week with service and no claim yet puts the lobby on the
	// reward phase: the blue button sends the claim.
	const auto pending = co_await database->execSqlCoro(
		"SELECT cycle FROM user_frohun_scores s WHERE s.user_id = $1 AND s.cycle < $2"
		" AND NOT EXISTS (SELECT 1 FROM user_frohun_claims c WHERE c.user_id = s.user_id AND c.cycle = s.cycle)"
		" GROUP BY cycle HAVING SUM(service) > 0 ORDER BY cycle LIMIT 1;",
		userId, now);
	if (!pending.empty())
	{
		state.stat = kStatRewards;
		state.cycle = pending[0]["cycle"].as<int64_t>();
		state.comment = eventData().endComment;
	}
	else
	{
		state.stat = kStatRunning;
		state.cycle = now;
	}
	state.term = weekTerm(state.cycle);

	const auto total = co_await placing(database, userId, state.cycle, boardExpression(kRankTotal, "0"));
	state.search = total.value;
	state.order = total.rank;
	if (state.stat == kStatRunning)
		state.comment = tierComment(total.rank);
	co_return state;
}

std::string userInfoJson(const LobbyState& state, const int32_t eventId)
{
	std::string out = "{";
	out += "\"TPR79fyI\":" + str(state.search);
	out += ",\"QG5kcBz6\":\"0\"";
	out += ",\"c5yZnpB4\":" + str(eventId);
	out += ",\"j0Uszek2\":" + str(state.stat);
	out += ",\"s2gM3deu\":" + jsonText(state.term);
	out += ",\"2Cn0bWt9\":\"0\"";
	out += ",\"vV4P53m7\":" + str(state.order);
	out += ",\"WDUyI85C\":" + jsonText(state.comment);
	out += "}";
	return out;
}

drogon::Task<std::string> missionBestJson(db::Database database, const std::string userId, const int64_t cycle)
{
	std::map<int32_t, int64_t> best;
	for (const auto mission : activeQuests())
		best[mission] = 0;
	const auto rows = co_await database->execSqlCoro(
		"SELECT mission_id, best_score FROM user_frohun_scores WHERE user_id = $1 AND cycle = $2;",
		userId, cycle);
	for (const auto& row : rows)
	{
		const auto mission = row["mission_id"].as<int32_t>();
		if (best.contains(mission))
			best[mission] = row["best_score"].as<int64_t>();
	}
	std::string out = "[";
	for (const auto& [mission, score] : best)
	{
		if (out.size() > 1)
			out += ",";
		out += "{\"j28VNcUW\":" + str(mission) + ",\"TPR79fyI\":" + str(score) + ",\"QG5kcBz6\":\"1\"}";
	}
	out += "]";
	co_return out;
}

void appendMembers(std::string& body, const std::string& members)
{
	if (members.empty() || body.size() < 2 || body.front() != '{' || body.back() != '}')
		return;
	body.pop_back();
	if (body.size() > 1)
		body += ',';
	body += members;
	body += '}';
}

drogon::Task<void> appendMissionStart(db::Database database, const std::string userId,
	const int32_t missionId, std::string& buffer)
{
	if (!questRules(missionId))
		co_return;
	try
	{
		const auto cycle = cycleNow();
		const auto rows = co_await database->execSqlCoro(
			"SELECT best_score FROM user_frohun_scores WHERE user_id = $1 AND cycle = $2 AND mission_id = $3;",
			userId, cycle, static_cast<int64_t>(missionId));
		const auto best = rows.empty() ? int64_t{ 0 } : rows[0]["best_score"].as<int64_t>();
		// ChallengeStartResponse: the best before this run.  The result screen
		// plays NEW RECORD when the run beats it (setRankSam @0x156E308).
		appendMembers(buffer, "\"7B5Ue9kH\":{\"TPR79fyI\":" + str(std::min<int64_t>(best, INT32_MAX)) + "}"
			+ ",\"cv9rC2KN\":" + co_await missionBestJson(database, userId, cycle));
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "FrontierHunter: MissionStart extras for " << userId << ": " << ex.what();
	}
}

drogon::Task<std::string> settleRun(db::Database transaction, const UserIdentity identity,
	const std::string requestJson, const int32_t missionId, const bool won,
	const int64_t zelGained, const int64_t karmaGained)
{
	const auto rules = questRules(missionId);
	if (!rules)
		co_return std::string{};
	const auto& data = eventData();

	// MissionEndRequest::createBody @0x13A82DC: TPR79fyI is the run's total
	// score as plain decimal, 7cev0fzQ the counter note, pG2n1A28 the last
	// battle cleared.  Read loosely: a missing group scores nothing.
	const raid::RaidRequest request(requestJson);
	const auto* result = request.node("ZYSWU9V1");
	const auto reported = raid::RaidRequest::numberOf(result, "TPR79fyI");
	const auto note = raid::RaidRequest::textOf(result, "7cev0fzQ");
	const auto progress = std::max<int64_t>(raid::RaidRequest::numberOf(result, "pG2n1A28"), 0);
	const auto score = std::clamp<int64_t>(reported, 0, rules->scoreCap);
	if (reported > rules->scoreCap)
	{
		LOG_WARN << "FrontierHunter: " << identity.userId << " reported " << reported << " on " << missionId
			<< ", capped at " << rules->scoreCap;
	}

	std::map<std::string, int64_t> counters;
	for (const auto& pair : split(note, ','))
	{
		const auto colon = pair.find(':');
		if (colon != std::string::npos)
			counters[pair.substr(0, colon)] = std::clamp<int64_t>(parseInt(pair.substr(colon + 1)), 0, 100'000);
	}
	const auto counter = [&](const char* key) { return won ? counters[key] : 0; };

	const auto cycle = cycleNow();
	const auto now = static_cast<int64_t>(GameClock::nowSeconds());
	const auto service = won ? static_cast<int64_t>(std::llround(
		static_cast<double>(score) * rules->servicePercent / 100.0)) : int64_t{ 0 };

	const auto totalBoard = boardExpression(kRankTotal, "0");
	const auto before = co_await placing(transaction, identity.userId, cycle, totalBoard);

	// Each ranked value keeps the time it last grew (*_at): a board's tie goes
	// to whoever reached the value first, not to whoever played last.  In an
	// upsert's SET every bare column is the row before the update.
	const auto recorded = won ? score : int64_t{ 0 };
	const auto sparks = counter("spark"), overkills = counter("overkill"), multikills = counter("multikill"),
		bbkills = counter("bbkill"), oneturns = counter("oneturn");
	const auto at = [now](const int64_t value) { return value > 0 ? now : int64_t{ 0 }; };
	co_await transaction->execSqlCoro(
		"INSERT INTO user_frohun_scores (user_id, cycle, mission_id, event_id, best_score, service, runs, clears,"
		" sparks, overkills, multikills, bbkills, oneturns, updated_at, best_at, service_at, runs_at,"
		" sparks_at, overkills_at, multikills_at, bbkills_at, oneturns_at)"
		" VALUES ($1, $2, $3, $4, $5, $6, 1, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21)"
		" ON CONFLICT (user_id, cycle, mission_id) DO UPDATE SET"
		" best_at = CASE WHEN excluded.best_score > best_score THEN excluded.best_at ELSE best_at END,"
		" best_score = MAX(best_score, excluded.best_score),"
		" service_at = CASE WHEN excluded.service > 0 THEN excluded.service_at ELSE service_at END,"
		" service = service + excluded.service,"
		" runs = runs + 1, runs_at = excluded.runs_at, clears = clears + excluded.clears,"
		" sparks_at = CASE WHEN excluded.sparks > 0 THEN excluded.sparks_at ELSE sparks_at END,"
		" sparks = sparks + excluded.sparks,"
		" overkills_at = CASE WHEN excluded.overkills > 0 THEN excluded.overkills_at ELSE overkills_at END,"
		" overkills = overkills + excluded.overkills,"
		" multikills_at = CASE WHEN excluded.multikills > 0 THEN excluded.multikills_at ELSE multikills_at END,"
		" multikills = multikills + excluded.multikills,"
		" bbkills_at = CASE WHEN excluded.bbkills > 0 THEN excluded.bbkills_at ELSE bbkills_at END,"
		" bbkills = bbkills + excluded.bbkills,"
		" oneturns_at = CASE WHEN excluded.oneturns > 0 THEN excluded.oneturns_at ELSE oneturns_at END,"
		" oneturns = oneturns + excluded.oneturns,"
		" event_id = excluded.event_id,"
		" updated_at = CASE WHEN excluded.clears > 0 THEN excluded.updated_at ELSE updated_at END;",
		identity.userId, cycle, static_cast<int64_t>(missionId), static_cast<int64_t>(rules->eventId),
		recorded, service, int64_t{ won ? 1 : 0 },
		sparks, overkills, multikills, bbkills, oneturns,
		won ? now : int64_t{ 0 }, at(recorded), at(service), now,
		at(sparks), at(overkills), at(multikills), at(bbkills), at(oneturns));

	const auto bestRow = co_await transaction->execSqlCoro(
		"SELECT best_score FROM user_frohun_scores WHERE user_id = $1 AND cycle = $2 AND mission_id = $3;",
		identity.userId, cycle, static_cast<int64_t>(missionId));
	const auto best = bestRow.empty() ? int64_t{ 0 } : bestRow[0]["best_score"].as<int64_t>();

	// POINT REWARDS, once a week each, as the quest's best reaches them
	// (ChallengeRewardScene::setList @0x1573F74 marks a row got when its
	// point_start is at most the best).  Into the present box, and listed on
	// the result screen (ZSC3t4Fv kind 1).  HR +N rows raise the rank.
	std::string rewardRows;
	int32_t hrGain = 0;
	if (won)
	{
		std::set<int32_t> paid;
		for (const auto& row : co_await transaction->execSqlCoro(
			"SELECT reward_id FROM user_frohun_rewards WHERE user_id = $1 AND cycle = $2;",
			identity.userId, cycle))
			paid.insert(row["reward_id"].as<int32_t>());

		const auto it = data.pointRewards.find(missionId);
		if (it != data.pointRewards.end())
		{
			for (const auto& reward : it->second)
			{
				if (reward.pointStart > best || paid.contains(reward.rewardId))
					continue;
				co_await transaction->execSqlCoro(
					"INSERT INTO user_frohun_rewards (user_id, cycle, reward_id, paid_at) VALUES ($1, $2, $3, $4);",
					identity.userId, cycle, static_cast<int64_t>(reward.rewardId), now);
				if (reward.type == kRewardHunterRank)
					hrGain += std::max(reward.count, 0);
				else
					co_await addUserPresent(transaction, identity, reward.type, std::to_string(reward.targetId),
						std::max(reward.count, 1), 0, "Frontier Hunter: survey reward");
				if (!rewardRows.empty())
					rewardRows += ",";
				rewardRows += "{\"IkmC8gG2\":\"1\",\"empaR60j\":" + str(reward.pointStart)
					+ ",\"30Kw4WBa\":" + str(reward.type) + ",\"TdDHf59J\":" + str(reward.targetId)
					+ ",\"wJsB35iH\":" + str(reward.count) + ",\"37moriMq\":" + jsonText(reward.param) + "}";
			}
		}
	}
	co_await raiseHunterRank(transaction, identity, hrGain);
	// Row 0 of ZSC3t4Fv clears the result screen's reward lists, and a kind
	// other than 1 or 2 is dropped (ChallengeRewardResponse::readParam
	// @0x13D3818), so a run with nothing new still clears the last run's.
	if (rewardRows.empty())
		rewardRows = kNoRewardRow;

	// The grade art (setRankSam: "1".."7" = D..SSS) from chlng_mission_grade_mst.
	std::string grade;
	if (won)
	{
		const auto it = data.grades.find(missionId);
		if (it != data.grades.end())
		{
			for (const auto& [threshold, gradeId] : it->second)
			{
				if (score >= threshold)
					grade = std::to_string(gradeId);
			}
		}
	}

	const auto after = co_await placing(transaction, identity.userId, cycle, totalBoard);
	// The result screen counts the header up by this run's service
	// (ChallengeResultScene::initialize @0x15689EC), so the header has to be
	// the week the run went into, even when a finished week is waiting for its
	// claim (a run that ends after Monday 00:00 UTC).  The phase stays.
	auto lobby = co_await loadLobbyState(transaction, identity.userId);
	lobby.search = after.value;
	lobby.order = after.rank;

	LOG_INFO << "FrontierHunter: " << identity.userId << (won ? " cleared " : " lost ") << missionId
		<< " week " << cycle << ": score " << score << ", +" << service << " service (total "
		<< after.value << ", rank " << before.rank << " -> " << after.rank << "), best " << best;

	std::string members;
	// The score shown is the one recorded: the client raises its own cached
	// best from it (ChallengeBase::maxPointUpData) and marks point rewards got
	// against that, so a lost run must not echo the score it reported.
	members += "\"ZYSWU9V1\":{\"TPR79fyI\":" + str(recorded) + ",\"7cev0fzQ\":" + jsonText(note)
		+ ",\"pG2n1A28\":" + str(progress) + "}";
	members += ",\"21BqKn8b\":{\"Cdv07KEU\":" + jsonText(grade)
		+ ",\"Najhr8m6\":" + str(zelGained) + ",\"HTVh8a65\":" + str(karmaGained)
		+ ",\"BSY9X6Mo\":" + str(before.rank) + ",\"vV4P53m7\":" + str(after.rank)
		+ ",\"TPR79fyI\":" + str(service) + ",\"5Z1LNoyH\":\"1.0\"}";
	members += ",\"ZSC3t4Fv\":[" + rewardRows + "]";
	members += ",\"cv9rC2KN\":" + co_await missionBestJson(transaction, identity.userId, cycle);
	members += ",\"s7A3bGLe\":" + userInfoJson(lobby, rules->eventId);
	members += ",\"kN2i7qds\":" + co_await headerJson(transaction, identity);
	co_return members;
}

drogon::Task<std::string> replayRun(db::Database transaction, const UserIdentity identity,
	const int32_t missionId)
{
	const auto rules = questRules(missionId);
	if (!rules)
		co_return std::string{};
	const auto cycle = cycleNow();
	const auto total = co_await placing(transaction, identity.userId, cycle, boardExpression(kRankTotal, "0"));
	auto lobby = co_await loadLobbyState(transaction, identity.userId);
	lobby.search = total.value;
	lobby.order = total.rank;

	std::string members;
	members += "\"21BqKn8b\":{\"Cdv07KEU\":\"\",\"Najhr8m6\":\"0\",\"HTVh8a65\":\"0\",\"BSY9X6Mo\":"
		+ str(total.rank) + ",\"vV4P53m7\":" + str(total.rank) + ",\"TPR79fyI\":\"0\",\"5Z1LNoyH\":\"1.0\"}";
	members += ",\"ZSC3t4Fv\":[" + std::string(kNoRewardRow) + "]";
	members += ",\"cv9rC2KN\":" + co_await missionBestJson(transaction, identity.userId, cycle);
	members += ",\"s7A3bGLe\":" + userInfoJson(lobby, rules->eventId);
	members += ",\"kN2i7qds\":" + co_await headerJson(transaction, identity);
	co_return members;
}

} // namespace gme::frohun
