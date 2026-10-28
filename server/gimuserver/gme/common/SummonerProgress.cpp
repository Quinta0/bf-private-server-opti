#include "SummonerProgress.hpp"

#include "Common.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace gme
{
namespace
{

/*! SP cap, from the wiki: "A maximum of 99,999 SP can be held at a time." */
constexpr int64_t kSummonerSpCap = 99'999;

/*! "EXP:500,SP:1,ELEM_EXP_RATE:3" -> {EXP: 500, SP: 1, ELEM_EXP_RATE: 3}. */
std::map<std::string, int64_t> unpackEp3Params(const std::string& packed)
{
	std::map<std::string, int64_t> out;
	size_t start = 0;
	while (start < packed.size())
	{
		const auto comma = packed.find(',', start);
		const auto entry = packed.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		const auto colon = entry.find(':');
		if (colon != std::string::npos)
		{
			try
			{
				out[entry.substr(0, colon)] = std::stoll(entry.substr(colon + 1));
			}
			catch (const std::exception&)
			{
				LOG_WARN << "SummonerProgress: unreadable mission_ep3_mst entry '" << entry << "'";
			}
		}
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}

/*! Walks thresholds in level order and returns the last level reached. */
int32_t levelReached(std::vector<std::pair<int32_t, int64_t>> rows, const int64_t exp)
{
	std::sort(rows.begin(), rows.end());
	int32_t level = 1;
	for (const auto& [lv, threshold] : rows)
	{
		if (lv <= level)
			continue;
		if (exp < threshold)
			break;
		level = lv;
	}
	return level;
}

/*! The threshold of the top row, where a total stops growing. */
int64_t topThreshold(const std::vector<std::pair<int32_t, int64_t>>& rows)
{
	const auto top = std::max_element(rows.begin(), rows.end());
	return top == rows.end() ? 0 : top->second;
}

std::vector<std::pair<int32_t, int64_t>> summonerCurve()
{
	std::vector<std::pair<int32_t, int64_t>> rows;
	for (const auto& row : theServer()->cache().summonerLevelMst())
		rows.emplace_back(row.lv, row.exp);
	return rows;
}

std::vector<std::pair<int32_t, int64_t>> elementCurve(const int32_t element)
{
	std::vector<std::pair<int32_t, int64_t>> rows;
	for (const auto& row : theServer()->cache().summonerElementLevelMst())
	{
		if (row.element == element)
			rows.emplace_back(row.lv, row.exp);
	}
	return rows;
}

} // namespace

std::optional<SummonerGain> ep3QuestGain(const int32_t missionId, const bool firstClear)
{
	const auto& rows = theServer()->cache().missionEp3Mst();
	const auto row = std::find_if(rows.begin(), rows.end(),
		[missionId](const ::MissionEp3Mst& r) { return r.id == missionId; });
	if (row == rows.end())
		return std::nullopt;

	const auto params = unpackEp3Params(row->ep3_params);
	const auto get = [&params](const char* key) {
		const auto it = params.find(key);
		return it == params.end() ? int64_t{0} : std::max<int64_t>(it->second, 0);
	};

	SummonerGain gain;
	const auto baseExp = get("EXP");
	gain.expClearBonus = firstClear ? get("FIRST_CLEAR_EXP") : 0;
	gain.exp = baseExp + gain.expClearBonus;
	gain.sp = get("SP");
	gain.elemExp = params.count("ELEM_EXP") ? get("ELEM_EXP") : baseExp * get("ELEM_EXP_RATE") / 100;
	gain.armExp = params.count("ARM_EXP") ? get("ARM_EXP") : baseExp * get("ARM_EXP_RATE") / 100;
	gain.summonSlot = firstClear ? static_cast<int32_t>(get("SUMMON_SLOT")) : 0;
	return gain;
}

SummonerGain regularQuestGain(const int64_t lifetimeBefore, const int64_t lifetimeAfter)
{
	SummonerGain gain;
	if (lifetimeAfter <= lifetimeBefore)
		return gain;
	const auto step = [&](const int64_t share, const int64_t divisor) {
		return lifetimeAfter * share / divisor - lifetimeBefore * share / divisor;
	};
	gain.exp = step(1, kPlayerExpPerSummonerExp);
	gain.elemExp = step(1, kPlayerExpPerSummonerExp * 100);
	gain.armExp = step(3, kPlayerExpPerSummonerExp * 100);
	return gain;
}

int64_t lifetimePlayerExp(const uint32_t level, const uint32_t exp)
{
	// levelUp's convention: row L holds the chunk that takes L-1 to L.
	const auto& progression = theServer()->cache().initializeResp().progression;
	int64_t total = exp;
	for (uint32_t lv = 2; lv <= level && lv <= progression.size(); ++lv)
		total += progression[lv - 1].exp;
	return total;
}

bool isChapter3Quest(const int32_t missionId)
{
	const auto& rows = theServer()->cache().missionEp3Mst();
	return std::any_of(rows.begin(), rows.end(),
		[missionId](const ::MissionEp3Mst& r) { return r.id == missionId; });
}

int32_t summonerLevelFor(const int64_t exp)
{
	return levelReached(summonerCurve(), exp);
}

int32_t elementLevelFor(const int32_t element, const int64_t exp)
{
	return levelReached(elementCurve(element), exp);
}

int64_t summonerLevelUpSp(const int32_t from, const int32_t to)
{
	int64_t sp = 0;
	for (const auto& row : theServer()->cache().summonerLevelMst())
	{
		if (row.lv > from && row.lv <= to)
			sp += std::max(row.get_sp, 0);
	}
	return sp;
}

drogon::Task<SummonerCredit> creditSummoner(
	const db::Database database, const UserIdentity identity, const SummonerGain gain)
{
	SummonerCredit credit;
	credit.gain = gain;

	// A save that never opened the arc has no row yet; the schema defaults are
	// init()'s values, so creating it here changes nothing else it sends.
	co_await database->execSqlCoro(
		"INSERT INTO user_summoner (user_id) VALUES ($1) ON CONFLICT(user_id) DO NOTHING;",
		identity.userId);
	const auto rows = co_await database->execSqlCoro(
		"SELECT level, exp, element, arm_id, sp, summon_limit,"
		" exp1, exp2, exp3, exp4, exp5, exp6,"
		" level1, level2, level3, level4, level5, level6"
		" FROM user_summoner WHERE user_id = $1;", identity.userId);
	if (rows.empty())
		throw std::runtime_error("user_summoner row missing after insert");
	const auto& row = rows[0];

	// SUMMONER LEVEL.  The total stops at the top row, so a capped Summoner's
	// bar reads full rather than overflowing.
	const auto curve = summonerCurve();
	credit.beforeLevel = row["level"].as<int32_t>();
	credit.beforeExp = row["exp"].as<int64_t>();
	const auto newExp = std::min(credit.beforeExp + gain.exp,
		std::max(topThreshold(curve), credit.beforeExp));
	credit.afterLevel = std::max(credit.beforeLevel, levelReached(curve, newExp));
	credit.levelUpSp = summonerLevelUpSp(credit.beforeLevel, credit.afterLevel);

	// SUMMONING ARTS: the current element only (help text, Summoning Arts).
	credit.element = std::clamp(row["element"].as<int32_t>(), 1, 6);
	const auto n = std::to_string(credit.element);
	const auto elemCurve = elementCurve(credit.element);
	credit.beforeElemLevel = row["level" + n].as<int32_t>();
	credit.beforeElemExp = row["exp" + n].as<int64_t>();
	const auto newElemExp = std::min(credit.beforeElemExp + gain.elemExp,
		std::max(topThreshold(elemCurve), credit.beforeElemExp));
	credit.afterElemLevel = std::max(credit.beforeElemLevel, levelReached(elemCurve, newElemExp));

	const auto newSp = std::min(row["sp"].as<int64_t>() + gain.sp + credit.levelUpSp, kSummonerSpCap);
	const auto newSlots = std::max(row["summon_limit"].as<int32_t>(), gain.summonSlot);

	// $1 first: SQLite numbers $N by first appearance (sqlite-placeholder-order).
	// ST points go to the spendable balance and the lifetime total alike.
	const auto st = std::max<int64_t>(gain.stPoints, 0);
	co_await database->execSqlCoro(
		"UPDATE user_summoner SET level = $1, exp = $2, sp = $3, summon_limit = $4,"
		" level" + n + " = $5, exp" + n + " = $6,"
		" friend_point = friend_point + $7, st_points_total = st_points_total + $7"
		" WHERE user_id = $8;",
		credit.afterLevel, newExp, std::max(newSp, row["sp"].as<int64_t>()), newSlots,
		credit.afterElemLevel, newElemExp, st, identity.userId);

	// SUMMONER ARM: only an arm the player holds, and only its EXP.
	credit.armLevel = 1;
	if (gain.armExp > 0)
	{
		const auto armId = row["arm_id"].as<std::string>();
		const auto arm = co_await database->execSqlCoro(
			"SELECT exp, level FROM user_summoner_arms WHERE user_id = $1 AND summoner_arm_id = $2;",
			identity.userId, armId);
		if (!arm.empty())
		{
			credit.beforeArmExp = arm[0]["exp"].as<int64_t>();
			credit.armLevel = arm[0]["level"].as<int32_t>();
			co_await database->execSqlCoro(
				"UPDATE user_summoner_arms SET exp = MIN(exp + $1, 2147483647)"
				" WHERE user_id = $2 AND summoner_arm_id = $3;",
				gain.armExp, identity.userId, armId);
			credit.armCredited = true;
		}
	}

	LOG_INFO << "SummonerProgress: " << identity.userId << " +" << gain.exp << " EXP (Lv "
		<< credit.beforeLevel << " -> " << credit.afterLevel << "), +" << gain.elemExp
		<< " element " << credit.element << " EXP (Lv " << credit.beforeElemLevel << " -> "
		<< credit.afterElemLevel << "), +" << (gain.sp + credit.levelUpSp) << " SP"
		<< (credit.armCredited ? ", +" + std::to_string(gain.armExp) + " arm EXP" : std::string{})
		<< (st > 0 ? ", +" + std::to_string(st) + " ST points" : std::string{});
	co_return credit;
}

drogon::Task<void> seedSummonerDecks(const db::Database database, const std::string userId)
{
	const auto seeded = co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO user_decks"
		" (user_id, user_unit_id, deck_type, deck_num, member_type, disp_order)"
		" WITH squads(n) AS (VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9))"
		" SELECT $1, -1, 4, n, 0, 2 FROM squads"
		" WHERE NOT EXISTS (SELECT 1 FROM user_decks WHERE user_id = $1 AND deck_type = 4);",
		userId);
	if (seeded.affectedRows() > 0)
		LOG_INFO << "SummonerProgress: seeded " << seeded.affectedRows() << " Chapter 3 squad(s) for " << userId;
}

} // namespace gme
