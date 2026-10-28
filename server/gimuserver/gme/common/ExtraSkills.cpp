#include "App.hpp"

#include <gimuserver/db/PacketInterface.hpp>
#include <gimuserver/gme/common/ExtraSkills.hpp>
#include <gimuserver/utils/Random.hpp>

#include <charconv>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace gme
{

namespace
{

// Built once: the MSTs load at startup and these two lookups run for every
// unit of every roster read.
const std::unordered_map<uint32_t, int32_t>& ownSkillByUnit()
{
	static const auto index = [] {
		std::unordered_map<uint32_t, int32_t> out;
		for (const auto& unit : theServer()->cache().unitMst())
			if (unit.extra_passive_skill_id > 0)
				out.emplace(static_cast<uint32_t>(unit.id), unit.extra_passive_skill_id);
		return out;
	}();
	return index;
}

const std::unordered_set<int32_t>& knownSkills()
{
	static const auto index = [] {
		std::unordered_set<int32_t> out;
		for (const auto& skill : theServer()->cache().initializeResp().extra_passive_skills)
			out.insert(skill.id);
		return out;
	}();
	return index;
}

} // namespace

int32_t randomElgifSkill()
{
	std::lock_guard lock(RandomMutex());
	const auto pick = std::uniform_int_distribution<size_t>(0, kElgifDropSkills.size() - 1)(RandomEngine());
	return kElgifDropSkills[pick];
}

int32_t extraSkillFromParam(const std::string_view packed)
{
	const auto colon = packed.rfind(':');
	if (colon == std::string_view::npos)
		return 0;
	const auto text = packed.substr(colon + 1);
	int32_t id = 0;
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
	if (ec != std::errc{} || end != text.data() + text.size() || id <= 0)
		return 0;
	return knownSkills().contains(id) ? id : 0;
}

int32_t unitOwnExtraSkill(const uint32_t unitId)
{
	const auto& index = ownSkillByUnit();
	const auto it = index.find(unitId);
	return it == index.end() ? 0 : it->second;
}

} // namespace gme

// Every UserUnitInfo the database layer reads goes out with the unit's own
// Extra Skill.  It is MST data, not a column, so evolutions and the units
// granted before #38 carry the right one with no migration.
void db::PacketReadHook<::UserUnitInfo>::apply(::UserUnitInfo& unit)
{
	unit.extra_passive_skill_id = gme::unitOwnExtraSkill(unit.unit_id);
}
