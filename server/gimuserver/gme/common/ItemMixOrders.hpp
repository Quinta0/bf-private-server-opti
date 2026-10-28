#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The synthesis order list both crafting requests send, shared by
// handlers/ItemMix.cpp (town synthesis) and RaidItemMix (the raid camp,
// docs/features/RAIDS_ROUND2_CAMP_ITEMS.md 4.3).
//
// ItemMixRequest::itemCsvData (libgame.so 0x13A67AC) and its raid twin
// RaidItemMixRequest::itemCsvData @0x13BFEA4 are the same routine: the
// client's ItemMixResultInfo list deduped by recipe id, counted, and joined as
// "<recipeId>:<count>,...".  The repeats come from the "craft again" button
// (MyTownItemMixScene::continueMix / RaidItemMixDetailScene::continueMix).
// The ids are ReceipeMst ids, not item ids: 4HqhTf3a is the key
// PermitRecipeResponse::readParam hands to setRecipeID.
namespace gme
{

/*! One recipe to craft, and how many times. */
struct MixOrder
{
	int32_t recipeId = 0;
	int32_t count = 0;
};

namespace mixcsv
{

/*! Split on `sep`, keeping empty fields ("a,,b" has an empty middle field). */
inline std::vector<std::string> splitOn(const std::string& in, const char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (true)
	{
		const auto at = in.find(sep, start);
		if (at == std::string::npos)
		{
			out.emplace_back(in.substr(start));
			return out;
		}
		out.emplace_back(in.substr(start, at - start));
		start = at + 1;
	}
}

/*! A strictly positive decimal int32, or 0 for anything else (sign, junk, overflow). */
inline int32_t toInt(const std::string& in)
{
	int32_t value = 0;
	const auto [end, error] = std::from_chars(in.data(), in.data() + in.size(), value);
	return error == std::errc{} && end == in.data() + in.size() && value > 0 ? value : 0;
}

} // namespace mixcsv

/*!
* Parse an order CSV.  A malformed batch is rejected as a whole (nullopt):
* it is never silently reinterpreted as one craft.  The total count is kept
* within int32 so a caller can sum crafts without overflowing.
*/
inline std::optional<std::vector<MixOrder>> parseMixOrders(const std::string& csv)
{
	if (csv.empty() || csv.size() > 65536)
		return std::nullopt;
	std::vector<MixOrder> orders;
	int64_t total = 0;
	for (const auto& pair : mixcsv::splitOn(csv, ','))
	{
		const auto parts = mixcsv::splitOn(pair, ':');
		if (parts.size() != 2)
			return std::nullopt;
		const MixOrder order{ mixcsv::toInt(parts[0]), mixcsv::toInt(parts[1]) };
		total += order.count;
		if (order.recipeId == 0 || order.count == 0 || total > std::numeric_limits<int32_t>::max())
			return std::nullopt;
		orders.push_back(order);
	}
	return orders;
}

/*!
* ReceipeMst.materials ("<itemId>:<count>,..." consumed by ONE craft) summed
* per item.  Empty fields are skipped; any other field that is not a pair of
* positive ints makes the whole list malformed (nullopt).
*/
inline std::optional<std::map<int32_t, int64_t>> parseMixMaterials(const std::string& materials)
{
	std::map<int32_t, int64_t> cost;
	for (const auto& pair : mixcsv::splitOn(materials, ','))
	{
		if (pair.empty())
			continue;
		const auto parts = mixcsv::splitOn(pair, ':');
		if (parts.size() != 2)
			return std::nullopt;
		const auto itemId = mixcsv::toInt(parts[0]);
		const auto qty = static_cast<int64_t>(mixcsv::toInt(parts[1]));
		if (itemId <= 0 || qty <= 0)
			return std::nullopt;
		cost[itemId] += qty;
	}
	return cost;
}

} // namespace gme
