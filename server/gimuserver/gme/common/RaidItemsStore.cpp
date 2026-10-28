#include "App.hpp"

#include <gimuserver/gme/common/RaidItems.hpp>

#include <algorithm>
#include <charconv>
#include <map>

// The database half of RaidItems.hpp: load and store the raid item lists, and
// the RoonItemEdit warehouse move.  Every function takes the caller's
// transaction as `database` and reads nothing outside it.
namespace gme::raid
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (see Raids.cpp): every statement
// below numbers its $N left to right and never repeats one.

namespace
{

/*! raid_mission_bag.item_id is TEXT (the harvest stores RaidMissionPointMst strings). */
int32_t parseItemId(const std::string& text)
{
	int32_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}

} // namespace

drogon::Task<std::vector<BagStack>> loadBag(const db::Database database, const int64_t runId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT frame_id, item_id, num, new_flg FROM raid_mission_bag WHERE room_mission_id = $1 AND user_id = $2"
		" ORDER BY frame_id;",
		runId, userId);
	std::vector<BagStack> out;
	for (const auto& row : rows)
	{
		BagStack stack{};
		stack.frameId = row["frame_id"].as<int32_t>();
		stack.itemId = parseItemId(row["item_id"].as<std::string>());
		stack.num = row["num"].as<int32_t>();
		stack.newFlg = row["new_flg"].as<int32_t>();
		if (stack.itemId <= 0 || stack.num < 0)
		{
			LOG_WARN << "Raid bag: ignoring frame " << stack.frameId << " of " << userId << " in run " << runId
				<< " (item '" << row["item_id"].as<std::string>() << "', num " << stack.num << ")";
			continue;
		}
		out.push_back(stack);
	}
	co_return out;
}

drogon::Task<void> storeBag(
	const db::Database database, const int64_t runId, const std::string userId, const std::vector<BagStack> bag)
{
	co_await database->execSqlCoro(
		"DELETE FROM raid_mission_bag WHERE room_mission_id = $1 AND user_id = $2;", runId, userId);
	std::set<int32_t> used;
	int32_t spare = 1;
	for (const auto& stack : bag)
		spare = std::max(spare, stack.frameId + 1);
	for (const auto& stack : bag)
	{
		// Emptied stacks are kept at 0 (see bagRows); the harvest tops them
		// up again before opening a new frame (bagAdd).
		if (stack.itemId <= 0 || stack.num < 0)
			continue;
		// The frame id is the row key: a missing or repeated one gets a fresh
		// id instead of failing the insert (and the whole transaction).
		auto frame = stack.frameId;
		if (frame <= 0 || !used.insert(frame).second)
			frame = spare++;
		co_await database->execSqlCoro(
			"INSERT INTO raid_mission_bag(room_mission_id, user_id, frame_id, item_id, num, new_flg)"
			" VALUES ($1, $2, $3, $4, $5, $6);",
			runId, userId, frame, std::to_string(stack.itemId), stack.num, stack.newFlg);
	}
}

drogon::Task<std::vector<SetSlot>> loadSet(const db::Database database, const std::string userId, const int32_t kind)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT disp_order, item_id, item_num FROM raid_equip_items WHERE user_id = $1 AND kind = $2"
		" ORDER BY disp_order;",
		userId, kind);
	std::vector<SetSlot> out;
	for (const auto& row : rows)
		out.push_back({ row["disp_order"].as<int32_t>(), row["item_id"].as<int32_t>(), row["item_num"].as<int32_t>() });
	co_return out;
}

drogon::Task<void> storeSet(
	const db::Database database, const std::string userId, const int32_t kind, const std::vector<SetSlot> set)
{
	co_await database->execSqlCoro("DELETE FROM raid_equip_items WHERE user_id = $1 AND kind = $2;", userId, kind);
	std::set<int32_t> used;
	for (const auto& slot : set)
	{
		// A slot used up in the field is kept at 0 (see setRows); a room edit
		// never sends one (validateSetRows drops count-0 rows).
		if (slot.itemId <= 0 || slot.num < 0)
			continue;
		if (!used.insert(slot.slot).second)
		{
			// Callers validate first; this only keeps a bug from failing the transaction.
			LOG_WARN << "Raid items: " << userId << " list " << kind << " repeats slot " << slot.slot << "; row skipped";
			continue;
		}
		co_await database->execSqlCoro(
			"INSERT INTO raid_equip_items(user_id, kind, disp_order, item_id, item_num) VALUES ($1, $2, $3, $4, $5);",
			userId, kind, slot.slot, slot.itemId, slot.num);
	}
}

drogon::Task<int32_t> spendSetItems(const db::Database database, const std::string userId, const std::string params)
{
	auto set = co_await loadSet(database, userId, kItemSetKind);
	int32_t taken = 0;
	size_t start = 0;
	while (start <= params.size())
	{
		const auto end = std::min(params.find(',', start), params.size());
		const auto entry = params.substr(start, end - start);
		start = end + 1;
		// "0:itemId:useNum" (BattleUseItemLogList::getCsvForRaid).
		const auto first = entry.find(':');
		const auto second = first == std::string::npos ? std::string::npos : entry.find(':', first + 1);
		if (second == std::string::npos)
			continue;
		const auto itemId = parseItemId(entry.substr(first + 1, second - first - 1));
		auto want = std::max(0, parseItemId(entry.substr(second + 1)));
		for (auto& slot : set)
		{
			if (slot.itemId != itemId || itemId <= 0 || want <= 0)
				continue;
			const auto use = std::min(slot.num, want);
			slot.num -= use;
			want -= use;
			taken += use;
		}
	}
	if (taken > 0)
		co_await storeSet(database, userId, kItemSetKind, set);
	co_return taken;
}

drogon::Task<bool> storeRoomItems(const db::Database database, const UserIdentity identity,
	const std::vector<SetSlot> set, const std::vector<SetSlot> reserve)
{
	// The reserve is capped by max_stack, not max_equipped: no client cap for
	// it was located, and conservation below already stops duplication.
	const auto setChecked = validateSetRows(set, "room set", SlotCap::Equipped);
	const auto reserveChecked = validateSetRows(reserve, "room reserve", SlotCap::Stack);
	for (const auto* checked : { &setChecked, &reserveChecked })
	{
		if (const auto* reason = std::get_if<std::string>(checked))
		{
			LOG_WARN << "RaidRoonItemEdit: " << identity.userId << ": " << *reason << "; nothing stored";
			co_return false;
		}
	}
	const auto& newSet = std::get<std::vector<SetSlot>>(setChecked);
	const auto& newReserve = std::get<std::vector<SetSlot>>(reserveChecked);

	// Both lists sit outside the warehouse, so only the per-item difference
	// against what they held before moves (ItemEdit.cpp's rule).
	std::map<int32_t, int64_t> delta;
	for (const auto* list : { &newSet, &newReserve })
		for (const auto& [itemId, count] : setTotals(*list))
			delta[itemId] += count;
	const auto oldSet = co_await loadSet(database, identity.userId, kItemSetKind);
	const auto oldReserve = co_await loadSet(database, identity.userId, kItemReserveKind);
	for (const auto* list : { &oldSet, &oldReserve })
		for (const auto& [itemId, count] : setTotals(*list))
			delta[itemId] -= count;

	for (const auto& [itemId, change] : delta)
	{
		if (change <= 0)
			continue;
		const auto stock = co_await database->execSqlCoro(
			"SELECT item_num FROM user_items WHERE user_id = $1 AND item_id = $2;", identity.userId, itemId);
		const auto held = stock.empty() ? int64_t{ 0 } : stock[0]["item_num"].as<int64_t>();
		if (held < change)
		{
			LOG_WARN << "RaidRoonItemEdit: " << identity.userId << " asked for " << change << " more of item "
				<< itemId << " but storage holds " << held << "; nothing stored";
			co_return false;
		}
	}

	for (const auto& [itemId, change] : delta)
	{
		// A row spent to zero is kept: the client still references its
		// instance id (the warehouse rule in Common.hpp, as ItemEdit.cpp).
		if (change > 0)
			co_await database->execSqlCoro(
				"UPDATE user_items SET item_num = item_num - $1 WHERE user_id = $2 AND item_id = $3;",
				change, identity.userId, itemId);
		else if (change < 0)
			co_await gme::addUserItem(database, identity, static_cast<uint32_t>(itemId), static_cast<uint32_t>(-change));
	}
	co_await storeSet(database, identity.userId, kItemSetKind, newSet);
	co_await storeSet(database, identity.userId, kItemReserveKind, newReserve);
	LOG_INFO << "RaidRoonItemEdit: stored " << newSet.size() << " set and " << newReserve.size()
		<< " reserve slot(s) for " << identity.userId;
	co_return true;
}

} // namespace gme::raid
