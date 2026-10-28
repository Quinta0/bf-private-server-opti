#include "RepeatClear.hpp"

#include <gimuserver/gme/common/ExtraSkills.hpp>

#include <charconv>
#include <map>

namespace forge
{

namespace
{

std::map<int32_t, RepeatRewards>& registry()
{
	static std::map<int32_t, RepeatRewards> instance;
	return instance;
}

std::optional<int32_t> number(std::string_view text)
{
	int32_t value = 0;
	const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size())
		return std::nullopt;
	return value;
}

std::vector<std::string_view> split(std::string_view text, char sep)
{
	std::vector<std::string_view> out;
	while (true)
	{
		const auto at = text.find(sep);
		out.push_back(text.substr(0, at));
		if (at == std::string_view::npos)
			break;
		text.remove_prefix(at + 1);
	}
	return out;
}

} // namespace

std::vector<ClearReward> parseClearRewards(const std::string_view raw)
{
	std::vector<ClearReward> out;
	if (raw.empty())
		return out;
	for (const auto entry : split(raw, ','))
	{
		// type:id:amount, two fields nothing reads, and the Elgif skill.
		const auto parts = split(entry, ':');
		if (parts.size() < 3)
			continue;
		const auto type = number(parts[0]);
		const auto amount = number(parts[2]);
		if (!type || !amount || *type <= 0 || *amount <= 0)
			continue;   // malformed, or the daily: prefix: skip it
		const auto extraSkill = (*type == 6 && parts.size() == 6) ? gme::extraSkillFromParam(entry) : 0;
		out.push_back({ *type, parts[1] == "0" ? std::string() : std::string(parts[1]), *amount, extraSkill });
	}
	return out;
}

std::optional<RepeatRewards> parseRepeatRewards(const std::string_view raw)
{
	RepeatRewards out;
	auto list = raw;
	if (list.starts_with("daily:"))
	{
		const auto comma = list.find(',');
		const auto cap = number(list.substr(6, comma == std::string_view::npos ? std::string_view::npos : comma - 6));
		if (!cap || *cap < 1)
			return std::nullopt;
		out.dailyLimit = *cap;
		list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
	}
	out.rewards = parseClearRewards(list);
	if (out.rewards.empty())
		return std::nullopt;
	return out;
}

void noteRepeatRewards(const int32_t missionId, const std::string_view raw)
{
	if (raw.empty())
		return;
	if (auto parsed = parseRepeatRewards(raw))
		registry().insert_or_assign(missionId, std::move(*parsed));
}

const RepeatRewards* repeatRewards(const int32_t missionId)
{
	const auto& r = registry();
	const auto it = r.find(missionId);
	return it == r.end() ? nullptr : &it->second;
}

} // namespace forge
