#pragma once

#include <gimuserver/archive/archive.hpp>
#include <gimuserver/packets/all.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Classic Raid battle content (RAIDS_ROUND2_CORE.md 5.4).
//
// The original server rolled raid battles from RaidBattleGroupMst, but the
// monster groups those rows name (102000, 20000000, ...) were never shipped to
// the client and exist nowhere in our archive.  So this server AUTHORS them:
//
//   * a route battle is one stage of an archive story mission, taken from the
//     point's own battle list (RaidMapPointMst 8f4NYKxb names story missions
//     of the same land: "3:10100,1:10101" on Moize's camp), scaled by the
//     raid mission's RC;
//   * a boss battle is the boss's RaidBossPartsMst parts as special monsters
//     carrying the raid sprites, with HP that persists per room mission.
//
// Both are served as the normal MissionStart battle groups for the RAID
// mission id: RaidUtils::initRaidBattle @0x11CBAEC sets UserState's last
// mission to RaidMissionPointMst::getMissionID, and MonsterParty::entry
// @0x10C0F8C resolves the wave from BattleGroupMstList by that id and the
// battle order.
namespace gme::raid
{

/*! One boss body part as the server tracks it. */
struct BossPart
{
	uint32_t monsterId = 0;
	bool body = false;
	int64_t maxHp = 0;
	int64_t nowHp = 0;
};

/*! Encode/decode the parts column ("monsterId:max:now:body,..."). */
std::string encodeParts(const std::vector<BossPart>& parts);
std::vector<BossPart> decodeParts(const std::string& text);

/*! Fresh parts for a boss at a raid class; empty when the boss has no parts row. */
std::vector<BossPart> initialBossParts(int32_t bossId, int32_t rc);

/*!
* Content for a route battle at `point` of the raid mission.
* @param seed Varies the stage pick per battle (the battle serial).
* @return nullopt when no archive mission could be used.
*/
std::optional<::RaidBattleContentResp> routeBattle(
	int32_t missionId, int32_t mapId, int32_t point, int32_t rc, uint32_t seed);

/*!
* Content for a boss battle: one monster per part, at the HP stored for this
* room mission (Fx60pCaU carries the same numbers for the client).
*/
std::optional<::RaidBattleContentResp> bossBattle(
	int32_t missionId, int32_t bossId, int32_t rc, const std::vector<BossPart>& parts);

/*!
* The F_SKILL_MST rows a battle's monsters need re-sent (8aiBoHg5): every
* skill named in a monster's monster_skill_id ("id[:flag]@...") whose row is
* not permanent (ToZj4zyn "0").  The client empties those rows on every Home,
* and GameScene::requestMonsterSummonFiles @0x16433CC dereferences the missing
* row at battle load (SIGSEGV 0x48) -- the same fix MissionStart carries
* (MissionArchiver::populatePacket).  nullopt when none is needed.
*/
std::optional<std::vector<::SkillMst>> battleSkillRows(const std::vector<::MonsterMst>& monsters);

} // namespace gme::raid
