#include "Rules.hpp"
#include "RulesMap.hpp"

#include <gimuserver/gme/common/GameDay.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <utility>

/*
* The rules snapshot's queries and the pure helpers (the Forge server design,
* section 4).  Loading and validation are in RulesLoad.cpp.
*/
namespace forge
{

namespace
{

using namespace rules_map;

constexpr int64_t kWeekSeconds = 7 * 86400;
// 1970-01-01 was a Thursday: add three days so Monday 00:00 UTC is 0.
constexpr int64_t kEpochToMonday = 3 * 86400;

/*! value mod divisor in [0, divisor), for negative values too. */
int64_t posMod(const int64_t value, const int64_t divisor)
{
	return value - gme::floorDiv(value, divisor) * divisor;
}

/*! Seconds since the Monday 00:00 UTC that starts t's week. */
int64_t weekSecond(const int64_t t)
{
	return posMod(t + kEpochToMonday, kWeekSeconds);
}

std::string quoted(const int32_t missionId)
{
	return "\"" + missionName(missionId) + "\"";
}

/*! "A", "A and B", "A, B and C", "A, B, C and 2 more quests" (joiner "and" or "or"). */
std::string questList(const std::vector<int32_t>& ids, const std::string& joiner)
{
	std::vector<std::string> names;
	for (size_t i = 0; i < ids.size() && i < 3; ++i)
		names.push_back(quoted(ids[i]));
	if (ids.size() > 3)
	{
		const auto more = ids.size() - 3;
		names.push_back(std::to_string(more) + (more == 1 ? " more quest" : " more quests"));
	}
	std::string out;
	for (size_t i = 0; i < names.size(); ++i)
	{
		if (i > 0)
			out += i + 1 == names.size() ? " " + joiner + " " : ", ";
		out += names[i];
	}
	return out;
}
} // namespace

// ---------------------------------------------------------------------------
// The snapshot's queries.
// ---------------------------------------------------------------------------

void RulesSnapshot::index()
{
	for (size_t i = 0; i < squads.size(); ++i)
		for (const auto m : squads[i].missions)
			m_squadsByMission[m].push_back(i);
	for (size_t i = 0; i < unlocks.size(); ++i)
	{
		const auto& u = unlocks[i];
		for (const auto m : u.missions) m_unlocksByMission[m].push_back(i);
		for (const auto d : u.dungeons) m_unlocksByDungeon[d].push_back(i);
		for (const auto a : u.targets.areas) m_unlocksByArea[a].push_back(i);
		for (const auto g : u.targets.gates) m_gateUnlocks[g].push_back(i);
		m_hidesTiles = m_hidesTiles || !u.missions.empty() || !u.dungeons.empty() || !u.targets.areas.empty();
	}
	for (size_t i = 0; i < schedules.size(); ++i)
	{
		const auto& s = schedules[i];
		for (const auto m : s.targets.missions) m_missionSchedules[m].push_back(i);
		for (const auto d : s.targets.dungeons) m_dungeonSchedules[d].push_back(i);
		for (const auto a : s.targets.areas) m_areaSchedules[a].push_back(i);
		for (const auto g : s.targets.gates) m_gateSchedules[g].push_back(i);
		m_hidesTiles = m_hidesTiles || !s.targets.missions.empty() || !s.targets.dungeons.empty() || !s.targets.areas.empty();
	}
}

RulesSnapshot::Levels RulesSnapshot::questLevels(const int32_t missionId) const
{
	Levels levels;
	const auto& idx = mapIndex();
	const auto mission = idx.missions.find(missionId);
	const auto dungeon = mission == idx.missions.end() ? 0 : mission->second.dungeon;
	auto area = areaOfDungeon(dungeon);
	if (area == 0 && mission != idx.missions.end())
		area = mission->second.area;
	for (const auto& [index, id] : { std::pair{ &m_missionSchedules, missionId },
		std::pair{ &m_dungeonSchedules, dungeon }, std::pair{ &m_areaSchedules, area } })
		if (const auto it = index->find(id); it != index->end())
			levels.push_back(&it->second);
	return levels;
}

bool RulesSnapshot::levelsOpen(const Levels& levels, const int64_t now) const
{
	return std::all_of(levels.begin(), levels.end(), [&](const std::vector<size_t>* level) {
		return std::any_of(level->begin(), level->end(), [&](const size_t i) { return isOpen(schedules[i], now); });
	});
}

std::optional<Refusal> RulesSnapshot::scheduleRefusal(const Levels& levels, const bool banner, const int64_t now) const
{
	if (levelsOpen(levels, now))
		return std::nullopt;
	// The rule that speaks: the first entry of the first closed level.
	const ScheduleRule* speaker = nullptr;
	for (const auto* level : levels)
		if (!levelsOpen({ level }, now))
		{
			speaker = &schedules[level->front()];
			break;
		}
	Refusal out{ speaker->id, speaker->message };
	if (!out.message.empty())
		return out;
	// When every level is open again: step to the latest next opening of the
	// closed levels until all agree (or one never opens again).
	std::optional<int64_t> when;
	int64_t t = now;
	for (int round = 0; round < 64 && !when; ++round)
	{
		int64_t latest = t;
		bool allOpen = true;
		bool never = false;
		for (const auto* level : levels)
		{
			if (levelsOpen({ level }, t))
				continue;
			allOpen = false;
			std::optional<int64_t> best;
			for (const auto i : *level)
				if (const auto next = nextOpening(schedules[i], t); next && (!best || *next < *best))
					best = next;
			if (!best)
			{
				never = true;
				break;
			}
			latest = std::max(latest, *best);
		}
		if (never)
			break;
		if (allOpen)
			when = t;
		t = latest;
	}
	const std::string what = banner ? "This banner" : "This quest";
	out.message = when ? what + " is closed right now. It opens again on " + openingText(*when) + "."
		: what + " has ended.";
	return out;
}

bool RulesSnapshot::unlockMet(const size_t rule, const std::set<int32_t>& cleared) const
{
	const auto& u = unlocks[rule];
	const auto done = [&cleared](const int32_t id) { return cleared.contains(id); };
	return u.needsAll ? std::all_of(u.needs.begin(), u.needs.end(), done)
		: std::any_of(u.needs.begin(), u.needs.end(), done);
}

std::optional<Refusal> RulesSnapshot::unlockRefusal(const std::vector<size_t>& rules, const std::set<int32_t>& cleared,
	const bool banner) const
{
	for (const auto i : rules)
	{
		if (unlockMet(i, cleared))
			continue;
		const auto& u = unlocks[i];
		Refusal out{ u.id, u.message };
		if (out.message.empty())
		{
			std::vector<int32_t> missing;
			for (const auto need : u.needs)
				if (!u.needsAll || !cleared.contains(need))
					missing.push_back(need);
			const auto list = questList(missing, u.needsAll ? "and" : "or");
			out.message = banner ? "This banner opens when you clear " + list + "."
				: "This quest is locked. Clear " + list + " first.";
		}
		return out;
	}
	return std::nullopt;
}

std::optional<Refusal> RulesSnapshot::questRefusal(const int32_t missionId, const std::set<int32_t>& cleared,
	const int64_t now) const
{
	if (auto closed = scheduleRefusal(questLevels(missionId), false, now))
		return closed;
	if (const auto it = m_unlocksByMission.find(missionId); it != m_unlocksByMission.end())
		return unlockRefusal(it->second, cleared, false);
	return std::nullopt;
}

std::optional<Refusal> RulesSnapshot::gateRefusal(const int32_t gateId, const std::set<int32_t>& cleared,
	const int64_t now) const
{
	if (const auto it = m_gateSchedules.find(gateId); it != m_gateSchedules.end())
		if (auto closed = scheduleRefusal({ &it->second }, true, now))
			return closed;
	if (const auto it = m_gateUnlocks.find(gateId); it != m_gateUnlocks.end())
		return unlockRefusal(it->second, cleared, true);
	return std::nullopt;
}

std::vector<const SquadRule*> RulesSnapshot::squadRulesFor(const int32_t missionId) const
{
	std::vector<const SquadRule*> out;
	if (const auto it = m_squadsByMission.find(missionId); it != m_squadsByMission.end())
		for (const auto i : it->second)
			out.push_back(&squads[i]);
	return out;
}

Hidden RulesSnapshot::hiddenFor(const std::set<int32_t>& cleared, const int64_t now) const
{
	Hidden out;
	if (!m_hidesTiles)
		return out;
	const auto& idx = mapIndex();
	const auto closed = [&](const std::vector<size_t>& level) { return !levelsOpen({ &level }, now); };
	const auto locked = [&](const std::vector<size_t>& rules) {
		return std::any_of(rules.begin(), rules.end(), [&](const size_t i) { return !unlockMet(i, cleared); });
	};

	// Down: closed or locked areas and dungeons, and every quest a rule reaches.
	std::set<int32_t> candidates;
	for (const auto& [m, _] : m_missionSchedules) candidates.insert(m);
	for (const auto& [m, _] : m_unlocksByMission) candidates.insert(m);
	for (const auto& [area, level] : m_areaSchedules)
	{
		if (!closed(level))
			continue;
		out.areas.insert(area);
		if (const auto it = idx.dungeonsByArea.find(area); it != idx.dungeonsByArea.end())
			out.dungeons.insert(it->second.begin(), it->second.end());
	}
	for (const auto& [dungeon, level] : m_dungeonSchedules)
		if (closed(level))
			out.dungeons.insert(dungeon);
	for (const auto& [dungeon, rules] : m_unlocksByDungeon)
		if (locked(rules))
			out.dungeons.insert(dungeon);
	for (const auto& [area, rules] : m_unlocksByArea)
		if (locked(rules))
			out.areas.insert(area);
	for (const auto dungeon : out.dungeons)
		if (const auto* list = missionsOf(dungeon))
			candidates.insert(list->begin(), list->end());
	for (const auto m : candidates)
		if (questRefusal(m, cleared, now))
			out.missions.insert(m);

	// Up: a dungeon whose every quest is hidden, then an area whose every dungeon is.
	std::set<int32_t> touched;
	for (const auto m : out.missions)
		if (const auto it = idx.missions.find(m); it != idx.missions.end())
			touched.insert(it->second.dungeon);
	for (const auto dungeon : touched)
	{
		const auto* list = missionsOf(dungeon);
		if (list && !list->empty()
			&& std::all_of(list->begin(), list->end(), [&](const int32_t m) { return out.missions.contains(m); }))
			out.dungeons.insert(dungeon);
	}
	std::set<int32_t> areas;
	for (const auto dungeon : out.dungeons)
		areas.insert(areaOfDungeon(dungeon));
	for (const auto area : areas)
	{
		const auto it = idx.dungeonsByArea.find(area);
		if (it != idx.dungeonsByArea.end() && !it->second.empty()
			&& std::all_of(it->second.begin(), it->second.end(), [&](const int32_t d) { return out.dungeons.contains(d); }))
			out.areas.insert(area);
	}
	return out;
}

std::set<int32_t> RulesSnapshot::hiddenGates(const std::set<int32_t>& cleared, const int64_t now) const
{
	std::set<int32_t> gates;
	for (const auto& [g, _] : m_gateSchedules) gates.insert(g);
	for (const auto& [g, _] : m_gateUnlocks) gates.insert(g);
	std::set<int32_t> out;
	for (const auto g : gates)
		if (gateRefusal(g, cleared, now))
			out.insert(g);
	return out;
}

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

bool isOpen(const ScheduleRule& rule, const int64_t now)
{
	if (rule.open && now < *rule.open)
		return false;
	if (rule.close && now >= *rule.close)
		return false;
	if (rule.weekly.empty())
		return true;
	const auto s = weekSecond(now);
	return std::any_of(rule.weekly.begin(), rule.weekly.end(), [s](const WeeklySpan& span) {
		const int64_t from = span.from * 60LL, to = span.to * 60LL;
		return from < to ? (s >= from && s < to) : (s >= from || s < to);
	});
}

std::optional<int64_t> nextOpening(const ScheduleRule& rule, const int64_t now)
{
	if (rule.close && now >= *rule.close)
		return std::nullopt;
	int64_t t = now;
	if (rule.open && t < *rule.open)
		t = *rule.open;
	if (!rule.weekly.empty() && !isOpen(rule, t))
	{
		const auto s = weekSecond(t);
		std::optional<int64_t> best;
		for (const auto& span : rule.weekly)
		{
			const auto delta = posMod(span.from * 60LL - s, kWeekSeconds);
			if (!best || t + delta < *best)
				best = t + delta;
		}
		t = *best;
	}
	if (rule.close && t >= *rule.close)
		return std::nullopt;
	return t;
}

std::string openingText(const int64_t when)
{
	static constexpr const char* kDays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	const auto t = static_cast<std::time_t>(when);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char clock[8];
	std::snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
	return std::string(kDays[tm.tm_wday]) + " " + std::to_string(tm.tm_mday) + " " + kMonths[tm.tm_mon] + " at " + clock + " UTC";
}

void dropHiddenPermitRows(std::string& rows, const Hidden& hidden)
{
	if (hidden.empty() || rows.empty())
		return;
	const auto hiddenRow = [&hidden](std::string_view row) {
		for (const auto& [key, ids] : { std::pair{ std::string_view(R"({"VjCY7rX4":")"), &hidden.areas },
			std::pair{ std::string_view(R"({"MHx05sXt":")"), &hidden.dungeons },
			std::pair{ std::string_view(R"({"j28VNcUW":")"), &hidden.missions } })
		{
			if (!row.starts_with(key))
				continue;
			row.remove_prefix(key.size());
			int32_t id = 0;
			const auto [ptr, ec] = std::from_chars(row.data(), row.data() + row.size(), id);
			return ec == std::errc{} && ptr < row.data() + row.size() && *ptr == '"' && ids->contains(id);
		}
		return false;
	};
	std::string kept;
	kept.reserve(rows.size());
	size_t pos = 0;
	while (pos < rows.size())
	{
		const auto split = rows.find("},{", pos);
		const auto end = split == std::string::npos ? rows.size() : split + 1;
		const std::string_view row(rows.data() + pos, end - pos);
		if (!hiddenRow(row))
		{
			if (!kept.empty())
				kept += ',';
			kept += row;
		}
		pos = split == std::string::npos ? rows.size() : split + 2;
	}
	rows = std::move(kept);
}

bool dropHiddenGates(std::string& list, const std::set<int32_t>& hiddenGates)
{
	if (hiddenGates.empty() || list.empty())
		return false;
	std::string kept;
	bool dropped = false;
	std::string_view rest = list;
	while (true)
	{
		const auto comma = rest.find(',');
		const auto item = rest.substr(0, comma);
		int32_t id = 0;
		const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), id);
		if (!item.empty() && ec == std::errc{} && ptr == item.data() + item.size() && hiddenGates.contains(id))
			dropped = true;
		else
		{
			if (!kept.empty())
				kept += ',';
			kept += item;
		}
		if (comma == std::string_view::npos)
			break;
		rest.remove_prefix(comma + 1);
	}
	if (dropped)
		list = std::move(kept);
	return dropped;
}

} // namespace forge
