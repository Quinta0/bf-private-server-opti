#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/GameDay.hpp>

#include <cstdint>
#include <cstdio>
#include <set>

namespace gme::merit_shop
{

/*!
* EVOLUTION MATERIALS RESTOCK EVERY WEEK (ticket #75).
*
* The Merit Shop's purchase limit (limit_count, S8rdp9zk) is a total per
* player, so an offer bought out was gone for good.  The evolution materials
* now count per WEEK instead: the week turns over on Monday at the game-day
* start, 08:00 UTC (GameDay.hpp), and user_achievement_trades.period holds the
* week a row's count belongs to.  A row from an older week is reset lazily by
* the purchase itself (AchievementTrade's upsert) and reads as 0 meanwhile.
* Every other offer keeps period 0 and its lifetime limit.
*
* Which offers: the ones that pay an evolution material, by the reward's own
* category -- a unit of kind 2 (UnitMst xP9oi5Z2: Nymph, Spirit, Idol, Totem,
* Pot, Bulb, Mecha God, the Mimics) or an item of item_type 4 (ItemMst
* h0K7wjeH, the MST_EVOITEM rows: Legend Stone, the elemental Shards,
* Elementum Tome).  scripts/gen_achievement_mst.py applies the same rule when
* it raises their limit_count to 99.
*
* Rainbow Crystal (unit 750006, kind 1) joins them by name: players asked for
* it in the Discord "Merit Shop Material" thread and the owner agreed
* (2026-10-21).  The generator lists the same id.
*/
inline constexpr int32_t kEvolutionUnitKind = 2;
inline constexpr int32_t kEvolutionItemType = 4;
inline constexpr int32_t kRainbowCrystalUnitId = 750006;

inline constexpr int64_t kSecondsPerWeek = 7 * kSecondsPerDay;

/*! Monday 1970-01-05 08:00 UTC; the epoch fell on a Thursday. */
inline constexpr int64_t kWeekStartSeconds = 4 * kSecondsPerDay + kGameDayStartSeconds;

/*! The shop week a UTC time falls in (always > 0 after 1970-01-12). */
inline int64_t weekAt(const int64_t unixSeconds)
{
	return floorDiv(unixSeconds - kWeekStartSeconds, kSecondsPerWeek);
}

/*! Seconds until the next Monday 08:00 UTC, 1..604800. */
inline int64_t secondsToNextWeek(const int64_t unixSeconds)
{
	return kWeekStartSeconds + (weekAt(unixSeconds) + 1) * kSecondsPerWeek - unixSeconds;
}

/*! Ids of the offers that restock weekly, worked out once from the cache. */
inline const std::set<int32_t>& weeklyOfferIds()
{
	static const std::set<int32_t> ids = []
	{
		const auto& cache = theServer()->cache();
		std::set<int32_t> evoUnits, evoItems;
		for (const auto& unit : cache.unitMst())
			if (unit.kind == kEvolutionUnitKind || unit.id == kRainbowCrystalUnitId)
				evoUnits.insert(unit.id);
		for (const auto& item : cache.itemMst())
			if (item.item_type == kEvolutionItemType)
				evoItems.insert(item.id);

		std::set<int32_t> weekly;
		for (const auto& offer : cache.achievementTradeMst())
		{
			int type = 0, target = 0;
			if (std::sscanf(offer.reward_info.c_str(), "%d:%d", &type, &target) != 2)
				continue;
			const bool evo = type == 6 ? evoUnits.contains(target)
				: (type == 4 || type == 5 || type == 7) && evoItems.contains(target);
			if (evo)
				weekly.insert(offer.id);
		}
		LOG_INFO << "Merit Shop: " << weekly.size() << " evolution offer(s) restock weekly";
		return weekly;
	}();
	return ids;
}

inline bool restocksWeekly(const ::AchievementTradeMst& offer)
{
	return weeklyOfferIds().contains(offer.id);
}

/*! The period a purchase of `offer` made at `unixSeconds` counts toward. */
inline int64_t periodOf(const ::AchievementTradeMst& offer, const int64_t unixSeconds)
{
	return restocksWeekly(offer) ? weekAt(unixSeconds) : 0;
}

} // namespace gme::merit_shop
