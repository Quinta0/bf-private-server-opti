#include "Colosseum.hpp"

#include "App.hpp"

#include <gimuserver/gme/common/Arena.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <set>

namespace gme::colosseum
{

namespace
{

/// Counters are read as 64-bit and saturated, as in gme::arena: as<int32_t>
/// throws past INT_MAX and these rows feed screens other players open.
int32_t counter(const drogon::orm::Field& field)
{
	if (field.isNull())
		return 0;
	const auto value = field.as<int64_t>();
	return static_cast<int32_t>(std::clamp<int64_t>(value, 0, std::numeric_limits<int32_t>::max()));
}

/// The game clock runs on UTC-8 (GameDay.hpp), so a season turns over with the
/// client's own midnight on the first of the month.
std::tm gameClockNow()
{
	const std::time_t t = static_cast<std::time_t>(unixNow() - kGameDayStartSeconds);
	std::tm tm{};
	gmtime_r(&t, &tm);
	return tm;
}

/// Season gems by final position.  Only players who fought that season are
/// placed, so last place still gets something for turning up.
int32_t seasonRewardGems(int32_t position)
{
	if (position == 1)
		return 50;
	if (position <= 3)
		return 30;
	if (position <= 10)
		return 20;
	return 10;
}

/// 1 + the players above `points` in a FINISHED season: those already closed
/// out (user_colosseum_season) and those not yet back since it ended.  Counted
/// in `userId`'s own pool (gme/common/Sandbox.hpp).
drogon::Task<int32_t> finalPosition(
	db::Database database, const std::string& userId, const std::string& seasonId, int32_t classId, int32_t points)
{
	const bool sandboxed = co_await sandbox::isOn(database, userId);
	const auto pool = sandbox::poolSql("user_id", sandboxed);
	const auto archived = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_colosseum_season"
		" WHERE season_id = $1 AND class_id = $2 AND points > $3" + pool + ";",
		seasonId, classId, points);
	const auto pending = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_colosseum_class"
		" WHERE season_id = $1 AND class_id = $2 AND season_battle_cnt > 0 AND points > $3" + pool + ";",
		seasonId, classId, points);
	co_return 1 + counter(archived[0]["n"]) + counter(pending[0]["n"]);
}

struct StageDef
{
	int32_t id;
	const char* name;
	const char* description;
};

constexpr std::array kStages{
	StageDef{ 1, "Bronze Vault", "Spend CBP on the rewards below, then move on to the next vault." },
	StageDef{ 2, "Silver Vault", "Bigger rewards for seasoned gladiators." },
	StageDef{ 3, "Gold Vault", "The Colosseum's finest prizes." },
	StageDef{ 4, "Champion's Market", "Rewards here never run out." },
};

// Present types: 3 zel, 8 gems, 11 karma, 6 unit, 5 material, 7 sphere.
// Units: 10312 Burst Frog, 60133 Metal King, 60134 Metal God, 60224 Metal
// Mimic.  Spheres: 35800 Medulla Gem, 47800 Sacred Crystal.
constexpr std::array kRewards{
	Reward{ 101, 1, 3, "", 100000, 200, "100,000 Zel", false },
	Reward{ 102, 1, 11, "", 5000, 200, "5,000 Karma", false },
	Reward{ 103, 1, 6, "60133", 1, 300, "Metal King", false },
	Reward{ 104, 1, 6, "10312", 2, 400, "2 Burst Frogs", false },
	Reward{ 105, 1, 8, "", 5, 600, "5 Gems", false },
	Reward{ 106, 1, 6, "60134", 1, 800, "Metal God", false },

	Reward{ 201, 2, 3, "", 300000, 600, "300,000 Zel", false },
	Reward{ 202, 2, 11, "", 15000, 700, "15,000 Karma", false },
	Reward{ 203, 2, 6, "10312", 5, 1200, "5 Burst Frogs", false },
	Reward{ 204, 2, 6, "60134", 2, 1500, "2 Metal Gods", false },
	Reward{ 205, 2, 7, "35800", 1, 1500, "Medulla Gem", false },
	Reward{ 206, 2, 8, "", 10, 2000, "10 Gems", false },

	Reward{ 301, 3, 3, "", 1000000, 2000, "1,000,000 Zel", false },
	Reward{ 302, 3, 11, "", 50000, 2500, "50,000 Karma", false },
	Reward{ 303, 3, 6, "60224", 1, 3000, "Metal Mimic", false },
	Reward{ 304, 3, 7, "47800", 1, 3000, "Sacred Crystal", false },
	Reward{ 305, 3, 6, "60134", 5, 4000, "5 Metal Gods", false },
	Reward{ 306, 3, 8, "", 25, 5000, "25 Gems", false },

	Reward{ 401, 4, 3, "", 200000, 300, "200,000 Zel", true },
	Reward{ 402, 4, 11, "", 10000, 400, "10,000 Karma", true },
	Reward{ 403, 4, 6, "10312", 1, 300, "Burst Frog", true },
	Reward{ 404, 4, 6, "60134", 1, 600, "Metal God", true },
	Reward{ 405, 4, 8, "", 1, 400, "1 Gem", true },
};

/// The category every reward sits in.  With no 1x6T2Trz rows the Top
/// screen's Reward button opens the exchange list directly
/// (colosseumRewardSceneMove), skipping the treasure-chest picker.
constexpr int32_t kRewardCategory = 1;

constexpr const char* kClassNames[kClassCount] = { "Hero Class", "Conqueror Class", "Gladiator Class", "Warrior Class" };

/// The one-off rewards a player has exchanged.
drogon::Task<std::set<int32_t>> takenRewards(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT reward_id FROM user_colosseum_reward WHERE user_id = $1;", userId);
	std::set<int32_t> taken;
	for (const auto& row : rows)
		taken.insert(row["reward_id"].as<int32_t>());
	co_return taken;
}

} // namespace

std::string currentSeason()
{
	const auto tm = gameClockNow();
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
	return buf;
}

std::string seasonTitle(const std::string& seasonId)
{
	static constexpr const char* kMonths[] = {
		"January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December",
	};
	if (seasonId.size() != 6)
		return seasonId;
	const auto month = std::atoi(seasonId.substr(4).c_str());
	if (month < 1 || month > 12)
		return seasonId;
	return std::string(kMonths[month - 1]) + " " + seasonId.substr(0, 4);
}

int32_t maxUnitCost(int32_t classId)
{
	for (const auto& row : theServer()->cache().colosseumClassMst())
	{
		if (row.id == classId)
			return row.max_cost;
	}
	return 999;
}

std::string className(int32_t classId)
{
	return classId >= 1 && classId <= kClassCount ? kClassNames[classId - 1] : "Colosseum";
}

drogon::Task<int32_t> livePosition(db::Database database, const std::string userId, int32_t classId, int32_t points,
	int32_t seasonBattles)
{
	if (seasonBattles <= 0)
		co_return 0;
	// Among `userId`'s own pool (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await sandbox::isOn(database, userId);
	const auto rows = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_colosseum_class"
		" WHERE class_id = $1 AND season_id = $2 AND season_battle_cnt > 0 AND points > $3"
		+ sandbox::poolSql("user_id", sandboxed) + ";",
		classId, currentSeason(), points);
	co_return 1 + counter(rows[0]["n"]);
}

drogon::Task<void> sync(db::Database database, const UserIdentity& identity)
{
	const auto& userId = identity.userId;
	const auto season = currentSeason();
	co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO user_colosseum (user_id) VALUES ($1);", userId);
	for (int32_t classId = 1; classId <= kClassCount; ++classId)
	{
		co_await database->execSqlCoro(
			"INSERT OR IGNORE INTO user_colosseum_class (user_id, class_id, season_id) VALUES ($1, $2, $3);",
			userId, classId, season);
	}

	// The day's free tickets: topped up to kDailyTickets, so they never pile up.
	const auto today = gameDayAt(unixNow());
	const auto claimedDay = co_await database->execSqlCoro(
		"UPDATE user_colosseum SET ticket_day = $1 WHERE user_id = $2 AND ticket_day < $1;",
		today, userId);
	if (claimedDay.affectedRows() > 0)
	{
		co_await database->execSqlCoro(
			"UPDATE user_info SET colosseum_tickets = MAX(colosseum_tickets, $1) WHERE id = $2;",
			kDailyTickets, userId);
	}

	// Classes still on a finished season.  The UPDATE is the claim: of two
	// requests racing here only one moves the row, and only that one pays.
	const auto stale = co_await database->execSqlCoro(
		"SELECT class_id, season_id, points, season_battle_cnt FROM user_colosseum_class"
		" WHERE user_id = $1 AND season_id != $2;",
		userId, season);
	std::string notice;
	for (const auto& row : stale)
	{
		const auto classId = row["class_id"].as<int32_t>();
		const auto oldSeason = row["season_id"].as<std::string>();
		const auto points = counter(row["points"]);
		const auto battles = counter(row["season_battle_cnt"]);
		const auto claimed = co_await database->execSqlCoro(
			"UPDATE user_colosseum_class SET season_id = $1, points = 0, season_battle_cnt = 0"
			" WHERE user_id = $2 AND class_id = $3 AND season_id = $4;",
			season, userId, classId, oldSeason);
		if (claimed.affectedRows() == 0 || battles == 0 || oldSeason.empty())
			continue;

		const auto position = co_await finalPosition(database, userId, oldSeason, classId, points);
		co_await database->execSqlCoro(
			"INSERT OR REPLACE INTO user_colosseum_season (user_id, season_id, class_id, points, position, battle_cnt)"
			" VALUES ($1, $2, $3, $4, $5, $6);",
			userId, oldSeason, classId, points, position, battles);
		const auto gems = seasonRewardGems(position);
		co_await addUserPresent(database, identity, 8, "", gems, 0,
			"Colosseum " + seasonTitle(oldSeason) + " season, " + className(classId) + ": #" + std::to_string(position));
		notice += (notice.empty() ? "The " + seasonTitle(oldSeason) + " Colosseum season has ended.<br>" : std::string{})
			+ className(classId) + ": #" + std::to_string(position) + " (" + std::to_string(points) + " CBP), "
			+ std::to_string(gems) + " Gems<br>";
		LOG_INFO << "Colosseum: " << userId << " closed season " << oldSeason << " class " << classId
		         << " at #" << position << " with " << points << " CBP";
	}
	if (!notice.empty())
	{
		notice += "Your rewards are in your Gift Box.";
		co_await database->execSqlCoro(
			"UPDATE user_colosseum SET notice = $1 WHERE user_id = $2;", notice, userId);
	}
}

drogon::Task<std::vector<::ColosseumClassInfo>> loadClasses(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT * FROM user_colosseum_class WHERE user_id = $1 ORDER BY class_id;", userId);
	const auto open = co_await database->execSqlCoro(
		"SELECT class_id, battle_id FROM colosseum_battle WHERE attacker_id = $1 AND status = 0;", userId);

	std::vector<::ColosseumClassInfo> classes;
	for (int32_t classId = 1; classId <= kClassCount; ++classId)
	{
		::ColosseumClassInfo info{};
		info.class_id = classId;
		info.atk_party = "[]";
		info.def_party = "[]";
		info.units = "[]";
		info.atk_formation_id = 101;
		info.atk_support_id = 600;
		info.def_formation_id = 201;
		info.def_support_id = 400;
		for (const auto& r : rows)
		{
			if (r["class_id"].as<int32_t>() != classId)
				continue;
			info.atk_party = r["atk_party"].as<std::string>();
			info.def_party = r["def_party"].as<std::string>();
			info.atk_formation_id = r["atk_formation"].as<int32_t>();
			info.atk_support_id = r["atk_support"].as<int32_t>();
			info.def_formation_id = r["def_formation"].as<int32_t>();
			info.def_support_id = r["def_support"].as<int32_t>();
			info.battle_cnt = counter(r["battle_cnt"]);
			info.win_cnt = counter(r["win_cnt"]);
			info.atk_series_win_cnt = counter(r["atk_series_win"]);
			info.battle_cnt_accept = counter(r["battle_cnt_accept"]);
			info.win_cnt_accept = counter(r["win_cnt_accept"]);
			info.total_cbp = counter(r["points"]);
			info.all_win_cnt = counter(r["all_win_cnt"]);
			info.all_series_win_cnt = counter(r["all_series_win"]);
			info.position = co_await livePosition(database, userId, classId, info.total_cbp, counter(r["season_battle_cnt"]));
		}
		for (const auto& b : open)
		{
			if (b["class_id"].as<int32_t>() == classId)
				info.battle_num = b["battle_id"].as<std::string>();
		}
		classes.push_back(std::move(info));
	}
	co_return classes;
}

drogon::Task<std::vector<::ColosseumUserInfo>> loadUserInfo(
	db::Database database, const std::string& userId, bool takeNotice)
{
	::ColosseumUserInfo info{};
	info.user_id = userId;
	info.duration_id = currentSeason();
	info.stage_id = std::to_string(co_await currentStage(database, userId));
	info.category_id = std::to_string(kRewardCategory);

	const auto me = co_await database->execSqlCoro(
		"SELECT cbp, total_cbp, notice FROM user_colosseum WHERE user_id = $1;", userId);
	if (!me.empty())
	{
		info.cbp = counter(me[0]["cbp"]);
		info.total_cbp = counter(me[0]["total_cbp"]);
		if (takeNotice)
			info.reward_info = me[0]["notice"].as<std::string>();
	}
	if (takeNotice && !info.reward_info.empty())
	{
		co_await database->execSqlCoro(
			"UPDATE user_colosseum SET notice = '' WHERE user_id = $1;", userId);
	}

	const auto sums = co_await database->execSqlCoro(
		"SELECT SUM(battle_cnt) AS b, SUM(win_cnt) AS w, SUM(battle_cnt_accept) AS ba, SUM(win_cnt_accept) AS wa"
		" FROM user_colosseum_class WHERE user_id = $1;", userId);
	if (!sums.empty())
	{
		info.battle_cnt = counter(sums[0]["b"]);
		info.win_cnt = counter(sums[0]["w"]);
		info.battle_cnt_accept = counter(sums[0]["ba"]);
		info.win_cnt_accept = counter(sums[0]["wa"]);
	}
	co_return std::vector<::ColosseumUserInfo>{ std::move(info) };
}

drogon::Task<std::vector<::ColosseumDurationInfo>> loadDurations(db::Database database, const std::string& userId)
{
	std::vector<::ColosseumDurationInfo> durations;
	const auto season = currentSeason();
	durations.push_back({ .duration_id = season, .title = seasonTitle(season) + " Season" });
	const auto past = co_await database->execSqlCoro(
		"SELECT DISTINCT season_id FROM user_colosseum_season WHERE user_id = $1 ORDER BY season_id DESC LIMIT 12;",
		userId);
	for (const auto& row : past)
	{
		const auto id = row["season_id"].as<std::string>();
		durations.push_back({ .duration_id = id, .title = seasonTitle(id) + " Season" });
	}
	co_return durations;
}

drogon::Task<std::vector<::ColosseumPastRanking>> loadPastRankings(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT season_id, class_id, position, points FROM user_colosseum_season"
		" WHERE user_id = $1 ORDER BY season_id DESC, class_id LIMIT 48;", userId);
	std::vector<::ColosseumPastRanking> past;
	for (const auto& row : rows)
	{
		past.push_back({
			.duration_id = row["season_id"].as<std::string>(),
			.class_id = row["class_id"].as<int32_t>(),
			.position = counter(row["position"]),
			.cbp = counter(row["points"]),
		});
	}
	co_return past;
}

drogon::Task<void> fillHub(db::Database database, const UserIdentity& identity, ::ColosseumHubResp& resp)
{
	co_await sync(database, identity);
	resp.classes = co_await loadClasses(database, identity.userId);
	resp.user_info = co_await loadUserInfo(database, identity.userId, true);
	resp.durations = co_await loadDurations(database, identity.userId);
	resp.past_rankings = co_await loadPastRankings(database, identity.userId);
}

drogon::Task<std::string> hubBody(db::Database database, const UserIdentity& identity)
{
	::ColosseumHubResp resp{};
	try
	{
		co_await fillHub(database, identity, resp);
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "Colosseum: hub rows failed for " << identity.userId << ": " << ex.what();
	}
	resp.arena_info = co_await arena::loadArenaInfo(database, identity);
	// After fillHub, so the HUD shows the day's free tickets.
	auto team = (co_await getTeamInfo(database, identity)).nonEmpty();
	team.fight_rest_timer = co_await arena::orbRestSeconds(database, identity.userId);
	resp.team_info = std::move(team);
	co_return glz::write_json(resp).value_or("{}");
}

const Reward* findReward(int32_t rewardId)
{
	for (const auto& reward : kRewards)
	{
		if (reward.id == rewardId)
			return &reward;
	}
	return nullptr;
}

std::vector<::ColosseumRewardStageMst> stageRows(int32_t fromStage)
{
	std::vector<::ColosseumRewardStageMst> rows;
	for (const auto& stage : kStages)
	{
		if (stage.id >= fromStage)
			rows.push_back({ .stage_id = stage.id, .stage_name = stage.name, .description = stage.description });
	}
	return rows;
}

drogon::Task<int32_t> currentStage(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT reward_stage FROM user_colosseum WHERE user_id = $1;", userId);
	const auto movedTo = rows.empty() ? kStages.front().id : rows[0]["reward_stage"].as<int32_t>();

	const auto taken = co_await takenRewards(database, userId);
	auto firstOpen = kStages.back().id;
	for (const auto& stage : kStages)
	{
		const auto open = std::any_of(kRewards.begin(), kRewards.end(), [&](const Reward& reward) {
			return reward.stageId == stage.id && !reward.repeatable && !taken.count(reward.id);
		});
		if (open)
		{
			firstOpen = stage.id;
			break;
		}
	}
	co_return std::clamp(std::max(movedTo, firstOpen), kStages.front().id, kStages.back().id);
}

int32_t nextStage(int32_t stageId)
{
	for (size_t i = 0; i + 1 < kStages.size(); ++i)
	{
		if (kStages[i].id == stageId)
			return kStages[i + 1].id;
	}
	return 0;
}

drogon::Task<void> moveToStage(db::Database database, const std::string& userId, int32_t stageId)
{
	co_await database->execSqlCoro(
		"UPDATE user_colosseum SET reward_stage = $1 WHERE user_id = $2 AND reward_stage < $1;",
		stageId, userId);
}

drogon::Task<std::vector<::ColosseumRewardInfo>> rewardRows(
	db::Database database, const std::string& userId, int32_t stageId)
{
	const auto exchanged = co_await takenRewards(database, userId);
	std::vector<::ColosseumRewardInfo> rewards;
	for (const auto& reward : kRewards)
	{
		if (reward.stageId != stageId)
			continue;
		const auto taken = !reward.repeatable && exchanged.count(reward.id) > 0;
		rewards.push_back({
			.category_id = kRewardCategory,
			.reward_id = reward.id,
			.present_type = reward.presentType,
			.target_id = reward.targetId[0] ? reward.targetId : "0",
			.target_param = "",
			.target_cnt = reward.count,
			.receive_status = taken ? 2 : 1,
			.need_cbp = reward.needCbp,
		});
	}
	co_return rewards;
}

} // namespace gme::colosseum
