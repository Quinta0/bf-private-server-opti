#pragma once

#include <gimuserver/gme/common/GameDay.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

// The game week: Monday 08:00 UTC to the next Monday 08:00 UTC, the boundary
// the Merit shop restocks on (merit_shop::weekAt numbers weeks the same way).
// Kept apart from MeritShop.hpp so weekly features need not pull in the shop.
namespace gme
{

/*! Seconds in one game week. */
inline constexpr int64_t kGameWeekSeconds = 7 * kSecondsPerDay;

/*!
* Monday 1970-01-05 08:00 UTC, the first week boundary after the epoch.
* 1970-01-01 was a Thursday, so the four days put it on a Monday.
*/
inline constexpr int64_t kGameWeekStartSeconds = 4 * kSecondsPerDay + kGameDayStartSeconds;

/*! Whole game weeks since kGameWeekStartSeconds. */
inline int64_t gameWeekAt(const int64_t unixSeconds)
{
	return floorDiv(unixSeconds - kGameWeekStartSeconds, kGameWeekSeconds);
}

/*! Unix seconds at which a game week starts. */
inline int64_t gameWeekStart(const int64_t week)
{
	return kGameWeekStartSeconds + week * kGameWeekSeconds;
}

/*! The game week starting on a "YYYY-MM-DD" Monday (at 08:00 UTC), or nullopt. */
inline std::optional<int64_t> gameWeekOfDate(const std::string& date)
{
	int y = 0;
	unsigned m = 0, d = 0;
	if (std::sscanf(date.c_str(), "%d-%u-%u", &y, &m, &d) != 3)
		return std::nullopt;
	const std::chrono::year_month_day ymd{ std::chrono::year{ y }, std::chrono::month{ m }, std::chrono::day{ d } };
	if (!ymd.ok() || std::chrono::weekday{ std::chrono::sys_days{ ymd } } != std::chrono::Monday)
		return std::nullopt;
	const int64_t midnight = std::chrono::sys_seconds{ std::chrono::sys_days{ ymd } }.time_since_epoch().count();
	return gameWeekAt(midnight + kGameDayStartSeconds);
}

} // namespace gme
