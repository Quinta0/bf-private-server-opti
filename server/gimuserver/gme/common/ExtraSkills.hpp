#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Extra Skills (ExtraPassiveSkillMst) on a player's units — ticket #38
//
// A UserUnitInfo carries three Extra Skill slots plus the Elgif slot.  Read off
// the client (UserUnitInfoResponse::readParam @0x1404E04 and the getters next
// to it), they are:
//
//   cP83zNsv  extra_passive_skill_id      the unit's OWN Extra Skill.  The
//             client never fills it from UnitMst for battle: BattleParty takes
//             UserUnitInfoBase::getExtraPassiveSkillIDList (@0x12B7328), which
//             is just the three per-copy strings.  We sent 0, so no unit's
//             Extra Skill ever worked in battle (Sero-Anya's barrier, #38).
//             Not stored: filled from UnitMst on every read (unitOwnExtraSkill).
//   LjY4DfRg  extra_passive_skill_id2     the skill an Elgif GAVE this unit.
//             UserUnitInfoBase::isHaveAddExtraSkill and the "has Elgif skill"
//             box filter test this slot; UnitMixMainScene::mixOverRideExSkill
//             @0x1C12290 warns when the base already holds one.
//             user_units.extra_passive_skill_id2.
//   T4rewHa9  add_extra_passive_skill_id  the skill an Elgif COPY carries and
//             hands over when fused (mixOverRideExSkill reads it off each
//             material).  user_units.add_extra_passive_skill_id.
//
// The game's only Elgif is unit 50792 "Imbued Tablet Elgif" (unit_ext_mst
// extra_skill_flg 1).  Its skill is per copy: every reward row that grants one
// names it in its last field (mission_mst '6:50792:1:1:1:1030000',
// frontier_gate_reward_mst param '1:0:1012700', achievement '6:50792:1:1:0:…').
// Monster drops name none, so those copies roll one of kElgifDropSkills.
//
// The Void Tablet Emgif (60812, extra_skill_flg 2) is the reverse: fused onto
// a unit it takes the Elgif skill off again (UnitMixMainScene::mixAbsorbExSkill
// checks UnitExtMst for flag 2 before warning about it).  UnitMix handles both.
// ---------------------------------------------------------------------------

namespace gme
{

inline constexpr int32_t kElgifUnitId = 50792;
/*! Void Tablet Emgif: removes the Elgif skill from the unit it is fused onto. */
inline constexpr int32_t kVoidTabletUnitId = 60812;

/*!
* Skills an Elgif gets when its source names none (monster drops, and the
* Elgifs granted before #38 was fixed).  The fifteen rarity-4 "Memories" skills
* Frontier Gate pays out on Elgifs: useful, never a top-tier reward.
*/
inline constexpr std::array<int32_t, 15> kElgifDropSkills{
	1012700, 1012710, 1012720, 1012730, 1012740,
	1012750, 1012760, 1012770, 1012780, 1012790,
	1015800, 1015810, 1015820, 1015830, 1015840,
};

/*!
* SQL expression that picks one of kElgifDropSkills per row (for backfills).
*/
inline std::string randomElgifSkillSql()
{
	std::string sql = "CASE abs(random()) % " + std::to_string(kElgifDropSkills.size());
	for (size_t i = 0; i < kElgifDropSkills.size(); ++i)
		sql += " WHEN " + std::to_string(i) + " THEN " + std::to_string(kElgifDropSkills[i]);
	return sql + " END";
}

/*!
* @return One of kElgifDropSkills at random.
*/
int32_t randomElgifSkill();

/*!
* The Extra Skill a reward entry names in its last ':' field, as in
* '6:50792:1:1:1:1030000' or '1:0:1012700'.
*
* @return The skill id when it is a known ExtraPassiveSkillMst row, else 0.
*/
int32_t extraSkillFromParam(std::string_view packed);

/*!
* @return The unit's own Extra Skill from UnitMst, 0 when it has none.
*/
int32_t unitOwnExtraSkill(uint32_t unitId);

} // namespace gme
