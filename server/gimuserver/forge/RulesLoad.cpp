#include "Rules.hpp"
#include "RulesMap.hpp"

#include "ForgeStatus.hpp"
#include "MstOverlay.hpp"
#include "ReadError.hpp"

#include <gimuserver/archive/GachaArchiver.hpp>
#include <gimuserver/gme/common/GameDay.hpp>

#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <tuple>
#include <variant>

#ifndef _WIN32
#include <sys/stat.h>
#endif

/*
* rules.json: the strict read, the per-rule checks and the live reload (the
* Forge server design, sections 4.1 and 4.2).  The queries are in Rules.cpp.
*/
namespace forge
{

namespace
{

using namespace rules_map;

// ---------------------------------------------------------------------------
// rules.json as written.  Strict: an unknown key anywhere refuses the file, so
// a typo can never silently drop a restriction.
// ---------------------------------------------------------------------------

struct RawWeekly
{
	std::string from;
	std::string to;
};

struct RawSquad
{
	std::string id;
	std::vector<int32_t> missions;
	std::vector<int32_t> dungeons;
	std::vector<int32_t> areas;
	std::vector<int32_t> elements;
	bool one_element = false;
	std::optional<int32_t> max_cost;
	std::optional<int32_t> max_units;
	std::vector<int32_t> banned_units;
	std::vector<int32_t> required_units;
	std::string helper = "any";
	std::vector<int32_t> helper_elements;
	std::string message;
};

struct RawUnlock
{
	std::string id;
	std::vector<int32_t> missions;
	std::vector<int32_t> dungeons;
	std::vector<int32_t> areas;
	std::vector<int32_t> gates;
	std::vector<int32_t> needs;
	std::string needs_mode = "all";
	std::string message;
};

struct RawSchedule
{
	std::string id;
	std::vector<int32_t> missions;
	std::vector<int32_t> dungeons;
	std::vector<int32_t> areas;
	std::vector<int32_t> gates;
	std::optional<std::string> open;
	std::optional<std::string> close;
	std::vector<RawWeekly> weekly;
	std::string message;
};

struct RawRules
{
	std::optional<int32_t> format;
	std::vector<RawSquad> squads;
	std::vector<RawUnlock> unlocks;
	std::vector<RawSchedule> schedules;
};

struct RulesOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};

constexpr RulesOpts kRulesOpts{};
constexpr uintmax_t kMaxRulesBytes = 1u << 20;
constexpr size_t kMaxMessageChars = 300;


} // namespace
} // namespace forge

template <> struct glz::meta<forge::RawWeekly>
{
	using T = forge::RawWeekly;
	static constexpr auto value = glz::object("from", &T::from, "to", &T::to);
};

template <> struct glz::meta<forge::RawSquad>
{
	using T = forge::RawSquad;
	static constexpr auto value = glz::object(
		"id", &T::id, "missions", &T::missions, "dungeons", &T::dungeons, "areas", &T::areas,
		"elements", &T::elements, "one_element", &T::one_element, "max_cost", &T::max_cost,
		"max_units", &T::max_units, "banned_units", &T::banned_units, "required_units", &T::required_units,
		"helper", &T::helper, "helper_elements", &T::helper_elements, "message", &T::message);
};

template <> struct glz::meta<forge::RawUnlock>
{
	using T = forge::RawUnlock;
	static constexpr auto value = glz::object(
		"id", &T::id, "missions", &T::missions, "dungeons", &T::dungeons, "areas", &T::areas,
		"gates", &T::gates, "needs", &T::needs, "needs_mode", &T::needs_mode, "message", &T::message);
};

template <> struct glz::meta<forge::RawSchedule>
{
	using T = forge::RawSchedule;
	static constexpr auto value = glz::object(
		"id", &T::id, "missions", &T::missions, "dungeons", &T::dungeons, "areas", &T::areas,
		"gates", &T::gates, "open", &T::open, "close", &T::close, "weekly", &T::weekly, "message", &T::message);
};

template <> struct glz::meta<forge::RawRules>
{
	using T = forge::RawRules;
	static constexpr auto value = glz::object(
		"format", &T::format, "squads", &T::squads, "unlocks", &T::unlocks, "schedules", &T::schedules);
};

namespace forge
{

namespace
{

using namespace rules_map;

constexpr const char* kWeekdays[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

size_t codePoints(const std::string& text)
{
	return static_cast<size_t>(std::count_if(text.begin(), text.end(),
		[](const char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

/*! Days from 1970-01-01 to y-m-d (proleptic Gregorian; H. Hinnant's days_from_civil). */
int64_t daysFromCivil(int64_t y, const unsigned m, const unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const auto yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digitsAt(const std::string& s, const size_t at, const size_t n, int& out)
{
	if (at + n > s.size())
		return false;
	out = 0;
	for (size_t i = at; i < at + n; ++i)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		out = out * 10 + (s[i] - '0');
	}
	return true;
}

/*! "YYYY-MM-DD HH:MM:SS" UTC to unix seconds. */
std::optional<int64_t> parseUtc(const std::string& s)
{
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
	if (s.size() != 19 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':'
		|| !digitsAt(s, 0, 4, y) || !digitsAt(s, 5, 2, mo) || !digitsAt(s, 8, 2, d)
		|| !digitsAt(s, 11, 2, h) || !digitsAt(s, 14, 2, mi) || !digitsAt(s, 17, 2, se))
		return std::nullopt;
	static constexpr int kMonthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (mo < 1 || mo > 12 || h > 23 || mi > 59 || se > 59 || y < 1970)
		return std::nullopt;
	const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	if (d < 1 || d > kMonthDays[mo - 1] + (mo == 2 && leap ? 1 : 0))
		return std::nullopt;
	return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + se;
}

/*! "sat 08:00" to minutes since Monday 00:00. */
std::optional<int32_t> parseWeekTime(const std::string& s)
{
	if (s.size() != 9 || s[3] != ' ' || s[6] != ':')
		return std::nullopt;
	std::string day = s.substr(0, 3);
	for (auto& c : day)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	int h = 0, m = 0;
	if (!digitsAt(s, 4, 2, h) || !digitsAt(s, 7, 2, m) || h > 23 || m > 59)
		return std::nullopt;
	for (int32_t i = 0; i < 7; ++i)
		if (day == kWeekdays[i])
			return i * 1440 + h * 60 + m;
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Validation, one rule at a time.  A reason drops the rule.
// ---------------------------------------------------------------------------

bool validId(const std::string& id)
{
	return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](const char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
	});
}

std::optional<std::string> commonProblem(const std::string& id, const std::string& message)
{
	if (!validId(id))
		return "A rule id must be 1-64 letters, digits, '_', '.' or '-'.";
	if (codePoints(message) > kMaxMessageChars)
		return "The message is " + std::to_string(codePoints(message)) + " characters long; at most 300 fit the popup.";
	return std::nullopt;
}

std::optional<std::string> targetProblem(const Targets& t, const bool takesGates)
{
	const auto& idx = mapIndex();
	if (t.missions.empty() && t.dungeons.empty() && t.areas.empty() && t.gates.empty())
		return takesGates ? "The rule names no quest, dungeon, area or banner."
			: "The rule names no quest, dungeon or area.";
	for (const auto id : t.missions)
		if (!idx.missions.contains(id))
			return "Quest " + std::to_string(id) + " is not in the game.";
	for (const auto id : t.dungeons)
		if (!idx.dungeons.contains(id))
			return "Dungeon " + std::to_string(id) + " is not in the game.";
	for (const auto id : t.areas)
	{
		if (!idx.areas.contains(id))
			return "Area " + std::to_string(id) + " is not in the game.";
		if (idx.categoryAreas.contains(id))
			return "Area " + std::to_string(id) + " is a category that holds other areas; put the rule on the areas inside it.";
	}
	for (const auto id : t.gates)
	{
		if (id == 2000)
			return "Banner 2000 is the tutorial summon; it cannot take a rule.";
		if (id <= 0 || !GachaArchiver::instance().lookup(static_cast<uint32_t>(id)))
			return "Banner " + std::to_string(id) + " is not in the game.";
	}
	return std::nullopt;
}

std::optional<std::string> elementProblem(const std::vector<int32_t>& elements, const char* field)
{
	for (const auto e : elements)
		if (e < 1 || e > 6)
			return std::string(field) + " has " + std::to_string(e)
				+ "; elements are 1 Fire, 2 Water, 3 Earth, 4 Thunder, 5 Light, 6 Dark.";
	return std::nullopt;
}

std::optional<std::string> unitProblem(const std::vector<int32_t>& units)
{
	for (const auto u : units)
		if (!mapIndex().units.contains(u))
			return "Unit " + std::to_string(u) + " is not in the game.";
	return std::nullopt;
}

std::variant<SquadRule, std::string> validateSquad(const RawSquad& r)
{
	if (auto why = commonProblem(r.id, r.message))
		return *why;
	SquadRule out;
	out.id = r.id;
	out.targets = { r.missions, r.dungeons, r.areas, {} };
	if (auto why = targetProblem(out.targets, false))
		return *why;
	if (auto why = elementProblem(r.elements, "elements"))
		return *why;
	if (auto why = elementProblem(r.helper_elements, "helper_elements"))
		return *why;
	if (r.max_cost && (*r.max_cost < 1 || *r.max_cost > 999))
		return "max_cost must be 1 to 999.";
	if (r.max_units && (*r.max_units < 1 || *r.max_units > 5))
		return "max_units must be 1 to 5 (the helper is not counted).";
	if (auto why = unitProblem(r.banned_units))
		return *why;
	if (auto why = unitProblem(r.required_units))
		return *why;
	if (r.required_units.size() > 5)
		return "required_units lists " + std::to_string(r.required_units.size()) + " units; a squad holds at most 5.";
	if (r.helper == "any") out.helper = HelperRule::Any;
	else if (r.helper == "none") out.helper = HelperRule::None;
	else if (r.helper == "own_units") out.helper = HelperRule::OwnUnits;
	else if (r.helper == "friends_only") out.helper = HelperRule::FriendsOnly;
	else
		return "helper \"" + r.helper + "\" is not one of any, none, own_units, friends_only.";
	if (out.helper == HelperRule::None && !r.helper_elements.empty())
		return "helper none takes no helper_elements: there is no helper to check.";
	out.elements = r.elements;
	out.oneElement = r.one_element;
	out.maxCost = r.max_cost.value_or(0);
	out.maxUnits = r.max_units.value_or(0);
	out.banned.insert(r.banned_units.begin(), r.banned_units.end());
	out.required = r.required_units;
	for (const auto unit : r.required_units)
		out.requiredLines.push_back(lineOf(unit));
	out.helperElements = r.helper_elements;
	out.message = r.message;
	std::set<int32_t> dungeons;
	expand(out.targets, out.missions, dungeons);
	return out;
}

std::variant<UnlockRule, std::string> validateUnlock(const RawUnlock& r)
{
	if (auto why = commonProblem(r.id, r.message))
		return *why;
	UnlockRule out;
	out.id = r.id;
	out.targets = { r.missions, r.dungeons, r.areas, r.gates };
	if (auto why = targetProblem(out.targets, true))
		return *why;
	if (r.needs.empty() || r.needs.size() > 20)
		return "needs must list 1 to 20 quests.";
	for (const auto need : r.needs)
		if (!mapIndex().missions.contains(need))
			return "Needed quest " + std::to_string(need) + " is not in the game.";
	if (r.needs_mode != "all" && r.needs_mode != "any")
		return "needs_mode \"" + r.needs_mode + "\" is not all or any.";
	out.needs = r.needs;
	out.needsAll = r.needs_mode == "all";
	out.message = r.message;
	expand(out.targets, out.missions, out.dungeons);
	for (const auto need : r.needs)
		if (out.missions.contains(need))
			return "The rule locks quest " + std::to_string(need) + ", which it also needs, so it could never open.";
	return out;
}

std::variant<ScheduleRule, std::string> validateSchedule(const RawSchedule& r)
{
	if (auto why = commonProblem(r.id, r.message))
		return *why;
	ScheduleRule out;
	out.id = r.id;
	out.targets = { r.missions, r.dungeons, r.areas, r.gates };
	if (auto why = targetProblem(out.targets, true))
		return *why;
	for (const auto& [field, text, slot] : { std::tuple{ "open", &r.open, &out.open },
		std::tuple{ "close", &r.close, &out.close } })
	{
		if (!*text)
			continue;
		const auto at = parseUtc(**text);
		if (!at)
			return std::string(field) + " \"" + **text + "\" is not a UTC time like 2026-10-30 08:00:00.";
		*slot = *at;
	}
	if (out.open && out.close && *out.close <= *out.open)
		return "close (" + *r.close + ") must be later than open (" + *r.open + ").";
	for (const auto& span : r.weekly)
	{
		const auto from = parseWeekTime(span.from);
		const auto to = parseWeekTime(span.to);
		if (!from || !to)
			return "Weekly span from \"" + span.from + "\" to \"" + span.to
				+ "\" is not \"<day> HH:MM\" with day mon, tue, wed, thu, fri, sat or sun.";
		if (*from == *to)
			return "Weekly span from " + span.from + " to " + span.to + " is empty.";
		out.weekly.push_back({ *from, *to });
	}
	out.message = r.message;
	return out;
}

/*! "squads #2" for a rule whose id is not usable as a name. */
std::string ruleLabel(const std::string& id, const char* list, const size_t at)
{
	return validId(id) ? id : std::string(list) + " #" + std::to_string(at + 1);
}

} // namespace

// ---------------------------------------------------------------------------
// Parse.
// ---------------------------------------------------------------------------

RulesParse parseRules(const std::string& text)
{
	RulesParse out;
	RawRules raw{};
	if (const auto ec = glz::read<kRulesOpts>(raw, text))
	{
		out.error = "rules.json does not read: " + plainReadError(ec, text);
		return out;
	}
	if (raw.format.value_or(0) != 1)
	{
		out.error = "rules.json must say \"format\": 1.";
		return out;
	}
	auto snapshot = std::make_shared<RulesSnapshot>();
	std::set<std::string> ids;
	const auto take = [&](const std::string& id, const char* list, const size_t at, auto&& result, auto& into) {
		const auto label = ruleLabel(id, list, at);
		if (const auto* why = std::get_if<std::string>(&result))
		{
			out.ruleErrors.emplace_back(label, *why);
			return;
		}
		if (!ids.insert(id).second)
		{
			out.ruleErrors.emplace_back(label, "Another rule already uses the id \"" + id + "\"; ids must be unique.");
			return;
		}
		into.push_back(std::move(std::get<0>(result)));
	};
	for (size_t i = 0; i < raw.squads.size(); ++i)
		take(raw.squads[i].id, "squads", i, validateSquad(raw.squads[i]), snapshot->squads);
	for (size_t i = 0; i < raw.unlocks.size(); ++i)
		take(raw.unlocks[i].id, "unlocks", i, validateUnlock(raw.unlocks[i]), snapshot->unlocks);
	for (size_t i = 0; i < raw.schedules.size(); ++i)
		take(raw.schedules[i].id, "schedules", i, validateSchedule(raw.schedules[i]), snapshot->schedules);
	snapshot->index();
	out.snapshot = std::move(snapshot);
	return out;
}

// ---------------------------------------------------------------------------
// The live snapshot and its reload.
// ---------------------------------------------------------------------------

namespace
{

struct FileKey
{
	uint64_t ino = 0;
	uintmax_t size = 0;
	int64_t mtimeNs = 0;
	int64_t ctimeNs = 0;
	bool operator==(const FileKey&) const = default;
};

std::optional<FileKey> statKey(const std::filesystem::path& path)
{
#ifndef _WIN32
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return std::nullopt;
	const auto ns = [](const timespec& t) { return static_cast<int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec; };
	return FileKey{ static_cast<uint64_t>(st.st_ino), static_cast<uintmax_t>(st.st_size), ns(st.st_mtim), ns(st.st_ctim) };
#else
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec) || ec)
		return std::nullopt;
	const auto size = std::filesystem::file_size(path, ec);
	const auto mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
	if (ec)
		return std::nullopt;
	return FileKey{ 0, size, static_cast<int64_t>(mtime.time_since_epoch().count()), 0 };
#endif
}

struct Live
{
	std::mutex reload;                       // one reload at a time; others use the current snapshot
	std::mutex pointer;                      // guards `snapshot` (a shared_ptr copy is not atomic)
	std::shared_ptr<const RulesSnapshot> snapshot = std::make_shared<RulesSnapshot>();
	std::atomic<int64_t> lastCheckMs{ INT64_MIN / 2 };
	std::optional<FileKey> seen;             // the file last read (taken or refused); nullopt = none
	RulesStatus status;                      // what forgeStatus.php says about the live snapshot
};

Live& live()
{
	static Live instance;
	return instance;
}

int64_t steadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void publish(Live& l, std::shared_ptr<const RulesSnapshot> snapshot)
{
	std::lock_guard lock(l.pointer);
	l.snapshot = std::move(snapshot);
}

void refuse(Live& l, const std::string& why)
{
	LOG_WARN << "forge: rules.json REJECTED: " << why << "; the previous rules stay live";
	l.status.error = why;
	setRulesStatus(l.status);
}

/*! Called with l.reload held. */
void refresh(Live& l)
{
	const auto& root = balanceRoot();
	if (root.empty())
		return;
	const auto path = std::filesystem::path(root) / "server" / "rules.json";
	const auto key = statKey(path);
	if (key == l.seen)
		return;
	l.seen = key;
	if (!key)
	{
		// Removed: no rules any more.
		LOG_INFO << "forge: rules.json is gone; no rules are enforced";
		publish(l, std::make_shared<RulesSnapshot>());
		l.status = RulesStatus{};
		l.status.engine = true;
		l.status.loadedAt = gme::unixNow();
		setRulesStatus(l.status);
		return;
	}
	if (key->size > kMaxRulesBytes)
		return refuse(l, "rules.json is " + std::to_string(key->size) + " bytes; at most 1 MiB is read.");
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return refuse(l, "rules.json cannot be opened.");
	std::ostringstream bytes;
	bytes << in.rdbuf();
	const auto text = bytes.str();
	if (text.size() > kMaxRulesBytes)
		return refuse(l, "rules.json is over 1 MiB.");
	auto parsed = parseRules(text);
	if (!parsed.snapshot)
		return refuse(l, parsed.error);

	RulesStatus status;
	status.engine = true;
	status.sha256 = sha256Hex(text);
	status.loadedAt = gme::unixNow();
	status.squads = static_cast<int32_t>(parsed.snapshot->squads.size());
	status.unlocks = static_cast<int32_t>(parsed.snapshot->unlocks.size());
	status.schedules = static_cast<int32_t>(parsed.snapshot->schedules.size());
	for (const auto& [rule, why] : parsed.ruleErrors)
	{
		LOG_WARN << "forge: rules.json rule " << rule << " dropped: " << why;
		status.errors.push_back({ rule, why });
	}
	LOG_INFO << "forge: rules.json loaded: " << status.squads << " squad, " << status.unlocks << " unlock, "
		<< status.schedules << " schedule rule(s), " << status.errors.size() << " dropped (sha256 "
		<< status.sha256 << ")";
	publish(l, std::move(parsed.snapshot));
	l.status = std::move(status);
	setRulesStatus(l.status);
}

} // namespace

std::shared_ptr<const RulesSnapshot> rules()
{
	auto& l = live();
	const auto now = steadyMs();
	if (now - l.lastCheckMs.load(std::memory_order_relaxed) >= 2000)
	{
		std::unique_lock lock(l.reload, std::try_to_lock);
		if (lock.owns_lock() && now - l.lastCheckMs.load(std::memory_order_relaxed) >= 2000)
		{
			try
			{
				refresh(l);
			}
			catch (const std::exception& e)
			{
				LOG_ERROR << "forge: rules.json reload failed: " << e.what();
			}
			l.lastCheckMs.store(steadyMs(), std::memory_order_relaxed);
		}
	}
	std::lock_guard lock(l.pointer);
	return l.snapshot;
}

void loadRulesAtBoot()
{
	auto& l = live();
	std::lock_guard lock(l.reload);
	l.status.engine = true;
	setRulesStatus(l.status);
	try
	{
		refresh(l);
	}
	catch (const std::exception& e)
	{
		LOG_ERROR << "forge: rules.json could not be read at boot: " << e.what();
	}
	l.lastCheckMs.store(steadyMs(), std::memory_order_relaxed);
}


} // namespace forge
