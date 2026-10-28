#pragma once

#include <gimuserver/db/Types.h>
#include <drogon/drogon.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

// SANDBOX MODE (ticket #137).  An account can leave progression
// behind: every area, dungeon, quest, trial and banner opens, and from then on
// it only meets other Sandbox accounts.  Helpers, friend suggestions, Arena,
// Colosseum, raids and the leaderboards are two pools that never mix.
// Friendships made before stay on the Social list; they just stop lending
// helpers across the line.
//
// ONE WAY.  user_info.sandbox_at is when the player confirmed it, 0 for a
// normal account, and no request sets it back (an account reset deletes the
// row).  The Serial Code screen switches it on (CampaignReceipt): SANDBOX once
// explains it and stamps sandbox_asked_at, SANDBOX again within kConfirmSeconds
// confirms.
//
// UNLOCKING works through what the gates read, not gate by gate: for a Sandbox
// account every quest counts as cleared (forGates), so the story's all-of
// lists, the Vortex ladder tiers, the Research Lab order and the Forge's
// rules.json unlocks (and its eras, where built) all pass.  On top of that the
// Vortex lists all seven days and the weekly summon rotation puts its whole pool
// on the rail.  The
// Forge's rules.json schedule windows still apply: a banner it has shut is shut
// for everyone.
namespace gme::sandbox
{
inline constexpr std::string_view kSerialCode = "SANDBOX";
inline constexpr int64_t kConfirmSeconds = 600;

/*! Whether the account is in Sandbox mode (an unknown or NPC id is not). */
drogon::Task<bool> isOn(db::Database database, std::string userId);

/*! Whether two accounts are in the same pool; an NPC id counts as normal. */
drogon::Task<bool> samePool(db::Database database, std::string userId, std::string otherId);

/*!
* SQL that keeps only rows of one pool: " AND <userColumn> IN (SELECT id FROM
* user_info WHERE sandbox_at > 0)", NOT IN for the normal pool, so ids with no
* user_info row (bots, NPCs) stay with normal players.  No placeholders, so it
* can go anywhere in a statement.
*/
std::string poolSql(std::string_view userColumn, bool sandbox);

/*! Every mission id in mission_mst, built once. */
const std::set<int32_t>& everyMission();

/*! The cleared set the unlock gates read: every mission for a Sandbox account. */
inline const std::set<int32_t>& forGates(const std::set<int32_t>& cleared, const bool sandbox)
{
	return sandbox ? everyMission() : cleared;
}
}
