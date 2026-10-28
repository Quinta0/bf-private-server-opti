#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Colosseum state shared by the Colosseum handlers (gme/handlers/Colosseum.cpp)
// and the arena hub reply (ArenaInfo in gme/handlers/Arena.cpp).  Wire shapes:
// packet-generator/assets/net/colosseum.kdl; rules and scene flow:
// docs/features/COLOSSEUM.md.

namespace gme::colosseum
{

/*! Class ids run 1..kClassCount (ColosseumClassMst). */
inline constexpr int32_t kClassCount = 4;

/*! Tickets every player has at the start of a game day (topped up, not added). */
inline constexpr int32_t kDailyTickets = 10;

/*! DefineMst max_colosseum_ticket: the most a player can hold. */
inline constexpr int32_t kMaxTickets = 99;

/*! The running season: the game clock's calendar month, "YYYYMM". */
std::string currentSeason();

/*! "October 2026" for "202610". */
std::string seasonTitle(const std::string& seasonId);

/*! ColosseumClassMst.max_cost of a class: the costliest unit its squads may hold. */
int32_t maxUnitCost(int32_t classId);

/*! The class's display name ("Hero Class"). */
std::string className(int32_t classId);

/*!
* Brings a player's Colosseum rows up to now.  Creates them on the first
* visit, tops the tickets up to kDailyTickets once per game day, and closes
* every class still on a finished season: the final position is archived and
* the season reward goes to the present box, with a summary the Top screen
* shows once.  Every Colosseum entry point calls this first.
*/
drogon::Task<void> sync(db::Database database, const UserIdentity& identity);

/*! HgYvs3am: all four classes, in class order. */
drogon::Task<std::vector<::ColosseumClassInfo>> loadClasses(db::Database database, const std::string& userId);

/*!
* FfBebG0R: exactly one row.  `takeNotice` hands out the pending season
* summary and clears it (the hub reply), otherwise it stays queued.
*/
drogon::Task<std::vector<::ColosseumUserInfo>> loadUserInfo(
	db::Database database, const std::string& userId, bool takeNotice = false);

/*! Q5jXpri7: the running season and every past season the player fought in. */
drogon::Task<std::vector<::ColosseumDurationInfo>> loadDurations(db::Database database, const std::string& userId);

/*! dSC5DzuN: the player's archived season standings. */
drogon::Task<std::vector<::ColosseumPastRanking>> loadPastRankings(db::Database database, const std::string& userId);

/*! 1 + the players above `points` in the class this season; 0 when unranked. */
drogon::Task<int32_t> livePosition(db::Database database, std::string userId, int32_t classId, int32_t points,
	int32_t seasonBattles);

/*! Fills the Colosseum groups of the hub reply (everything but arena_info and team_info). */
drogon::Task<void> fillHub(db::Database database, const UserIdentity& identity, ::ColosseumHubResp& resp);

/*!
* The whole hub reply as JSON: the arena standing, the HUD and every Colosseum
* group.  ArenaInfo answers with it, and ChallengeArenaResetInfo adds it to its
* own: the hub screen (1801) sends only that request before it opens the
* Colosseum, and Class Select / Top use the class rows unchecked.  A Colosseum
* failure is logged and leaves the arena half intact.
*/
drogon::Task<std::string> hubBody(db::Database database, const UserIdentity& identity);

// ---------------------------------------------------------------------------
// The CBP exchange
// ---------------------------------------------------------------------------

/*! One exchange row.  `repeatable` rows never run out (the last stage). */
struct Reward
{
	int32_t id;
	int32_t stageId;
	int32_t presentType;
	const char* targetId;
	int32_t count;
	int32_t needCbp;
	const char* name;
	bool repeatable;
};

/*! The reward with this id, or nullptr. */
const Reward* findReward(int32_t rewardId);

/*!
* U6MWwoW4: the player's stage and the ones after it, in order.  The shop's
* "Go to Next Stage" target is ColosseumRewardStageMstList::getNextStageID,
* which returns the FIRST row whose id differs from the shop's stage, so the
* list must start at the current stage: with all four sent, stage 2 asked for
* stage 1 next and the player could never pass it (ticket #140).
*/
std::vector<::ColosseumRewardStageMst> stageRows(int32_t fromStage);

/*!
* The vault the player is on: the stage they moved on to with "Go to Next
* Stage", or the first one they have not cleared if that is further (clearing
* a vault moves on by itself).  Stages left behind cannot be reopened.
*/
drogon::Task<int32_t> currentStage(db::Database database, const std::string& userId);

/*! The stage after `stageId`, or 0 when it is the last. */
int32_t nextStage(int32_t stageId);

/*! Moves the player on to `stageId`; never backwards. */
drogon::Task<void> moveToStage(db::Database database, const std::string& userId, int32_t stageId);

/*! xNCYRb5G: a stage's rewards with the player's receive status. */
drogon::Task<std::vector<::ColosseumRewardInfo>> rewardRows(
	db::Database database, const std::string& userId, int32_t stageId);

} // namespace gme::colosseum
