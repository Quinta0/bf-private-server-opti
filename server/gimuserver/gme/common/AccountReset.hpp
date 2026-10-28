#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <optional>
#include <string>

// A player resets their own account from inside the game (ticket #53,
// docs/features/ACCOUNT_RESET.md).  This client has no reset button, so the
// Add Friend search box carries it: FriendIdSearchFriendRequestScene shows a
// refuseToRetry text as a popup and goes back to the search box
// (FRIENDS_SPEC §1.4), which makes every step a message the server writes.
//
//   1. Search kResetStartCode.  The popup names the account and gives a
//      random 6-digit code that lasts kResetCodeLifeSec.
//   2. Search that code.  A row goes into account_reset_queue.
//   3. An external tool polls the queue and runs the reset itself:
//      backup, snapshot, erase, audit lines.  The erase ends the session,
//      so a client still open goes back to the title; the next start runs the tutorial.
//
// Nothing here changes the save but the queue row, so a mistake on this side
// can at worst queue a reset the player asked for; the tool that runs the
// reset skips an account that is already gone.
namespace gme
{

/*! What the player searches to start.  Real friend IDs are 9 digits from 100000000. */
inline constexpr const char* kResetStartCode = "000000";

/*! How long a confirmation code stays good. */
inline constexpr int64_t kResetCodeLifeSec = 600;

/*!
* The reset's answer to a FriendSearch for `digits`, or nullopt when the
* search is an ordinary one.  The answer is a popup text for refuseToRetry.
*
* @param database Database client.
* @param identity The searching player.
* @param digits   The typed text, already reduced to 1-12 digits.
*/
drogon::Task<std::optional<std::string>> accountResetSearch(
	db::Database database, UserIdentity identity, std::string digits);

} // namespace gme
