#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/SummonerJournal.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// AUTO-SELL (ticket #27).  A player who turns it on has the units a
// stage drops that are clutter sold at the end of the stage, so a mid-game box
// does not fill with 1-3 star units to sell by hand a hundred at a time.
//
// WHAT SELLS: normal units (UnitMst kind 0) of kMaxRarity stars or fewer.
// Materials (kind 1 enhancement, 2 evolution, 3 sell units) never do: they take
// no box space (Common.hpp materialUnitIds) and stack in the V8+ APK.  A drop is
// never a favourite.  Each one is still created first (addUserUnit records it in
// the Unit Guide), then deleted for UnitMst.sell_price, exactly what UnitSell
// pays, with the same trophy and Journal credit.
//
// THE CLIENT never sees a sold unit: MissionEnd leaves it out of the unit list
// (unit_info) and of the result screen's unit rows (reward_units), which the
// result screen draws from the reply alone.  The Zel arrives in team_info.
//
// THE SWITCH is the Serial Code screen (CampaignReceipt): AUTOSELL turns it on,
// AUTOSELL again turns it off.  user_info.auto_sell, 0 by default.
namespace gme::autosell
{
inline constexpr std::string_view kSerialCode = "AUTOSELL";
inline constexpr int32_t kMaxRarity = 3;   // AUTHORED: 3 star and below
inline constexpr int64_t kMaxZel = 99'999'999;

/*! The unit's UnitMst row, or nullptr. */
inline const UnitMst* unitMstRow(int32_t unitId)
{
	const auto& units = theServer()->cache().unitMst();
	const auto it = std::find_if(units.begin(), units.end(), [unitId](const UnitMst& u) { return u.id == unitId; });
	return it == units.end() ? nullptr : &*it;
}

/*! Whether auto-sell takes this species: a normal unit of kMaxRarity stars or fewer. */
inline bool sells(int32_t unitId)
{
	const auto* unit = unitMstRow(unitId);
	return unit && unit->kind == 0 && unit->rarity >= 1 && unit->rarity <= kMaxRarity;
}

/*! Whether the player has auto-sell on. */
inline drogon::Task<bool> isOn(db::Database database, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro("SELECT auto_sell FROM user_info WHERE id = $1;", userId);
	co_return !rows.empty() && rows[0]["auto_sell"].as<int32_t>() != 0;
}

/*! Flips the switch; returns the new state. */
inline drogon::Task<bool> toggle(db::Database database, const std::string userId)
{
	co_await database->execSqlCoro(
		"UPDATE user_info SET auto_sell = CASE auto_sell WHEN 0 THEN 1 ELSE 0 END WHERE id = $1;", userId);
	co_return co_await isOn(database, userId);
}

/*!
* Sells the drops auto-sell takes, already created by addUserUnit, and drops
* them from `units` and the matching `newFlags`.  `database` is MissionEnd's
* transaction.
*
* @return the Zel credited (0 when nothing sold).
*/
inline drogon::Task<int64_t> sellDrops(db::Database database, const UserIdentity identity,
	std::vector<UserUnitInfo>& units, std::vector<bool>& newFlags)
{
	std::vector<UserUnitInfo> kept;
	std::vector<bool> keptNew;
	int64_t zel = 0;
	int32_t sold = 0;
	for (size_t i = 0; i < units.size(); ++i)
	{
		const auto unitId = static_cast<int32_t>(units[i].unit_id);
		if (!sells(unitId))
		{
			kept.push_back(std::move(units[i]));
			keptNew.push_back(i < newFlags.size() && newFlags[i]);
			continue;
		}
		co_await database->execSqlCoro("DELETE FROM user_units WHERE user_id = $1 AND user_unit_id = $2;",
			identity.userId, static_cast<int64_t>(units[i].user_unit_id));
		zel += unitMstRow(unitId)->sell_price;
		++sold;
	}
	units = std::move(kept);
	newFlags = std::move(keptNew);
	if (sold == 0)
		co_return 0;

	co_await database->execSqlCoro(
		"UPDATE user_info SET zel = MAX(zel, MIN(zel + $1, $2)) WHERE id = $3;", zel, kMaxZel, identity.userId);
	co_await bumpArchiveCounters(database, identity, { { "zel_unit_sale", zel } });
	co_await addJournalProgress(database, identity, kJournalTaskSellUnits, static_cast<int32_t>(zel));
	LOG_INFO << "AutoSell: " << identity.userId << " sold " << sold << " dropped unit(s) for " << zel << " Zel";
	co_return zel;
}
} // namespace gme::autosell
