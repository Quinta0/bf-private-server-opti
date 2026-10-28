#include "App.hpp"

#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/gme/common/RaidBattles.hpp>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdint>
#include <format>
#include <set>
#include <sstream>
#include <unordered_map>

namespace gme::raid
{

namespace
{

// DESIGN NUMBERS, not recovered data (RAIDS_ROUND2_CORE.md 5.4).  Raid bosses
// were tuned for four players over 30 minutes; these are sized so one player
// with an ordinary RC-appropriate squad can finish a run in a few battles,
// and every number grows with the raid class.
constexpr int64_t kBossHpPerRc = 40000;
constexpr uint32_t kBossAtkBase = 300;
constexpr uint32_t kBossAtkPerRc = 150;
constexpr uint32_t kBossDefPerRc = 80;
constexpr uint32_t kRouteHpScalePct = 100;   // archive HP x RC x this / 100
// The client reads hp as int32.  A hand-built story wave may carry up to this
// (MissionOverlay.cpp), so the RC scaling must clamp instead of wrapping.
constexpr uint64_t kMaxRouteHp = INT32_MAX;
constexpr int32_t kFallbackRouteMission = 10100;   // "Lava Rock Furnace", Moize
// Juggernaut's frame data (archive mission 85): a generic large monster,
// borrowed for boss parts because RaidBossPartsMst carries only the sprites.
constexpr auto kBossEffectFrame = "24:124:1";
constexpr auto kBossDamageFrame = "30:100:2:1";
constexpr uint32_t kBossAiId = 1;   // ai.json "Attack random"

int32_t clampRc(int32_t rc)
{
	return std::clamp(rc, 1, 7);
}

int64_t toInt(const std::string& text, int64_t fallback = 0)
{
	int64_t value = fallback;
	const auto* begin = text.data();
	const auto [ptr, ec] = std::from_chars(begin, begin + text.size(), value);
	return ec == std::errc{} ? value : fallback;
}

/*! Archive story missions named by a raid point's battle list ("weight:missionId,..."). */
std::vector<int32_t> pointMissions(int32_t mapId, int32_t point)
{
	std::vector<int32_t> out;
	for (const auto& row : theServer()->cache().raidPointMst())
	{
		if (row.map_id != mapId || row.point_id != point)
			continue;
		for (const auto& entry : row.battle_effect)
		{
			const auto colon = entry.find(':');
			if (colon != std::string::npos)
				out.push_back(static_cast<int32_t>(toInt(entry.substr(colon + 1))));
		}
	}
	out.push_back(kFallbackRouteMission);
	return out;
}

/*! Strip everything a raid battle must not pay out: the server credits raid rewards at MissionEnd only. */
void stripDrops(BattleMonster& monster)
{
	monster.unit_drop_id = 0;
	monster.unit_drop_level = 0;
	monster.unit_drop_type = 0;
	monster.unit_drop_chance = 0;
	monster.authored_capture = true;
	monster.zel_max_drop = 0;
	monster.zel_drop_count = 0;
	monster.karma_max_drop = 0;
	monster.karma_drop_count = 0;
	monster.treasure_chest_chance = 0;
	monster.treasure_drops.clear();
}

/*! A one-stage record labelled as the raid mission (populatePacket stamps record.id into every group). */
MissionRecord oneStageRecord(int32_t missionId, MissionStage stage)
{
	MissionRecord record{};
	record.id = static_cast<uint32_t>(missionId);
	record.name = "Raid battle";
	record.stages.push_back(std::move(stage));
	return record;
}

std::optional<::RaidBattleContentResp> populate(const MissionRecord& record)
{
	::RaidBattleContentResp resp{};
	if (!MissionArchiver::populatePacket(record, resp.ais)
		|| !MissionArchiver::populatePacket(record, resp.monsters)
		|| !MissionArchiver::populatePacket(record, resp.monster_cgs)
		|| !MissionArchiver::populatePacket(record, resp.battle_monster_groups)
		|| !MissionArchiver::populatePacket(record, resp.battle_groups)
		|| !MissionArchiver::populatePacket(record, resp.mission_num))
	{
		LOG_ERROR << "Raid battle: could not build battle content for raid mission " << record.id;
		return std::nullopt;
	}
	return resp;
}

std::vector<const ::RaidBossPartsMst*> partsOf(int32_t bossId)
{
	std::vector<const ::RaidBossPartsMst*> out;
	for (const auto& part : theServer()->cache().raidBossPartsMst())
		if (part.boss_id == bossId)
			out.push_back(&part);
	std::sort(out.begin(), out.end(), [](const auto* a, const auto* b) { return a->z_order < b->z_order; });
	return out;
}

const ::RaidBossMst* findBoss(int32_t bossId)
{
	for (const auto& boss : theServer()->cache().raidBossMst())
		if (boss.id == bossId)
			return &boss;
	return nullptr;
}

/*! A boss part as a special monster: raid sprites, no unit archive lookup. */
BattleMonster partMonster(const ::RaidBossPartsMst& part, const std::string& name, int32_t rc, int64_t maxHp)
{
	BattleMonster monster{};
	monster.id = part.monster_id;
	monster.name = name;
	monster.unit_id = 0;
	monster.position = part.body_flg == 1 ? "180:330" : "120:300";
	monster.hp = static_cast<uint32_t>(std::max<int64_t>(1, maxHp));
	monster.atk = kBossAtkBase + kBossAtkPerRc * static_cast<uint32_t>(rc);
	monster.def = kBossDefPerRc * static_cast<uint32_t>(rc);
	monster.ai_id = kBossAiId;
	monster.act_min = 1;
	monster.act_max = 2;
	monster.act_rate = static_cast<pkg::float32>(50);
	monster.wait = 5;
	stripDrops(monster);

	MonsterVisuals visuals{};
	visuals.img_a = part.img_a;
	visuals.anm_cgg = part.anm_cgg;
	visuals.cgs_idle = part.cgs_idle;
	// The parts table names only the idle animation.  Re-using it for attacks
	// keeps every requested file one the client's MST already promises exists.
	visuals.cgs_atk = part.cgs_idle;
	visuals.cgs_skill = part.cgs_idle;
	visuals.element = 1;
	visuals.effect_frame = kBossEffectFrame;
	visuals.damage_frame = kBossDamageFrame;
	visuals.drop_check_count = 1;
	visuals.move_speed = 2;
	visuals.attack_move_type = 1;
	visuals.back_move_type = 1;
	visuals.after_image = 1;
	visuals.skill_move_type = 1;
	visuals.cursor_disp_pos = "0,-40";
	visuals.hp_disp_pos = "0,-60";
	monster.visuals = std::move(visuals);
	return monster;
}

} // namespace

std::string encodeParts(const std::vector<BossPart>& parts)
{
	std::string out;
	for (const auto& part : parts)
	{
		if (!out.empty())
			out += ',';
		out += std::format("{}:{}:{}:{}", part.monsterId, part.maxHp, part.nowHp, part.body ? 1 : 0);
	}
	return out;
}

std::vector<BossPart> decodeParts(const std::string& text)
{
	std::vector<BossPart> out;
	std::stringstream rows(text);
	std::string row;
	while (std::getline(rows, row, ','))
	{
		std::stringstream cols(row);
		std::string id, maxHp, nowHp, body;
		if (!std::getline(cols, id, ':') || !std::getline(cols, maxHp, ':')
			|| !std::getline(cols, nowHp, ':') || !std::getline(cols, body, ':'))
			continue;
		out.push_back({ static_cast<uint32_t>(toInt(id)), body == "1", toInt(maxHp), toInt(nowHp) });
	}
	return out;
}

std::vector<BossPart> initialBossParts(int32_t bossId, int32_t rc)
{
	std::vector<BossPart> out;
	const auto hp = kBossHpPerRc * clampRc(rc);
	for (const auto* part : partsOf(bossId))
	{
		// Side parts take a third of the body's HP: breaking them is optional,
		// the body is what the clear condition needs.
		const auto partHp = part->body_flg == 1 ? hp : hp / 3;
		out.push_back({ part->monster_id, part->body_flg == 1, partHp, partHp });
	}
	return out;
}

std::optional<::RaidBattleContentResp> routeBattle(
	int32_t missionId, int32_t mapId, int32_t point, int32_t rc, uint32_t seed)
{
	for (const auto archiveId : pointMissions(mapId, point))
	{
		// A point may name a story mission this server has no battle record
		// for (80100, 30200, ...): skip it quietly and try the next, rather
		// than letting lookup() log an ERROR for an expected gap.
		if (!MissionArchiver::instance().archived(static_cast<uint32_t>(archiveId)))
			continue;
		auto source = MissionArchiver::instance().lookup(static_cast<uint32_t>(archiveId));
		if (!source || source->stages.empty())
			continue;
		// Ordinary waves only: a story boss belongs to its own mission.
		std::vector<MissionStage> waves;
		for (const auto& stage : source->stages)
			if (!stage.is_boss && !stage.battle_monsters.empty())
				waves.push_back(stage);
		if (waves.empty())
			continue;

		auto stage = waves[seed % waves.size()];
		stage.is_boss = false;
		stage.first_attack_rate = 0;
		for (auto& monster : stage.battle_monsters)
		{
			monster.hp = static_cast<uint32_t>(std::min(
				static_cast<uint64_t>(monster.hp) * clampRc(rc) * kRouteHpScalePct / 100, kMaxRouteHp));
			stripDrops(monster);
		}
		LOG_INFO << "Raid battle: route battle at point " << point << " of raid mission " << missionId
			<< " uses a wave of archive mission " << archiveId;
		return populate(oneStageRecord(missionId, std::move(stage)));
	}
	LOG_ERROR << "Raid battle: no archive mission usable for point " << point << " of map " << mapId;
	return std::nullopt;
}

std::optional<::RaidBattleContentResp> bossBattle(
	int32_t missionId, int32_t bossId, int32_t rc, const std::vector<BossPart>& parts)
{
	const auto* boss = findBoss(bossId);
	const auto partRows = partsOf(bossId);
	if (boss == nullptr || partRows.empty())
	{
		LOG_ERROR << "Raid battle: boss " << bossId << " has no RaidBossMst or parts rows";
		return std::nullopt;
	}

	MissionStage stage{};
	stage.is_boss = true;
	stage.first_attack_rate = 0;
	std::vector<::RaidBattleMonsterGroupInfo> partHp;
	for (const auto* row : partRows)
	{
		const auto stored = std::find_if(parts.begin(), parts.end(),
			[&](const BossPart& part) { return part.monsterId == row->monster_id; });
		if (stored == parts.end())
			continue;
		// A broken side part stays off the field; the body is always there
		// (at 0 HP the run is already won and no battle starts).
		if (!stored->body && stored->nowHp <= 0)
			continue;
		stage.battle_monsters.push_back(partMonster(*row, boss->name, clampRc(rc), stored->maxHp));
		partHp.push_back({ std::to_string(row->monster_id), stored->body ? 1 : 0, 0, stored->nowHp });
	}
	if (stage.battle_monsters.empty())
		return std::nullopt;

	auto resp = populate(oneStageRecord(missionId, std::move(stage)));
	if (!resp)
		return std::nullopt;

	// The boss's own monster group id: MissionScene::downloadFiles @0x17FFAFC
	// fetches the part sprites from BattleMonsterGroupMstList::getList of
	// RaidBossMst.Qzhp8B40, so the group must carry that id.
	for (auto& group : resp->battle_groups)
		group.battle_monster_group_id = boss->battle_monster_group_id;
	for (auto& member : resp->battle_monster_groups)
		member.battle_monster_group_id = boss->battle_monster_group_id;
	resp->boss_parts = std::move(partHp);
	return resp;
}

std::optional<std::vector<::SkillMst>> battleSkillRows(const std::vector<::MonsterMst>& monsters)
{
	static const auto index = [] {
		std::unordered_map<int32_t, const ::SkillMst*> byId;
		for (const auto& skill : theServer()->cache().skillMst())
			byId.emplace(skill.id, &skill);
		return byId;
	}();
	std::vector<::SkillMst> rows;
	std::set<int32_t> seen;
	for (const auto& monster : monsters)
	{
		std::stringstream entries(monster.monster_skill_id);
		std::string entry;
		while (std::getline(entries, entry, '@'))
		{
			int32_t id = 0;
			const auto colon = entry.find(':');
			const auto* end = entry.data() + (colon == std::string::npos ? entry.size() : colon);
			if (std::from_chars(entry.data(), end, id).ec != std::errc{} || id <= 0 || !seen.insert(id).second)
				continue;
			const auto it = index.find(id);
			if (it == index.end())
			{
				LOG_WARN << "Raid battle: monster " << monster.monster_id << " uses skill " << id
					<< ", which skill_mst.json does not have";
				continue;
			}
			if (it->second->permanent == "0")
				rows.push_back(*it->second);
		}
	}
	if (rows.empty())
		return std::nullopt;
	return rows;
}

} // namespace gme::raid
