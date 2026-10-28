#pragma once

#include "Rules.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/*!
* The map and unit facts the Forge rules are checked and expanded against:
* boot-immutable cache data (balance overlays applied), indexed once on first
* use.  Internal to forge/Rules*.cpp.
*/
namespace forge::rules_map
{

struct MissionFacts
{
	int32_t dungeon = 0;
	int32_t area = 0;
	std::string name;
};

struct DungeonFacts
{
	int32_t area = 0;
	int32_t type = 0;
};

struct MapIndex
{
	std::unordered_map<int32_t, MissionFacts> missions;
	std::unordered_map<int32_t, DungeonFacts> dungeons;
	std::set<int32_t> areas;
	std::set<int32_t> categoryAreas;                    // another area's sHMai2ZG parent
	std::map<int32_t, std::vector<int32_t>> dungeonsByArea;
	std::unordered_map<int32_t, UnitFacts> units;
	std::unordered_map<int32_t, int32_t> lineRoot;      // evolution line, union-find root
};


const MapIndex& mapIndex();

/*! mission_mst quests of a dungeon (missionsByDungeon), or null. */
const std::vector<int32_t>* missionsOf(int32_t dungeonId);

/*! dungeon_mst area of a dungeon, or 0. */
int32_t areaOfDungeon(int32_t dungeonId);

/*! The quests and dungeons a rule's map targets cover: named, and everything inside. */
void expand(const Targets& targets, std::set<int32_t>& missions, std::set<int32_t>& dungeons);

/*! Every unit of the evolution line (unit_evo_mst, both ways); the unit alone when it has none. */
std::set<int32_t> lineOf(int32_t unitId);

} // namespace forge::rules_map
