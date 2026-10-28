#pragma once

#include <gimuserver/db/Types.h>

#include <drogon/drogon.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct AchievementSubjectMst;
struct UserAchievementSubjectInfo;

// THE DAILY CHALLENGES -- the Challenges tab of the Randall Merit hall,
// achievement category 2 (ticket #18).
//
// WHAT THE ORIGINAL DID.  F_ACHIEVEMENT_SUBJECT_MST shipped a fresh, dated set
// of category-2 rows for every day: 14,192 rows, 2015-05-27 to 2022-07-03,
// each ending at 07:59:59 UTC.  From 2018-12 to 2022-01 the sets follow one
// fixed weekly template -- five challenges a weekday at 400 Merit Points, and
// ten "(Event)" challenges worth 1,000 to 4,000 that run across Saturday and
// Sunday.  A challenge had to be STARTED to count (the 2016 rows say so in
// their own text, "You can complete the challenge at any time after you accept
// it"), at most DefineMst p3pXbuHA = 5 held at once.
//
// THE IDS ARE THE ORIGINAL ONES, and that is not a choice.  The client draws a
// user row only if its own in-memory F_ACHIEVEMENT_SUBJECT_MST has the id
// (UserAchievementSubjectInfoList::addObject @0x12C2B34 drops it silently
// otherwise), and that copy is the v503 .dat download -- the H9ATfJ38 table
// GetAchievementInfo sends is saved to a file nothing reads back.  So the
// server replays one original week (89008881..89008915, the sets that ended
// 2021-06-08 .. 2021-06-14) every week, and the names, targets and Merit
// Points the player sees are the client's own.
//
// THE DAY IS THE CLIENT'S DAY: midnight at UTC-8, i.e. 08:00 UTC all year.
// ServerTimeInfo::onNetworkRequestComplete @0x1CD72B0 stores server time minus
// 28,800 s and getTimeLeftToMidnight counts down in that shifted clock, which
// is exactly where the original rows end (07:59:59 UTC).
//
// WHERE PROGRESS COMES FROM.  The client never reports challenge progress; it
// reports the battle.  MissionEnd's battle log (rXvA1E5y), ArenaBattleEnd's
// result (yTEvm85f) and a won raid run carry every count the template asks
// for, and the handlers call advanceDailyChallenges with what they saw.
namespace gme
{
struct UserIdentity;

namespace challenge
{

/*! The achievement category the Challenges tab asks for (mode 1, category 2). */
inline constexpr int32_t kCategory = 2;

/*! Every category-2 row has cond_type 0, which is the client's bucket key. */
inline constexpr int32_t kCondType = 0;

/*! The challenge day starts this long after midnight UTC. */
inline constexpr int64_t kDayOffsetSeconds = 8 * 3600;

inline constexpr int64_t kSecondsPerDay = 86400;

/*! A run of consecutive original ids, `first` .. `first + count - 1`. */
struct IdRun
{
	int32_t first = 0;
	int32_t count = 0;
};

/*!
* The canonical week, by challenge weekday (0 = Monday).  Saturday and Sunday
* share one 48-hour window, as in the original: no row ends on a Sunday.
*/
inline constexpr std::array<IdRun, 7> kWeek = { {
	{ 89008881, 5 },    // Mon: arena battles, ailments, Earth in arena and quest, arena BB kills
	{ 89008886, 5 },    // Tue: arena battles, Fire in arena and quest, criticals, raid clears
	{ 89008891, 5 },    // Wed: quest clears, BC in one quest, arena wins, Thunder in arena and quest
	{ 89008896, 5 },    // Thu: quest clears, arena BB kills, Water in arena and quest, sparks in one quest
	{ 89008901, 5 },    // Fri: arena overkills, multi-kills, overkills, damage to stronger elements, criticals
	{ 89008906, 10 },   // Sat + Sun: the ten (Event) challenges
	{ 89008906, 10 },
} };

/*! One window of challenges: when it opened, when it closes, what is in it. */
struct Window
{
	int64_t startDay = 0;   //!< challenge day (days since epoch at UTC-8) it opened
	int64_t endsAt = 0;     //!< UTC seconds it closes
	std::vector<int32_t> subjectIds;
};

/*! The challenge day a UTC time falls in. */
inline int64_t dayAt(const int64_t unixSeconds)
{
	const auto shifted = unixSeconds - kDayOffsetSeconds;
	// Floor, not truncation, so a time before the epoch still lands right.
	return shifted >= 0 ? shifted / kSecondsPerDay : -((-shifted + kSecondsPerDay - 1) / kSecondsPerDay);
}

/*! 0 = Monday .. 6 = Sunday.  Day 0 (1970-01-01) was a Thursday. */
inline int32_t weekdayOf(const int64_t day)
{
	return static_cast<int32_t>(((day + 3) % 7 + 7) % 7);
}

/*! The window open at a UTC time. */
inline Window windowAt(const int64_t unixSeconds)
{
	const auto day = dayAt(unixSeconds);
	const auto weekday = weekdayOf(day);
	Window window{};
	// The weekend window opens on Saturday and runs two days.
	window.startDay = weekday == 6 ? day - 1 : day;
	const int64_t length = weekday >= 5 ? 2 : 1;
	window.endsAt = (window.startDay + length) * kSecondsPerDay + kDayOffsetSeconds;
	const auto& run = kWeek[static_cast<size_t>(weekday)];
	for (int32_t i = 0; i < run.count; ++i)
		window.subjectIds.push_back(run.first + i);
	return window;
}

/*! Whether an id belongs to the canonical week at all. */
inline bool isChallengeId(const int64_t subjectId)
{
	return subjectId >= kWeek.front().first
		&& subjectId < kWeek[5].first + kWeek[5].count;
}

/*!
* What a challenge counts, read off its MST row.
*
* ⚠ THE CONDITION IS `disp_order`, NOT THE ID OR `cond_type`.  Every category-2
* row has cond_type 0 and the ids are a plain running number, but disp_order
* names the condition the same way on every row of the eight years: 8500 is
* "Clear N Quests" in 2015 and in 2022.
*/
enum class Kind
{
	Unknown,
	QuestClears,          // 8500 "Clear N Quests."
	RaidClears,           // 9900 "Clear N Raid Quests."
	ArenaBattles,         // 7600 "Battle N times in the Arena."
	ArenaWins,            // 7700 "Win N times in the Arena."
	ArenaFlawless,        // 8100 "Win in the Arena N times with all your Units still standing."
	ArenaBbKills,         // 8200 "Defeat N enemies using your Units' BB in the Arena."
	ArenaOverkills,       // 8300 "Perform N Overkills in the Arena."
	ArenaElementKills,    // 8000 "Defeat N <Element> Type enemies in the Arena." -- "<element>,<N>"
	QuestElementKills,    // 6606 "Defeat N <Element> Type enemies in Quest mode." -- "<element>:<N>"
	QuestEnemies,         // 6500 "Defeat N enemies in Quest mode."
	Overkills,            // 5701 "Perform N Overkills in Quest mode."
	MultiKills,           // 7201 "Perform N Multi-Kills in Quest mode."
	Ailments,             // 7301 "Inflict Status Ailments on your enemies N times in Quest mode."
	Criticals,            // 7401 "Land N Critical Hits in Quest mode."
	StrongerElemDamage,   // 7001 "Dealt N points worth of damage to Units of stronger elements"
	BestSparks,           // 6201 "Produce N Sparks during a single Quest."     -- a high-water mark
	BestBattleCrystals,   // 6301 "Collect N Battle Crystals during a single Quest." -- likewise
};

struct Condition
{
	Kind kind = Kind::Unknown;
	int32_t element = 0;   //!< 1 Fire, 2 Water, 3 Earth, 4 Thunder, 5 Light, 6 Dark
	int64_t target = 0;
	bool highWater = false;   //!< best single quest, not a running total
};

/*! The condition of one category-2 row. */
Condition conditionOf(const ::AchievementSubjectMst& subject);

/*!
* What one finished battle or run did, in the units the conditions count.
* A handler fills what it observed and leaves the rest at 0.
*/
struct Progress
{
	int64_t questClears = 0;
	int64_t raidClears = 0;
	int64_t arenaBattles = 0;
	int64_t arenaWins = 0;
	int64_t arenaFlawless = 0;
	int64_t arenaBbKills = 0;
	int64_t arenaOverkills = 0;
	int64_t questEnemies = 0;
	int64_t overkills = 0;
	int64_t multiKills = 0;
	int64_t ailments = 0;
	int64_t criticals = 0;
	int64_t strongerElemDamage = 0;
	int64_t bestSparks = 0;
	int64_t bestBattleCrystals = 0;
	std::array<int64_t, 7> questElementKills{};   //!< by element code 1..6
	std::array<int64_t, 7> arenaElementKills{};

	bool empty() const;
};

/*! How much of a condition one Progress satisfies. */
int64_t amountFor(const Condition& condition, const Progress& progress);

/*!
* The counts in a MissionEnd body.
*
* @param json The raw request body.
* @param won  Whether the mission was cleared (only a clear is a "Quest Cleared").
*/
Progress progressFromMissionEnd(const std::string& json, bool won);

/*!
* The counts in an ArenaBattleEnd body.
*
* @param json The raw request body.
* @param won  The settled result.
*/
Progress progressFromArenaEnd(const std::string& json, bool won);

} // namespace challenge

/*!
* Adds what a battle did to every challenge this player has STARTED in the
* open window.  Does nothing for challenges not started, and nothing at all
* when `progress` is empty, so a call site can fire unconditionally.
*
* @param database Transaction of the handler that observed the event.
*/
drogon::Task<void> advanceDailyChallenges(
	db::Database database,
	const UserIdentity& identity,
	const challenge::Progress& progress);

/*!
* The Challenges tab's rows, as the client's (2, 0) bucket.
*
* Sends, in order: challenges finished in an earlier window and not yet
* claimed (still claimable: nobody loses Merit Points to the clock), then the
* open window's set -- 0 Start, 1 In Progress or 2 Completed, with the seconds
* left on the window -- and, once each, a state-3 row for a challenge that was
* started and ran out of time, which the list shows as an "expired" notice.
*
* @param database Database client or transaction.
*/
drogon::Task<std::vector<::UserAchievementSubjectInfo>> loadDailyChallengeRows(
	db::Database database,
	const UserIdentity& identity);

/*! Why Start / Give Up did or did not change anything. */
enum class ChallengeAcceptResult
{
	Started,
	GaveUp,
	NotOffered,      //!< not in the open window
	AlreadyHeld,
	CapReached,
	NotGivenUp,      //!< not in progress, so nothing to give up
};

/*!
* AchievementAccept for a category-2 row: flag 1 starts it, 2 gives it up.
*
* Giving up drops the attempt and its progress; the challenge goes back to
* Start, as the client's own Cancel dialog expects.
*/
drogon::Task<ChallengeAcceptResult> acceptDailyChallenge(
	db::Database database,
	const UserIdentity& identity,
	int32_t subjectId,
	int32_t acceptFlag);

/*!
* Latches the reward of one finished, unclaimed challenge -- the oldest one
* with that id.  The caller pays the MST row's points.
*
* @param database The claim's transaction.
* @return Whether a challenge was latched (false: unfinished or already paid).
*/
drogon::Task<bool> claimDailyChallenge(
	db::Database database,
	const UserIdentity& identity,
	int32_t subjectId);

/*! How many challenges are finished and unclaimed, for the Challenges badge. */
drogon::Task<int32_t> dailyChallengesClaimable(
	db::Database database,
	const UserIdentity& identity);

} // namespace gme
