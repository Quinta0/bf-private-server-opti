#pragma once

#include <gimuserver/archive/archive.hpp>
#include <gimuserver/packets/all.hpp>

#include <drogon/drogon.h>

#include <json/value.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*!
* One enemy a mission slot can roll at MissionStart (archive/mission_variants.json).
*
* The original game rolled the enemies of its material and EXP fights per run
* (Metal Ghost / King / God, the element of an Idol or Totem); an archive record
* is fixed, so each listed slot keeps its alternatives here, with weights from
* the 2014 captures' repeated runs or the wiki quest's capture list
* (scripts/gen_mission_variants.py).  The chosen enemy replaces the slot whole,
* its own capture and chest included, at the slot's position.
*/
struct SlotVariant
{
	uint32_t stage = 0;     // index into MissionRecord::stages
	uint32_t slot = 0;      // index into that stage's battle_monsters
	uint32_t weight = 0;
	BattleMonster monster;
};

template <>
struct glz::meta<SlotVariant>
{
	using T = SlotVariant;
	static constexpr auto value = glz::object(
		"stage", &T::stage, "slot", &T::slot, "weight", &T::weight, "monster", &T::monster);
};

/*!
* Farmable bosses (ticket #82, archive/boss_drops.json).
*
* #47 took the capture off every boss the original game never let you catch.
* The players asked for it back as an intentional, rarer feature: a boss of
* an archive mission drawn as a normal unit (unit_mst kind 0, named as one of
* its line's forms) can drop the BASE form of that line, at a chance set by
* the line's top rarity (8 = Omni).  MissionStart decides it per run, after
* the per-run enemy rolls.  Copies from any mission drop carry
* user_units.source = 'drop' and trade in for drop_merit_percent of a
* summoned copy's Merit.  The file is optional; these are the defaults.
*/
struct BossDropPolicy
{
	// Index = the line's top rarity, 1-8.  Index 0 is unused.
	std::vector<uint32_t> chance_by_line_rarity{ 0, 20, 20, 20, 20, 15, 8, 4, 2 };
	uint32_t drop_merit_percent = 25;
	std::string note;
};

template <>
struct glz::meta<BossDropPolicy>
{
	using T = BossDropPolicy;
	static constexpr auto value = glz::object(
		"chance_by_line_rarity", &T::chance_by_line_rarity,
		"drop_merit_percent", &T::drop_merit_percent, "note", &T::note);
};

/*!
* Bookkeeping types for the hand-built mission overlay (MissionOverlay.cpp).
*/
namespace mission_overlay
{
// What one overlay file looked like on disk when its bytes were read.  A new
// inode (os.replace), size, mtime or ctime means the bytes changed.
struct FileKey
{
	uint64_t ino = 0;
	uintmax_t size = 0;
	int64_t mtimeNs = 0;
	int64_t ctimeNs = 0;
	bool operator==(const FileKey&) const = default;
};

// Reference tables an overlay record is checked against, built once at boot.
struct Refs
{
	std::unordered_map<uint32_t, int32_t> missionEnergy; // mission_mst id -> energy_use
	std::unordered_set<uint32_t> itemIds;               // item_mst
	std::unordered_map<uint32_t, int32_t> unitMaxLv;    // unit_mst, balance rows applied
};

// Exactly-named overlay files in the folder.  nullopt: listed, but its stat
// failed this round, so it is neither re-read nor treated as removed.
using Listing = std::map<std::string, std::optional<FileKey>>;

// What one refresh pass did, for the boot summary.
struct ScanResult
{
	size_t accepted = 0;
	size_t rejected = 0;
	size_t removed = 0;
};
} // namespace mission_overlay

/*!
* Loads and serves server-owned curated mission archive records.
*
* The archive stores logical mission data only. Client battle group ids and
* encoded AI/drop wire strings are generated while formatting a mission-start
* response.
*
* Hand-built battles: when balance_root is set, every
* <balance_root>/server/missions/<mission id>.json holds one MissionRecord that
* replaces (or adds) the archive record with that id.  The folder is re-read
* every 2 s on the main loop; a file is checked as a whole (parse, references,
* a MissionStart dry run) before it is served, and a bad file never affects
* any other mission.  See docs/features/BATTLE_BUILDER.md.
*/
class MissionArchiver final
{
public:
	using MissionId = uint32_t;

	/*!
	* Gets the process-wide mission archiver.
	*
	* Setup() must be called during server initialization before request handlers
	* depend on archive data.
	*/
	static MissionArchiver& instance();

	/*!
	* Loads mission and AI archive records from archive_root, then (when
	* balance_root is set) the hand-built overlay, and starts watching it.
	* @param serverObj Server configuration object from the Drogon plugin config.
	*/
	void setup(const Json::Value& serverObj);

	/*!
	* Looks up a mission archive record: the hand-built overlay record when
	* one is being served, else the archive's.  Never does file IO.
	*
	* @param mission_id Mission id from mission.json.
	* @return Matching archive record, or std::nullopt when not found.
	*/
	std::optional<MissionRecord> lookup(MissionId mission_id) const;

	/*!
	* Rolls this run's enemy for every slot with alternatives (see SlotVariant).
	* Only archive records roll: a hand-built overlay record is served as built.
	* MissionStart calls it once per battle, before the response is filled, so
	* the waves, MonsterMst rows, AI and drop_info all describe the same enemies.
	*
	* @param record The record lookup() returned for this battle.
	* @return How many slots rolled.
	*/
	size_t rollVariants(MissionRecord& record) const;

	/*!
	* Gives this run's bosses their base-form capture (see BossDropPolicy).
	* Only archive records: a hand-built overlay record (the Karma dungeon,
	* Juggernaut) is served as built, and the tutorial missions 1 and 2 too.
	* A boss is a monster of an is_boss stage with at least half that stage's
	* top HP; one that already has a capture keeps it.  MissionStart calls it
	* after rollVariants, so the drop follows the rolled enemy.
	*
	* @param record The record lookup() returned for this battle.
	* @return How many bosses got a capture.
	*/
	size_t applyBossDrops(MissionRecord& record) const;

	/*! The loaded boss drop chances and the Merit rate for dropped copies. */
	const BossDropPolicy& bossDropPolicy() const { return bossDrops_; }

	/*! What a caught copy trades in for, in percent: tuning.json's boss_drop_merit_pct, else the policy's. */
	uint32_t bossDropMeritPercent() const;

	/*!
	* Raises every capturable wave monster of this battle without authored_capture to
	* `floor` percent (tuning.json capture_floor_pct, ticket #164).  MissionStart
	* calls it after rollVariants and before applyBossDrops, so a changed floor counts
	* from the next battle.  Archive and hand-built overlay records alike (an overlay is
	* checked at load with the floor applied); the tutorial's missions 1 and 2 never.
	*
	* @return How many monsters were raised.
	*/
	size_t applyCaptureFloor(MissionRecord& record, uint32_t floor) const;

	/*!
	* Whether an archive record exists for a mission, without building a copy
	* of it and without logging a miss.
	*
	* PermitPlace needs this: `missionsByDungeon` is indexed from
	* `mission_mst.json` (3433 rows) while the archive holds 3399, so the quest
	* map can otherwise advertise a tile whose battle does not exist.  Entering
	* one fails lookup(), the handler throws, and the session closes — which
	* reads to a player as a crash rather than as missing content.
	*
	* @param mission_id Mission id from mission_mst.
	* @return True when the mission has an archive or hand-built record.
	*/
	bool archived(MissionId mission_id) const;

	/*! True when the Forge's hand-built overlay serves this mission. */
	bool handBuilt(MissionId mission_id) const;

	/*!
	* Populates all AI MST rows referenced by the mission record.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate.
	* @return True if the packet rows were populated, false if referenced AI
	* archive data is missing or invalid.
	*/
	static bool populatePacket(const MissionRecord& record, std::vector<AiMst>& msts);

	/*!
	* AI MST rows for every AI id the unit master uses (UnitMst.ai_id), for
	* battles in which the units act on their own: Arena, both sides.  The
	* client walks these rows to decide when a unit casts its BB or SBB; a unit
	* whose AI id has no row only ever attacks.  An id with no archive record
	* is logged and left out, so a gap costs those units their bursts instead
	* of the battle.
	*
	* @return The rows, AI id by AI id; empty when no unit AI is archived.
	*/
	static std::vector<AiMst> unitAiRows();

	/*!
	* Populates all monster MST rows referenced by the mission record,
	* including the script_monsters its client-side mission script swaps in.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate.
	* @return True if the packet rows were populated, false if referenced unit
	* archive data is missing, a scripted monster id is invalid or ambiguous,
	* or the mission has no monsters.
	*/
	static bool populatePacket(const MissionRecord& record, std::vector<MonsterMst>& msts);

	/*!
	* Populates the per-animation sprite rows for any special (non-obtainable)
	* monsters in the mission.
	*
	* Every special monster gets all four cgs types; an unauthored slot borrows
	* the nearest authored animation, because a missing row makes the client read
	* its download directory as a file (see the .cpp).
	*
	* Sets nullopt when the mission has no special monsters, so the 8hoyIF9Q
	* block is omitted entirely rather than sent empty -- its readParam clears
	* the client's cached list before repopulating it.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate, or nullopt.
	* @return False if a special monster has no cgs file at all; a mission with
	* no special monsters is not an error.
	*/
	static bool populatePacket(
		const MissionRecord& record,
		std::optional<std::vector<MonsterCgsMst>>& msts);

	/*!
	* Populates the F_SKILL_MST rows of the battle-only skills the mission's
	* monsters use (ToZj4zyn "0"), which the client drops on every Home and
	* must be handed again for the battle (see the .cpp).
	*
	* Sets nullopt when no monster uses such a skill, so the 8aiBoHg5 block is
	* omitted.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate, or nullopt.
	* @return False if a monster names a skill id absent from skill_mst.json.
	*/
	static bool populatePacket(
		const MissionRecord& record,
		std::optional<std::vector<SkillMst>>& msts);

	/*!
	* Populates all battle monster group MST rows referenced by the mission
	* record.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate.
	* @return True if the packet rows were populated, false if the mission has no
	* monsters.
	*/
	static bool populatePacket(const MissionRecord& record, std::vector<BattleMonsterGroupMst>& msts);

	/*!
	* Populates all battle group MST rows referenced by the mission record.
	*
	* @param record Archive record to read from.
	* @param msts Packet rows to populate.
	* @return True if the packet rows were populated, false if the mission has no
	* stages.
	*/
	static bool populatePacket(const MissionRecord& record, std::vector<BattleGroupMst>& msts);

	/*!
	* Populates mission number data for the mission-start response.
	*
	* @param record Archive record to read from.
	* @param mst Packet object to populate.
	* @return True if the packet was populated.
	*/
	static bool populatePacket(const MissionRecord& record, MissionNumInfo& mst);

	/*!
	* Rolls a unit personality type using F_UNIT_TYPE_MST's own appearance
	* rates (23/23/22/22/10/0 — Rex never rolls).  Used for every unit a
	* mission drop or capture awards (whatever unit_drop_type says) and for
	* every unit a summon gate hands out.
	*
	* @return Unit type id 1-6; 1 (Lord) if the MST has no usable rates.
	*/
	static uint32_t rollUnitType();

	MissionArchiver(const MissionArchiver&) = delete;
	MissionArchiver& operator=(const MissionArchiver&) = delete;

private:
	using AiId = uint32_t;
	using MissionRecordCache = std::unordered_map<MissionId, MissionRecord>;
	using AiRecordCache = std::unordered_map<AiId, AiRecord>;
	using RecordPtr = std::shared_ptr<const MissionRecord>;
	using OverlayMap = std::unordered_map<MissionId, RecordPtr>;

	static constexpr size_t kMaxPartyConditions = 4;
	static constexpr size_t kMaxSelfConditions = 5;
	static constexpr size_t kMaxActionFlagCount = 12;
	static constexpr size_t kMaxMonstersPerStage = 6;
	static constexpr uint32_t kMinCaptureChance = 40;

	MissionArchiver() = default;

	/*!
	* The capture floor for one monster (applyCaptureFloor's per-monster rule).
	*
	* @return Whether the monster was raised.
	*/
	static bool floorMonster(BattleMonster& monster, uint32_t floor);

	/*!
	* Loads archive/mission_variants.json (optional) after the archive is
	* cached, keeping only alternatives whose stage and slot exist.
	*/
	void loadVariants(const std::string& archiveRoot);

	/*!
	* Merges the shared mimic.json pool into a record that has chests and no
	* random_mimics of its own: what MissionStart actually serves.
	*
	* @param record Record to adjust in place.
	*/
	void applyMimicPolicy(MissionRecord& record) const;

	// --- Hand-built overlay (MissionOverlay.cpp) ---

	// Builds the reference tables, loads the folder once and starts the timer.
	// Only called at the end of a successful setup().
	void setupOverlay();

	// The overlay records being served now; never null.
	std::shared_ptr<const OverlayMap> snapshot() const;

	// Re-reads what changed in the folder and publishes a new snapshot.  The
	// main-loop timer and setupOverlay() are its only callers; never throws.
	mission_overlay::ScanResult refreshOverlay() noexcept;
	mission_overlay::ScanResult scanOverlay();

	// Files gone from the listing: drop their ids from `next` and seen_.
	bool forgetRemoved(const mission_overlay::Listing& listing, OverlayMap& next,
		mission_overlay::ScanResult& result);

	// New or changed files: check each and put the good ones into `next`.
	bool parseChanged(const mission_overlay::Listing& listing, OverlayMap& next,
		mission_overlay::ScanResult& result);

	// Reads and checks one file.  Null with `why` set when it must not be
	// served; null with `why` empty when it vanished before it could be
	// opened.  `key` is set from the descriptor that was actually read.
	RecordPtr acceptFile(const std::filesystem::path& path, MissionId stemId,
		mission_overlay::FileKey& key, std::string& why) const;

	// File name, reserved ids, mission_mst (and its energy_use) and the
	// archive's scripted swaps.
	std::optional<std::string> identityProblem(const MissionRecord& record, MissionId stemId) const;

	// Logs a folder error or failed pass once per distinct message.
	void reportScanError(const std::string& error);

	/*!
	* Encodes the condition string for one AI action.
	*
	* @param action Archive action to read from.
	* @return Client-formatted condition string.
	*/
	static std::string encodeAIConditions(const AiAction& action);

	/*!
	* Encodes the command string for one AI action.
	*
	* @param action Archive action command to read from.
	* @return Client-formatted action string.
	*/
	static std::string encodeAIAction(const Action& action);

	/*!
	* Appends one AI record's action rows, in the order the client evaluates
	* them.
	*
	* @param record AI archive record to read from.
	* @param msts Packet rows to append to.
	*/
	static void appendAiRows(const AiRecord& record, std::vector<AiMst>& msts);

	/*!
	* Selects and encodes the unit drop for one mission monster.
	*
	* Unit drops are resolved server-side. The client only receives the final
	* result: whether a unit dropped, and which unit it was.
	*
	* @param monsterIdx Zero-based monster index inside the stage.
	* @param monster Archive monster to read from.
	* @return Client-formatted unit drop string, or an empty string when no unit
	* drops.
	*/
	static std::string encodeUnitDrop(size_t monsterIdx, const BattleMonster& monster);

	// Encode a side-group chest with optional capture data.
	static std::string encodeMimicDrop(size_t host, size_t group, const BattleMonster& monster);

	/*!
	* Parses an F_UNIT_TYPE_MST decimal rate string ("23.00") to a double.
	*
	* @param rate The decimal string.
	* @return The parsed value, or 0.0 when it does not parse.
	*/
	static double parseRate(const std::string& rate);

	/*!
	* Selects and encodes the treasure drop for one mission monster.
	*
	* Treasure rolls are resolved server-side. The client only receives the final
	* result: whether a chest dropped, and which reward that chest contains.
	*
	* @param monsterIdx Zero-based monster index inside the stage.
	* @param monster Archive monster to read from.
	* @return Client-formatted treasure drop string, or an empty string when no
	* treasure drops.
	*/
	static std::string encodeTreasureDrop(size_t monsterIdx, const BattleMonster& monster);

	/*!
	* Encodes all selected mission drops for MissionNumInfo.
	*
	* @param record Archive record to read from.
	* @return Client-formatted mission drop info string.
	*/
	static std::string encodeMissionDropInfo(const MissionRecord& record);
	
	/*!
	* Checks whether a treasure drop target type is supported.
	*
	* @param targetType Treasure target type from mission.json.
	* @return True if the treasure target type can be encoded.
	*/
	static bool validTreasureType(uint32_t targetType)
	{
		switch (targetType)
		{
		case 1:
		case 2:
		case 3:
		case 4:
			return true;
		default:
			LOG_ERROR << "Unsupported treasure drop target type " << targetType;
			return false;
		}
	}

	// What a boss drawn as unit X may drop: X's line, from unit_mst and
	// unit_evo_mst.  Only kind-0 units whose base form can be caught.
	struct BossLine
	{
		uint32_t base = 0;                 // lowest-rarity form, what drops
		uint32_t topRarity = 0;            // the chance is looked up per battle (bossChance)
		std::vector<std::string> names;    // every form's name, lower case
	};

	/*!
	* Loads archive/boss_drops.json (defaults when absent or invalid) and
	* builds bossLines_.  Needs the unit tables, so after ServerCache::Setup.
	*/
	void loadBossDrops(const std::string& archiveRoot);
	/*! The line of a boss drawn as `monster`, or null when it may not drop. */
	const BossLine* bossLine(const BattleMonster& monster) const;
	/*! The drop chance of a line topping out at `rarity`: tuning.json's boss_drop_pct_r<N>, else boss_drops.json's. */
	uint32_t bossChance(uint32_t rarity) const;

	std::optional<RandomMimicChests> randomMimics_;
	BossDropPolicy bossDrops_;
	std::unordered_map<uint32_t, BossLine> bossLines_;
	MissionRecordCache missionCache_;
	std::unordered_map<MissionId, std::vector<SlotVariant>> variants_;
	AiRecordCache aiCache_;

	// Overlay: set once in setupOverlay(), read-only afterwards.
	std::filesystem::path overlayDir_;                  // empty = feature off
	mission_overlay::Refs overlayRefs_;

	// The served overlay.  The mutex guards the pointer only (copy or swap),
	// never IO or parsing; readers keep their copy of the pointer.
	mutable std::mutex snapshotMutex_;
	std::shared_ptr<const OverlayMap> overlay_ = std::make_shared<const OverlayMap>();

	// Scan bookkeeping, touched only under scanMutex_ by the single refresher.
	std::mutex scanMutex_;
	std::map<std::string, mission_overlay::FileKey> seen_;  // parsed files, rejected ones included
	std::set<std::string> misnamed_;                        // misnamed files already reported
	std::string lastScanError_;                             // reported once until it changes
};
