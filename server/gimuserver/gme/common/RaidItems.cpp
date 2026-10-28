#include "App.hpp"

#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidItems.hpp>

#include <algorithm>
#include <limits>
#include <map>

namespace gme::raid
{

namespace
{

/*!
* Sanity bound on the stacks one bagAdd may append.  The client's incBagItem
* has no cap at all and every server caller adds a handful of items (a set
* coming back, one craft, one harvest), so this only stops a corrupt count
* from allocating millions of stacks.
*/
constexpr int32_t kBagAddStackLimit = 1000;

const ::ItemMst* findItem(int32_t itemId)
{
	if (itemId <= 0)
		return nullptr;
	for (const auto& row : theServer()->cache().itemMst())
		if (row.id == itemId)
			return &row;
	return nullptr;
}

/*! The recipe list ItemMix.cpp resolves ids against (initializeResp().receipe). */
const ::ReceipeMst* findRecipe(int32_t recipeId)
{
	for (const auto& row : theServer()->cache().initializeResp().receipe)
		if (row.id == recipeId)
			return &row;
	return nullptr;
}

int32_t toCount(int64_t value)
{
	return static_cast<int32_t>(std::clamp<int64_t>(value, 0, std::numeric_limits<int32_t>::max()));
}

int32_t narrow(int64_t value)
{
	const bool fits = value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max();
	return fits ? static_cast<int32_t>(value) : -1;
}

int32_t nextFrameId(const std::vector<BagStack>& bag)
{
	int32_t top = 0;
	for (const auto& stack : bag)
		top = std::max(top, stack.frameId);
	return top + 1;
}

/*! A recipe a raid mix may use: its product and per-craft materials. */
struct MixPlan
{
	int32_t productId = 0;
	std::map<int32_t, int64_t> cost;
};

/*! The checks of CAMP_ITEMS 4.3 that do not depend on the bag; nullopt (logged) stops the order. */
std::optional<MixPlan> mixPlan(int32_t recipeId, const std::set<int32_t>& permittedRecipes)
{
	const auto* recipe = findRecipe(recipeId);
	if (recipe == nullptr || !permittedRecipes.count(recipeId))
	{
		LOG_WARN << "Raid mix: recipe " << recipeId << (recipe ? " is not unlocked" : " is unknown");
		return std::nullopt;
	}
	// The mix list only offers recipes whose product is a consumable (1) or a
	// material (2) (RaidItemMixListScene::setRecipeList @0x19F9DC4).
	const auto* product = findItem(recipe->item_id);
	const auto materials = parseMixMaterials(recipe->materials);
	// A once-only recipe (craft cap >= 1, ReceipeMst.required_material_amount:
	// Memory Fragment, Toy Medal) is crafted in town, where its cap is counted
	// (ItemMix, ticket #166); the bag cannot see that count.
	if (recipe->required_material_amount > 0)
	{
		LOG_WARN << "Raid mix: recipe " << recipeId << " is once-only; refused in a raid";
		return std::nullopt;
	}
	if (product == nullptr || (product->item_type != 1 && product->item_type != 2) || !materials
		|| materials->empty())
	{
		// An empty material list would craft from nothing: never a raid recipe.
		LOG_WARN << "Raid mix: recipe " << recipeId << " (item " << recipe->item_id << ") cannot be mixed in a raid";
		return std::nullopt;
	}
	return MixPlan{ recipe->item_id, *materials };
}

/*! One craft's materials out of the bag, smallest stacks first; nullopt when any is short. */
std::optional<std::vector<BagStack>> drainMaterials(const std::vector<BagStack>& bag, const std::map<int32_t, int64_t>& cost)
{
	auto out = bag;
	for (const auto& [itemId, qty] : cost)
	{
		auto taken = bagTake(out, itemId, toCount(qty));
		if (!taken)
			return std::nullopt;
		out = std::move(*taken);
	}
	return out;
}

} // namespace

int32_t itemMaxStack(int32_t itemId)
{
	const auto* item = findItem(itemId);
	return item == nullptr ? 0 : std::max(1, item->max_stack);
}

int32_t itemMaxEquipped(int32_t itemId)
{
	const auto* item = findItem(itemId);
	return item == nullptr ? 0 : std::max(0, item->max_equipped);
}

std::variant<std::vector<SetSlot>, std::string> validateSetRows(
	const std::vector<SetSlot>& slots, const std::string& list, const SlotCap capKind)
{
	std::vector<SetSlot> out;
	std::set<int32_t> used;
	for (const auto& slot : slots)
	{
		if (slot.itemId == 0 || slot.num == 0)
			continue;
		const auto where = list + " slot " + std::to_string(slot.slot);
		if (slot.slot < 0 || slot.slot >= kItemSetSlots)
			return where + " is outside 0.." + std::to_string(kItemSetSlots - 1);
		if (!used.insert(slot.slot).second)
			return where + " is repeated";
		if (findItem(slot.itemId) == nullptr)
			return where + " names unknown item " + std::to_string(slot.itemId);
		const auto cap = capKind == SlotCap::Equipped ? itemMaxEquipped(slot.itemId) : itemMaxStack(slot.itemId);
		if (slot.num < 0 || slot.num > cap)
			return where + " holds " + std::to_string(slot.num) + " of item " + std::to_string(slot.itemId)
				+ " (cap " + std::to_string(cap) + ")";
		out.push_back(slot);
	}
	std::sort(out.begin(), out.end(), [](const SetSlot& a, const SetSlot& b) { return a.slot < b.slot; });
	return out;
}

std::map<int32_t, int64_t> setTotals(const std::vector<SetSlot>& slots)
{
	std::map<int32_t, int64_t> out;
	for (const auto& slot : slots)
		if (slot.itemId > 0 && slot.num > 0)
			out[slot.itemId] += slot.num;
	return out;
}

std::vector<BagStack> bagAdd(const std::vector<BagStack>& bag, int32_t itemId, int32_t n)
{
	auto out = bag;
	const auto maxStack = itemMaxStack(itemId);
	if (n <= 0 || maxStack <= 0)
	{
		if (n > 0)
			LOG_WARN << "Raid bag: dropped " << n << " of unknown item " << itemId;
		return out;
	}
	auto left = n;
	// Top up the item's stacks in list order first (emptied ones included:
	// the client keeps them in its list at 0 until the next bag reply) ...
	for (auto& stack : out)
	{
		if (left == 0)
			break;
		if (stack.itemId != itemId || stack.num >= maxStack)
			continue;
		const auto moved = std::min(left, maxStack - std::max(stack.num, 0));
		stack.num = std::max(stack.num, 0) + moved;
		left -= moved;
	}
	// ... then new stacks.  The client names them 0x7fffffff - itemId until
	// the next bag reply replaces its list; the server mints real ids.
	auto frame = nextFrameId(out);
	for (int32_t appended = 0; left > 0 && appended < kBagAddStackLimit; ++appended)
	{
		const auto moved = std::min(left, maxStack);
		out.push_back({ frame++, itemId, moved, 1 });
		left -= moved;
	}
	if (left > 0)
		LOG_WARN << "Raid bag: dropped " << left << " of item " << itemId << " past " << kBagAddStackLimit << " new stacks";
	return out;
}

std::optional<std::vector<BagStack>> bagTake(const std::vector<BagStack>& bag, int32_t itemId, int32_t n)
{
	if (n < 0 || bagCount(bag, itemId) < n)
		return std::nullopt;
	// decBagItem sorts the item's stacks by (count, pointer) ascending and
	// drains the smallest first; list order stands in for allocation order.
	std::vector<size_t> order;
	for (size_t i = 0; i < bag.size(); ++i)
		if (bag[i].itemId == itemId && bag[i].num > 0)
			order.push_back(i);
	std::stable_sort(order.begin(), order.end(), [&bag](size_t a, size_t b) { return bag[a].num < bag[b].num; });

	auto out = bag;
	auto left = n;
	for (const auto index : order)
	{
		if (left == 0)
			break;
		const auto moved = std::min(left, out[index].num);
		out[index].num -= moved;
		left -= moved;
	}
	return out;
}

std::vector<BagStack> bagDrop(const std::vector<BagStack>& bag, int32_t frameId, int32_t n)
{
	auto out = bag;
	if (n <= 0)
		return out;
	for (auto& stack : out)
		if (stack.frameId == frameId && stack.num > 0)
			stack.num -= std::min(n, stack.num);
	return out;
}

int32_t bagActiveStacks(const std::vector<BagStack>& bag)
{
	return static_cast<int32_t>(std::count_if(bag.begin(), bag.end(), [](const BagStack& s) { return s.num > 0; }));
}

int32_t bagCount(const std::vector<BagStack>& bag, int32_t itemId)
{
	int64_t total = 0;
	for (const auto& stack : bag)
		if (stack.itemId == itemId && stack.num > 0)
			total += stack.num;
	return toCount(total);
}

std::variant<SetEdit, std::string> applySetEdit(const std::vector<SetSlot>& oldSet,
	const std::vector<SetSlot>& newSet, const std::vector<BagStack>& bag)
{
	auto checked = validateSetRows(newSet, "set", SlotCap::Equipped);
	if (const auto* reason = std::get_if<std::string>(&checked))
		return *reason;
	auto set = std::get<std::vector<SetSlot>>(std::move(checked));

	std::map<int32_t, int64_t> delta;
	for (const auto& [itemId, count] : setTotals(set))
		delta[itemId] += count;
	for (const auto& [itemId, count] : setTotals(oldSet))
		delta[itemId] -= count;

	// Gains come out of the bag first, then losses go back, so a returned
	// stack never lands in a frame the same edit is draining.
	auto out = bag;
	for (const auto& [itemId, change] : delta)
	{
		if (change <= 0)
			continue;
		auto taken = bagTake(out, itemId, toCount(change));
		if (!taken)
			return "the set gains " + std::to_string(change) + " of item " + std::to_string(itemId)
				+ " but the bag holds " + std::to_string(bagCount(out, itemId));
		out = std::move(*taken);
	}
	for (const auto& [itemId, change] : delta)
		if (change < 0)
			out = bagAdd(out, itemId, toCount(-change));
	return SetEdit{ std::move(set), std::move(out) };
}

MixOutcome mixInBag(const std::vector<MixOrder>& orders, const std::set<int32_t>& permittedRecipes,
	const std::vector<BagStack>& bag)
{
	MixOutcome out{ bag, 0 };
	for (const auto& order : orders)
	{
		const auto plan = mixPlan(order.recipeId, permittedRecipes);
		if (!plan)
			continue;
		int32_t made = 0;
		// Bounded by the bag: every craft drains at least one material.
		for (; made < order.count; ++made)
		{
			// continueMix @0x19F8B74 refuses (RAID_ITEMMIX_NG) once the bag
			// already holds raid_bag_cnt active stacks.
			if (bagActiveStacks(out.bag) >= kBagCapacity)
				break;
			auto drained = drainMaterials(out.bag, plan->cost);
			if (!drained)
				break;
			out.bag = bagAdd(*drained, plan->productId, 1);
		}
		if (made < order.count)
			LOG_INFO << "Raid mix: recipe " << order.recipeId << " made " << made << " of " << order.count
				<< " (bag full or materials short)";
		out.crafted += made;
	}
	return out;
}

std::vector<SetSlot> readSetRows(const RaidRequest& req, const char* group)
{
	std::vector<SetSlot> out;
	for (const auto* row : req.rows(group))
	{
		SetSlot slot{};
		slot.itemId = narrow(RaidRequest::numberOf(row, "kixHbe54"));
		slot.slot = narrow(RaidRequest::numberOf(row, "XuJL4pc5", -1));
		slot.num = narrow(RaidRequest::numberOf(row, "wgV86x1q"));
		out.push_back(slot);
	}
	return out;
}

std::vector<::RaidUserBagInfo> bagRows(const std::string& userId, const std::vector<BagStack>& bag)
{
	std::vector<::RaidUserBagInfo> out;
	for (const auto& stack : bag)
	{
		// An emptied stack goes out at 0, as RaidUtils::decBagItem keeps it:
		// the bag list is replaced only by a non-empty reply, so dropping the
		// row would leave the client showing the last item (CORE 8).
		if (stack.num < 0 || stack.itemId <= 0)
			continue;
		::RaidUserBagInfo row{};
		row.frame_id = stack.frameId;
		row.user_id = userId;
		row.item_id = std::to_string(stack.itemId);
		row.item_num = stack.num;
		row.diff_type = 0;   // no consumer found (CAMP_ITEMS 4.1)
		row.new_flg = stack.newFlg;
		row.event_token_id = "";   // non-empty switches the icon to an event token
		out.push_back(std::move(row));
	}
	// A bag with nothing in it still goes out, as one row with an EMPTY item
	// id: an empty list cannot clear the client's, so a run that starts with
	// an empty bag showed the previous run's items.  The bag screens skip a
	// row whose id is empty (RaidFieldItemScene::setBagItemList and
	// RaidFieldCarryItemScene::setItemList test its length, @0x1961ACC /
	// @0x195EF28) and getActiveCount counts only num > 0.
	if (out.empty())
	{
		::RaidUserBagInfo none{};
		none.frame_id = 0;
		none.user_id = userId;
		none.item_id = "";
		none.item_num = 0;
		out.push_back(std::move(none));
	}
	return out;
}

std::vector<::UserEquipItemInfo> setRows(const std::vector<SetSlot>& set)
{
	std::vector<::UserEquipItemInfo> out;
	for (const auto& slot : set)
	{
		// An emptied slot goes out at 0, as the bag's stacks do: the set list
		// is replaced only by a non-empty reply.
		if (slot.itemId <= 0 || slot.num < 0 || slot.slot < 0)
			continue;
		::UserEquipItemInfo row{};
		row.item_id = static_cast<uint32_t>(slot.itemId);
		row.disp_order = static_cast<uint32_t>(slot.slot);
		row.item_num = static_cast<uint32_t>(slot.num);
		out.push_back(row);
	}
	return out;
}

} // namespace gme::raid
