#include "MissionArchiver.hpp"

#include "UnitArchiver.hpp"

#include <gimuserver/App.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/forge/Tuning.hpp>
#include <gimuserver/utils/JsonFile.hpp>
#include <gimuserver/utils/Random.hpp>

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// Wave groups retain ids 1..N; each chest has a single-monster side group.
// entryMimic @0x10C218C looks up that group without advancing BattleGroupMst.
std::vector<std::span<const BattleMonster>> missionMonsterGroups(const MissionRecord& record)
{
	std::vector<std::span<const BattleMonster>> groups;
	groups.reserve(record.stages.size() + (record.mimic_chests ? record.mimic_chests->size() : 0));
	for (const auto& stage : record.stages)
		groups.emplace_back(stage.battle_monsters);
	if (record.mimic_chests)
		for (const auto& chest : *record.mimic_chests)
			groups.emplace_back(&chest.monster, 1);
	if (record.random_mimics && record.random_mimics->chance != 0)
		for (const auto& monster : record.random_mimics->monsters)
			groups.emplace_back(&monster, 1);
	return groups;
}

// Every monster the MissionStart response must describe: the wave and chest
// groups above, plus the ones the client's mission script swaps in mid-battle.
// Scripted monsters get MonsterMst, AI and art rows but never a group, since a
// group is what places a monster on the opening field.
std::vector<std::span<const BattleMonster>> describedMonsters(const MissionRecord& record)
{
	auto monsters = missionMonsterGroups(record);
	if (record.script_monsters && !record.script_monsters->empty())
		monsters.emplace_back(*record.script_monsters);
	return monsters;
}

// MonsterParty::changeMonster @0x10C3160 looks the new monster up by id alone,
// so a scripted id must name exactly one monster in the response.
bool validScriptMonsters(const MissionRecord& record)
{
	if (!record.script_monsters)
		return true;
	std::set<uint32_t> waveIds;
	for (const auto& monsters : missionMonsterGroups(record))
		for (const auto& monster : monsters)
			waveIds.insert(monster.id);
	std::set<uint32_t> seen;
	for (const auto& monster : *record.script_monsters)
	{
		if (monster.id == 0 || monster.hp == 0 || waveIds.contains(monster.id)
			|| !seen.insert(monster.id).second)
		{
			LOG_ERROR << "Invalid scripted monster " << monster.id << " in mission " << record.id;
			return false;
		}
	}
	return true;
}

bool validMimicMonster(const BattleMonster& monster)
{
	// Captures use the same species as the side-group monster, whose assets
	// already travel with this response. Zero chance keeps the one-token form.
	return monster.id != 0 && monster.hp != 0 && monster.unit_drop_chance <= 100
		&& (monster.unit_drop_chance == 0
			|| (monster.unit_drop_id != 0 && monster.unit_drop_id == monster.unit_id
				&& monster.unit_drop_level > 0 && monster.unit_drop_type <= 6))
		&& monster.treasure_chest_chance == 0 && monster.treasure_drops.empty();
}

bool validRandomMimics(const RandomMimicChests& policy)
{
	return policy.chance <= 100
		&& (policy.chance == 0 || !policy.monsters.empty())
		&& std::all_of(policy.monsters.begin(), policy.monsters.end(), validMimicMonster);
}

bool validMimicChests(const MissionRecord& record)
{
	if (record.random_mimics && !validRandomMimics(*record.random_mimics))
	{
		LOG_ERROR << "Invalid random Mimic policy in mission " << record.id;
		return false;
	}
	if (!record.mimic_chests)
		return true;
	std::set<std::pair<uint32_t, uint32_t>> hosts;
	for (const auto& chest : *record.mimic_chests)
	{
		const auto& monster = chest.monster;
		if (chest.stage == 0 || chest.stage > record.stages.size()
			|| chest.monster_order >= record.stages[chest.stage - 1].battle_monsters.size()
			|| chest.chance > 100 || !hosts.emplace(chest.stage, chest.monster_order).second
			|| !validMimicMonster(monster))
		{
			LOG_ERROR << "Invalid Mimic chest in mission " << record.id;
			return false;
		}
	}
	return true;
}
} // namespace

std::string MissionArchiver::encodeAIConditions(const AiAction& action)
{
	// For string values, the client expects "non" to mean empty,
	// so we default to that when encoding empty strings.
	const auto nonIfEmpty = [](const std::string& value) -> std::string_view {
		return value.empty() ? std::string_view("non") : std::string_view(value);
	};

	std::ostringstream stream;

	const auto partyCnt = std::min(action.party_conditions.size(), kMaxPartyConditions);
	for (size_t i = 0; i < partyCnt; ++i)
	{
		const auto& condition = action.party_conditions[i];
		stream << std::format(
			"{}:{}:{}:{}@",
			condition.target_id,
			nonIfEmpty(condition.target_parameter),
			nonIfEmpty(condition.type),
			nonIfEmpty(condition.parameters));
	}
	for (size_t i = partyCnt; i < kMaxPartyConditions; ++i)
	{
		stream << "0:non:non:non@";
	}

	stream << '#';

	const auto selfCnt = std::min(action.self_conditions.size(), kMaxSelfConditions);
	for (size_t i = 0; i < selfCnt; ++i)
	{
		const auto& condition = action.self_conditions[i];
		stream << std::format(
			"{}:{}@",
			nonIfEmpty(condition.type),
			condition.parameter);
	}
	for (size_t i = selfCnt; i < kMaxSelfConditions; ++i)
	{
		stream << "non:0@";
	}

	return stream.str();
}

std::string MissionArchiver::encodeAIAction(const Action& action)
{
	std::ostringstream stream;
	stream << action.type << '@';
	const auto flagCnt = std::min(action.flag_changes.size(), kMaxActionFlagCount);
	for (size_t i = 0; i < flagCnt; ++i)
	{
		stream << action.flag_changes[i] << ',';
	}
	for (size_t i = flagCnt; i < kMaxActionFlagCount; ++i)
	{
		stream << "-1,";
	}
	stream << std::format(
		"@{}@{}@{}",
		action.counts_as_action ? 1 : 0,
		action.move_wait_frames,
		action.end_wait_frames);
	return stream.str();
}

uint32_t MissionArchiver::rollUnitType()
{
	// F_UNIT_TYPE_MST.appearance_rate is the game's own distribution over the
	// six personality types — 23/23/22/22/10/0, summing to exactly 100, with
	// Rex at 0 because it is never handed out by a normal drop.  Rolling
	// against it means an authored mission does not have to hardcode a type,
	// and each play of the same mission can award a different one.
	const auto& types = theServer()->cache().unitTypeMst();

	double total = 0.0;
	for (const auto& t : types)
	{
		total += parseRate(t.appearance_rate);
	}
	if (total <= 0.0)
	{
		return 1;   // no usable rates — fall back to Lord
	}

	// RandomUInt is integral, so roll in hundredths of a percent to keep the
	// 22-vs-23 split honest rather than rounding it away.
	auto ticket = static_cast<double>(RandomUInt(1, static_cast<uint32_t>(total * 100.0))) / 100.0;
	for (const auto& t : types)
	{
		const auto rate = parseRate(t.appearance_rate);
		if (rate <= 0.0)
		{
			continue;
		}
		if (ticket <= rate)
		{
			return static_cast<uint32_t>(t.unit_type_id);
		}
		ticket -= rate;
	}
	return 1;
}

double MissionArchiver::parseRate(const std::string& rate)
{
	try
	{
		return std::stod(rate);
	}
	catch (const std::exception&)
	{
		return 0.0;
	}
}

std::string MissionArchiver::encodeUnitDrop(size_t monsterIdx, const BattleMonster& monster)
{
	if (monster.unit_drop_id == 0 || RandomUInt(1, 100) > monster.unit_drop_chance)
	{
		return "";
	}

	// Every captured unit gets a rolled type, like a summon (owner, 2026-10-18,
	// ticket #21).  The archive fixes unit_drop_type to 5 (Oracle) on
	// most drops -- the captured story data as much as the Vortex fill -- so
	// players only ever caught Oracles.  The stored type is no longer used for
	// the roll.  It happens here, per mission start, alongside the drop roll
	// above; the client shows this type and echoes it back at MissionEnd.
	const auto dropType = rollUnitType();

	return std::format(
		"{}:0:{}:{}:{}",
		monsterIdx,
		monster.unit_drop_id,
		monster.unit_drop_level,
		dropType);
}

std::string MissionArchiver::encodeMimicDrop(size_t host, size_t group, const BattleMonster& monster)
{
	auto drop = std::format("{}/2/{}", host, group);
	// BattleTreasure::create @0x10E7AAC reads tokens 3/4/5 for capture;
	// entryMimic @0x10C2314 attaches the BattleDropUnit to the side enemy.
	// Reuse the normal drop roll with order zero: group:0:0:unit:level:type.
	// Slots 1/2 are unused in that consumer. Never emit a 2-5 token payload.
	const auto capture = encodeUnitDrop(0, monster);
	if (!capture.empty()) drop += ':' + capture;
	return drop;
}

std::string MissionArchiver::encodeTreasureDrop(size_t monsterIdx, const BattleMonster& monster)
{
	if (monster.treasure_drops.empty()
		|| monster.treasure_chest_chance == 0
		|| RandomUInt(1, 100) > monster.treasure_chest_chance)
	{
		return "";
	}

	uint32_t total = 0;
	std::vector<const TreasureDrop*> drops;
	drops.reserve(monster.treasure_drops.size());
	for (const auto& drop : monster.treasure_drops)
	{
		if (drop.weight == 0 || drop.amount == 0 || !validTreasureType(drop.target_type))
		{
			continue;
		}

		if (drop.target_type == 4 && drop.target_id.empty())
		{
			continue;
		}

		total += drop.weight;
		drops.push_back(&drop);
	}

	if (drops.empty())
	{
		return "";
	}

	// Go through each possible drop and return the selected one.
	auto roll = RandomUInt(1, total);
	for (const auto drop : drops)
	{
		if (roll > drop->weight)
		{
			roll -= drop->weight;
			continue;
		}

		// A ranged prize (amount_max) rolls its count here; the client shows and
		// reports that count, and MissionEnd pays what it reports.
		const uint32_t top = drop->amount_max.value_or(0);
		const uint32_t amount = top > drop->amount ? RandomUInt(drop->amount, top) : drop->amount;
		if (drop->target_type == 4)
		{
			return std::format(
				"{}/1/{}:{}:{}:{}",
				monsterIdx,
				drop->target_type,
				drop->weight,
				drop->target_id,
				amount);
		}

		return std::format(
			"{}/1/{}:{}:{}",
			monsterIdx,
			drop->target_type,
			drop->weight,
			amount);
	}

	return "";
}

std::string MissionArchiver::encodeMissionDropInfo(const MissionRecord& record)
{
	std::ostringstream stream;
	for (size_t stageIdx = 0; stageIdx < record.stages.size(); ++stageIdx)
	{
		const auto& stage = record.stages[stageIdx];
		const auto stageId = static_cast<uint32_t>(stageIdx + 1);
		if (stageIdx != 0)
		{
			stream << '@';
		}

		stream << stageId << '|';

		std::string unitDrops;
		std::string treasureDrops;
		for (size_t monsterIdx = 0; monsterIdx < stage.battle_monsters.size(); ++monsterIdx)
		{
			const auto unitDrop = encodeUnitDrop(monsterIdx, stage.battle_monsters[monsterIdx]);
			if (!unitDrop.empty() && !unitDrops.empty())
			{
				unitDrops += '-';
			}
			unitDrops += unitDrop;

			std::string treasureDrop;
			bool mimicHost = false;
			if (record.mimic_chests)
			{
				for (size_t i = 0; i < record.mimic_chests->size(); ++i)
				{
					const auto& chest = (*record.mimic_chests)[i];
					if (chest.stage != stageId || chest.monster_order != monsterIdx)
						continue;
					mimicHost = true;
					if (chest.chance != 0 && RandomUInt(1, 100) <= chest.chance)
					{
						treasureDrop = encodeMimicDrop(monsterIdx, record.stages.size() + i + 1, chest.monster);
					}
					break;
				}
			}
			if (!mimicHost)
				treasureDrop = encodeTreasureDrop(monsterIdx, stage.battle_monsters[monsterIdx]);
			// Roll only AFTER an ordinary chest exists, preserving its original
			// appearance rate. A failed substitution leaves its reward intact.
			if (!mimicHost && !treasureDrop.empty() && record.random_mimics
				&& record.random_mimics->chance != 0
				&& RandomUInt(1, 100) <= record.random_mimics->chance)
			{
				const auto& pool = record.random_mimics->monsters;
				const auto selected = RandomUInt(0, static_cast<uint32_t>(pool.size() - 1));
				const auto fixedCount = record.mimic_chests ? record.mimic_chests->size() : 0;
				treasureDrop = encodeMimicDrop(monsterIdx,
					record.stages.size() + fixedCount + selected + 1, pool[selected]);
			}
			if (!treasureDrop.empty() && !treasureDrops.empty())
			{
				treasureDrops += '-';
			}
			treasureDrops += treasureDrop;
		}

		stream << (unitDrops.empty() ? " " : unitDrops) << '|';
		stream << (treasureDrops.empty() ? " " : treasureDrops);
	}

	return stream.str();
}

MissionArchiver& MissionArchiver::instance()
{
	static MissionArchiver instance;
	return instance;
}

namespace
{

/*!
* Why the Forge's ai.json record cannot be used, or nullopt.  The limits are
* the wire encoder's (MissionStart's AI rows): it silently drops conditions
* and flags beyond them and splits its rows on ':' '@' '#', so a record past
* them would fight differently from what the balancer wrote.
*/
std::optional<std::string> forgeAiProblem(const AiRecord& ai)
{
	const auto id = std::to_string(ai.id);
	const auto separator = [](const std::string& text) { return text.find_first_of(":@#") != std::string::npos; };
	if (ai.id == 0)
		return std::string("An AI id must be above 0.");
	if (ai.actions.empty() || ai.actions.size() > 64)
		return "AI " + id + " must have from 1 to 64 actions.";
	std::set<uint32_t> priorities;
	for (const auto& act : ai.actions)
	{
		const auto where = "AI " + id + ", action with priority " + std::to_string(act.priority);
		if (!priorities.insert(act.priority).second)
			return "AI " + id + " has two actions with priority " + std::to_string(act.priority) + ".";
		const auto percent = static_cast<double>(act.percent);
		if (!(percent >= 0.0 && percent <= 100.0))
			return where + ": the chance must be from 0 to 100 percent.";
		if (act.act_target < 1 || act.act_target > 4)
			return where + ": act_target must be from 1 to 4.";
		if (act.self_conditions.size() > 5)
			return where + ": at most 5 conditions on the enemy itself.";
		if (act.party_conditions.size() > 4)
			return where + ": at most 4 conditions on the party.";
		if (act.action.flag_changes.size() > 12)
			return where + ": at most 12 flag changes.";
		if (act.action.type.empty())
			return where + ": the action needs a type.";
		bool bad = separator(act.search_term) || separator(act.action.type);
		for (const auto& c : act.self_conditions)
			bad = bad || separator(c.type);
		for (const auto& c : act.party_conditions)
			bad = bad || separator(c.target_parameter) || separator(c.type) || separator(c.parameters);
		if (bad)
			return where + ": no text may contain ':', '@' or '#'.";
	}
	return std::nullopt;
}

bool validBossDropPolicy(const BossDropPolicy& policy)
{
	return policy.chance_by_line_rarity.size() == 9
		&& std::ranges::all_of(policy.chance_by_line_rarity, [](uint32_t c) { return c <= 100; })
		&& policy.drop_merit_percent <= 100;
}

} // namespace

void MissionArchiver::setup(const Json::Value& serverObj)
{
	LOG_INFO << "Setting up mission archiver cache. "
		"If you see this after initialization, it is a bug.";

	const auto archiveRoot = serverObj["archive_root"].asString();
	if (archiveRoot.empty())
	{
		LOG_ERROR << "Unable to set up mission archiver cache: archive_root is empty";
		return;
	}

	std::vector<MissionRecord> missions;
	std::vector<AiRecord> ais;
	try
	{
		missions = LoadJson<std::vector<MissionRecord>>(archiveRoot, "mission.json");
		ais = LoadJson<std::vector<AiRecord>>(archiveRoot, "ai.json");
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "Unable to set up mission archiver cache: " << ex.what();
		return;
	}

	// An optional shared archive policy avoids copying the same balanced
	// catalog into hundreds of missions. No player state is persisted here.
	randomMimics_.reset();
	const auto policyPath = std::filesystem::path(archiveRoot) / "mimic.json";
	if (std::filesystem::exists(policyPath))
	{
		try
		{
			auto policy = LoadJson<RandomMimicChests>(policyPath.string());
			if (validRandomMimics(policy))
				randomMimics_ = std::move(policy);
			else
				LOG_ERROR << "Invalid mimic.json: random chest substitution disabled";
		}
		catch (const std::exception& ex)
		{
			LOG_ERROR << "Unable to load mimic.json: " << ex.what();
		}
	}

	// The capture floor is applied per battle at MissionStart (applyCaptureFloor), so the
	// Forge's tuning.json capture_floor_pct takes effect without a restart (#164).

	// The Forge's ai.json records, before the hand-built missions below are
	// checked against the cache: a battle made in the Forge may name them.
	ais = forge::mergeRecords<AiRecord>("ai.json", std::move(ais),
		[](const AiRecord& r) { return std::to_string(r.id); },
		[](const std::string& text, AiRecord& out) { return forge::readStrict(out, text); },
		forgeAiProblem, /*allowDelete=*/false);

	AiRecordCache nextAiCache;
	nextAiCache.reserve(ais.size());
	for (auto& ai : ais)
	{
		nextAiCache.insert_or_assign(ai.id, std::move(ai));
	}

	aiCache_ = std::move(nextAiCache);

	MissionRecordCache nextMissionCache;
	nextMissionCache.reserve(missions.size());
	for (auto& mission : missions)
	{
		nextMissionCache.insert_or_assign(mission.id, std::move(mission));
	}

	missionCache_ = std::move(nextMissionCache);

	LOG_INFO << "Loaded " << missionCache_.size()
		<< " mission archive records and " << aiCache_.size()
		<< " AI archive records from " << archiveRoot;

	loadVariants(archiveRoot);
	loadBossDrops(archiveRoot);

	// Last, so a hand-built record is checked against the full AI cache and
	// the dry run sees exactly what MissionStart will (MissionOverlay.cpp).
	setupOverlay();
}

size_t MissionArchiver::applyCaptureFloor(MissionRecord& mission, uint32_t floor) const
{
	// Every capturable monster gets at least the floor's capture chance (40% by
	// default, tuning.json capture_floor_pct; applied per battle since #164).  Evan's
	// offline policy (2026-09-11) — authored, not recovered data: the
	// regenerated archive left 216 of 942 missions with nothing to capture and
	// most of the rest at 5%.  "Capturable" is exactly what MissionEnd will
	// accept — parseUnitDrops -> fromArchivedUnit needs a UnitArchiver record —
	// so a special boss whose unit does not resolve stays loot-only rather than
	// reporting a capture that would abort the whole reward.  A higher authored
	// chance (the parades' 60, the tutorial's scripted 100) is left alone, and
	// Mimic side-monsters keep their own policy (they are not stage monsters).
	//
	// A monster flagged `authored_capture` is left alone at ANY chance,
	// including 0.  The floor cannot tell a junk 5 from a deliberate 10, so the
	// event dungeons say which theirs is: their rule is a top-form boss whose
	// BASE form is capturable at 10% and escorts that are not capturable at all
	// (Evan, 2026-09-19), and a blanket raise to 40 would erase both halves.
	//
	// The tutorial's missions 1 and 2 keep their scripted captures exactly:
	// forced tutorial navigation walks a unit list it expects to know.
	if (mission.id == 1 || mission.id == 2)
		return 0;

	size_t raised = 0;
	for (auto& stage : mission.stages)
	{
		for (auto& monster : stage.battle_monsters)
			raised += floorMonster(monster, floor);
	}
	return raised;
}

bool MissionArchiver::floorMonster(BattleMonster& monster, uint32_t floor)
{
	if (monster.authored_capture.value_or(false))
		return false;

	const auto unitId = monster.unit_drop_id != 0 ? monster.unit_drop_id : monster.unit_id;
	if (unitId == 0 || floor == 0 || monster.unit_drop_chance >= floor
		|| !UnitArchiver::instance().lookup(unitId))
	{
		return false;
	}
	monster.unit_drop_id = unitId;
	monster.unit_drop_level = std::max<uint32_t>(monster.unit_drop_level, 1);
	monster.unit_drop_chance = floor;
	return true;
}

void MissionArchiver::loadVariants(const std::string& archiveRoot)
{
	variants_.clear();
	const auto path = std::filesystem::path(archiveRoot) / "mission_variants.json";
	if (!std::filesystem::exists(path))
		return;

	std::map<std::string, std::vector<SlotVariant>> file;
	try
	{
		file = LoadJson<std::map<std::string, std::vector<SlotVariant>>>(path.string());
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "Unable to load mission_variants.json, enemies will not roll: " << ex.what();
		return;
	}

	size_t kept = 0, dropped = 0;
	for (auto& [key, variants] : file)
	{
		MissionId missionId = 0;
		const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), missionId);
		const auto record = missionCache_.find(missionId);
		if (error != std::errc{} || end != key.data() + key.size() || record == missionCache_.end())
		{
			dropped += variants.size();
			continue;
		}

		std::vector<SlotVariant> valid;
		for (auto& variant : variants)
		{
			const auto& stages = record->second.stages;
			const bool placed = variant.stage < stages.size()
				&& variant.slot < stages[variant.stage].battle_monsters.size();
			auto& monster = variant.monster;
			if (!placed || variant.weight == 0 || monster.id == 0 || monster.hp == 0
				|| monster.unit_drop_chance > 100 || monster.visuals
				|| !UnitArchiver::instance().lookup(monster.unit_id))
			{
				++dropped;
				continue;
			}
			monster.position = stages[variant.stage].battle_monsters[variant.slot].position;
			valid.push_back(std::move(variant));
		}
		std::stable_sort(valid.begin(), valid.end(), [](const auto& a, const auto& b)
			{ return std::tie(a.stage, a.slot) < std::tie(b.stage, b.slot); });
		kept += valid.size();
		if (!valid.empty())
			variants_.insert_or_assign(missionId, std::move(valid));
	}
	LOG_INFO << "Mission variants: " << kept << " alternative enemies in " << variants_.size()
		<< " missions" << (dropped ? ", " + std::to_string(dropped) + " rejected" : std::string());
}

size_t MissionArchiver::rollVariants(MissionRecord& record) const
{
	const auto found = variants_.find(record.id);
	if (found == variants_.end() || snapshot()->contains(record.id))
		return 0;

	size_t rolled = 0;
	const auto& variants = found->second;
	for (size_t begin = 0; begin < variants.size();)
	{
		size_t end = begin;
		uint32_t total = 0;
		while (end < variants.size() && variants[end].stage == variants[begin].stage
			&& variants[end].slot == variants[begin].slot)
		{
			total += variants[end].weight;
			++end;
		}
		auto ticket = RandomUInt(1, total);
		for (size_t i = begin; i < end; ++i)
		{
			if (ticket > variants[i].weight)
			{
				ticket -= variants[i].weight;
				continue;
			}
			auto& stage = record.stages.at(variants[i].stage);
			stage.battle_monsters.at(variants[i].slot) = variants[i].monster;
			++rolled;
			break;
		}
		begin = end;
	}
	return rolled;
}

namespace
{
std::string lowerTrimmed(std::string_view text)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);
	// ASCII-folds the Latin-1 letters (U+00C0..U+00FF, UTF-8 C3 80..C3 BF): the mission archive
	// writes "Natalamé" where unit_mst has "Natalame", and bossLine's stand-in check compares the
	// two names, so an accent alone made a real boss look like an impostor and dropped nothing.
	constexpr std::string_view kLatin1Fold =
		"AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTs"
		"aaaaaaaceeeeiiiidnooooo/ouuuuyty";
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i)
	{
		const auto c = static_cast<unsigned char>(text[i]);
		if (c == 0xC3 && i + 1 < text.size())
		{
			const auto next = static_cast<unsigned char>(text[i + 1]);
			if (next >= 0x80 && next <= 0xBF)
			{
				out += static_cast<char>(std::tolower(static_cast<unsigned char>(kLatin1Fold[next - 0x80])));
				++i;
				continue;
			}
		}
		out += static_cast<char>(std::tolower(c));
	}
	return out;
}

// Bosses get a capture only in is_boss stages, and only those with at least
// half the stage's top HP: the weaker ones are minions (gen_capture_rules.py).
template <typename Fn>
void forEachBoss(MissionRecord& record, Fn&& fn)
{
	for (auto& stage : record.stages)
	{
		if (!stage.is_boss)
			continue;
		uint64_t top = 0;
		for (const auto& monster : stage.battle_monsters)
			top = std::max<uint64_t>(top, monster.hp);
		for (auto& monster : stage.battle_monsters)
		{
			if (monster.hp != 0 && uint64_t{ monster.hp } * 2 >= top)
				fn(monster);
		}
	}
}
} // namespace

void MissionArchiver::loadBossDrops(const std::string& archiveRoot)
{
	bossDrops_ = BossDropPolicy{};
	// The Forge's replacement first, strictly: an overlay that does not pass
	// is reported and the image's file is read as it always was.
	bool fromForge = false;
	if (auto overlay = forge::readFileOverlay("boss_drops.json"))
	{
		BossDropPolicy policy{};
		std::string why;
		if (const auto missing = forge::missingTopKey(overlay->bytes, { "chance_by_line_rarity", "drop_merit_percent" }))
			why = *missing;
		else if (const auto ec = glz::read<glz::opts{}>(policy, overlay->bytes))
			why = "The file does not read: " + forge::plainReadError(ec, overlay->bytes);
		else if (!validBossDropPolicy(policy))
			why = "chance_by_line_rarity must list 9 chances from 0 to 100, and drop_merit_percent must be from 0 to 100.";
		if (why.empty())
		{
			bossDrops_ = std::move(policy);
			fromForge = true;
		}
		forge::recordFileOverlay("boss_drops.json", overlay->sha256, why.empty(), why);
	}
	const auto policyPath = std::filesystem::path(archiveRoot) / "boss_drops.json";
	if (!fromForge && std::filesystem::exists(policyPath))
	{
		try
		{
			auto policy = LoadJson<BossDropPolicy>(policyPath.string());
			const bool valid = validBossDropPolicy(policy);
			if (valid)
				bossDrops_ = std::move(policy);
			else
				LOG_ERROR << "Invalid boss_drops.json (9 chances of 0-100, drop_merit_percent 0-100): using the defaults";
		}
		catch (const std::exception& ex)
		{
			LOG_ERROR << "Unable to load boss_drops.json, using the defaults: " << ex.what();
		}
	}

	struct Form { int32_t rarity = 0; int32_t kind = 0; std::string name; };
	std::unordered_map<uint32_t, Form> forms;
	for (const auto& row : theServer()->cache().unitMst())
		forms.insert_or_assign(static_cast<uint32_t>(row.id), Form{ row.rarity, row.kind, lowerTrimmed(row.unit_name) });
	std::unordered_map<uint32_t, uint32_t> next;
	std::unordered_map<uint32_t, std::vector<uint32_t>> prev;
	for (const auto& row : theServer()->cache().unitEvoMst())
	{
		const auto from = static_cast<uint32_t>(row.unit_id);
		const auto to = static_cast<uint32_t>(row.evo_unit_id);
		if (forms.contains(from) && forms.contains(to) && next.emplace(from, to).second)
			prev[to].push_back(from);
	}

	bossLines_.clear();
	for (const auto& [unitId, form] : forms)
	{
		if (form.kind != 0)
			continue;   // farm units keep their own archive drops (814c3de)
		// Every form before and after this one.  The cycle guard is the
		// `line` set itself: a form is walked once.
		std::set<uint32_t> line{ unitId };
		std::vector<uint32_t> open{ unitId };
		while (!open.empty())
		{
			const auto at = open.back();
			open.pop_back();
			if (const auto it = prev.find(at); it != prev.end())
				for (const auto from : it->second)
					if (line.insert(from).second)
						open.push_back(from);
		}
		for (auto at = unitId; next.contains(at) && line.insert(next.at(at)).second;)
			at = next.at(at);

		BossLine boss;
		int32_t baseRarity = INT32_MAX, topRarity = 0;
		for (const auto id : line)
		{
			const auto& f = forms.at(id);
			topRarity = std::max(topRarity, f.rarity);
			if (f.kind == 0 && f.rarity < baseRarity)   // ids ascend, so ties keep the lowest
			{
				baseRarity = f.rarity;
				boss.base = id;
			}
			boss.names.push_back(f.name);
		}
		if (boss.base == 0 || topRarity < 1
			|| static_cast<size_t>(topRarity) >= bossDrops_.chance_by_line_rarity.size()
			|| !UnitArchiver::instance().contains(boss.base))
			continue;
		// Every line is kept, even at 0%: tuning.json may raise its chance later.
		boss.topRarity = static_cast<uint32_t>(topRarity);
		bossLines_.insert_or_assign(unitId, std::move(boss));
	}

	size_t slots = 0;
	for (auto& [id, record] : missionCache_)
	{
		if (id == 1 || id == 2)
			continue;
		forEachBoss(record, [&](const BattleMonster& monster) {
			slots += (monster.unit_drop_id == 0 || monster.unit_drop_chance == 0) && bossLine(monster);
		});
	}
	std::string chances;
	for (size_t rarity = 1; rarity < bossDrops_.chance_by_line_rarity.size(); ++rarity)
		chances += std::format("{}{}:{}%", chances.empty() ? "" : " ", rarity,
			bossDrops_.chance_by_line_rarity[rarity]);
	LOG_INFO << "Boss drops: " << bossLines_.size() << " units can drop their base form; "
		<< slots << " archive boss slots; chance by line top rarity " << chances
		<< "; dropped copies trade in at " << bossDrops_.drop_merit_percent << "% Merit";
}

uint32_t MissionArchiver::bossChance(uint32_t rarity) const
{
	if (rarity >= bossDrops_.chance_by_line_rarity.size())
		return 0;
	const auto tune = forge::tuning();
	return tune.bossDropPct[rarity].value_or(bossDrops_.chance_by_line_rarity[rarity]);
}

uint32_t MissionArchiver::bossDropMeritPercent() const
{
	return forge::tuning().bossDropMeritPct.value_or(bossDrops_.drop_merit_percent);
}

const MissionArchiver::BossLine* MissionArchiver::bossLine(const BattleMonster& monster) const
{
	if (monster.visuals)
		return nullptr;
	const auto it = bossLines_.find(monster.unit_id);
	if (it == bossLines_.end())
		return nullptr;
	// A stand-in ("Thief Leon's Double" on Great Thief Leon, "Furious Nick"
	// on Cayena) is not the unit it is drawn as, so it drops nothing.
	const auto name = lowerTrimmed(monster.name);
	return std::ranges::find(it->second.names, name) != it->second.names.end() ? &it->second : nullptr;
}

size_t MissionArchiver::applyBossDrops(MissionRecord& record) const
{
	if (record.id == 1 || record.id == 2 || snapshot()->contains(record.id))
		return 0;

	size_t given = 0;
	forEachBoss(record, [&](BattleMonster& monster) {
		if (monster.unit_drop_id != 0 && monster.unit_drop_chance != 0)
			return;   // an evidenced capture (#47) stays as it is
		const auto* line = bossLine(monster);
		if (!line || bossChance(line->topRarity) == 0)
			return;
		monster.unit_drop_id = line->base;
		monster.unit_drop_level = 1;
		monster.unit_drop_type = 0;   // encodeUnitDrop rolls the type
		monster.unit_drop_chance = bossChance(line->topRarity);
		monster.authored_capture = true;
		++given;
	});
	return given;
}

bool MissionArchiver::handBuilt(MissionId mission_id) const
{
	return snapshot()->contains(mission_id);
}

bool MissionArchiver::archived(MissionId mission_id) const
{
	// PermitPlace asks this once per tile on every UserInfo: the archive
	// answers most of them without touching the overlay's lock.
	return missionCache_.contains(mission_id) || snapshot()->contains(mission_id);
}

std::optional<MissionRecord> MissionArchiver::lookup(MissionId mission_id) const
{
	// A hand-built record wins over the archive's.  Holding the snapshot
	// pointer keeps the record alive however the folder changes meanwhile.
	const auto overlay = snapshot();
	std::optional<MissionRecord> record;
	if (const auto hit = overlay->find(mission_id); hit != overlay->end())
	{
		record = *hit->second;
	}
	else if (const auto it = missionCache_.find(mission_id); it != missionCache_.end())
	{
		record = it->second;
	}
	else
	{
		LOG_ERROR << "Unable to find mission archive record " << mission_id;
		return std::nullopt;
	}

	applyMimicPolicy(*record);
	return record;
}

void MissionArchiver::applyMimicPolicy(MissionRecord& record) const
{
	if (record.random_mimics || !randomMimics_ || randomMimics_->chance == 0)
		return;

	bool hasChests = false;
	for (const auto& stage : record.stages)
		for (const auto& monster : stage.battle_monsters)
			hasChests |= monster.treasure_chest_chance != 0 && !monster.treasure_drops.empty();
	if (!hasChests)
		return;

	RandomMimicChests selected{};
	selected.chance = randomMimics_->chance;
	for (const auto& candidate : randomMimics_->monsters)
	{
		const BattleMonster* existing = nullptr;
		for (const auto& stage : record.stages)
			for (const auto& monster : stage.battle_monsters)
				if (!existing && monster.unit_id == candidate.unit_id)
					existing = &monster;
		if (existing)
		{
			auto monster = *existing;
			// Keep existing battle stats, but use the catalog's capture policy.
			monster.unit_drop_id = candidate.unit_drop_id;
			monster.unit_drop_level = candidate.unit_drop_level;
			monster.unit_drop_type = candidate.unit_drop_type;
			monster.unit_drop_chance = candidate.unit_drop_chance;
			monster.treasure_chest_chance = 0;
			monster.treasure_drops.clear();
			selected.monsters.push_back(std::move(monster));
		}
	}
	if (selected.monsters.empty())
		selected.monsters.push_back(randomMimics_->monsters.front());
	record.random_mimics = std::move(selected);
}

bool MissionArchiver::populatePacket(const MissionRecord& record, std::vector<AiMst>& msts)
{
	// Gather unique AI ids referenced by the mission record.
	std::set<AiId> ids;
	for (const auto& monsters : describedMonsters(record))
	{
		for (const auto& monster : monsters)
		{
			ids.insert(monster.ai_id);
		}
	}
	if (ids.empty())
	{
		LOG_ERROR << "Unable to populate AI MST rows: mission " << record.id
			<< " does not reference any AI";
		return false;
	}

	msts.clear();
	// Most tutorial AI records have one action, but records can expand to
	// multiple MST rows when they contain multiple actions.
	msts.reserve(ids.size());
	for (const auto id : ids)
	{
		const auto it = instance().aiCache_.find(id);
		if (it == instance().aiCache_.end())
		{
			LOG_ERROR << "Unable to populate AI MST rows: missing AI archive record " << id;
			return false;
		}

		const auto& aiRecord = it->second;
		if (aiRecord.actions.empty())
		{
			LOG_ERROR << "Unable to populate AI MST rows: AI archive record "
				<< id << " has no actions";
			return false;
		}

		appendAiRows(aiRecord, msts);
	}

	return true;
}

std::vector<AiMst> MissionArchiver::unitAiRows()
{
	std::set<AiId> ids;
	for (const auto& unit : theServer()->cache().unitMst())
	{
		if (unit.ai_id > 0)
			ids.insert(static_cast<AiId>(unit.ai_id));
	}

	std::vector<AiMst> msts;
	for (const auto id : ids)
	{
		const auto it = instance().aiCache_.find(id);
		if (it == instance().aiCache_.end() || it->second.actions.empty())
		{
			LOG_WARN << "Unit AI " << id << " has no AI archive record: units with it never cast a BB";
			continue;
		}
		appendAiRows(it->second, msts);
	}
	return msts;
}

void MissionArchiver::appendAiRows(const AiRecord& record, std::vector<AiMst>& msts)
{
	for (const auto& action : record.actions)
	{
		msts.push_back({
			.ai_id = record.id,
			.priority = action.priority,
			.name = record.name,
			.conditions = encodeAIConditions(action),
			.act_target = action.act_target,
			.search_term = action.search_term,
			.action = encodeAIAction(action.action),
			.percent = action.percent
		});
	}
}

namespace
{

// Packs BattleMonster.skills into MonsterMst.monster_skill_id.
//
// The client's own parser defines the format: BattleUnit::setUnitSkill
// @0x1120CF0 splits on '@' and then each entry on ':', taking [0] as the skill
// id and [1] (optional, 0 when absent) as the "announce the skill name" flag.
// The id resolves in UnitSkillMstList -- the ORDINARY skill table -- so a boss
// skill needs no master data this server does not already ship.
//
// A monster with no skills gets an empty string, which is what every captured
// live row carries.
std::string monsterSkillList(const BattleMonster& monster)
{
	if (!monster.skills || monster.skills->empty())
		return {};

	std::string packed;
	for (const auto& skill : *monster.skills)
	{
		if (!packed.empty())
			packed += '@';
		packed += std::to_string(skill.skill_id);
		// The flag is only worth sending when it is on: an entry of one element
		// is explicitly legal and reads as 0.
		if (skill.show_name)
			packed += ":1";
	}
	return packed;
}

// Packs BattleMonster.ailment_resists into MonsterMst.bad_state_resists.
//
// Six colon-separated percentages.  The SLOT ORDER is the unproven part and it
// is deliberately expressed only here and in the archive schema, so a future
// correction is a one-line change rather than a sweep of authored missions.
std::string monsterAilmentResists(const BattleMonster& monster)
{
	if (!monster.ailment_resists)
		return "0:0:0:0:0:0";

	const auto& r = *monster.ailment_resists;
	return std::format("{}:{}:{}:{}:{}:{}",
		r.poison, r.weak, r.sick, r.injury, r.curse, r.paralysis);
}

} // namespace

bool MissionArchiver::populatePacket(const MissionRecord& record, std::vector<MonsterMst>& msts)
{
	if (!validScriptMonsters(record))
		return false;

	msts.clear();
	msts.reserve(record.stages.size() * kMaxMonstersPerStage);
	const auto groups = describedMonsters(record);
	for (size_t stageIdx = 0; stageIdx < groups.size(); ++stageIdx)
	{
		const auto& monsters = groups[stageIdx];
		for (size_t monsterIdx = 0; monsterIdx < monsters.size(); ++monsterIdx)
		{
			const auto& monster = monsters[monsterIdx];

			// A special monster (a story boss) is one the client cannot resolve
			// to a playable unit: GameScene::requestMonsterFiles @0x160F648 asks
			// UnitMstList for this row's unit_id and switches asset root on the
			// answer -- /unit/ with derived filenames when it resolves,
			// /monster/ with the literal img_a / anm_cgg filenames when it does
			// not.  So the archive record carries its own visuals and we must
			// NOT look up a unit; doing so would also fail, since a boss's
			// unit_id is deliberately unresolvable.
			const MonsterVisuals* visuals = monster.visuals ? &*monster.visuals : nullptr;

			const UnitRecord* unit = nullptr;
			std::optional<UnitRecord> unitRecord;
			if (!visuals)
			{
				// Visual and movement fields come from the curated unit archive.
				unitRecord = UnitArchiver::instance().lookup(monster.unit_id);
				if (!unitRecord)
				{
					LOG_ERROR << "Unable to populate monster MST rows: missing unit archive record "
						<< monster.unit_id << " for mission " << record.id;
					return false;
				}
				unit = &*unitRecord;
			}
			else if (visuals->img_a.empty() || visuals->anm_cgg.empty())
			{
				// Half a boss renders nothing at all, so refuse it here rather
				// than let the client silently draw an empty sprite.
				LOG_ERROR << "Unable to populate monster MST rows: monster " << monster.id
					<< " in mission " << record.id
					<< " has visuals but img_a or anm_cgg is empty";
				return false;
			}

			msts.push_back({
				.monster_id = monster.id,
				// The client shows getMstText("MST_MONSTERS_" + monster_id +
				// "_NAME", base_name) -- so this is the FALLBACK used when the
				// id has no localisation entry, not the name itself.
				//
				// It used to be the id as a string, which is why an enemy whose
				// id is not a real monster id displayed as a bare number on
				// screen.  The authored archives are full of those: a unit id
				// makes a poor monster id, and 431 of them are actively WRONG
				// rather than merely missing (unit 10050 is Goblin, but
				// MST_MONSTERS_10050_NAME is "Burning Vargas").
				//
				// Feeding the archive's own name means a correct monster id
				// still wins through localisation, and everything else falls
				// back to the right English name instead of a number.
				.base_name = monster.name,
				.hp = monster.hp,
				.atk = monster.atk,
				.def = monster.def,
				.element = visuals ? visuals->element : unit->element,
				.effect_frame = visuals ? visuals->effect_frame : unit->effect_frame,
				.damage_frame = visuals ? visuals->damage_frame : unit->damage_frame,
				.drop_check_count = visuals ? visuals->drop_check_count : unit->drop_check_count,
				.img_a = visuals ? visuals->img_a : "",
				.anm_cgg = visuals ? visuals->anm_cgg : "",
				// Deliberately the AUTHORED id, not unit->id: for a special
				// monster this is the value that must fail the client's lookup.
				.unit_id = visuals ? monster.unit_id : unit->id,
				.move_speed = visuals ? visuals->move_speed : unit->move_speed,
				.attack_move_type = visuals ? visuals->attack_move_type : unit->attack_move_type,
				.back_move_type = visuals ? visuals->back_move_type : unit->back_move_type,
				.ai_id = monster.ai_id,
				.after_image = visuals ? visuals->after_image : unit->after_image,
				.max_act_count = monster.act_max,
				.min_act_count = monster.act_min,
				// act_rate and the three constants below are what every captured
				// live row carried.  They are seeded rather than defaulted because
				// the KDL fields are new (the wire surface is 38 keys, we modelled
				// 24) and a zero-initialised act_rate would ship "0" where the live
				// server always sent "100".  A boss may author its own act_rate.
				.act_rate = monster.act_rate.value_or(100.0f),
				.monster_skill_id = monsterSkillList(monster),
				.skill_move_type = visuals ? visuals->skill_move_type : unit->skill_move_type,
				.bad_state_resists = monsterAilmentResists(monster),
				.max_zel_drop = monster.zel_max_drop,
				.zel_drop_count = monster.zel_drop_count,
				.max_karma_drop = monster.karma_max_drop,
				.karma_drop_count = monster.karma_drop_count,
				.hp_disp_pos = visuals ? visuals->hp_disp_pos : "",
				// The enemy's passives from the battle's start (#162); "" like every live row.
				.passive_process_id = monster.passive_procs.value_or(""),
				.passive_process_param = monster.passive_params.value_or(""),
				.move_offset = "0,0",
				.debuff_resist = "0,0,0",
				.wait = monster.wait,
				.cursor_disp_pos = visuals ? visuals->cursor_disp_pos : unit->cursor_disp_pos
			});
		}
	}

	if (msts.empty())
	{
		LOG_ERROR << "Unable to populate monster MST rows: mission " << record.id
			<< " has no monsters";
		return false;
	}

	return true;
}

bool MissionArchiver::populatePacket(
	const MissionRecord& record,
	std::optional<std::vector<MonsterCgsMst>>& msts)
{
	// One row for EVERY animation slot, keyed on the MONSTER id -- not the unit
	// id the sibling UnitCgsMst table uses (GameUtils::getMonsterCgs @0x1EAFD74
	// reads a different list on each side of the branch).
	//
	// A slot must never be missing.  MonsterUnit::initialize @0x1155FDC loads
	// all four types through MonsterUnit::addCgs @0x1158924, and a type with no
	// row reads back as "".  CommonUtils::getLocalPath("") is the writable
	// directory itself, so CCFileUtils::getFileData fopen()s a DIRECTORY and
	// sizes it with fseek(SEEK_END)/ftell.  ext4 with hashed directory indexes
	// answers 0x7fffffffffffffff, new[] fails and the game aborts the moment the
	// monster is created: the Juggernaut crash on mission 85.  f2fs and the
	// Android Studio emulator answer the directory's real size, which is why it
	// never crashed there.  The unit-side table never leaves a slot empty
	// either: unit_cgs_mst's type 2 is a move csv or the idle csv.
	//
	// So an unauthored slot borrows the nearest authored animation, in this
	// order.  The fallbacks all index the same cgg as the slot they replace.
	using Slot = std::string MonsterVisuals::*;
	static constexpr std::pair<uint32_t, std::array<Slot, 4>> kSlots[] = {
		{ 1, { &MonsterVisuals::cgs_idle, &MonsterVisuals::cgs_atk, &MonsterVisuals::cgs_skill, &MonsterVisuals::cgs_move } },
		{ 2, { &MonsterVisuals::cgs_move, &MonsterVisuals::cgs_idle, &MonsterVisuals::cgs_atk, &MonsterVisuals::cgs_skill } },
		{ 3, { &MonsterVisuals::cgs_atk, &MonsterVisuals::cgs_skill, &MonsterVisuals::cgs_idle, &MonsterVisuals::cgs_move } },
		{ 4, { &MonsterVisuals::cgs_skill, &MonsterVisuals::cgs_atk, &MonsterVisuals::cgs_idle, &MonsterVisuals::cgs_move } }
	};

	std::vector<MonsterCgsMst> rows;
	for (const auto& monsters : describedMonsters(record))
	{
		for (const auto& monster : monsters)
		{
			if (!monster.visuals)
			{
				continue;
			}

			for (const auto& [cgsType, preference] : kSlots)
			{
				const auto pick = std::find_if(preference.begin(), preference.end(),
					[&](Slot slot) { return !((*monster.visuals).*slot).empty(); });
				if (pick == preference.end())
				{
					// No animation at all: every slot would read the directory.
					LOG_ERROR << "Unable to populate monster cgs rows: monster " << monster.id
						<< " in mission " << record.id << " has visuals but no cgs file";
					return false;
				}

				rows.push_back({
					.monster_id = monster.id,
					.cgs_type = cgsType,
					.anm_cgs = (*monster.visuals).**pick
				});
			}
		}
	}

	// Leave the block ABSENT rather than empty when the mission has no special
	// monsters.  MonsterCgsMstResponse::readParam @0x13E4E20 opens with
	// MonsterCgsMstList::removeAllObjects, so an empty array would wipe the
	// client's cached list on every ordinary mission start.
	msts = rows.empty() ? std::nullopt : std::optional{ std::move(rows) };
	return true;
}

bool MissionArchiver::populatePacket(
	const MissionRecord& record,
	std::optional<std::vector<SkillMst>>& msts)
{
	// The client keeps a skill row only for the session it was loaded in when
	// its ToZj4zyn is "0": UnitSkillMstResponse::readParam @0x13F9E04 files it
	// with addObjectTemporary, and GameUtils::battleTemporaryMstRemove
	// @0x1176238 empties that list on every Home and after every mission
	// result.  21,174 of the 33,381 rows are like that -- every monster-only
	// skill -- so by the time a battle loads they are gone, and
	// GameScene::requestMonsterSummonFiles @0x16433CC calls getProcessID on the
	// NULL UnitSkillMstList::getObject returns: Trial No. 001's boss crashed the
	// game at load.  So the battle start hands those rows back, under the key
	// that APPENDS (8aiBoHg5, UnitSkillMstResponse(true)); they go back into the
	// temporary list and are dropped again at the result scene, as the client
	// intends.  Rows with ToZj4zyn "1" never left and are not re-sent.
	static const auto index = [] {
		std::unordered_map<int32_t, const SkillMst*> byId;
		for (const auto& skill : theServer()->cache().skillMst())
			byId.emplace(skill.id, &skill);
		return byId;
	}();

	std::vector<SkillMst> rows;
	std::set<int32_t> seen;
	for (const auto& monsters : describedMonsters(record))
	{
		for (const auto& monster : monsters)
		{
			if (!monster.skills)
				continue;

			for (const auto& skill : *monster.skills)
			{
				const auto id = static_cast<int32_t>(skill.skill_id);
				if (!seen.insert(id).second)
					continue;

				const auto it = index.find(id);
				if (it == index.end())
				{
					// The client would look it up and dereference NULL.
					LOG_ERROR << "Unable to populate battle skill rows: monster " << monster.id
						<< " in mission " << record.id << " uses skill " << id
						<< ", which skill_mst.json does not have";
					return false;
				}
				if (it->second->permanent == "0")
					rows.push_back(*it->second);
			}
		}
	}

	msts = rows.empty() ? std::nullopt : std::optional{ std::move(rows) };
	return true;
}

bool MissionArchiver::populatePacket(
	const MissionRecord& record,
	std::vector<BattleMonsterGroupMst>& msts)
{
	msts.clear();
	msts.reserve(record.stages.size() * kMaxMonstersPerStage);
	const auto groups = missionMonsterGroups(record);
	for (size_t stageIdx = 0; stageIdx < groups.size(); ++stageIdx)
	{
		const auto& monsters = groups[stageIdx];
		const auto stageId = static_cast<uint32_t>(stageIdx + 1);
		for (size_t monsterIdx = 0; monsterIdx < monsters.size(); ++monsterIdx)
		{
			const auto& monster = monsters[monsterIdx];

			msts.push_back({
				.battle_monster_group_id = stageId,
				.monster_id = monster.id,
				.group_order = static_cast<uint32_t>(monsterIdx),
				.position = monster.position,
				// The client does not use this field to resolve unit drops; it uses the
				// encoded drop info in MissionNumInfo instead. This value only tells the
				// client which unit assets may be needed, so keep the other fields as zero.
				.unit_drop = (monster.unit_drop_id == 0 || monster.unit_drop_chance == 0)
					? "0:0:0:0"
					: std::format("0:{}:0:0", monster.unit_drop_id)
			});
		}
	}

	if (msts.empty())
	{
		LOG_ERROR << "Unable to populate battle monster group MST rows: mission "
			<< record.id << " has no monsters";
		return false;
	}

	return true;
}

bool MissionArchiver::populatePacket(const MissionRecord& record, std::vector<BattleGroupMst>& msts)
{
	if (record.stages.empty())
	{
		LOG_ERROR << "Unable to populate battle group MST rows: mission "
			<< record.id << " has no stages";
		return false;
	}

	msts.clear();
	msts.reserve(record.stages.size());

	for (size_t stageIdx = 0; stageIdx < record.stages.size(); ++stageIdx)
	{
		const auto& stage = record.stages[stageIdx];
		const auto stageId = static_cast<uint32_t>(stageIdx + 1);

		msts.push_back({
			.battle_group_id = stageId,
			.battle_monster_group_id = stageId,
			.mission_id = record.id,
			.battle_order = stageId,
			.first_attack_rate = stage.first_attack_rate,
			.boss_flag = stage.is_boss
		});
	}

	return true;
}

bool MissionArchiver::populatePacket(const MissionRecord& record, MissionNumInfo& mst)
{
	if (!validMimicChests(record))
		return false;
	mst.serial_id = record.id;
	mst.drop_info = encodeMissionDropInfo(record);
	return true;
}
