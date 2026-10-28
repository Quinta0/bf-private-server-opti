#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Classic Raid: the raid blacklist (RaidUpdateBlackList `4igfcyIm`,
// RAIDS_ROUND2_MULTIPLAYER.md 4.9).
//
// raid_blacklist(user_id, target_user_id) holds one row per player the owner
// blocked from a member card (RaidUnitScene::connectBlock @0x19E668C).  It
// only ever hides rooms: room lists, auto-join and RaidEnter skip a room when
// a link exists in EITHER direction, so neither side meets the other again.
// Rooms both are already in are left alone; the lobby warns on its own
// (roomMemberBlackListCheck, withBlackListMemberWindow).
//
// Only real players (user_info rows) can be blocked: the member card is the
// only add path, and members are always real accounts.
namespace gme::raid
{

/*! INFERRED cap (the client shows none); MULTIPLAYER 3.2. */
inline constexpr int32_t kBlacklistMax = 100;

/*! `mnZ5K4Ii` values, OBSERVED: add from RaidUnitScene, remove from 1226 (confirm id 200). 2 is never sent. */
inline constexpr int32_t kBlackListOpAdd = 1;
inline constexpr int32_t kBlackListOpRemove = 3;

enum class BlackListResult
{
	Ok,       // applied, or nothing to do (idempotent, unknown op)
	Full,     // add refused: kBlacklistMax rows already
	Invalid,  // empty target, the caller themself, or no such player
};

/*! One `kKpAdqAu` request node: `KpTZlmkh` target user id, `mnZ5K4Ii` op. */
struct BlackListEdit
{
	std::string targetUserId;
	int32_t op = 0;
};

/*! Read the `kKpAdqAu` group loosely (missing group: empty target, op 0). */
BlackListEdit readBlackListEdit(const RaidRequest& req);

/*!
* Apply one edit: op 1 adds, op 3 removes, any other op is a no-op Ok.  Both
* are idempotent; re-adding a listed player is Ok even when the list is full.
* The cap check and the insert are one statement (MULTIPLAYER 3.3 rule 5).
*/
drogon::Task<BlackListResult> updateBlackList(const db::Database database, const UserIdentity identity,
	const std::string targetUserId, const int32_t op);

/*!
* The caller's full list as `Qazc0Qv8` rows, newest first: each target's
* public card (the same one the Social menu shows), friend type 0.  A target
* whose account is gone is skipped.
*/
drogon::Task<std::vector<::FriendInfo>> blackListRows(const db::Database database, const UserIdentity identity);

/*! Everyone `userId` blocked or was blocked by: the set room search and RaidEnter exclude. */
drogon::Task<std::set<std::string>> blackListLinks(const db::Database database, const std::string userId);

} // namespace gme::raid
