#include "App.hpp"

#include <gimuserver/gme/common/RaidCamp.hpp>

#include <algorithm>
#include <optional>

namespace gme::raid
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (see Raids.cpp): every statement
// below numbers its $N left to right and never repeats one.

namespace
{

/*! One unit after `ticks` camp ticks (CAMP_ITEMS 3.5). */
int64_t healUnit(int64_t maxHp, int64_t nowHp, int64_t ticks)
{
	// Nothing to heal towards, or already full: keep the stored HP as is
	// (a value above max is the field band's to fix, not the camp's to lower).
	if (maxHp <= 0 || nowHp >= maxHp)
		return nowHp;
	const auto hp = std::max<int64_t>(nowHp, 0);
	const auto perTick = (maxHp * kCampHealPercentPerTick + 99) / 100;
	// Compare tick counts instead of multiplying: a long absence must not
	// overflow ticks * perTick.
	const auto ticksToFull = (maxHp - hp + perTick - 1) / perTick;
	return ticks >= ticksToFull ? maxHp : hp + ticks * perTick;
}

struct CampRow
{
	bool camping = false;
	int64_t healedUntil = 0;
};

drogon::Task<std::optional<CampRow>> loadCampRow(const db::Database database, const int64_t runId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT camp_flg, healed_until FROM raid_mission_user WHERE room_mission_id = $1 AND user_id = $2;",
		runId, userId);
	if (rows.empty())
		co_return std::nullopt;
	co_return CampRow{ rows[0]["camp_flg"].as<int32_t>() == 1, rows[0]["healed_until"].as<int64_t>() };
}

} // namespace

bool isCampPoint(int32_t mapId, int32_t pointNum)
{
	// Not always point 1: map 8706 has camps at 1, 10 and 12.
	for (const auto& point : theServer()->cache().raidPointMst())
		if (point.map_id == mapId && point.point_id == pointNum && point.point_type == kCampPointType)
			return true;
	return false;
}

int32_t campTickSec()
{
	for (const auto& world : theServer()->cache().raidWorldMst())
		if (world.interval_sec > 0)
			return world.interval_sec;
	return kCampTickFallbackSec;
}

CampHeal campHeal(const std::vector<std::pair<int64_t, int64_t>>& maxAndNowHp,
	int64_t healedUntil, int64_t now, int32_t tickSec)
{
	CampHeal out{};
	out.healedUntil = healedUntil;
	for (const auto& unit : maxAndNowHp)
		out.nowHp.push_back(unit.second);
	if (healedUntil <= 0)
	{
		out.healedUntil = now;
		return out;
	}
	// A clock in the future (host clock stepped back) or a broken tick heals
	// nothing and leaves the clock alone.
	if (tickSec <= 0 || now <= healedUntil)
		return out;
	const int64_t ticks = (now - healedUntil) / tickSec;
	if (ticks == 0)
		return out;
	out.healedUntil = healedUntil + ticks * tickSec;
	for (size_t i = 0; i < maxAndNowHp.size(); ++i)
		out.nowHp[i] = healUnit(maxAndNowHp[i].first, maxAndNowHp[i].second, ticks);
	return out;
}

drogon::Task<bool> startCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now)
{
	// Conditional on camp_flg 0: a retried Rest neither resets the heal clock
	// nor reports a second start.
	const auto started = co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET camp_flg = 1, camp_since = $1, healed_until = $2"
		" WHERE room_mission_id = $3 AND user_id = $4 AND camp_flg = 0;",
		now, now, runId, userId);
	if (started.affectedRows() == 0)
		co_return false;
	LOG_INFO << "Raid camp: " << userId << " rests in run " << runId;
	co_return true;
}

drogon::Task<void> restAtCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now)
{
	const auto row = co_await loadCampRow(database, runId, userId);
	if (!row || !row->camping)
		co_return;

	// Every unit of the caller heals, the helper included: the camp scene
	// draws all six of the caller's wc2DiMY1 rows.
	const auto party = co_await database->execSqlCoro(
		"SELECT disp_order, max_hp, now_hp FROM raid_mission_party WHERE room_mission_id = $1 AND user_id = $2"
		" ORDER BY disp_order;",
		runId, userId);
	std::vector<std::pair<int64_t, int64_t>> units;
	for (const auto& unit : party)
		units.emplace_back(unit["max_hp"].as<int64_t>(), unit["now_hp"].as<int64_t>());
	const auto heal = campHeal(units, row->healedUntil, now, campTickSec());

	if (heal.healedUntil == row->healedUntil)
		co_return;
	// Claim the ticks first, as the harvest claims its count: of two polls
	// that read the same clock only one heals.  No healing while a battle is
	// open (a boss walked into the camp): the battle owns the party HP.
	const auto claimed = co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET healed_until = $1 WHERE room_mission_id = $2 AND user_id = $3"
		" AND camp_flg = 1 AND healed_until = $4 AND battle_serial = 0;",
		heal.healedUntil, runId, userId, row->healedUntil);
	if (claimed.affectedRows() == 0)
		co_return;
	for (size_t i = 0; i < units.size(); ++i)
	{
		if (heal.nowHp[i] == units[i].second)
			continue;
		co_await database->execSqlCoro(
			"UPDATE raid_mission_party SET now_hp = $1 WHERE room_mission_id = $2 AND user_id = $3 AND disp_order = $4;",
			heal.nowHp[i], runId, userId, party[i]["disp_order"].as<int32_t>());
	}
}

drogon::Task<void> endCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now)
{
	co_await restAtCamp(database, runId, userId, now);
	const auto left = co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET camp_flg = 0 WHERE room_mission_id = $1 AND user_id = $2 AND camp_flg = 1;",
		runId, userId);
	if (left.affectedRows() > 0)
		LOG_INFO << "Raid camp: " << userId << " left the camp in run " << runId;
}

} // namespace gme::raid
