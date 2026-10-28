#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/*!
* Overlay rules: <balance_root>/server/rules.json.
*
* Squad rules (what a squad may field in a quest), unlock rules (a quest,
* dungeon, area or banner needs quests cleared first) and schedule windows
* (a quest, dungeon, area or banner is open only between UTC times, with an
* optional weekly repeat).  Format, semantics and messages:
* BALANCE_OVERLAYS.md, section 4.
*
* The file is re-read live: rules() stats it at most every 2 s and builds a
* new immutable snapshot when it changed.  A file that does not read keeps the
* previous snapshot live (at boot: no rules) and says why in forgeStatus.php;
* a rule that does not check out is dropped on its own and listed, the rest
* load.  Every query below is a pure function of the snapshot, the player's
* cleared missions and the time, so handlers never lock anything.
*/
namespace forge
{

enum class HelperRule : uint8_t
{
	Any,
	None,         //!< no helper at all
	OwnUnits,     //!< only one of the player's own units (the MYUNIT cards)
	FriendsOnly,  //!< only a helper from the friend list
};

/*! What a rule names, as written: mission_mst, dungeon_mst and area_mst ids, and summon gates. */
struct Targets
{
	std::vector<int32_t> missions;
	std::vector<int32_t> dungeons;
	std::vector<int32_t> areas;
	std::vector<int32_t> gates;
};

struct SquadRule
{
	std::string id;
	Targets targets;
	std::vector<int32_t> elements;          //!< allowed squad elements, file order; empty = any
	bool oneElement = false;
	int32_t maxCost = 0;                    //!< 0 = no limit (helper not counted)
	int32_t maxUnits = 0;                   //!< 0 = no limit (helper not counted)
	std::set<int32_t> banned;               //!< exact unit ids; the helper too
	std::vector<int32_t> required;          //!< as listed
	std::vector<std::set<int32_t>> requiredLines;  //!< each required unit's whole evolution line
	HelperRule helper = HelperRule::Any;
	std::vector<int32_t> helperElements;    //!< empty = any
	std::string message;
	std::set<int32_t> missions;             //!< expanded: every quest the rule covers
};

struct UnlockRule
{
	std::string id;
	Targets targets;
	std::vector<int32_t> needs;
	bool needsAll = true;
	std::string message;
	std::set<int32_t> missions;             //!< expanded quests
	std::set<int32_t> dungeons;             //!< expanded dungeons (named, and those of named areas)
};

/*! A weekly span in minutes since Monday 00:00 UTC; from > to wraps past Sunday. */
struct WeeklySpan
{
	int32_t from = 0;
	int32_t to = 0;
};

struct ScheduleRule
{
	std::string id;
	Targets targets;
	std::optional<int64_t> open;            //!< inclusive, unix seconds
	std::optional<int64_t> close;           //!< exclusive
	std::vector<WeeklySpan> weekly;
	std::string message;
};

/*! Why a start or a pull is refused: the rule's id (for the log) and the player's message. */
struct Refusal
{
	std::string rule;
	std::string message;
};

/*! What the permit lists must not show right now. */
struct Hidden
{
	std::set<int32_t> missions;
	std::set<int32_t> dungeons;
	std::set<int32_t> areas;
	bool empty() const { return missions.empty() && dungeons.empty() && areas.empty(); }
};

/*! One unit as the squad rules read it (unit_mst, balance overlay applied). */
struct UnitFacts
{
	int32_t element = 0;
	int32_t cost = 0;
	std::string name;
	int32_t rarity = 0;                                 //!< 1-7, 8 = Omni
	int32_t maxHp = 0, maxAtk = 0, maxDef = 0, maxRec = 0;  //!< max-level stats (a computer helper's)
};

class RulesSnapshot
{
public:
	std::vector<SquadRule> squads;
	std::vector<UnlockRule> unlocks;
	std::vector<ScheduleRule> schedules;

	bool empty() const { return squads.empty() && unlocks.empty() && schedules.empty(); }

	/*! Schedule, then unlock: why the quest cannot be started now, or nullopt. */
	std::optional<Refusal> questRefusal(int32_t missionId, const std::set<int32_t>& cleared, int64_t now) const;

	/*! Schedule, then unlock: why the banner cannot be pulled now, or nullopt. */
	std::optional<Refusal> gateRefusal(int32_t gateId, const std::set<int32_t>& cleared, int64_t now) const;

	/*! The squad rules that apply to a mission (named by it, its dungeon or its area), file order. */
	std::vector<const SquadRule*> squadRulesFor(int32_t missionId) const;

	/*! Whether any rule can hide a quest map tile / a rail banner. */
	bool hidesTiles() const { return m_hidesTiles; }
	bool hidesGates() const { return !m_gateSchedules.empty() || !m_gateUnlocks.empty(); }
	/*! Whether a banner rule depends on the player's clears (so the rail must read them). */
	bool gateUnlocks() const { return !m_gateUnlocks.empty(); }

	/*! Every quest, dungeon and area closed or locked for this player now, expanded down and up the map. */
	Hidden hiddenFor(const std::set<int32_t>& cleared, int64_t now) const;

	/*! Every banner closed or locked for this player now. */
	std::set<int32_t> hiddenGates(const std::set<int32_t>& cleared, int64_t now) const;

	/*! Builds the lookups below from the final rule lists. */
	void index();

private:
	using Index = std::map<int32_t, std::vector<size_t>>;
	using Levels = std::vector<const std::vector<size_t>*>;
	Levels questLevels(int32_t missionId) const;
	bool levelsOpen(const Levels& levels, int64_t now) const;
	std::optional<Refusal> scheduleRefusal(const Levels& levels, bool banner, int64_t now) const;
	bool unlockMet(size_t rule, const std::set<int32_t>& cleared) const;
	std::optional<Refusal> unlockRefusal(const std::vector<size_t>& rules, const std::set<int32_t>& cleared,
		bool banner) const;

	bool m_hidesTiles = false;
	Index m_squadsByMission;      // expanded
	Index m_unlocksByMission;     // expanded
	Index m_unlocksByDungeon;     // expanded
	Index m_unlocksByArea;        // as named
	Index m_gateUnlocks;
	Index m_missionSchedules;     // as named, one index per map level
	Index m_dungeonSchedules;
	Index m_areaSchedules;
	Index m_gateSchedules;
};

/*! The live rules (never null; empty without a file).  Re-reads the file at most every 2 s. */
std::shared_ptr<const RulesSnapshot> rules();

/*! End of boot: the first read, so forgeStatus.php reports the file from the start. */
void loadRulesAtBoot();

/*! The mission's name from the server's mission_mst, or "quest <id>". */
std::string missionName(int32_t missionId);

/*! The mission's dungeon_mst dungeon_type (2 Trial Zone, 8 Strategy Zone, 9 Battle Hall), or 0. */
int32_t missionDungeonType(int32_t missionId);

/*!
* The trial squads (user_trial_decks deck numbers, inclusive) that fight a
* trial mission, or nullopt for a quest fought with a normal squad.  The
* client names its normal squad in Z0Y4RoD7 even for a trial, so a trial is
* told by its dungeon_type: 2 Trial Zone 0-2, 8 Strategy Zone 99, 9 Battle Hall 10.
*/
std::optional<std::pair<int32_t, int32_t>> trialDeckRange(int32_t missionId);

/*! unit_mst facts for a unit id, or null for a unit the game does not have. */
const UnitFacts* unitFacts(int32_t unitId);

/*! Element 1..6 as the game names it ("Fire" ... "Dark"). */
std::string_view elementName(int32_t element);

/*! Whether one schedule entry is open at `now`, and when it next opens (nullopt: never again). */
bool isOpen(const ScheduleRule& rule, int64_t now);
std::optional<int64_t> nextOpening(const ScheduleRule& rule, int64_t now);

/*! "Sat 1 Nov at 08:00 UTC". */
std::string openingText(int64_t when);

/*!
* Drops permit rows (`{"<key>":"<id>",...},{...}`) whose first pair names a
* hidden area (VjCY7rX4), dungeon (MHx05sXt) or mission (j28VNcUW).
*/
void dropHiddenPermitRows(std::string& rows, const Hidden& hidden);

/*! Removes hidden gates from one comma list (3rCmq58M); true when something was removed. */
bool dropHiddenGates(std::string& list, const std::set<int32_t>& hiddenGates);

/*!
* Removes hidden gates from every rail tile's gate list (3rCmq58M) and drops
* the tiles left with none.  disp_order is kept: the client sorts by it.
*/
template <class Category>
void filterRail(std::vector<Category>& categories, const std::set<int32_t>& hiddenGates)
{
	if (hiddenGates.empty())
		return;
	std::vector<Category> kept;
	kept.reserve(categories.size());
	for (auto& category : categories)
	{
		dropHiddenGates(category.gacha_id_list, hiddenGates);
		if (!category.gacha_id_list.empty())
			kept.push_back(std::move(category));
	}
	categories = std::move(kept);
}

/*! A parse of rules.json text (no file, no status): the snapshot or the file-level error. */
struct RulesParse
{
	std::shared_ptr<RulesSnapshot> snapshot;      //!< null when the file is refused
	std::string error;                            //!< file-level reason
	std::vector<std::pair<std::string, std::string>> ruleErrors;   //!< {rule id, reason}
};
RulesParse parseRules(const std::string& text);

} // namespace forge
