#include "RulesMap.hpp"

#include "App.hpp"

#include <algorithm>
#include <charconv>

namespace forge::rules_map
{

static int32_t findRoot(std::unordered_map<int32_t, int32_t>& parent, int32_t id)
{
	auto it = parent.try_emplace(id, id).first;
	while (it->second != id)
	{
		id = it->second;
		it = parent.find(id);
	}
	return id;
}

static MapIndex buildIndex()
{
	MapIndex out;
	const auto& cache = theServer()->cache();
	for (const auto& m : cache.missionMst())
		out.missions[m.id] = { m.dungeon_id, m.area_id, m.name_key };
	for (const auto& d : cache.dungeonMst())
	{
		out.dungeons[d.dungeon_id] = { d.area_id, d.dungeon_type };
		out.dungeonsByArea[d.area_id].push_back(d.dungeon_id);
	}
	for (const auto& a : cache.areaMst())
	{
		out.areas.insert(a.area_id);
		if (!a.parent_area_id || a.parent_area_id->empty())
			continue;
		int32_t parent = 0;
		const auto& text = *a.parent_area_id;
		const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parent);
		if (ec == std::errc{} && ptr == text.data() + text.size() && parent != 0)
			out.categoryAreas.insert(parent);
	}
	for (const auto& u : cache.unitMst())
		out.units[u.id] = { u.element, u.cost, u.unit_name, u.rarity, u.max_hp, u.max_atk, u.max_def, u.max_rec };
	std::unordered_map<int32_t, int32_t> parent;
	for (const auto& e : cache.unitEvoMst())
	{
		if (e.unit_id <= 0 || e.evo_unit_id <= 0)
			continue;
		const auto a = findRoot(parent, e.unit_id);
		const auto b = findRoot(parent, e.evo_unit_id);
		if (a != b)
			parent[std::max(a, b)] = std::min(a, b);
	}
	for (const auto& [id, _] : parent)
		out.lineRoot[id] = 0;
	for (auto& [id, root] : out.lineRoot)
		root = findRoot(parent, id);
	return out;
}

const MapIndex& mapIndex()
{
	static const MapIndex index = buildIndex();
	return index;
}

const std::vector<int32_t>* missionsOf(const int32_t dungeonId)
{
	const auto& byDungeon = theServer()->cache().missionsByDungeon();
	const auto it = byDungeon.find(dungeonId);
	return it == byDungeon.end() ? nullptr : &it->second;
}

int32_t areaOfDungeon(const int32_t dungeonId)
{
	const auto& d = mapIndex().dungeons;
	const auto it = d.find(dungeonId);
	return it == d.end() ? 0 : it->second.area;
}

void expand(const Targets& t, std::set<int32_t>& missions, std::set<int32_t>& dungeons)
{
	const auto& idx = mapIndex();
	missions.insert(t.missions.begin(), t.missions.end());
	std::vector<int32_t> named = t.dungeons;
	for (const auto area : t.areas)
		if (const auto it = idx.dungeonsByArea.find(area); it != idx.dungeonsByArea.end())
			named.insert(named.end(), it->second.begin(), it->second.end());
	for (const auto dungeon : named)
	{
		dungeons.insert(dungeon);
		if (const auto* list = missionsOf(dungeon))
			missions.insert(list->begin(), list->end());
	}
}

std::set<int32_t> lineOf(const int32_t unitId)
{
	const auto& roots = mapIndex().lineRoot;
	const auto it = roots.find(unitId);
	if (it == roots.end())
		return { unitId };
	std::set<int32_t> out;
	for (const auto& [id, root] : roots)
		if (root == it->second)
			out.insert(id);
	return out;
}

} // namespace forge::rules_map

namespace forge
{

using rules_map::mapIndex;

std::string missionName(const int32_t missionId)
{
	const auto& m = mapIndex().missions;
	const auto it = m.find(missionId);
	if (it == m.end() || it->second.name.empty())
		return "quest " + std::to_string(missionId);
	return it->second.name;
}

int32_t missionDungeonType(const int32_t missionId)
{
	const auto& idx = mapIndex();
	const auto m = idx.missions.find(missionId);
	if (m == idx.missions.end())
		return 0;
	const auto d = idx.dungeons.find(m->second.dungeon);
	return d == idx.dungeons.end() ? 0 : d->second.type;
}

std::optional<std::pair<int32_t, int32_t>> trialDeckRange(const int32_t missionId)
{
	switch (missionDungeonType(missionId))
	{
	case 2: return std::pair{ 0, 2 };     // Trial Zone: the three trial squads
	case 8: return std::pair{ 99, 99 };   // Strategy Zone
	case 9: return std::pair{ 10, 10 };   // Battle Hall (Ep3)
	default: return std::nullopt;
	}
}

const UnitFacts* unitFacts(const int32_t unitId)
{
	const auto& u = mapIndex().units;
	const auto it = u.find(unitId);
	return it == u.end() ? nullptr : &it->second;
}

std::string_view elementName(const int32_t element)
{
	static constexpr std::string_view kNames[] = { "Fire", "Water", "Earth", "Thunder", "Light", "Dark" };
	return element >= 1 && element <= 6 ? kNames[element - 1] : std::string_view("unknown");
}

} // namespace forge
