#include "App.hpp"

#include <gimuserver/gme/common/SummonRotation.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/GameWeek.hpp>
#include <gimuserver/gme/common/SummonGates.hpp>
#include <gimuserver/archive/GachaArchiver.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/utils/JsonFile.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gme
{

namespace
{

/*! One gate in the pool.  `art` is its rail tile image, empty for the default. */
struct RotationGate
{
	uint32_t gate{};
	std::string art;
};

/*! deploy/archive/summon_rotation.json. */
struct RotationFile
{
	std::string about;
	/*! "YYYY-MM-DD", a Monday: the week starting that day at 08:00 UTC shows pool[0..slots). */
	std::string first_week;
	/*! How long after Monday 08:00 last week's gates may still be pulled. */
	int64_t grace_seconds{};
	/*! Tile image for a pool gate with no art of its own. */
	std::string default_art;
	/*! The featured tiles sit right after this core tile. */
	int32_t after_tile{};
	/*! One tile id per featured slot, fixed for good. */
	std::vector<int32_t> slot_tiles;
	/*! THE ORDER IS THE CALENDAR: changing it reshuffles every future week. */
	std::vector<RotationGate> pool;
};

RotationFile g_rotation;
int64_t g_firstWeek = 0;
std::unordered_set<uint32_t> g_poolGates;

int64_t floorMod(const int64_t value, const int64_t divisor)
{
	return value - floorDiv(value, divisor) * divisor;
}

const std::string& artFor(const uint32_t gate)
{
	for (const auto& entry : g_rotation.pool)
		if (entry.gate == gate && !entry.art.empty())
			return entry.art;
	return g_rotation.default_art;
}

/*! A comma-separated gate list without the pool gates. */
std::string withoutPoolGates(std::string_view list)
{
	std::string kept;
	while (!list.empty())
	{
		const auto comma = list.find(',');
		const auto id = list.substr(0, comma);
		uint32_t gate = 0;
		const bool numeric = std::from_chars(id.data(), id.data() + id.size(), gate).ec == std::errc{};
		if (!numeric || !g_poolGates.contains(gate))
		{
			if (!kept.empty())
				kept += ',';
			kept += id;
		}
		if (comma == std::string_view::npos)
			break;
		list.remove_prefix(comma + 1);
	}
	return kept;
}

/*! "Mon 27 Oct, 08:00 UTC" for a week boundary. */
std::string boundaryText(const int64_t unixSeconds)
{
	static constexpr std::array<const char*, 7> kDays{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static constexpr std::array<const char*, 12> kMonths{ "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	const std::chrono::sys_days day{ std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{ std::chrono::seconds{ unixSeconds } }) };
	const std::chrono::year_month_day ymd{ day };
	const auto secondsIntoDay = floorMod(unixSeconds, kSecondsPerDay);
	return std::format("{} {} {}, {:02}:{:02} UTC",
		kDays[std::chrono::weekday{ day }.c_encoding()],
		static_cast<unsigned>(ymd.day()),
		kMonths[static_cast<unsigned>(ymd.month()) - 1],
		secondsIntoDay / 3600, secondsIntoDay % 3600 / 60);
}

/*! The title between the first [ ] of a caption, tags dropped. */
std::string captionTitle(const std::string& caption)
{
	const auto open = caption.find('[');
	const auto close = caption.find(']', open);
	if (open == std::string::npos || close == std::string::npos)
		return {};
	std::string title;
	bool inTag = false;
	for (const char c : caption.substr(open + 1, close - open - 1))
	{
		if (c == '<')
			inTag = true;
		else if (c == '>')
			inTag = false;
		else if (!inTag)
			title += c;
	}
	return title;
}

/*! Escape a string for a JSON string literal. */
std::string jsonString(const std::string& text)
{
	std::string out = "\"";
	for (const char c : text)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		if (static_cast<unsigned char>(c) < 0x20)
			out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
		else
			out += c;
	}
	return out + '"';
}

} // namespace

namespace
{

/*!
* Why the Forge's summon_rotation.json cannot be used, or nullopt.  The base
* loader's checks, made strict: where it would log and drop a gate, an
* overlay is refused, so "accepted" means every gate in it rotates.
*/
std::optional<std::string> forgeRotationProblem(const RotationFile& file)
{
	if (!gameWeekOfDate(file.first_week))
		return "first_week " + file.first_week + " is not a YYYY-MM-DD Monday.";
	if (file.grace_seconds < 0 || file.grace_seconds > 86400)
		return std::string("grace_seconds must be from 0 to 86400.");
	std::set<uint32_t> seen;
	for (const auto& entry : file.pool)
	{
		if (!GachaArchiver::instance().lookup(entry.gate))
			return "Banner " + std::to_string(entry.gate) + " is not in gacha.json, so it cannot rotate.";
		if (!seen.insert(entry.gate).second)
			return "Banner " + std::to_string(entry.gate) + " is in the pool twice.";
	}
	if (file.slot_tiles.empty())
		return std::string("slot_tiles must list at least one tile.");
	if (file.pool.size() < file.slot_tiles.size())
		return "The pool has " + std::to_string(file.pool.size()) + " banners for " + std::to_string(file.slot_tiles.size())
			+ " featured slots; it needs at least one per slot.";
	std::set<int32_t> staticTiles;
	for (const auto& tile : theServer()->cache().gachaListRsp().gacha_categories)
		staticTiles.insert(tile.id);
	for (const auto tile : file.slot_tiles)
		if (staticTiles.contains(tile))
			return "Featured slot tile " + std::to_string(tile) + " is also a tile in gacha_category_mst; pick another id.";
	if (!staticTiles.contains(file.after_tile))
		return "after_tile " + std::to_string(file.after_tile) + " is not a tile in gacha_category_mst.";
	return std::nullopt;
}

/*! The Forge's rotation when it passes; false (reported) otherwise. */
bool loadForgeRotation()
{
	const auto overlay = forge::readFileOverlay("summon_rotation.json");
	if (!overlay)
		return false;
	RotationFile file{};
	std::string why;
	if (const auto missing = forge::missingTopKey(overlay->bytes,
		{ "first_week", "grace_seconds", "default_art", "after_tile", "slot_tiles", "pool" }))
		why = *missing;
	else if (const auto ec = glz::read<glz::opts{}>(file, overlay->bytes))
		why = "The file does not read: " + forge::plainReadError(ec, overlay->bytes);
	else if (const auto problem = forgeRotationProblem(file))
		why = *problem;
	forge::recordFileOverlay("summon_rotation.json", overlay->sha256, why.empty(), why);
	if (!why.empty())
		return false;

	g_rotation = std::move(file);
	g_firstWeek = *gameWeekOfDate(g_rotation.first_week);
	for (const auto& entry : g_rotation.pool)
		g_poolGates.insert(entry.gate);
	LOG_INFO << "SummonRotation: " << g_rotation.pool.size() << " gates over "
		<< g_rotation.slot_tiles.size() << " slots, first week " << g_rotation.first_week << " (Balance Forge)";
	return true;
}

} // namespace

void loadSummonRotation(const std::string& archiveRoot)
{
	g_rotation = {};
	g_poolGates.clear();
	if (loadForgeRotation())
		return;
	try
	{
		g_rotation = LoadJson<RotationFile>(archiveRoot, "summon_rotation.json");
	}
	catch (const std::exception& e)
	{
		LOG_ERROR << "SummonRotation: failed to load summon_rotation.json, no featured banners: " << e.what();
		return;
	}

	const auto firstWeek = gameWeekOfDate(g_rotation.first_week);
	if (!firstWeek)
	{
		LOG_ERROR << "SummonRotation: first_week \"" << g_rotation.first_week
			<< "\" is not a YYYY-MM-DD Monday, no featured banners";
		g_rotation = {};
		return;
	}
	g_firstWeek = *firstWeek;

	// A gate the archive cannot sell would be a tile the client hides (no
	// GachaInfo row, hasValidGachaID) and a pull GachaAction refuses: drop it so
	// the rest of the pool keeps rotating.
	std::vector<RotationGate> pool;
	for (const auto& entry : g_rotation.pool)
	{
		if (!GachaArchiver::instance().lookup(entry.gate))
		{
			LOG_ERROR << "SummonRotation: gate " << entry.gate << " is not in gacha.json, left out of the rotation";
			continue;
		}
		if (!g_poolGates.insert(entry.gate).second)
		{
			LOG_ERROR << "SummonRotation: gate " << entry.gate << " is in the pool twice, second one left out";
			continue;
		}
		pool.push_back(entry);
	}
	g_rotation.pool = std::move(pool);

	if (g_rotation.slot_tiles.empty() || g_rotation.pool.size() < g_rotation.slot_tiles.size())
	{
		LOG_ERROR << "SummonRotation: " << g_rotation.pool.size() << " gates for "
			<< g_rotation.slot_tiles.size() << " slots, no featured banners";
		g_rotation = {};
		g_poolGates.clear();
		return;
	}

	for (const auto& tile : theServer()->cache().gachaListRsp().gacha_categories)
	{
		if (std::find(g_rotation.slot_tiles.begin(), g_rotation.slot_tiles.end(), tile.id) != g_rotation.slot_tiles.end())
			LOG_ERROR << "SummonRotation: slot tile id " << tile.id << " is also a static tile in gacha_category_mst.json";
	}

	LOG_INFO << "SummonRotation: " << g_rotation.pool.size() << " gates over "
		<< g_rotation.slot_tiles.size() << " slots, first week " << g_rotation.first_week;
}

bool summonRotationActive()
{
	return !g_rotation.pool.empty();
}

std::vector<uint32_t> featuredGates(const int64_t week)
{
	std::vector<uint32_t> gates;
	if (!summonRotationActive())
		return gates;
	const auto slots = static_cast<int64_t>(g_rotation.slot_tiles.size());
	const auto size = static_cast<int64_t>(g_rotation.pool.size());
	for (int64_t slot = 0; slot < slots; ++slot)
		gates.push_back(g_rotation.pool[floorMod((week - g_firstWeek) * slots + slot, size)].gate);
	return gates;
}

bool isRotatingGate(const uint32_t gachaId)
{
	return g_poolGates.contains(gachaId);
}

bool isFeaturedNow(const uint32_t gachaId, const int64_t now)
{
	const auto week = gameWeekAt(now);
	const auto isIn = [gachaId](const std::vector<uint32_t>& gates) {
		return std::find(gates.begin(), gates.end(), gachaId) != gates.end();
	};
	if (isIn(featuredGates(week)))
		return true;
	return now - gameWeekStart(week) < g_rotation.grace_seconds && isIn(featuredGates(week - 1));
}

void applyWeeklyRail(std::vector<::GachaCategory>& categories, const int64_t now, const bool wholePool)
{
	if (!summonRotationActive())
		return;

	for (auto& tile : categories)
		tile.gacha_id_list = withoutPoolGates(tile.gacha_id_list);
	std::erase_if(categories, [](const ::GachaCategory& tile) { return tile.gacha_id_list.empty(); });
	std::stable_sort(categories.begin(), categories.end(),
		[](const ::GachaCategory& a, const ::GachaCategory& b) { return a.disp_order < b.disp_order; });

	const auto gates = featuredGates(gameWeekAt(now));
	std::vector<std::string> lists;
	for (const auto gate : gates)
		lists.push_back(std::to_string(gate));
	if (wholePool && !lists.empty())
	{
		size_t next = 0;
		for (const auto& entry : g_rotation.pool)
			if (std::find(gates.begin(), gates.end(), entry.gate) == gates.end())
				lists[next++ % lists.size()] += "," + std::to_string(entry.gate);
	}
	std::vector<::GachaCategory> featured;
	for (size_t slot = 0; slot < gates.size(); ++slot)
	{
		::GachaCategory tile{};
		tile.id = g_rotation.slot_tiles[slot];
		tile.img = artFor(gates[slot]);
		tile.gacha_id_list = lists[slot];
		pkg::unix_to_chrono(0, tile.start_date);
		pkg::unix_to_chrono(2147483647, tile.end_date);
		featured.push_back(std::move(tile));
	}

	// Right after the core tile, so the tiles above it keep their places: the
	// tutorial taps the Rare gate's tile (3) at a fixed screen position.
	auto at = std::find_if(categories.begin(), categories.end(),
		[](const ::GachaCategory& tile) { return tile.id == g_rotation.after_tile; });
	at = at == categories.end() ? categories.begin() : std::next(at);
	categories.insert(at, std::make_move_iterator(featured.begin()), std::make_move_iterator(featured.end()));

	int32_t order = 1;
	for (auto& tile : categories)
		tile.disp_order = order++;
}

void applyFeaturedNotes(std::vector<::GachaInfoMst>& gates, const int64_t now)
{
	if (!summonRotationActive())
		return;
	const auto week = gameWeekAt(now);
	const auto thisWeek = featuredGates(week);
	const auto until = "Featured until " + boundaryText(gameWeekStart(week + 1));
	for (auto& gate : gates)
	{
		const auto id = static_cast<uint32_t>(gate.id);
		const auto token = gate.caption_msg.find(kFeaturedToken);
		if (!isRotatingGate(id) || token == std::string::npos)
			continue;
		const bool featured = std::find(thisWeek.begin(), thisWeek.end(), id) != thisWeek.end();
		gate.caption_msg.replace(token, kFeaturedToken.size(), featured ? until : "Weekly featured banner");
	}
}

std::string summonRotationJson(const int64_t now)
{
	const auto week = gameWeekAt(now);
	const auto list = [](const std::vector<uint32_t>& gates) {
		std::string out = "[";
		for (const auto gate : gates)
		{
			const auto record = GachaArchiver::instance().lookup(gate);
			const auto title = record && record->caption_msg ? captionTitle(*record->caption_msg) : std::string{};
			if (out.size() > 1)
				out += ',';
			out += std::format("{{\"gate\":{},\"title\":{},\"art\":{}}}", gate, jsonString(title), jsonString(artFor(gate)));
		}
		return out + ']';
	};
	return std::format("{{\"week\":{},\"starts\":{},\"ends\":{},\"featured\":{},\"next\":{}}}",
		week, gameWeekStart(week), gameWeekStart(week + 1),
		list(featuredGates(week)), list(featuredGates(week + 1)));
}

std::string summonUnitsJson(const int64_t now)
{
	const auto week = gameWeekAt(now);

	// The static rail's gates in tile order, then the pool's.
	std::vector<uint32_t> gates;
	const auto add = [&gates](const uint32_t gate) {
		if (gate != kTutorialGachaId && std::find(gates.begin(), gates.end(), gate) == gates.end())
			gates.push_back(gate);
	};
	for (const auto& tile : theServer()->cache().gachaListRsp().gacha_categories)
	{
		std::string_view list = tile.gacha_id_list;
		while (!list.empty())
		{
			const auto comma = list.find(',');
			const auto id = list.substr(0, comma);
			uint32_t gate = 0;
			if (std::from_chars(id.data(), id.data() + id.size(), gate).ec == std::errc{})
				add(gate);
			if (comma == std::string_view::npos)
				break;
			list.remove_prefix(comma + 1);
		}
	}
	for (const auto& entry : g_rotation.pool)
		add(entry.gate);

	std::unordered_map<int32_t, int32_t> rarity;
	for (const auto& unit : theServer()->cache().unitMst())
		rarity.emplace(unit.id, unit.rarity);

	struct Unit
	{
		std::string name;
		std::vector<std::pair<uint32_t, double>> in;
	};
	std::map<uint32_t, Unit> units;
	std::string gateJson = "[";
	for (const auto gate : gates)
	{
		const auto record = GachaArchiver::instance().lookup(gate);
		::GachaInfoMst mst{};
		if (!record || !GachaArchiver::instance().populatePacket(*record, mst))
			continue;

		bool featured = false;
		int64_t next = 0;
		if (isRotatingGate(gate))
		{
			const auto isIn = [gate](const std::vector<uint32_t>& list) {
				return std::find(list.begin(), list.end(), gate) != list.end();
			};
			featured = isIn(featuredGates(week));
			for (int64_t k = 1; k <= static_cast<int64_t>(g_rotation.pool.size()) && next == 0; ++k)
				if (isIn(featuredGates(week + k)))
					next = gameWeekStart(week + k);
		}
		if (gateJson.size() > 1)
			gateJson += ',';
		gateJson += std::format(
			"{{\"gate\":{},\"title\":{},\"gems\":{},\"honor\":{},\"rotating\":{},\"featured\":{},\"until\":{},\"next\":{}}}",
			gate, jsonString(captionTitle(record->caption_msg.value_or(mst.caption_msg))),
			mst.gems, mst.friend_points, isRotatingGate(gate), featured,
			featured ? gameWeekStart(week + 1) : 0, next);

		// One unit's chance: its weight over the pool's total.
		uint64_t total = 0;
		std::map<uint32_t, uint64_t> weights;
		for (const auto& pull : record->pool)
		{
			if (pull.weight == 0)
				continue;
			total += pull.weight;
			weights[pull.unit_id] += pull.weight;
			units[pull.unit_id].name = pull.name;
		}
		for (const auto& [unit, weight] : weights)
			units[unit].in.emplace_back(gate, 100.0 * static_cast<double>(weight) / static_cast<double>(total));
	}
	gateJson += ']';

	std::string unitJson = "[";
	for (const auto& [id, unit] : units)
	{
		if (unitJson.size() > 1)
			unitJson += ',';
		const auto found = rarity.find(static_cast<int32_t>(id));
		unitJson += std::format("{{\"id\":{},\"name\":{},\"rarity\":{},\"in\":[", id, jsonString(unit.name),
			found == rarity.end() ? 0 : found->second);
		for (size_t i = 0; i < unit.in.size(); ++i)
			unitJson += std::format("{}[{},{:.2f}]", i ? "," : "", unit.in[i].first, unit.in[i].second);
		unitJson += "]}";
	}
	unitJson += ']';

	return std::format("{{\"week\":{},\"gates\":{},\"units\":{}}}", week, gateJson, unitJson);
}

} // namespace gme
