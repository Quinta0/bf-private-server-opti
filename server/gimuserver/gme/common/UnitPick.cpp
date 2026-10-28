#include "App.hpp"

#include <gimuserver/gme/common/UnitPick.hpp>
#include <gimuserver/archive/GachaArchiver.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/utils/JsonFile.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace gme
{

namespace
{

/*!
* The gates the 24 pickers open from: original 2021 "Featured Summon" rows
* (Fire/Water/... Featured Summon, Vortex Arena Resummon) that no archive gate,
* selector or summon ticket uses.  They are rows the client's own GachaMst
* already holds; name, art, text and price are replaced per reply.
*
* ⚠ If a later banner puts one of these on the rail, loadUnitPick turns the
* offer off and says so in the log.  Pick other unused GachaMst ids then.
*/
constexpr std::array<int32_t, 24> kGates = {
	20187, 20188, 20189, 20193, 20194, 20195,  // Omni      Fire..Dark
	20196, 20197, 20198, 20199, 20200, 20201,  // 7★
	20202, 20203, 20204, 20205, 20206, 20207,  // 5-6★
	20208, 20209, 20210, 20211, 20212, 20213,  // 1-4★
};

/*! Ticket ids 9001-9024, far from the original selectors' 1-54. */
constexpr int32_t kFirstTicket = 9001;

/*! Rail tile ids, one per band.  The selector tiles use 100 + ticket id. */
constexpr int32_t kFirstTile = 9101;

/*! After the permanent tiles (1-7) and the held-selector tiles. */
constexpr int32_t kFirstTileOrder = 40;

/*!
* The race pick's 12 gates (Omni Fire..Dark, then 7★): more unused 2021
* "Featured Summon" GachaMst rows, the same kind as kGates.  Its tickets are
* 9031-9042 and its two tiles 9111-9112, just before the 50-gem pick's.
*/
constexpr std::array<int32_t, 12> kRaceGates = {
	20186, 20185, 20184, 20179, 20178, 20176,  // Omni      Fire..Dark
	20175, 20174, 20169, 20167, 20166, 20161,  // 7★
};
constexpr int32_t kFirstRaceTicket = 9031;
constexpr int32_t kFirstRaceTile = 9111;
constexpr int32_t kFirstRaceTileOrder = 38;

/*! deploy/archive/race_pick.json (scripts/gen_race_pick.py). */
struct RacePickFile
{
	std::string about;
	std::vector<int32_t> units;
};

struct Band
{
	const char* name;      // in labels: "Fire Omni"
	const char* caption;   // in gate text
	uint32_t lo, hi;       // rarity range
};

constexpr std::array<Band, 4> kBands = { {
	{ "Omni", "Omni", 8, 8 },
	{ "7★", "7★", 7, 7 },
	{ "5-6★", "5★ and 6★", 5, 6 },
	{ "1-4★", "1★ to 4★", 1, 4 },
} };

struct Element
{
	const char* name;
	const char* color;  // the client's markup colour for that element
};

// Archive element ids 1-6 (Fencer Vargas 1, Selena 2, Lance 3, Eze 4, Atro 5,
// Magress 6).  Colours as the Battle Maiden Selector's text writes them.
constexpr std::array<Element, 6> kElements = { {
	{ "Fire", "255:0:0" },
	{ "Water", "0:96:255" },
	{ "Earth", "0:255:0" },
	{ "Thunder", "255:255:0" },
	{ "Light", "247:229:156" },
	{ "Dark", "117:0:117" },
} };

/*! One family of pickers: what loadUnitPick builds and replies carry. */
struct Family
{
	std::vector<UnitPickOffer> offers;
	std::vector<::GachaInfoMst> gates;
	std::vector<::UnitSelectorGachaMst> selectors;
	std::vector<::GachaCategory> tiles;
};

Family g_pick;   // the 50-gem unit pick
Family g_race;   // the level 999 race pick

std::string gateCaption(const Band& band, const Element& element, size_t units)
{
	return std::string("<center>[<color=255:255:0>Unit Pick: ") + element.name + " " + band.name
		+ "</color>]<br>Choose <color=255:0:0>any one</color> of these <color=" + element.color + ">"
		+ std::to_string(units) + " " + element.name + "</color> " + band.caption
		+ " units for <color=255:255:0>" + std::to_string(kUnitPickPrice) + " gems</color>."
		+ "<br>One pick per account, across every Unit Pick gate.<center>";
}

std::string gateDetail()
{
	return "<center>Pick <color=255:0:0>one</color> unit. Costs <color=255:255:0>"
		+ std::to_string(kUnitPickPrice) + " gems</color>, once per account.<center>";
}

std::string raceCaption(const Band& band, const Element& element, size_t units)
{
	return std::string("<center>[<color=255:255:0>Level 999 Race: ") + element.name + " " + band.name
		+ "</color>]<br>Choose <color=255:0:0>any one</color> of these <color=" + element.color + ">"
		+ std::to_string(units) + " " + element.name + "</color> " + band.caption
		+ " units, <color=255:255:0>free</color>.<br>One pick, across every Level 999 Race gate.<center>";
}

std::string raceDetail()
{
	return "<center>Your reward for being one of the first ten players to reach "
		"<color=255:255:0>level 999</color>. Pick <color=255:0:0>one</color> unit.<center>";
}

/*! How one family's gates read: name prefix, price, caption and detail text. */
struct Look
{
	const char* prefix;   // "Unit Pick" / "Level 999 Race"
	int32_t gems;
	std::string (*caption)(const Band&, const Element&, size_t);
	std::string (*detail)();
	std::string selectorSuffix;
};

/*!
* Build the pickers of `bands` (indexes into kBands), one per band and element,
* from `wanted` (a unit id filter).  Empty when a gate is unusable; the error is
* in the log.
*/
template <typename Wanted>
Family buildFamily(const std::vector<size_t>& bands, const int32_t* gateIds, int32_t firstTicket,
	int32_t firstTile, int32_t firstTileOrder, const Look& look, const std::vector<int32_t>& archiveGates,
	Wanted wanted)
{
	Family family;
	for (size_t b = 0; b < bands.size(); ++b)
	{
		const auto& band = kBands[bands[b]];
		for (size_t e = 0; e < kElements.size(); ++e)
		{
			const auto& element = kElements[e];
			const auto index = b * kElements.size() + e;

			UnitPickOffer offer;
			offer.ticketId = firstTicket + static_cast<int32_t>(index);
			offer.gateId = gateIds[index];
			offer.label = std::string(element.name) + " " + band.name;
			for (const auto& [id, unit] : UnitArchiver::instance().all())
			{
				if (unit.element == e + 1 && unit.rarity >= band.lo && unit.rarity <= band.hi
					&& wanted(static_cast<int32_t>(id)))
					offer.pool.push_back(static_cast<int32_t>(id));
			}
			// Id order keeps a unit line's stages next to each other.
			std::sort(offer.pool.begin(), offer.pool.end());

			if (std::find(archiveGates.begin(), archiveGates.end(), offer.gateId) != archiveGates.end())
			{
				LOG_ERROR << look.prefix << ": gate " << offer.gateId
					<< " is an archive gate already; this pick is OFF (gme/common/UnitPick.cpp)";
				return {};
			}
			GachaRecord record{};
			record.id = static_cast<uint32_t>(offer.gateId);
			record.background = "gacha_hero_summon.png";
			record.door = "gacha_rare_door.png";
			record.caption_msg = look.caption(band, element, offer.pool.size());
			record.detail_msg = look.detail();
			record.priority = 1;
			::GachaInfoMst gate{};
			if (!GachaArchiver::instance().populatePacket(record, gate))
			{
				LOG_ERROR << look.prefix << ": gate " << offer.gateId
					<< " has no GachaMst row; this pick is OFF";
				return {};
			}
			gate.name = std::string(look.prefix) + ": " + offer.label;
			gate.gems = look.gems;
			family.gates.push_back(std::move(gate));

			::UnitSelectorGachaMst selector{};
			selector.selector_id = offer.ticketId;
			selector.gacha_id = offer.gateId;
			selector.name = std::string(look.prefix) + ": " + offer.label + look.selectorSuffix;
			selector.image = "unit_selector_ticket.png";
			selector.unit_pool.assign(offer.pool.begin(), offer.pool.end());
			family.selectors.push_back(std::move(selector));

			family.offers.push_back(std::move(offer));
		}
	}

	for (size_t b = 0; b < bands.size(); ++b)
	{
		::GachaCategory tile{};
		tile.id = firstTile + static_cast<int32_t>(b);
		tile.img = "gacha_hero_selector_banner.png";
		tile.disp_order = firstTileOrder + static_cast<int32_t>(b);
		for (size_t e = 0; e < kElements.size(); ++e)
		{
			if (e)
				tile.gacha_id_list += ',';
			tile.gacha_id_list += std::to_string(gateIds[b * kElements.size() + e]);
		}
		// Always open; see selectorGachaCategories for the 32-bit end date.
		pkg::unix_to_chrono(0, tile.start_date);
		pkg::unix_to_chrono(2147483647, tile.end_date);
		family.tiles.push_back(std::move(tile));
	}
	return family;
}

size_t unitsIn(const Family& family)
{
	size_t total = 0;
	for (const auto& offer : family.offers)
		total += offer.pool.size();
	return total;
}

const UnitPickOffer* offerOf(const Family& family, uint32_t ticketId)
{
	const auto it = std::find_if(family.offers.begin(), family.offers.end(),
		[ticketId](const UnitPickOffer& offer)
		{ return static_cast<uint32_t>(offer.ticketId) == ticketId; });
	return it == family.offers.end() ? nullptr : &*it;
}

/*! 0: no race pick, 1: granted and not used yet, 2: used. */
drogon::Task<int> racePickState(const db::Database database, const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT picked_at FROM user_race_pick WHERE user_id = $1;", identity.userId);
	if (rows.empty())
		co_return 0;
	co_return rows[0]["picked_at"].as<int64_t>() == 0 ? 1 : 2;
}

} // namespace

void loadUnitPick(const std::string& archiveRoot)
{
	g_pick = {};
	g_race = {};

	// The archive's own gates (lookup() would log an error for every id it lacks).
	std::vector<int32_t> archiveGates;
	for (const auto& gate : GachaArchiver::instance().populateAllPackets())
		archiveGates.push_back(gate.id);

	const Look pickLook{ "Unit Pick", kUnitPickPrice, gateCaption, gateDetail,
		" (" + std::to_string(kUnitPickPrice) + " gems)" };
	g_pick = buildFamily({ 0, 1, 2, 3 }, kGates.data(), kFirstTicket, kFirstTile, kFirstTileOrder,
		pickLook, archiveGates, [](int32_t) { return true; });
	LOG_INFO << "Unit pick: " << g_pick.offers.size() << " pickers, " << unitsIn(g_pick) << " units, "
		<< kUnitPickPrice << " gems once per account";

	std::set<int32_t> raceUnits;
	try
	{
		const auto file = LoadJson<RacePickFile>(archiveRoot, "race_pick.json");
		raceUnits.insert(file.units.begin(), file.units.end());
	}
	catch (const std::exception& e)
	{
		LOG_ERROR << "Level 999 Race: failed to load race_pick.json, the race pick is OFF: " << e.what();
		return;
	}
	const Look raceLook{ "Level 999 Race", 0, raceCaption, raceDetail, "" };
	g_race = buildFamily({ 0, 1 }, kRaceGates.data(), kFirstRaceTicket, kFirstRaceTile, kFirstRaceTileOrder,
		raceLook, archiveGates, [&raceUnits](int32_t id) { return raceUnits.contains(id); });
	LOG_INFO << "Level 999 Race: " << g_race.offers.size() << " pickers, " << unitsIn(g_race)
		<< " units (of " << raceUnits.size() << " in race_pick.json)";
}

const UnitPickOffer* unitPickOffer(uint32_t ticketId)
{
	return offerOf(g_pick, ticketId);
}

const UnitPickOffer* racePickOffer(uint32_t ticketId)
{
	return offerOf(g_race, ticketId);
}

drogon::Task<bool> unitPickUsed(const db::Database database, const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM user_unit_pick WHERE user_id = $1;", identity.userId);
	co_return !rows.empty();
}

drogon::Task<void> addUnitPick(
	const db::Database database,
	const UserIdentity identity,
	std::vector<::GachaCategory>& categories,
	std::vector<::GachaInfoMst>& gates,
	std::vector<::UnitSelectorGachaMst>& selectors)
{
	// Reversed: a tile pages through its gates in the REVERSE of GachaInfoList
	// order (seen on the emulator: Dark came up as 1/6), which is also why
	// GachaArchiver::populateAllPackets walks gacha.json backwards.
	for (const Family* family : { &g_race, &g_pick })
	{
		gates.insert(gates.end(), family->gates.rbegin(), family->gates.rend());
		selectors.insert(selectors.end(), family->selectors.begin(), family->selectors.end());
	}
	if (!g_race.offers.empty() && co_await racePickState(database, identity) == 1)
		categories.insert(categories.end(), g_race.tiles.begin(), g_race.tiles.end());
	if (!g_pick.offers.empty() && !(co_await unitPickUsed(database, identity)))
		categories.insert(categories.end(), g_pick.tiles.begin(), g_pick.tiles.end());
}

drogon::Task<void> addUnitPickTickets(
	const db::Database database,
	const UserIdentity identity,
	std::vector<::UnitSelectorGachaUserInfo>& tickets)
{
	if (!g_pick.offers.empty())
	{
		const auto used = co_await unitPickUsed(database, identity);
		for (const auto& offer : g_pick.offers)
			tickets.push_back({ .ticket_id = std::to_string(offer.ticketId), .count = used ? 0 : 1 });
	}
	if (!g_race.offers.empty())
	{
		const auto open = co_await racePickState(database, identity) == 1;
		for (const auto& offer : g_race.offers)
			tickets.push_back({ .ticket_id = std::to_string(offer.ticketId), .count = open ? 1 : 0 });
	}
}

} // namespace gme
