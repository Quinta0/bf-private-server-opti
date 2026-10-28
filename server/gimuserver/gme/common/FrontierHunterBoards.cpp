// Frontier Hunter: the ranking boards (ChallengeRanking).
#include "FrontierHunterDetail.hpp"

#include "Common.hpp"
#include "GameClock.hpp"
#include "HunterOrbs.hpp"
#include "RaidRequest.hpp"
#include "Sandbox.hpp"

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
// Boards.
// ---------------------------------------------------------------------------

BoardSql boardExpression(const int32_t rankType, const std::string& missionId)
{
	if (rankType == kRankTotal)
		return { "SUM(service)", "MAX(service_at)" };
	if (rankType >= kRankQuestFirst && rankType <= kRankQuestLast)
	{
		const auto mission = std::to_string(parseInt(missionId));
		return { "MAX(CASE WHEN mission_id = " + mission + " THEN best_score ELSE 0 END)",
			"MAX(CASE WHEN mission_id = " + mission + " THEN best_at ELSE 0 END)" };
	}
	switch (rankType)
	{
	case 201: return { "SUM(sparks)", "MAX(sparks_at)" };
	case 202: return { "SUM(overkills)", "MAX(overkills_at)" };
	case 203: return { "SUM(multikills)", "MAX(multikills_at)" };
	case 204: return { "SUM(bbkills)", "MAX(bbkills_at)" };
	case 205: return { "SUM(oneturns)", "MAX(oneturns_at)" };
	case 206: return { "SUM(runs)", "MAX(runs_at)" };
	default:  return {};   // 207 "times helped": nothing counts it
	}
}

/*!
* A week's board as a CTE.  Only players with Frontier Service that week are
* placed on any board: they are the ones who can claim it (the lobby and the
* claim both need service), so a place is never held by someone it cannot pay.
*/
/*! The week's scores of one pool only: Sandbox and normal boards (and prizes) never mix. */
std::string boardTable(const BoardSql& board, const bool sandbox)
{
	return "WITH t AS (SELECT user_id, " + board.value + " AS v, " + board.reached + " AS ts"
		" FROM user_frohun_scores WHERE cycle = $1" + sandbox::poolSql("user_id", sandbox)
		+ " GROUP BY user_id HAVING SUM(service) > 0) ";
}

/*!
* A player's value and place on one board of a week.  Ties go to whoever got
* there first, then to the user id, so places are unique.
*/
drogon::Task<Placing> placing(db::Database database, const std::string userId,
	const int64_t cycle, const BoardSql expression)
{
	Placing result;
	if (expression.empty())
		co_return result;
	const bool sandboxed = co_await sandbox::isOn(database, userId);
	const auto rows = co_await database->execSqlCoro(
		boardTable(expression, sandboxed) +
		"SELECT me.v AS v, (SELECT COUNT(*) FROM t o WHERE o.v > 0 AND (o.v > me.v"
		" OR (o.v = me.v AND (o.ts < me.ts OR (o.ts = me.ts AND o.user_id < me.user_id))))) + 1 AS r"
		" FROM t me WHERE me.user_id = $2;",
		cycle, userId);
	if (rows.empty() || rows[0]["v"].isNull())
		co_return result;
	result.value = rows[0]["v"].as<int64_t>();
	if (result.value > 0)
		result.rank = rows[0]["r"].as<int32_t>();
	co_return result;
}

drogon::Task<std::vector<BoardEntry>> topOfBoard(db::Database database, const int64_t cycle,
	const BoardSql expression, const int32_t limit, const bool sandbox)
{
	std::vector<BoardEntry> out;
	if (expression.empty())
		co_return out;
	const auto rows = co_await database->execSqlCoro(
		boardTable(expression, sandbox) +
		"SELECT t.user_id AS user_id, t.v AS v,"
		" (SELECT SUM(runs) FROM user_frohun_scores s WHERE s.cycle = $1 AND s.user_id = t.user_id) AS runs"
		" FROM t WHERE t.v > 0 ORDER BY t.v DESC, t.ts ASC, t.user_id ASC LIMIT $2;",
		cycle, static_cast<int64_t>(limit));
	int32_t order = 0;
	for (const auto& row : rows)
	{
		out.push_back(BoardEntry{ row["user_id"].as<std::string>(), row["v"].as<int64_t>(),
			row["runs"].isNull() ? 0 : row["runs"].as<int64_t>(), ++order });
	}
	co_return out;
}

struct Profile
{
	std::string name = "Summoner";
	int32_t level = 1;
	std::string unitId = "10017";
	int32_t unitLv = 1;
};

/*! Names, levels and party leaders of the players on the boards, in one query. */
drogon::Task<std::unordered_map<std::string, Profile>> loadProfiles(db::Database database,
	const std::set<std::string> userIds)
{
	std::unordered_map<std::string, Profile> out;
	std::string list;
	for (const auto& id : userIds)
	{
		if (!safeId(id))
			continue;
		list += list.empty() ? "'" : ",'";
		list += id + "'";
	}
	if (list.empty())
		co_return out;
	// The leader of the active quest party, as loadPlayerCard picks it.
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.id AS id, i.username AS name, i.level AS level,"
		" (SELECT CAST(u.unit_id AS TEXT) || ':' || u.unit_lvl FROM user_units u"
		"  LEFT JOIN user_decks d ON d.user_id = i.id AND d.user_unit_id = u.user_unit_id"
		"   AND d.deck_type = 1 AND d.deck_num = i.active_deck AND d.member_type = 0"
		"  WHERE u.user_id = i.id ORDER BY (d.user_unit_id IS NOT NULL) DESC, u.unit_lvl DESC LIMIT 1) AS leader"
		" FROM user_info i WHERE i.id IN (" + list + ");");
	for (const auto& row : rows)
	{
		Profile profile;
		profile.name = row["name"].isNull() ? "Summoner" : row["name"].as<std::string>();
		profile.level = row["level"].isNull() ? 1 : row["level"].as<int32_t>();
		if (!row["leader"].isNull())
		{
			const auto parts = split(row["leader"].as<std::string>(), ':');
			if (parts.size() == 2 && parseInt(parts[0]) > 0)
			{
				profile.unitId = parts[0];
				profile.unitLv = static_cast<int32_t>(std::max<int64_t>(parseInt(parts[1], 1), 1));
			}
		}
		out[row["id"].as<std::string>()] = std::move(profile);
	}
	co_return out;
}

/*! One Hmiwj75u row (ChallengeRankingResponse::readParam @0x13D2B44). */
std::string boardRow(const BoardEntry& entry, const Profile& profile, const int32_t infoType,
	const int32_t rankType, const std::string& missionId)
{
	std::string row = "{";
	row += "\"h7eY3sAK\":" + jsonText(entry.userId);
	row += ",\"B5JQyV8j\":" + jsonText(profile.name);
	row += ",\"2Fh3J7ng\":" + str(profile.level);
	row += ",\"pn16CNah\":" + jsonText(profile.unitId);
	row += ",\"4A6LzBxr\":" + str(profile.unitLv);
	row += ",\"U4pMNjy0\":" + str(entry.value);    // the sort key and the PT column
	row += ",\"pG2n1A28\":" + str(0);              // formatted, never drawn
	row += ",\"2Cn0bWt9\":" + str(entry.runs);     // the participation column
	row += ",\"vV4P53m7\":" + str(entry.order);
	row += ",\"NH65Wj0f\":" + str(infoType);       // 0 friends, 1 total, 2 MVP
	row += ",\"9w5e3ZJB\":" + str(rankType);
	row += ",\"j28VNcUW\":" + jsonText(missionId);
	row += ",\"2pAyFjmZ\":" + str(1);
	row += ",\"jkldTrhL\":" + str(0);
	row += ",\"9i2xhMaJ\":" + str(1);
	row += ",\"iNy0ZU5M\":" + str(1);
	row += ",\"btZizNep\":\"1\",\"RVVgyuor\":\"1\"}";
	return row;
}

} // namespace detail

drogon::Task<std::string> rankingJson(db::Database database, const std::string userId)
{
	const auto& data = eventData();
	const auto lobby = co_await loadLobbyState(database, userId);
	const auto cycle = lobby.cycle;
	const bool sandboxed = co_await sandbox::isOn(database, userId);

	struct Board
	{
		int32_t infoType = 0;
		int32_t rankType = 0;
		std::string missionId = "0";
		std::vector<BoardEntry> entries;
	};
	std::vector<Board> boards;
	std::set<std::string> people{ userId };

	// The total board (InfoType 1), plus the viewer's own place below the top
	// 30.  An unplaced viewer gets no total row: the scene copies that row's
	// RankOrder into the header (ChallengeNewRankingScene::initialize ->
	// setOrder), and with none it keeps s7A3bGLe's 0, which hides the rank.
	{
		Board board{ 1, 0, "0", co_await topOfBoard(database, cycle, boardExpression(kRankTotal, "0"), kBoardRows, sandboxed) };
		const bool listed = std::any_of(board.entries.begin(), board.entries.end(),
			[&](const BoardEntry& e) { return e.userId == userId; });
		if (!listed && lobby.order > 0)
		{
			const auto runs = co_await database->execSqlCoro(
				"SELECT COALESCE(SUM(runs), 0) AS n FROM user_frohun_scores WHERE user_id = $1 AND cycle = $2;",
				userId, cycle);
			board.entries.push_back(BoardEntry{ userId, lobby.search, runs[0]["n"].as<int64_t>(), lobby.order });
		}
		boards.push_back(std::move(board));
	}

	// The friend board (InfoType 0): real friends and the viewer, by service.
	{
		std::string friends;
		for (const auto& row : co_await database->execSqlCoro(
			"SELECT other_id FROM user_relations WHERE user_id = $1 AND state = 1"
			+ sandbox::poolSql("other_id", sandboxed) + ";", userId))
		{
			const auto id = row["other_id"].as<std::string>();
			if (!safeId(id))
				continue;
			friends += ",'" + id + "'";
		}
		const auto rows = co_await database->execSqlCoro(
			"SELECT user_id, SUM(service) AS v, SUM(runs) AS runs FROM user_frohun_scores"
			" WHERE cycle = $1 AND user_id IN ($2" + friends + ") GROUP BY user_id"
			" HAVING SUM(service) > 0 OR user_id = $2 ORDER BY v DESC, MAX(service_at) ASC LIMIT " +
			std::to_string(kFriendRows) + ";",
			cycle, userId);
		Board board{ 0, 0, "0", {} };
		bool self = false;
		for (const auto& row : rows)
		{
			const auto id = row["user_id"].as<std::string>();
			self = self || id == userId;
			board.entries.push_back(BoardEntry{ id, row["v"].as<int64_t>(), row["runs"].as<int64_t>(),
				static_cast<int32_t>(board.entries.size() + 1) });
		}
		if (!self)
			board.entries.push_back(BoardEntry{ userId, 0, 0, static_cast<int32_t>(board.entries.size() + 1) });
		boards.push_back(std::move(board));
	}

	// The MVP boards (InfoType 2), one per challenge_mvp_mst row of the event.
	for (const auto& mvp : data.mvpBoards)
	{
		const auto expression = boardExpression(mvp.rankType, mvp.missionId);
		if (expression.empty())
			continue;
		Board board{ 2, mvp.rankType, mvp.missionId, co_await topOfBoard(database, cycle, expression, kBoardRows, sandboxed) };
		if (board.entries.empty())
			continue;
		const bool listed = std::any_of(board.entries.begin(), board.entries.end(),
			[&](const BoardEntry& e) { return e.userId == userId; });
		if (!listed)
		{
			const auto mine = co_await placing(database, userId, cycle, expression);
			board.entries.push_back(BoardEntry{ userId, mine.value, 0, mine.rank > 0 ? mine.rank : kUnplacedOrder });
		}
		boards.push_back(std::move(board));
	}

	for (const auto& board : boards)
		for (const auto& entry : board.entries)
			people.insert(entry.userId);
	const auto profiles = co_await loadProfiles(database, people);

	std::string out = "[";
	for (const auto& board : boards)
	{
		for (const auto& entry : board.entries)
		{
			const auto it = profiles.find(entry.userId);
			if (out.size() > 1)
				out += ",";
			out += boardRow(entry, it == profiles.end() ? Profile{} : it->second,
				board.infoType, board.rankType, board.missionId);
		}
	}
	out += "]";
	co_return out;
}

} // namespace gme::frohun
