#pragma once

#include "Common.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// BATTLE ITEMS: THE LOADOUT AND THE WAREHOUSE ARE DISJOINT.
//
// The client keeps two separate stores and moves counts between them itself
// (libgame.so arm64):
//
//   * ItemEditSelectCntScene::setEqpItem @0x17198C8 returns the slot's old
//     count to the warehouse (GameUtils::incWarehouseItem) and takes the new
//     count out of it (GameUtils::decWarehouseItem); ItemEditListScene::removeEqp
//     returns a slot; ItemBonusListScene::addEqpList does the same for the bonus
//     bar.  GameUtils::eqpItemFull(bool,bool) @0x1188D34 -- run by
//     MissionCheckScene::setParams before every quest -- tops each slot up to
//     ItemMst::getMaxEquipNum (t1i2vIbT) out of the warehouse and queues an
//     ItemEditRequest.  The server only ever sees the resulting loadout.
//   * In battle, MissionBattleManager::useItem @0x1078B0C spends from the
//     LOADOUT (UserEquipItemInfo::useItem @0x128ED50 decrements its count) and
//     records the use in BattleUseItemLogList, which MissionEnd reports as
//     47cJBUxz "itemId:count,...".
//
// So the server has to mirror both moves.  ItemEdit used to replace the loadout
// without touching user_items (every equipped item existed twice -- in the bar
// and still in storage), and MissionEnd ignored the use log (the bar came back
// full at the next login).  Those were the "duplicated unused items" and "kept
// the items I used".
//
// Items a battle SUPPLIES never pass through here: the Battle Simulator's 99s
// (SandbagEquipItemInfoList), a challenge's selected item set
// (BattleState::setChallengeItemSetID), raid and Grand Quest bars all live in
// separate client lists (GameUtils::getBattleEquipItemInfo @0x11A1008), and a
// use log can only ever take items OUT of the owned loadout, never put any in.
namespace gme
{

/// One occupied slot of either bar, as the request / table carries it.
struct LoadoutSlot
{
	int32_t dispOrder = 0;
	int32_t itemId = 0;
	int32_t count = 0;
};

/*!
* Parses MissionEnd's use-item log (47cJBUxz, BattleUseItemLogList::getCsv
* @0x1055330): comma-separated `itemId:count`, both positive decimals.  Empty
* means nothing was used.  Repeated ids are summed.
*
* @throws std::runtime_error on anything else, so a malformed log rejects the
*         whole result instead of being half-applied.
*/
inline std::map<int32_t, int64_t> parseUseItemLog(const std::string& log)
{
	std::map<int32_t, int64_t> used;
	if (log.empty())
		return used;

	const auto number = [](std::string_view text) {
		int32_t value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
			throw std::runtime_error("Invalid use-item log number: " + std::string(text));
		return value;
	};

	size_t at = 0;
	while (true)
	{
		const auto comma = log.find(',', at);
		const std::string_view entry(log.data() + at,
			(comma == std::string::npos ? log.size() : comma) - at);
		const auto colon = entry.find(':');
		if (colon == std::string_view::npos || entry.find(':', colon + 1) != std::string_view::npos)
			throw std::runtime_error("Invalid use-item log entry: " + std::string(entry));
		used[number(entry.substr(0, colon))] += number(entry.substr(colon + 1));
		if (comma == std::string::npos)
			break;
		at = comma + 1;
		if (at == log.size())
			throw std::runtime_error("Trailing use-item log delimiter");
	}
	return used;
}

/*!
* Whether a mission's battle spends the player's OWN loadout.
*
* Dungeon types whose battles run on a supplied item list report uses of THAT
* list, which must not be taken out of the player's bar:
*   1 / 3  normal / SP challenge -- the selected ChallengeItemMst set
*          (GameUtils::isNormalChallengeMission / isSPChallengeMission)
*   6      the Battle Simulator -- "items are unlimited", 99 each
*          (GameUtils::isSandbag @0x1EC18D8)
*/
inline bool missionUsesOwnedLoadout(const int32_t missionId)
{
	const auto& cache = theServer()->cache();
	for (const auto& [dungeonId, missions] : cache.missionsByDungeon())
	{
		if (std::find(missions.begin(), missions.end(), missionId) == missions.end())
			continue;
		for (const auto& dungeon : cache.dungeonMst())
		{
			if (dungeon.dungeon_id != dungeonId)
				continue;
			const auto type = dungeon.dungeon_type;
			return type != 1 && type != 3 && type != 6;
		}
		break;
	}
	return true;
}

/*!
* Spends a battle's reported item uses from the owned loadout.
*
* Each used item comes out of that item's slots in slot order and never takes a
* slot below zero.  Uses beyond what the bar holds are NOT taken from the
* warehouse: the warehouse was never in the battle, and the excess can only be
* a supplied item or a log we cannot trust -- so it is logged and ignored.
* Slots are kept at zero rather than deleted: the client keeps the empty slot
* on its bar, and the next eqpItemFull refills it by id.
*
* @return What was actually taken, item id -> count.
*/
inline drogon::Task<std::map<int32_t, int64_t>> consumeLoadoutItems(
	const db::Database database,
	const UserIdentity identity,
	const std::map<int32_t, int64_t>& used)
{
	std::map<int32_t, int64_t> taken;
	for (const auto& [itemId, count] : used)
	{
		int64_t remaining = count;
		const auto slots = co_await database->execSqlCoro(
			"SELECT disp_order, item_num FROM user_equip_items"
			" WHERE user_id = $1 AND item_id = $2 ORDER BY disp_order;",
			identity.userId, itemId);
		for (const auto& slot : slots)
		{
			if (remaining <= 0)
				break;
			const auto held = slot["item_num"].as<int64_t>();
			const auto take = std::min(held, remaining);
			if (take <= 0)
				continue;
			co_await database->execSqlCoro(
				"UPDATE user_equip_items SET item_num = item_num - $1"
				" WHERE user_id = $2 AND disp_order = $3;",
				take, identity.userId, slot["disp_order"].as<int32_t>());
			remaining -= take;
			taken[itemId] += take;
		}
		if (remaining > 0)
		{
			LOG_WARN << "Battle items: " << identity.userId << " reported using " << count
				<< " of item " << itemId << " but the owned bar held only "
				<< (count - remaining) << "; the rest is not charged to the warehouse";
		}
	}
	co_return taken;
}

/*!
* The battle-item bar (71U5wzhI) as stored, every slot in slot order, zero
* counts kept.  UserInfo reports it, and MissionEnd re-sends it after
* consumeLoadoutItems: the client clears its bar at row 0 and refills it from
* this, so used items leave the bar at once instead of at the next restart
* (ticket #9, where a stale bar let them be used again for free).
*/
inline drogon::Task<std::vector<UserEquipItemInfo>> loadEquipBar(
	const db::Database database,
	const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT disp_order, item_id, item_num FROM user_equip_items"
		" WHERE user_id=$1 ORDER BY disp_order;",
		identity.userId);
	std::vector<UserEquipItemInfo> bar;
	bar.reserve(rows.size());
	for (const auto& row : rows)
	{
		bar.push_back(UserEquipItemInfo{
			.item_id    = row["item_id"].as<uint32_t>(),
			.disp_order = row["disp_order"].as<uint32_t>(),
			.item_num   = row["item_num"].as<uint32_t>(),
		});
	}
	co_return bar;
}

/*!
* An EXP booster in the bonus bar (nAligJSQ): the slot and how much it adds.
*
* Experience Token (120000) and Experience Badge (120001), and the untimed
* Pill/Serum pair, are item_type 5 with skill 79 ("2Smu5Mtq" = the percent):
* "Multiplies experience by 1.5 / 2 after Quest is cleared".  The players put
* them in the quest-prep screen's bonus bar (MissionCheckScene ->
* ItemBonusListScene), which ItemEdit stores in user_equip_bonus_items.  The
* timed pair (Experience Water/Tonic, "50,30" = 30 minutes after use) belongs
* to the time-limit flow (ItemUseTimeLimitRequest), which this server does not
* run, so they never count here.  The client refuses two boosters of the same
* skill in the bar (ItemBonusListScene::checkSameProcessId); if a save holds
* two anyway, the bigger one is used.
*/
struct ExpBooster
{
	int32_t dispOrder = 0;
	int32_t itemId = 0;
	int32_t percent = 0;
};

inline drogon::Task<std::optional<ExpBooster>> findExpBooster(
	const db::Database database,
	const UserIdentity identity)
{
	constexpr const char* kExpBoostSkill = "79";
	const auto rows = co_await database->execSqlCoro(
		"SELECT disp_order, item_id FROM user_equip_bonus_items"
		" WHERE user_id = $1 AND item_num > 0 ORDER BY disp_order;",
		identity.userId);
	const auto& mst = theServer()->cache().itemMst();
	std::optional<ExpBooster> best;
	for (const auto& row : rows)
	{
		const auto itemId = row["item_id"].as<int32_t>();
		const auto it = std::find_if(mst.begin(), mst.end(),
			[itemId](const auto& item) { return item.id == itemId; });
		if (it == mst.end() || it->skill_type != kExpBoostSkill)
			continue;
		const auto& param = it->passive_skill_info;
		int32_t percent = 0;
		const auto [end, ec] = std::from_chars(param.data(), param.data() + param.size(), percent);
		if (ec != std::errc{} || end != param.data() + param.size() || percent <= 0)
			continue;  // a duration ("50,30") or no number: not a per-quest booster
		if (!best || percent > best->percent)
			best = ExpBooster{ row["disp_order"].as<int32_t>(), itemId, percent };
	}
	co_return best;
}

/*!
* Uses up one of the booster in its slot.  The slot stays at zero, like a spent
* battle item: the reply carries the whole bar, and the client's bar is a full
* replace that an empty list would never reach (readParam is not called for
* []), so a deleted last slot would linger on screen.  Auto Equip
* (GameUtils::eqpItemFull) refills a zero slot from storage on the client,
* which ItemEdit then moves over.
*/
inline drogon::Task<void> spendExpBooster(
	const db::Database database,
	const UserIdentity identity,
	const ExpBooster& booster)
{
	co_await database->execSqlCoro(
		"UPDATE user_equip_bonus_items SET item_num = item_num - 1"
		" WHERE user_id = $1 AND disp_order = $2 AND item_num > 0;",
		identity.userId, booster.dispOrder);
}

/*!
* The bonus bar (nAligJSQ) as stored, every slot in slot order, zero counts
* kept.  UserInfo reports it; MissionEnd re-sends it after a booster is used.
*/
inline drogon::Task<std::vector<UserEquipBoostItemInfo>> loadBonusBar(
	const db::Database database,
	const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT disp_order, item_id, item_num FROM user_equip_bonus_items"
		" WHERE user_id=$1 ORDER BY disp_order;",
		identity.userId);
	std::vector<UserEquipBoostItemInfo> bar;
	bar.reserve(rows.size());
	for (const auto& row : rows)
	{
		bar.push_back(UserEquipBoostItemInfo{
			.item_id    = row["item_id"].as<int32_t>(),
			.disp_order = row["disp_order"].as<int32_t>(),
			.item_num   = row["item_num"].as<int32_t>(),
		});
	}
	co_return bar;
}

/*!
* Adds the bonus bar to a serialized reply.  The client reads nAligJSQ from any
* reply (UserEquipBoostItemInfoResponse), so it is spliced in the way
* appendHunterOrbs does rather than declared on MissionEndResp.  Never throws:
* a reply without the bar beats an error after the EXP is paid.
*/
inline void appendBonusBar(std::string& body, const std::vector<UserEquipBoostItemInfo>& bar)
{
	std::string block;
	if (bar.empty() || glz::write_json(bar, block) || body.size() < 2
		|| body.front() != '{' || body.back() != '}')
	{
		if (!bar.empty())
			LOG_WARN << "appendBonusBar: reply left without the bonus bar";
		return;
	}
	body.pop_back();
	if (body.size() > 1)
		body += ',';
	body += "\"nAligJSQ\":";
	body += block;
	body += '}';
}

} // namespace gme
