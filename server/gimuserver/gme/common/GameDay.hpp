#pragma once

#include <drogon/orm/DbClient.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace gme
{

/*!
* THE GAME DAY STARTS AT 08:00 UTC, all year.
*
* The client keeps its own clock on UTC-8 with no daylight saving:
* ServerTimeInfo::onNetworkRequestComplete @0x1CD72B0 stores the server time
* minus 28800, getTimeLeftToMidnight counts down to midnight on that clock, and
* every "resets in" timer reads it — the News board labels it "Server Time
* ... PST", and the Brave Points screen's "Time till refresh" counted down to
* 08:00 UTC on emulator-5558 (21:26 left at 10:33 UTC).  A server day that
* turns over at 00:00 UTC is therefore 8 hours off from what the player is
* told, so daily features key on this day instead.
*
* Same boundary as gme::challenge::dayAt (daily Challenges).
*/
inline constexpr int64_t kGameDayStartSeconds = 8 * 3600;

inline constexpr int64_t kSecondsPerDay = 86400;

/*! Seconds since the epoch, now. */
inline int64_t unixNow()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

/*! Floor division, so a time before the epoch still lands on the right day. */
inline int64_t floorDiv(const int64_t value, const int64_t divisor)
{
	return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

/*!
* The game day a UTC time falls in: day N runs from N * 86400 + 08:00 UTC to
* the next 08:00 UTC.
*/
inline int64_t gameDayAt(const int64_t unixSeconds)
{
	return floorDiv(unixSeconds - kGameDayStartSeconds, kSecondsPerDay);
}

/*! The old midnight-UTC day a time falls in, which stored day numbers used. */
inline int64_t utcDayAt(const int64_t unixSeconds)
{
	return floorDiv(unixSeconds, kSecondsPerDay);
}

/*!
* What to add to gameDayAt() so a feature that numbered its days by utcDayAt()
* keeps the day running at `unixSeconds` on the number it already had when it
* moves to the game day: 1 before 08:00 UTC, 0 from 08:00 on.  See DayClock
* for why that matters.
*/
inline int64_t gameDayShiftAt(const int64_t unixSeconds)
{
	return utcDayAt(unixSeconds) - gameDayAt(unixSeconds);
}

/*!
* THE DAILY FEATURES THAT TURN OVER ON THE GAME DAY.
*
* ⚠ THEIR DAY IS STORED.  Each keeps "today" in its rows — a day number
* (user_daily_tasks.utc_day, user_daily_spin.last_spin_utc_day) or a
* "YYYY-MM-DD" key (giftToday()) — and some seed a daily choice on it, so
* moving the boundary must not renumber the day that is running when the new
* rule goes live:
*
*   * numbered gameDayAt() as-is, a switch between 00:00 and 08:00 UTC drops
*     back to YESTERDAY — what was already claimed today could be claimed
*     again once the clock passes 08:00;
*   * numbered one higher, a switch between 08:00 and 24:00 jumps to
*     TOMORROW — today's progress and limits reset at the deploy.
*
* So the migration that moved a clock recorded, in server_state under
* dayClockKey(), the shift that keeps the running day on the number it
* already had: gameDayShiftAt(<when it ran>), 1 before 08:00 UTC and 0 after.
* That day then ends at the next 08:00 UTC (8 or 32 hours after it began) and
* every day after it is a whole game day.  A recorded shift never changes, and
* each clock has its own because each moved at its own deploy.
*
* ⚠ DO NOT ROLL PROD BACK PAST THOSE MIGRATIONS, and retag bf-server:prev from
* a build that has them once they are live.  An older build numbers the days
* by midnight UTC, which agrees with a recorded numbering for only part of each
* day, so a rollback can reopen what was already claimed that day.  Nor does
* rolling forward again undo it: with a shift of 0 the older build's rows are
* dated LATER than the clock and are held (the features never move a stored
* day backwards), but with a shift of 1 the rows it writes between 08:00 and
* 24:00 UTC are dated one day EARLIER, and those players can claim again the
* same game day once this build is back.
*/
enum class DayClock
{
	DailyTasks,   //!< Brave Points tasks (DailyTask.cpp)
	DailySpin,    //!< the login spin (DailySpin.cpp)
	Gifts,        //!< giftToday(): login bonus, friend gifts, helper lends, friend picker
};

/*! server_state.name holding a clock's recorded shift. */
const char* dayClockKey(DayClock clock);

/*!
* Reads every clock's recorded shift from server_state.  Call once at boot,
* after MigrationManager::RunMigrations and before any request is served.
* Throws std::runtime_error when a shift cannot be read: the server must not
* start on a guessed day.
*
* @param db Database client.
*/
void loadDayShifts(const drogon::orm::DbClientPtr& db);

/*! A clock's recorded shift, 0 or 1. */
int64_t dayShift(DayClock clock);

/*! The day a clock is on at a UTC time. */
inline int64_t dayOf(const DayClock clock, const int64_t unixSeconds)
{
	return gameDayAt(unixSeconds) + dayShift(clock);
}

/*! The day a clock is on now. */
inline int64_t currentDay(const DayClock clock)
{
	return dayOf(clock, unixNow());
}

/*! A day number as "YYYY-MM-DD", the civil date that numbers it. */
std::string dayKey(int64_t day);

/*!
* Today's UTC date as "YYYY-MM-DD", for RECORD dates rather than daily limits
* (a guild's founding date is shown on Guild Detail).  Not a DayClock key: with
* a recorded shift of 1 that key reads tomorrow's date for most of the day.
*/
inline std::string utcDateToday()
{
	return dayKey(utcDayAt(unixNow()));
}

} // namespace gme
