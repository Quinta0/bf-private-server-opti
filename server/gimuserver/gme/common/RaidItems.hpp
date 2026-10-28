#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/ItemMixOrders.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

// Classic Raid round 2, band "camp_items": the three raid item lists
// (docs/features/RAIDS_ROUND2_CAMP_ITEMS.md 4).
//
//   * the SET (Fihk73ZR) and the RESERVE (GvV1w4yM): per player, persistent,
//     slots 0..4 (RaidItemEditScene::setItemLayout @0x19F2D18), stored in
//     raid_equip_items as kind 0 / 1.  Like the normal item bar they are OUT
//     of the warehouse: RaidUtils::raidEqpItemFull @0x11CC728 tops both up and
//     calls GameUtils::decWarehouseItem.
//   * the BAG (UsDq1E6o): per player per run (raid_mission_bag), stacks of
//     1..ItemMst max_stack, at most DefineMst raid_bag_cnt (50) of them with a
//     count (RaidUserBagInfoList::getActiveCount @0x12FE168).
//
// The client edits all three locally and only reports the outcome (RaidItemEdit
// and RaidItemMix are queued and fire later), so the server REPLAYS the same
// arithmetic and answers with its own lists.  The pure functions below mirror
// RaidUtils::incBagItem / decBagItem and work on value vectors, so they can be
// checked without a database (RaidItems.cpp); the Task functions only load and
// store (RaidItemsStore.cpp).
namespace gme::raid
{

/*! raid_equip_items.kind: the raid item set (Fihk73ZR) and the reserve (GvV1w4yM). */
inline constexpr int32_t kItemSetKind = 0;
inline constexpr int32_t kItemReserveKind = 1;
/*! Slots per set / reserve: the client lays out getObjectWithDispOrder(0..4). */
inline constexpr int32_t kItemSetSlots = 5;

/*! One bag stack (UsDq1E6o row).  num 0 is an emptied stack: stored and sent at 0, like the client keeps it. */
struct BagStack
{
	int32_t frameId = 0;
	int32_t itemId = 0;
	int32_t num = 0;
	int32_t newFlg = 0;
};

/*! One set / reserve slot (kixHbe54 item, XuJL4pc5 slot, wgV86x1q count). */
struct SetSlot
{
	int32_t slot = 0;
	int32_t itemId = 0;
	int32_t num = 0;
};

// ---- pure parts ------------------------------------------------------------

/*! ItemMst max_stack (m9gd5h1u), at least 1 for a known item; 0 for an unknown one. */
int32_t itemMaxStack(int32_t itemId);

/*! ItemMst max_equipped (t1i2vIbT); 0 for an unknown or uncarryable item (materials). */
int32_t itemMaxEquipped(int32_t itemId);

/*! Which ItemMst column caps a slot's count. */
enum class SlotCap
{
	Equipped,   // max_equipped: the set, as ItemEdit caps the normal bar
	Stack,      // max_stack: the reserve (no client cap for it was located)
};

/*!
* CAMP_ITEMS 4.2 rule 2 (also the RoonItemEdit check of 4.5): rows with item 0
* or count 0 are empty slots the client still lists and are dropped; every
* other row needs a slot in 0..4 used once, a known item and 1..cap of it.
* @param list Name for the refusal text ("set", "room reserve", ...).
* @return The kept rows sorted by slot, or why the list is refused.
*/
std::variant<std::vector<SetSlot>, std::string> validateSetRows(
	const std::vector<SetSlot>& slots, const std::string& list, const SlotCap capKind);

/*! Per-item totals of a list's non-empty slots. */
std::map<int32_t, int64_t> setTotals(const std::vector<SetSlot>& slots);

/*!
* Add `n` of an item the way RaidUtils::incBagItem @0x11C6D40 does: top up the
* item's stacks to max_stack in list order, then append new stacks of up to
* max_stack.  No bag cap.  New stacks get real frame ids (max + 1) and
* newFlg 1.  An unknown item or n <= 0 leaves the bag unchanged.
*/
std::vector<BagStack> bagAdd(const std::vector<BagStack>& bag, int32_t itemId, int32_t n);

/*!
* Take `n` of an item the way RaidUtils::decBagItem @0x11C70EC does: the
* smallest stacks first.  Emptied stacks stay at num 0, as on the client.
* @return nullopt when the bag holds fewer than n (or n < 0).
*/
std::optional<std::vector<BagStack>> bagTake(const std::vector<BagStack>& bag, int32_t itemId, int32_t n);

/*!
* RaidItemDelete (CAMP_ITEMS 4.4, dead code in this client): remove up to `n`
* from one frame.  An unknown frame or n <= 0 leaves the bag unchanged.
*/
std::vector<BagStack> bagDrop(const std::vector<BagStack>& bag, int32_t frameId, int32_t n);

/*! Stacks with a count: what the 50-stack cap is measured against. */
int32_t bagActiveStacks(const std::vector<BagStack>& bag);

/*! How many of an item the bag holds over all its stacks. */
int32_t bagCount(const std::vector<BagStack>& bag, int32_t itemId);

/*! A RaidItemEdit that passed: the validated new set (by slot) and the bag after the trade. */
struct SetEdit
{
	std::vector<SetSlot> set;
	std::vector<BagStack> bag;
};

/*!
* CAMP_ITEMS 4.2 rules 2-4: validate `newSet`, then move the per-item
* difference to `oldSet` between the set and the bag (gains drain the bag,
* losses go back into it).  Nothing touches the warehouse in the field.
* @return The new set and bag, or the reason the edit was refused (for the log).
*/
std::variant<SetEdit, std::string> applySetEdit(const std::vector<SetSlot>& oldSet,
	const std::vector<SetSlot>& newSet, const std::vector<BagStack>& bag);

/*! What a RaidItemMix did: the bag after it and how many products were made. */
struct MixOutcome
{
	std::vector<BagStack> bag;
	int32_t crafted = 0;
};

/*!
* CAMP_ITEMS 4.3: craft from bag materials only (no karma, no warehouse).
* Each craft adds exactly ONE product (procMix @0x19F9034 hard-codes 1).  An
* order stops at its first craft that cannot happen: unknown or unpermitted
* recipe, product not item_type 1/2, bag already at 50 active stacks, or a
* material short.  The next order still runs.
*/
MixOutcome mixInBag(const std::vector<MixOrder>& orders, const std::set<int32_t>& permittedRecipes,
	const std::vector<BagStack>& bag);

/*!
* Set / reserve rows of a request group (71U5wzhI, Fihk73ZR, GvV1w4yM), read
* leniently: a value outside int32 becomes -1 and fails validation later.
*/
std::vector<SetSlot> readSetRows(const RaidRequest& req, const char* group);

/*! UsDq1E6o rows for the owner's bag, emptied stacks included at 0. */
std::vector<::RaidUserBagInfo> bagRows(const std::string& userId, const std::vector<BagStack>& bag);

/*! Fihk73ZR / GvV1w4yM rows (empty slots left out). */
std::vector<::UserEquipItemInfo> setRows(const std::vector<SetSlot>& set);

// ---- database parts (run inside the caller's transaction) ------------------

/*! The member's bag in a run, by frame id. */
drogon::Task<std::vector<BagStack>> loadBag(const db::Database database, const int64_t runId, const std::string userId);

/*! Replace the member's bag with `bag` (emptied stacks dropped). */
drogon::Task<void> storeBag(
	const db::Database database, const int64_t runId, const std::string userId, const std::vector<BagStack> bag);

/*! The player's set (kItemSetKind) or reserve (kItemReserveKind), by slot. */
drogon::Task<std::vector<SetSlot>> loadSet(const db::Database database, const std::string userId, const int32_t kind);

/*! Replace one list with `set` (empty slots dropped). */
drogon::Task<void> storeSet(
	const db::Database database, const std::string userId, const int32_t kind, const std::vector<SetSlot> set);

/*!
* A battle's item use (client event type 2, "0:itemId:useNum,..."): take the
* counts off the raid item set, slot by slot, never below 0.  Items the set
* does not hold are ignored (the client already spent them).
* @return How many items were taken.
*/
drogon::Task<int32_t> spendSetItems(const db::Database database, const std::string userId, const std::string params);

/*!
* RaidRoonItemEdit (CAMP_ITEMS 4.5): store the room's set and reserve and move
* (new set + new reserve) - (old set + old reserve) out of / back into
* user_items, the ItemEdit.cpp model.  All or nothing.
* @return false (nothing written) when a row is invalid or the warehouse is short.
*/
drogon::Task<bool> storeRoomItems(const db::Database database, const UserIdentity identity,
	const std::vector<SetSlot> set, const std::vector<SetSlot> reserve);

} // namespace gme::raid
