#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Gifts.hpp>      // giftToday(): the game day key (08:00 UTC)
#include <gimuserver/gme/common/Relations.hpp>  // isRealPlayer
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <chrono>
#include <string>

// Honor for LENDING a helper: what the owner of a borrowed unit earns when
// another player clears a quest with it.
//
// THE ORIGINAL GAME PAID THE LENDER.  DefineMst carries eight helper-Honor
// fields (DefineMstResponse::readParam @0x13D5240-0x13D538C), and every getter
// (DefineMst::getFriendPointRein*, 0x1319ABC-0x1319FDC) has zero callers in the
// client -- the SERVER applied them.  Four are the borrower's side and four,
// the `*My` setters (86sx4FfJ setFriendPointReinFriendMy, VkA3nu0b
// setFriendPointReinOtherMy), are "my" side: the lender's.  In the shipped
// defines_mst.json the lender's pair equals the borrower's (10 friend, 5
// stranger), so the lender earned exactly what the borrower's card promised.
// This server pays the borrower its own authored rates (FriendPoints.hpp), so
// the lender mirrors those.
//
// The client has no screen for it: no text says "your helper was used", and
// UserTeamInfo.friend_point (J3stQ7jd) is an absolute set, so the lender simply
// sees the new balance on their next UserInfo / HomeInfo / UpdateInfoLight,
// and friend_p_get (trophy 100090) on the next UserInfo.
//
// LIMITS (AUTHORED -- the client enforces none, and the original's helper
// cooldown, "won't show up on the list again for a certain period of time"
// in MST_HELP_SUBTOPIC_400_3, has no recorded length):
//   - only a REAL account is credited (user_info), never an NPC id, a story
//     guest or a made-up string -- and never yourself;
//   - once per borrower, lender and game day, however many quests they clear;
//   - at most kLendHonorDailyCap a day per lender, however many borrowers;
//   - only for a cleared battle that settled its own MissionStart (MissionEnd's
//     ONE RESULT PER BATTLE check returns before this is reached).
namespace gme
{

/*! Most Honor one lender earns from lending in a game day.  Authored. */
inline constexpr int32_t kLendHonorDailyCap = 300;

/*! The Honor balance cap, DefineMst max_friend_point (4YCzox9t). */
inline constexpr int32_t kMaxHonorPoints = 999999;

/*! Lend rows older than this many days are pruned; only today's are ever read. */
inline constexpr int32_t kLendHistoryDays = 7;

/*! What the lender earns: the same rate the borrower's card promised. */
inline int32_t lendHonorRate(const bool mutualFriends)
{
	return mutualFriends ? kHonorPerFriendHelper : kHonorPerNormalHelper;
}

/*!
* Credit the owner of a borrowed helper for one cleared quest.
*
* Runs in its OWN transaction on theDb(), so call it after the borrower's
* MissionEnd transaction has closed: a failure here must never roll back the
* borrower's result (drogon rolls a whole transaction back on any failed
* statement, and a MissionEnd error closes the client).  Exactly once without
* that transaction, too: a resent MissionEnd never gets this far, and the
* (lender, borrower, day) key refuses a second row.
*
* @param borrowerId    The player who cleared the quest.
* @param lenderId      The helper MissionStart recorded (reinforce_user_id).
* @param mutualFriends gme::isFriend for the pair -- symmetric for real players.
* @return The Honor credited, 0 when nothing was owed.
*/
inline drogon::Task<int32_t> creditHelperLender(
	const std::string borrowerId,
	const std::string lenderId,
	const bool mutualFriends)
{
	if (!borrowedHelper(lenderId) || lenderId == borrowerId || lenderId.size() > 32)
		co_return 0;
	if (!co_await isRealPlayer(theDb(), lenderId))
		co_return 0;

	// One clock read for the day key and the row's timestamp.
	const auto now = unixNow();
	const auto day = dayKey(dayOf(DayClock::Gifts, now));

	auto transaction = co_await gme::newTransaction(theDb());

	// ⚠ ROWS DATED LATER THAN TODAY COUNT AS TODAY'S, here and in the pair
	// check below: only a rollback past the 08:00 UTC switch or a clock step
	// back makes one, and paying an earlier day after it would pay twice
	// (GameDay.hpp, DayClock).
	const auto used = co_await transaction->execSqlCoro(
		"SELECT COALESCE(SUM(honor), 0) AS n FROM user_helper_lends WHERE user_id = $1 AND day >= $2;",
		lenderId, day);
	const auto spent = used.empty() ? 0 : used[0]["n"].as<int32_t>();
	const auto amount = std::min(lendHonorRate(mutualFriends), kLendHonorDailyCap - spent);
	if (amount <= 0)
		co_return 0;

	// The row FIRST: it is the once-a-day guard, and if it already exists the
	// lender has been paid for this borrower today.
	const auto row = co_await transaction->execSqlCoro(
		"INSERT OR IGNORE INTO user_helper_lends (user_id, borrower_id, day, honor, created_at)"
		" SELECT $1, $2, $3, $4, $5 WHERE NOT EXISTS (SELECT 1 FROM user_helper_lends"
		" WHERE user_id = $1 AND borrower_id = $2 AND day > $3);",
		lenderId, borrowerId, day, amount, now);
	if (row.affectedRows() == 0)
		co_return 0;

	co_await transaction->execSqlCoro(
		"UPDATE user_info SET friend_points = MIN(friend_points + $1, $2) WHERE id = $3;",
		amount, kMaxHonorPoints, lenderId);

	// TROPHY 100090 (Honor earned), as MissionEnd counts the borrower's.
	// Parameterised here rather than through bumpArchiveCounters, which
	// splices its user id into the statement text.
	co_await transaction->execSqlCoro(
		"INSERT INTO user_team_archive (user_id, friend_p_get) VALUES ($1, $2)"
		" ON CONFLICT(user_id) DO UPDATE SET friend_p_get = friend_p_get + excluded.friend_p_get;",
		lenderId, amount);

	co_await transaction->execSqlCoro(
		"DELETE FROM user_helper_lends WHERE user_id = $1 AND day < date($2, $3);",
		lenderId, day, "-" + std::to_string(kLendHistoryDays) + " day");

	LOG_INFO << "creditHelperLender: " << lenderId << " earned " << amount
		<< " Honor for lending to " << borrowerId
		<< (mutualFriends ? " (friends)" : " (not friends)")
		<< ", " << (spent + amount) << "/" << kLendHonorDailyCap << " today";
	co_return amount;
}

} // namespace gme
