#include "Tuning.hpp"

#include "MstOverlay.hpp"

#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <algorithm>

namespace forge
{

namespace
{

struct RawTuning
{
	std::optional<double> fusion_sp_chance_pct;
	std::optional<double> fusion_sp_chance_per_star_pct;
	std::optional<double> dup_evo_enabled;
	std::optional<double> dup_evo_zel_pct;
	std::optional<double> dup_evo_karma_pct;
	std::optional<double> capture_floor_pct;
	std::optional<double> boss_drop_pct_r1, boss_drop_pct_r2, boss_drop_pct_r3, boss_drop_pct_r4;
	std::optional<double> boss_drop_pct_r5, boss_drop_pct_r6, boss_drop_pct_r7, boss_drop_pct_r8;
	std::optional<double> boss_drop_merit_pct;
};

struct TuningOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};

constexpr TuningOpts kTuningOpts = [] {
	TuningOpts opts{};
	opts.error_on_unknown_keys = false;
	return opts;
}();

struct Live
{
	std::mutex mutex;
	bool looked = false;
	std::filesystem::file_time_type mtime{};
	uintmax_t size = 0;
	Tuning values;
};

/*! The error with a reason, or nullopt and the values written into `out`. */
std::optional<std::string> parse(const std::string& text, Tuning& out)
{
	RawTuning raw;
	if (const auto ec = glz::read<kTuningOpts>(raw, text))
		return "not valid JSON: " + glz::format_error(ec, text);
	const auto percent = [](const std::optional<double>& v, double& into, const char* key) -> std::optional<std::string> {
		if (!v)
			return std::nullopt;
		if (!(*v >= 0.0 && *v <= 100.0))
			return std::string(key) + " must be from 0 to 100";
		into = *v;
		return std::nullopt;
	};
	Tuning next;
	if (auto why = percent(raw.fusion_sp_chance_pct, next.fusionSpChancePct, "fusion_sp_chance_pct"))
		return why;
	if (auto why = percent(raw.fusion_sp_chance_per_star_pct, next.fusionSpChancePerStarPct,
			"fusion_sp_chance_per_star_pct"))
		return why;
	if (raw.dup_evo_enabled)
	{
		if (*raw.dup_evo_enabled != 0.0 && *raw.dup_evo_enabled != 1.0)
			return std::string("dup_evo_enabled must be 0 or 1");
		next.dupEvoEnabled = *raw.dup_evo_enabled == 1.0;
	}
	if (auto why = percent(raw.dup_evo_zel_pct, next.dupEvoZelPct, "dup_evo_zel_pct"))
		return why;
	if (auto why = percent(raw.dup_evo_karma_pct, next.dupEvoKarmaPct, "dup_evo_karma_pct"))
		return why;
	// Whole percentages: they become uint32 chances on the wire.
	const auto whole = [](const std::optional<double>& v, const std::string& key) -> std::variant<std::monostate, uint32_t, std::string> {
		if (!v)
			return std::monostate{};
		if (!(*v >= 0.0 && *v <= 100.0) || *v != static_cast<double>(static_cast<uint32_t>(*v)))
			return key + " must be a whole number from 0 to 100";
		return static_cast<uint32_t>(*v);
	};
	const auto take = [&](const std::optional<double>& v, const std::string& key, auto&& into) -> std::optional<std::string> {
		auto got = whole(v, key);
		if (auto* why = std::get_if<std::string>(&got))
			return *why;
		if (auto* n = std::get_if<uint32_t>(&got))
			into(*n);
		return std::nullopt;
	};
	if (auto why = take(raw.capture_floor_pct, "capture_floor_pct", [&](uint32_t n) { next.captureFloorPct = n; }))
		return why;
	const std::optional<double>* boss[9] = { nullptr, &raw.boss_drop_pct_r1, &raw.boss_drop_pct_r2, &raw.boss_drop_pct_r3,
		&raw.boss_drop_pct_r4, &raw.boss_drop_pct_r5, &raw.boss_drop_pct_r6, &raw.boss_drop_pct_r7, &raw.boss_drop_pct_r8 };
	for (size_t r = 1; r < 9; ++r)
	{
		if (auto why = take(*boss[r], "boss_drop_pct_r" + std::to_string(r), [&](uint32_t n) { next.bossDropPct[r] = n; }))
			return why;
	}
	if (auto why = take(raw.boss_drop_merit_pct, "boss_drop_merit_pct", [&](uint32_t n) { next.bossDropMeritPct = n; }))
		return why;
	out = next;
	return std::nullopt;
}

void refresh(Live& l)
{
	const auto& root = balanceRoot();
	if (root.empty())
		return;
	const auto path = std::filesystem::path(root) / "server" / "tuning.json";
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
		l.values = Tuning{};
		LOG_INFO << "forge: no tuning.json; the server's own defaults apply";
		return;
	}
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	if (const auto why = parse(text.str(), l.values))
	{
		LOG_WARN << "forge: tuning.json REJECTED: " << *why << "; the last good values stay in force";
		return;
	}
	LOG_INFO << "forge: tuning.json taken: fusion_sp_chance_pct " << l.values.fusionSpChancePct
		<< ", fusion_sp_chance_per_star_pct " << l.values.fusionSpChancePerStarPct
		<< ", dup_evo_enabled " << l.values.dupEvoEnabled
		<< ", dup_evo_zel_pct " << l.values.dupEvoZelPct
		<< ", dup_evo_karma_pct " << l.values.dupEvoKarmaPct
		<< ", capture_floor_pct " << l.values.captureFloorPct
		<< ", boss drop overrides " << std::ranges::count_if(l.values.bossDropPct, [](const auto& v) { return v.has_value(); })
		<< (l.values.bossDropMeritPct ? ", boss_drop_merit_pct " + std::to_string(*l.values.bossDropMeritPct) : std::string());
}

} // namespace

Tuning tuning()
{
	static Live l;
	std::lock_guard lock(l.mutex);
	refresh(l);
	return l.values;
}

} // namespace forge
