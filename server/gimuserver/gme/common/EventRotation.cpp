#include "App.hpp"

#include <gimuserver/gme/common/EventRotation.hpp>
#include <gimuserver/gme/common/EventTokens.hpp>
#include <gimuserver/gme/common/GameWeek.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/utils/JsonFile.hpp>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>

namespace gme
{

namespace
{

/*! One event of the pool: its token, what pays it, and its Bazaar. */
struct RotationEvent
{
	std::string name;
	int32_t token{};
	std::vector<EventPayout> payouts;
	/*! token_id may be left out; it is always the event's token. */
	std::vector<ExchangeOffer> offers;
};

/*! deploy/archive/event_rotation.json. */
struct EventRotationFile
{
	std::string about;
	/*! "YYYY-MM-DD", a Monday: the week starting that day at 08:00 UTC features pool[0]. */
	std::string first_week;
	/*! How long after Monday 08:00 last week's event still pays and sells. */
	int64_t grace_seconds{};
	/*! THE ORDER IS THE CALENDAR: changing it reshuffles every future week. */
	std::vector<RotationEvent> pool;
};

EventRotationFile g_rotation;
int64_t g_firstWeek = 0;
std::vector<ExchangeOffer> g_offers;
/*! Mission -> the pool index of its event and its payout. */
std::map<uint32_t, std::pair<size_t, EventPayout>> g_payouts;
std::map<int32_t, size_t> g_eventOfToken;
std::map<int32_t, std::string> g_tokenDungeons;

/*! The pool index featured in a week; nullopt before first_week or when nothing is loaded. */
std::optional<size_t> featuredIndex(const int64_t week)
{
	if (g_rotation.pool.empty() || week < g_firstWeek)
		return std::nullopt;
	return static_cast<size_t>((week - g_firstWeek) % static_cast<int64_t>(g_rotation.pool.size()));
}

void fail(const std::string& why)
{
	throw std::runtime_error("event_rotation.json: " + why);
}

/*! The Forge's event_rotation.json is live (its sha256, for a later fallback). */
bool g_fromForge = false;
std::string g_forgeSha;

void resetRotation()
{
	g_rotation = {};
	g_firstWeek = 0;
	g_offers.clear();
	g_payouts.clear();
	g_eventOfToken.clear();
	g_tokenDungeons.clear();
}

/*! Checks g_rotation and builds the indexes from it; throws (fail) on a data error. */
void indexRotation();

/*! The image's file, exactly as before the Forge: a parse error means no featured event, a data error stops the boot. */
void loadBaseRotation(const std::string& archiveRoot)
{
	resetRotation();
	try
	{
		g_rotation = LoadJson<EventRotationFile>(archiveRoot, "event_rotation.json");
	}
	catch (const std::exception& e)
	{
		LOG_ERROR << "EventRotation: failed to load event_rotation.json, no featured event: " << e.what();
		return;
	}
	indexRotation();
}

/*! The Forge's replacement when it passes every check the base would throw on; false (reported) otherwise. */
bool loadForgeRotation()
{
	g_fromForge = false;
	const auto overlay = forge::readFileOverlay("event_rotation.json");
	if (!overlay)
		return false;
	g_forgeSha = overlay->sha256;
	std::string why;
	EventRotationFile file{};
	if (const auto missing = forge::missingTopKey(overlay->bytes, { "first_week", "grace_seconds", "pool" }))
		why = *missing;
	else if (const auto ec = glz::read<glz::opts{}>(file, overlay->bytes))
		why = "The file does not read: " + forge::plainReadError(ec, overlay->bytes);
	else
	{
		resetRotation();
		g_rotation = std::move(file);
		try
		{
			indexRotation();
		}
		catch (const std::exception& e)
		{
			why = e.what();
		}
	}
	forge::recordFileOverlay("event_rotation.json", overlay->sha256, why.empty(), why);
	if (!why.empty())
	{
		resetRotation();
		return false;
	}
	g_fromForge = true;
	return true;
}

} // namespace

void loadEventRotation(const std::string& archiveRoot)
{
	if (loadForgeRotation())
		return;
	loadBaseRotation(archiveRoot);
}

bool eventRotationFromForge()
{
	return g_fromForge;
}

void fallBackToBaseEventRotation(const std::string& archiveRoot, const std::string& why)
{
	forge::recordFileOverlay("event_rotation.json", g_forgeSha, false, why);
	g_fromForge = false;
	loadBaseRotation(archiveRoot);
}

namespace
{

void indexRotation()
{
	const auto firstWeek = gameWeekOfDate(g_rotation.first_week);
	if (!firstWeek)
		fail("first_week \"" + g_rotation.first_week + "\" is not a YYYY-MM-DD Monday");
	g_firstWeek = *firstWeek;

	std::map<int32_t, int32_t> dungeonOf;
	for (const auto& [dungeon, missions] : theServer()->cache().missionsByDungeon())
		for (const auto mission : missions)
			dungeonOf.emplace(mission, dungeon);

	// Every check below is a data error that would otherwise surface as a
	// token the client cannot name, a payout nobody can earn, or a mission
	// that pays two currencies.  Failing the boot is how the event catalog
	// next to it (loadExchangeArchive) treats a bad offer, too.
	for (size_t index = 0; index < g_rotation.pool.size(); ++index)
	{
		const auto& event = g_rotation.pool[index];
		const auto token = std::to_string(event.token);
		if (!isKnownEventToken(token))
			fail("token " + token + " is not in event_token_mst");
		if (!g_eventOfToken.emplace(event.token, index).second)
			fail("token " + token + " belongs to two events");

		std::set<int32_t> dungeons;
		for (const auto& payout : event.payouts)
		{
			const auto mission = std::to_string(payout.mission);
			if (payout.amount <= 0 || (payout.mode != "each" && payout.mode != "daily" && payout.mode != "run"))
				fail("mission " + mission + " needs a positive amount and mode each, daily or run");
			const auto dungeon = dungeonOf.find(static_cast<int32_t>(payout.mission));
			if (dungeon == dungeonOf.end())
				fail("mission " + mission + " is not in mission_mst");
			if (!g_payouts.emplace(payout.mission, std::pair{ index, payout }).second)
				fail("mission " + mission + " pays twice");
			dungeons.insert(dungeon->second);
		}

		std::string list;
		for (const auto dungeon : dungeons)
		{
			if (!list.empty())
				list += ',';
			list += std::to_string(dungeon);
		}
		g_tokenDungeons[event.token] = std::move(list);

		for (auto offer : event.offers)
		{
			offer.token_id = event.token;
			g_offers.push_back(std::move(offer));
		}
	}

	LOG_INFO << "EventRotation: " << g_rotation.pool.size() << " event(s), "
	         << g_payouts.size() << " paying mission(s), " << g_offers.size()
	         << " Bazaar offer(s), first week " << g_rotation.first_week;
}

} // namespace

const std::vector<ExchangeOffer>& rotationOffers() { return g_offers; }

bool isRotatingToken(const int32_t tokenId) { return g_eventOfToken.contains(tokenId); }

bool isDailyPayoutMission(const uint32_t missionId)
{
	const auto it = g_payouts.find(missionId);
	return it != g_payouts.end() && it->second.second.mode == "daily";
}

std::optional<int64_t> featuredRunWeek(const int32_t tokenId, const int64_t now)
{
	const auto event = g_eventOfToken.find(tokenId);
	if (event == g_eventOfToken.end())
		return std::nullopt;
	const auto week = gameWeekAt(now);
	if (featuredIndex(week) == event->second)
		return week;
	if (now - gameWeekStart(week) < g_rotation.grace_seconds && featuredIndex(week - 1) == event->second)
		return week - 1;
	return std::nullopt;
}

int64_t featuredSecondsLeft(const int32_t tokenId, const int64_t now)
{
	const auto event = g_eventOfToken.find(tokenId);
	const auto week = gameWeekAt(now);
	if (event == g_eventOfToken.end() || featuredIndex(week) != event->second)
		return 0;
	return gameWeekStart(week + 1) - now;
}

std::string rotationTokenDungeons(const int32_t tokenId)
{
	const auto it = g_tokenDungeons.find(tokenId);
	return it == g_tokenDungeons.end() ? std::string{} : it->second;
}

std::optional<std::string> eventShopFor(const int32_t tokenId, const int64_t now)
{
	if (!isRotatingToken(tokenId))
		return std::string("event");
	const auto run = featuredRunWeek(tokenId, now);
	if (!run)
		return std::nullopt;
	return "event@" + std::to_string(*run);
}

drogon::Task<bool> payFeaturedEventTokens(db::Database database, UserIdentity identity,
	const uint32_t missionId, const int64_t now)
{
	const auto it = g_payouts.find(missionId);
	if (it == g_payouts.end())
		co_return false;
	const auto& [index, payout] = it->second;
	const auto token = g_rotation.pool[index].token;
	const auto run = featuredRunWeek(token, now);
	if (!run)
		co_return false;

	if (payout.mode != "each")
	{
		// One claim row per game day or per featured week; the primary key
		// makes a second clear in the same period insert nothing.
		const auto period = payout.mode == "daily"
			? "@d" + std::to_string(gameDayAt(now))
			: "@w" + std::to_string(*run);
		const auto claimed = co_await database->execSqlCoro(
			"INSERT INTO user_exchange_purchases(user_id,shop,offer_id,count) VALUES($1,'event-claim',$2,1)"
			" ON CONFLICT(user_id,shop,offer_id) DO NOTHING RETURNING count;",
			identity.userId, std::to_string(missionId) + period);
		if (claimed.empty())
			co_return false;
	}

	co_await grantEventToken(database, identity, std::to_string(token), payout.amount);
	co_return true;
}

namespace
{

struct ScheduleEvent
{
	std::string name;
	int32_t token{};
	int64_t at{};
};

struct EventSchedule
{
	int64_t week{};
	int64_t starts{};
	int64_t ends{};
	std::optional<ScheduleEvent> event;
	std::optional<ScheduleEvent> next_event;
	std::vector<std::string> every_day;
	std::vector<std::vector<std::string>> vortex;
	int32_t today{};
	int64_t next_day{};
};

} // namespace

std::string eventScheduleJson(const int64_t now)
{
	EventSchedule out;
	out.week = gameWeekAt(now);
	out.starts = gameWeekStart(out.week);
	out.ends = gameWeekStart(out.week + 1);
	if (const auto index = featuredIndex(out.week))
		out.event = ScheduleEvent{ g_rotation.pool[*index].name, g_rotation.pool[*index].token, out.ends };
	if (const auto index = featuredIndex(out.week + 1))
		out.next_event = ScheduleEvent{ g_rotation.pool[*index].name, g_rotation.pool[*index].token, out.ends };

	const auto& cache = theServer()->cache();
	std::map<int32_t, std::string> names;
	for (const auto& dungeon : cache.dungeonMst())
		names.emplace(dungeon.dungeon_id, dungeon.name);
	const auto named = [&names](const auto& ids) {
		std::vector<std::string> list;
		for (const auto id : ids)
			if (const auto it = names.find(id); it != names.end() && std::find(list.begin(), list.end(), it->second) == list.end())
				list.push_back(it->second);
		return list;
	};
	out.every_day = named(cache.serverConfig().vortexAlwaysOpenDungeons);
	for (size_t day = 0; day < 7; ++day)
		out.vortex.push_back(named(cache.vortexDayPermits(day).dungeons));

	// The weekday PermitPlace serves: the server's local day, Monday first.
	const auto t = static_cast<std::time_t>(now);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	out.today = (local.tm_wday + 6) % 7;
	out.next_day = now - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) + 86400;

	std::string buffer;
	if (glz::write_json(out, buffer))
		return "{}";
	return buffer;
}

} // namespace gme
