#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Classic Raid round 2, band "camp_items": resting at a camp point
// (docs/features/RAIDS_ROUND2_CAMP_ITEMS.md 3).
//
// The camp scene (1256) polls RaidCampRestGet once per RaidWorldMst interval
// and only redraws the HP bars wc2DiMY1 carries; neither the client nor any
// master table has a heal rate, so the rule is a server design (3.5).  As
// everywhere in the raid code nothing ticks: the heal is derived from
// raid_mission_user.healed_until whenever a camp request reads it, so a server
// restart or a missed poll loses nothing.
namespace gme::raid
{

/*! Percent of max HP one camp tick restores (owner decision, CAMP_ITEMS 3.5 / unknown 1). */
inline constexpr int32_t kCampHealPercentPerTick = 10;
/*! RaidPointMst point_type (AZFM14In) of a camp: fleldPointBtnSet @0x198EEC0 shows Rest (111) for it. */
inline constexpr int32_t kCampPointType = 1;
/*! RaidWorldMst interval (Rqc8pi0x) of the shipped data, used when the table is empty. */
inline constexpr int32_t kCampTickFallbackSec = 4;

/*! True when RaidPointMst has `pointNum` on map `mapId` as a camp (point_type 1). */
bool isCampPoint(int32_t mapId, int32_t pointNum);

/*! The camp tick: RaidWorldMst interval_sec, the same period connectCampGet @0x195E404 polls at. */
int32_t campTickSec();

/*! Units' HP after a lazy heal, in input order, and the new heal clock. */
struct CampHeal
{
	std::vector<int64_t> nowHp;
	int64_t healedUntil = 0;
};

/*!
* CAMP_ITEMS 3.5: ticks = floor((now - healedUntil) / tick); every unit gains
* ticks * ceil(max_hp * 10 %) up to max_hp (units at 0 HP too: resting
* revives, an owner decision); the clock advances by whole ticks only, so a
* partial tick carries over to the next poll.  HP is never lowered.  A clock
* that never started (healedUntil <= 0) starts at `now` without healing.
* @param maxAndNowHp One (max_hp, now_hp) pair per unit.
*/
CampHeal campHeal(const std::vector<std::pair<int64_t, int64_t>>& maxAndNowHp,
	int64_t healedUntil, int64_t now, int32_t tickSec);

/*!
* Put a run member into the camp: camp_flg 1, camp_since = healed_until = now.
* Already camping keeps both timers (CAMP_ITEMS 3.4).  The camp-point, live-run
* and not-in-battle checks are the caller's.
* @return true only when this call started the rest (not for a member already camping).
*/
drogon::Task<bool> startCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now);

/*! Apply the lazy heal to the member's raid_mission_party rows; nothing when not camping. */
drogon::Task<void> restAtCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now);

/*! The final heal, then camp_flg 0.  Not camping: nothing changes. */
drogon::Task<void> endCamp(const db::Database database, const int64_t runId, const std::string userId, const int64_t now);

} // namespace gme::raid
