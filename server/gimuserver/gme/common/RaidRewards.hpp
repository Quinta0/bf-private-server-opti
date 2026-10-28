#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidField.hpp>

#include <vector>

// Classic Raid mission end: the payout and Raid Class progression
// (RAIDS_ROUND2_CORE.md 5.5, 5.6).
//
// Credited at MissionEnd only, so a retire forfeits everything.  A won run
// pays RaidMissionMst zel/karma straight to the header, and its Brave Medals
// (b5yeVr61) and the mission bag (harvested items) to the present box; the
// first clear records raid_clear;
// clearing a class-up mission (RaidMissionMst UJTR1h5r = 1) at the player's
// own RC raises the RC by one and pays that RaidRcMst row's rewards.
namespace gme::raid
{

struct RaidPayout
{
	bool firstClear = false;
	bool rcUp = false;
	int32_t rcBefore = 1;
	std::vector<::RaidMissionRewardInfo> rewards;
};

/*!
* Pay a member's share of a finished run.  Must run inside the caller's
* transaction; nothing here reads outside `database`.
*/
drogon::Task<RaidPayout> payRun(
	const db::Database database, const UserIdentity identity, const RaidRun run);

} // namespace gme::raid
