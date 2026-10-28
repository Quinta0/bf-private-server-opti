#include "Capture.hpp"

#include "MstOverlay.hpp"

#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace forge
{

namespace
{

struct RawRule
{
	std::optional<bool> enabled;
	std::optional<double> rate;
};

struct RawCapture
{
	std::map<std::string, RawRule> units;
	std::map<std::string, std::map<std::string, RawRule>> missions;
};

struct CaptureOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};

constexpr CaptureOpts kCaptureOpts = [] {
	CaptureOpts opts{};
	opts.error_on_unknown_keys = false;   // a newer writer may add more
	return opts;
}();

std::optional<uint32_t> idOf(const std::string& text)
{
	uint32_t id = 0;
	const auto* end = text.data() + text.size();
	const auto [ptr, ec] = std::from_chars(text.data(), end, id);
	if (text.empty() || ec != std::errc{} || ptr != end || id == 0)
		return std::nullopt;
	return id;
}

/*! The error with a reason, or nullopt and the rule written into `out`. */
std::optional<std::string> ruleOf(const RawRule& raw, const std::string& where, CaptureRule& out)
{
	if (!raw.enabled && !raw.rate)
		return where + " needs \"enabled\" or \"rate\"";
	if (raw.rate)
	{
		const double r = *raw.rate;
		if (!(r >= 0.0 && r <= 100.0) || r != static_cast<double>(static_cast<uint32_t>(r)))
			return where + " rate must be a whole number from 0 to 100";
		out.rate = static_cast<uint32_t>(r);
	}
	out.enabled = raw.enabled;
	return std::nullopt;
}

std::optional<std::string> parse(const std::string& text, CaptureRules& out)
{
	RawCapture raw;
	if (const auto ec = glz::read<kCaptureOpts>(raw, text))
		return "not valid JSON: " + glz::format_error(ec, text);
	CaptureRules next;
	for (const auto& [key, rule] : raw.units)
	{
		const auto unit = idOf(key);
		if (!unit)
			return "units: \"" + key + "\" is not a unit id";
		if (auto why = ruleOf(rule, "unit " + key, next.units[*unit]))
			return why;
	}
	for (const auto& [mkey, rules] : raw.missions)
	{
		const auto mission = idOf(mkey);
		if (!mission)
			return "missions: \"" + mkey + "\" is not a mission id";
		for (const auto& [ukey, rule] : rules)
		{
			const auto unit = idOf(ukey);
			if (!unit)
				return "mission " + mkey + ": \"" + ukey + "\" is not a unit id";
			if (auto why = ruleOf(rule, "mission " + mkey + " unit " + ukey, next.missions[*mission][*unit]))
				return why;
		}
	}
	out = std::move(next);
	return std::nullopt;
}

struct Live
{
	std::mutex mutex;
	bool looked = false;
	std::filesystem::file_time_type mtime{};
	uintmax_t size = 0;
	std::shared_ptr<const CaptureRules> rules = std::make_shared<CaptureRules>();
};

void refresh(Live& l)
{
	const auto& root = balanceRoot();
	if (root.empty())
		return;
	const auto path = std::filesystem::path(root) / "server" / "capture.json";
	std::error_code ec;
	const bool present = std::filesystem::is_regular_file(path, ec);
	const auto size = present ? std::filesystem::file_size(path, ec) : 0;
	const auto mtime = present ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
	if (l.looked && size == l.size && mtime == l.mtime)
		return;
	l.looked = true;
	l.size = size;
	l.mtime = mtime;
	if (!present)
	{
		l.rules = std::make_shared<CaptureRules>();
		LOG_INFO << "forge: no capture.json; every battle keeps the archive's capture chances";
		return;
	}
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	CaptureRules next;
	if (const auto why = parse(text.str(), next))
	{
		LOG_WARN << "forge: capture.json REJECTED: " << *why << "; the last good rules stay in force";
		return;
	}
	size_t missionRules = 0;
	for (const auto& [_, rules] : next.missions)
		missionRules += rules.size();
	LOG_INFO << "forge: capture.json taken: " << next.units.size() << " unit rule(s), "
		<< missionRules << " quest rule(s) in " << next.missions.size() << " quest(s)";
	l.rules = std::make_shared<const CaptureRules>(std::move(next));
}

} // namespace

const CaptureRule* CaptureRules::find(uint32_t mission, uint32_t unit) const
{
	if (const auto m = missions.find(mission); m != missions.end())
	{
		if (const auto r = m->second.find(unit); r != m->second.end())
			return &r->second;
	}
	const auto u = units.find(unit);
	return u == units.end() ? nullptr : &u->second;
}

std::shared_ptr<const CaptureRules> captureRules()
{
	static Live l;
	std::lock_guard lock(l.mutex);
	refresh(l);
	return l.rules;
}

size_t applyCaptureRules(MissionRecord& record, const CaptureRules& rules, bool (*capturable)(uint32_t unit),
	bool handBuilt)
{
	// The tutorial walks a unit list it expects to know (MissionArchiver::applyCaptureFloor).
	if (record.id == 1 || record.id == 2 || (rules.units.empty() && rules.missions.empty()))
		return 0;
	size_t changed = 0;
	for (auto& stage : record.stages)
	{
		for (auto& monster : stage.battle_monsters)
		{
			// A rule turning on the unit the monster is DRAWN as wins over what it drops
			// today: a boss's base-form drop (applyBossDrops, #82) or an archive capture of
			// another form would otherwise hide it (7* Kyo 810187 drops his 6* 810186).
			// A hand-built battle says itself which form a capture joins as ("Joins as"),
			// so its capture keeps that unit (a 7* chosen over the drawn Omni stays the 7*).
			const bool chosen = handBuilt && monster.unit_drop_id != 0 && monster.unit_drop_chance != 0;
			if (!chosen && monster.unit_id != 0 && monster.unit_id != monster.unit_drop_id)
			{
				const auto* drawn = rules.find(record.id, monster.unit_id);
				if (drawn && drawn->enabled == true && capturable(monster.unit_id))
				{
					monster.unit_drop_id = monster.unit_id;
					monster.unit_drop_level = std::max<uint32_t>(monster.unit_drop_level, 1);
					monster.unit_drop_chance = drawn->rate.value_or(kDefaultCaptureRate);
					++changed;
					continue;
				}
			}
			const uint32_t unit = monster.unit_drop_id != 0 ? monster.unit_drop_id : monster.unit_id;
			const auto* rule = unit == 0 ? nullptr : rules.find(record.id, unit);
			if (!rule)
				continue;
			const bool captured = monster.unit_drop_id != 0 && monster.unit_drop_chance != 0;
			if (rule->enabled == false)
			{
				if (monster.unit_drop_chance != 0)
				{
					monster.unit_drop_chance = 0;
					++changed;
				}
				continue;
			}
			// A rate alone only changes a capture that is there; "enabled" adds one.
			if (!captured && (rule->enabled != true || !capturable(unit)))
				continue;
			const uint32_t chance = rule->rate.value_or(captured ? monster.unit_drop_chance : kDefaultCaptureRate);
			if (captured && monster.unit_drop_chance == chance)
				continue;
			monster.unit_drop_id = unit;
			monster.unit_drop_level = std::max<uint32_t>(monster.unit_drop_level, 1);
			monster.unit_drop_chance = chance;
			++changed;
		}
	}
	return changed;
}

} // namespace forge
