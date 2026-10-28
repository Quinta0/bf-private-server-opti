#include "MstChecks.hpp"

#include "JsonText.hpp"

#include <climits>
#include <cstdint>
#include <tuple>

namespace forge
{

std::optional<std::string> rewardListError(const std::string& text, bool allowDaily)
{
	static const std::set<int64_t> payable{ 3, 4, 5, 6, 7, 8, 9, 11, 12, 17, 18, 8000, 8001, 8002, 8004, 8005 };
	std::vector<std::string> entries;
	size_t start = 0;
	while (true)
	{
		const auto comma = text.find(',', start);
		entries.push_back(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	size_t rewards = 0;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const auto& entry = entries[i];
		if (entry.empty())
			return "has an empty entry (two commas in a row, or a comma at an end)";
		if (entry.rfind("daily:", 0) == 0)
		{
			if (!allowDaily || i != 0)
				return "\"" + entry + "\": daily:N may only open the repeat-clear list";
			int64_t n = 0;
			const auto digits = entry.substr(6);
			const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
			if (ec != std::errc{} || ptr != digits.data() + digits.size() || n < 1 || n > 100)
				return "\"" + entry + "\": the daily cap must be a whole number from 1 to 100";
			continue;
		}
		std::vector<int64_t> fields;
		size_t f = 0;
		while (true)
		{
			const auto colon = entry.find(':', f);
			const auto part = entry.substr(f, colon == std::string::npos ? std::string::npos : colon - f);
			int64_t n = 0;
			const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), n);
			if (part.empty() || ec != std::errc{} || ptr != part.data() + part.size() || n < 0)
				return "\"" + entry + "\" is not type:id:amount:0:0 with whole numbers";
			fields.push_back(n);
			if (colon == std::string::npos)
				break;
			f = colon + 1;
		}
		if (fields.size() != 5 && fields.size() != 6)
			return "\"" + entry + "\" is not type:id:amount:0:0";
		if (!payable.contains(fields[0]))
			return "\"" + entry + "\": the present box cannot pay reward type " + std::to_string(fields[0]);
		if (fields[2] <= 0)
			return "\"" + entry + "\": the amount must be at least 1";
		if (fields.size() == 6 && fields[0] != 6)
			return "\"" + entry + "\": only a unit (type 6) carries a sixth field, its Elgif skill";
		++rewards;
	}
	if (rewards == 0)
		return "names no reward";
	return std::nullopt;
}

std::optional<std::string> tableProblem(std::string_view table, const std::vector<const Json::Value*>& overlayRows,
	const std::set<std::string>& addedIds, const Json::Value& merged)
{
	if (table == "gacha_category_mst.json")
	{
		for (const auto* row : overlayRows)
		{
			const auto id = wholeNumber((*row)["vx9uyQVQ"]).value_or(-1);
			if ((id >= 21 && id <= 23) || (id >= 101 && id <= 199) || (id >= 9101 && id <= 9104) || (id >= 9111 && id <= 9112))
				return std::string("Rail tile " + std::to_string(id) + " is a tile id the server fills itself "
					"(21-23 featured, 101-199 selectors, 9101-9104 Unit Pick, 9111-9112 Race Pick); pick another id.");
		}
	}
	else if (table == "gacha_mst.json")
	{
		for (const auto& id : addedIds)
		{
			const auto n = wholeNumber(Json::Value(id)).value_or(0);
			if (n >= 20161 && n <= 20213)
				return std::string("Banner " + id + " is in 20161-20213, the ids Race Pick and Unit Pick use; pick another id.");
		}
	}
	else if (table == "gacha_effect_mst.json")
	{
		int64_t last = INT64_MIN;
		for (const auto& row : merged)
		{
			const auto rarity = wholeNumber(row["7ofj5xa1"]).value_or(0);
			if (rarity < last)
				return std::string("Summon effects must stay sorted by rarity (7ofj5xa1): a pull stops at the first higher rarity.");
			last = rarity;
		}
	}
	else if (table == "area_mst.json")
	{
		std::set<std::string> areas;
		for (const auto& row : merged)
			areas.insert(valueText(row["VjCY7rX4"]));
		for (const auto* row : overlayRows)
		{
			const auto& parent = (*row)["sHMai2ZG"];
			if (parent.isNull() || valueText(parent) == "0")
				continue;
			if (!areas.contains(valueText(parent)))
				return std::string("Area " + valueText((*row)["VjCY7rX4"]) + " names parent area " + valueText(parent)
					+ ", which is not in the area table.");
		}
	}
	else if (table == "mission_mst.json")
	{
		for (const auto* row : overlayRows)
		{
			const auto id = valueText((*row)["j28VNcUW"]);
			for (const auto& [column, what, daily] : { std::tuple{ "SiYs27Cj", "first-clear rewards", false },
				std::tuple{ "S2ChOKo6", "repeat-clear rewards", true } })
			{
				const auto& v = (*row)[column];
				if (!v.isString() || v.asString().empty())
					continue;
				if (const auto why = rewardListError(v.asString(), daily))
					return std::string("Mission " + id + "'s " + what + " (" + column + ") " + *why + ".");
			}
		}
	}
	return std::nullopt;
}

} // namespace forge
