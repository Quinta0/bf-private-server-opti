#include "App.hpp"

#include <gimuserver/gme/common/GameDay.hpp>

#include <atomic>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace gme
{

namespace
{

constexpr DayClock kClocks[] = { DayClock::DailyTasks, DayClock::DailySpin, DayClock::Gifts };
constexpr size_t kClockCount = sizeof(kClocks) / sizeof(kClocks[0]);

// Before loadDayShifts has run.  A real shift is 0 or 1.
constexpr int64_t kUnloaded = std::numeric_limits<int64_t>::min();

std::atomic<int64_t> g_shifts[kClockCount] = { kUnloaded, kUnloaded, kUnloaded };
std::atomic<bool> g_warnedUnloaded{ false };

size_t slot(const DayClock clock)
{
	return static_cast<size_t>(clock);
}

} // namespace

const char* dayClockKey(const DayClock clock)
{
	switch (clock)
	{
	case DayClock::DailyTasks: return "daily_task_day_shift";
	case DayClock::DailySpin:  return "daily_spin_day_shift";
	case DayClock::Gifts:      return "gift_day_shift";
	}
	return "";
}

void loadDayShifts(const drogon::orm::DbClientPtr& db)
{
	// ⚠ NO GUESSING.  The migrations wrote every row before this runs, so a
	// missing row or a failed read means the save is not what this build
	// expects.  Serving on a guessed 0 when the recorded shift is 1 would put
	// every clock a day behind, and the next healthy start would reopen that
	// day for everyone who claimed meanwhile -- so refuse to start.  A THROW,
	// not app().quit(): quit() only queues the teardown, and the listeners
	// would open (and could take a request) before it ran.  Thrown out of the
	// beginning advice it ends run() before startListening, and main exits 1.
	for (const auto clock : kClocks)
	{
		std::string why;
		try
		{
			const auto rows = db->execSqlSync(
				"SELECT value FROM server_state WHERE name = $1;", std::string{ dayClockKey(clock) });
			if (!rows.empty())
			{
				const auto shift = rows[0]["value"].as<int64_t>();
				g_shifts[slot(clock)].store(shift, std::memory_order_relaxed);
				LOG_INFO << "GameDay: " << dayClockKey(clock) << " = " << shift;
				continue;
			}
			why = "no row in server_state";
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			why = ex.base().what();
		}
		LOG_ERROR << "GameDay: cannot read " << dayClockKey(clock) << " (" << why << "); refusing to start";
		throw std::runtime_error(std::string{ "GameDay: cannot read " } + dayClockKey(clock) + ": " + why);
	}
}

int64_t dayShift(const DayClock clock)
{
	const auto shift = g_shifts[slot(clock)].load(std::memory_order_relaxed);
	if (shift != kUnloaded)
		return shift;
	if (!g_warnedUnloaded.exchange(true))
		LOG_ERROR << "GameDay: a day was asked for before loadDayShifts ran; using shift 0";
	return 0;
}

std::string dayKey(const int64_t day)
{
	using namespace std::chrono;
	const year_month_day ymd{ sys_days{ days{ day } } };
	char buf[16]{};
	std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u",
		static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
	return buf;
}

} // namespace gme
