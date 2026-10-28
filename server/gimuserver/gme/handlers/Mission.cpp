#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/forge/Capture.hpp>
#include <gimuserver/forge/ClearLog.hpp>
#include <gimuserver/forge/RepeatClear.hpp>
#include <gimuserver/forge/Rules.hpp>
#include <gimuserver/forge/SquadCheck.hpp>
#include <gimuserver/forge/Tuning.hpp>
#include <gimuserver/gme/common/Achievements.hpp>
#include <gimuserver/gme/common/AutoSell.hpp>
#include <gimuserver/gme/common/BattleItems.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/DailyChallenges.hpp>
#include <gimuserver/gme/common/DailyTask.hpp>
#include <gimuserver/gme/common/EventRotation.hpp>
#include <gimuserver/gme/common/FrontierHunter.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/HelperLends.hpp>
#include <gimuserver/gme/common/HunterOrbs.hpp>
#include <gimuserver/gme/common/MissionBreak.hpp>
#include <gimuserver/gme/common/MissionRuns.hpp>
#include <gimuserver/gme/common/FrontierGate.hpp>
#include <gimuserver/gme/common/FeSkills.hpp>
#include <gimuserver/gme/common/PermitPlace.hpp>
#include <gimuserver/gme/common/ResearchLab.hpp>
#include <gimuserver/gme/common/SummonerProgress.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <gimuserver/gme/common/Sandbox.hpp>

namespace
{
constexpr uint32_t kFirstTutorialMission = 1;
constexpr uint32_t kSecondTutorialMission = 2;

// Tutorial CHAPTER ids (9sQM2XcN), not script numbers — the client maps one to
// the other in GameUtils::getTutorialScriptFile (0x1EB8758), and the mapping is
// NOT the identity:
//
//     chapter 1-9   -> tuto1-9.txt
//     chapter 50-54 -> tuto10-14.txt
//     chapter 10    -> tuto15.txt   (the free-summon tutorial)
//     chapter 11    -> tuto16.txt   (Tilith's farewell)
//     chapter 12    -> tuto17.txt   (does not exist -> nothing plays = done)
//
// ⚠ THE SECOND CHECKPOINT USED TO BE 10, AND THAT SKIPPED FIVE SCRIPTS.
// Chapter 10 is tuto15, so clearing mission 2 jumped the player straight from
// tuto2 to the free-summon tutorial and the whole 50..54 band -- tuto10 through
// tuto14 -- never ran.  The cost was not cosmetic: **tuto14 is the one that
// issues `battle_ui_on`**, so the BB/SBB/UBB/DBB commands were never enabled
// and manual play was left with Guard.  It also fires the summon tutorial ahead
// of the ones meant to precede it, which is why fresh saves see tutorials out
// of order.
//
// These checkpoints are RESUME points, not the driver: the client walks the
// sequence itself and reports each step through TutorialUpdate, which stores
// what it is told.  A resume point ahead of the player therefore does not
// "skip ahead harmlessly" -- it tells the client it is further along than it
// is, and everything in between is lost.
//
// 50 is the chapter that follows tuto9, so the client resumes at tuto10 and
// walks 50->54 then 10, 11, 12 (tuto17 does not exist = done) in order.
constexpr uint8_t kFirstTutorialCheckpoint = 2;
constexpr uint8_t kSecondTutorialCheckpoint = 50;

// What Karl hands over in tuto15.txt ("You received 5 Gems"), which is exactly
// the cost of summon gate 2000 — the gate the client itself labels
// "Tutorial Gacha" (type 20000, GachaActionScene::initConnect @ 0x16988C0).
constexpr uint32_t kTutorialSummonGems = 5;

// Clearing every mission in a Grand Gaia quest dungeon pays this, once — the
// "1 Gem from fully completing a stage in a Quest map" on the wiki's In-Game
// Credits page.  203 dungeons across the campaign, so ~1 Gem per 5 missions.
// See ServerCache::missionQuestDungeon for why the dungeon and not the area.
//
// This is a SERVER-SIDE rule, not MST data: F_MISSION_MST carries exp, zel and
// karma per mission and no gem column at all, and neither AreaMst nor
// DungeonMst has a reward field — so the live server is what paid it, and this
// is where we pay it.  See tools/wiki_gem_scrape.py for the source.
constexpr int64_t kDungeonClearGems = 1;

// Matches CampaignReceipt / DailySpin — the client's counter is 4 digits.
constexpr int64_t kMaxGems = 9'999LL;

/*!
* Pays the clear Gem when a first clear completes a Grand Gaia quest dungeon.
*
* Called only on a FIRST clear, which is what makes it pay once per dungeon
* with no extra bookkeeping: the grant needs the last uncleared mission in the
* dungeon to become cleared, and once that has happened there are no first
* clears left in that dungeon to trigger it again.  Replaying a mission is not
* a first clear, so it cannot pay twice.
*
* @param database Transaction the surrounding MissionEnd is running in.
* @param identity Resolved caller.
* @param missionId The mission just cleared.
*/
// ---------------------------------------------------------------------------
// First-clear rewards
//
// F_MISSION_MST::clear_rewards (SiYs27Cj) carries them on 787 missions as
// comma-separated `type:id:amount:?:?`, in the SAME reward vocabulary the
// present box and CampaignReceipt already dispatch on -- 3 zel, 8 gem, 6 unit,
// 4/5/7 item/material/sphere, 11 karma, 17 summoner SP.
//
// They are delivered as PRESENTS rather than granted inline.  That is the
// deferred half of the reward system this server already has (addUserPresent),
// it reuses one tested payout path instead of adding a second per-type switch,
// and it matches what the game told the player: "Gifts have been awarded to
// you. Visit your presents box to receive them."
// ---------------------------------------------------------------------------
// A sixth field names the skill an Elgif reward carries ('6:50792:1:1:1:1030000',
// ticket #38); extraSkill is 0 everywhere else.
// The grammar (and the repeat-clear list's daily: prefix) lives in
// forge/RepeatClear.hpp, shared with the Balance Forge's mission_mst checks.
using forge::ClearReward;
using forge::parseClearRewards;

// The result screen's own bonus line.  MissionResultBaseScene::getRewardBonusZel
// @0x18B9780 parses this as comma-separated `type:_:amount` triples and sums
// the entries matching its type -- and it only ever asks for 3 (zel), 11
// (karma) and 17 (SP).  Everything else in the reward list is delivered by
// present and would simply be ignored here, so it is not emitted.
static std::string encodeClearBonus(const std::vector<ClearReward>& rewards)
{
	std::string out;
	for (const auto& r : rewards)
	{
		if (r.type != 3 && r.type != 11 && r.type != 17)
			continue;
		if (!out.empty()) out += ',';
		out += std::to_string(r.type) + ":0:" + std::to_string(r.amount);
	}
	return out;
}

// Called on a FIRST clear only.  Returns the quest dungeon this clear completed
// (every one of its missions now cleared), or 0 -- which is also exactly the
// dungeon MissionEnd reports as newly cleared (4sQ8vBXm).
drogon::Task<int32_t> grantDungeonClearGem(
	const db::Database database,
	const gme::UserIdentity identity,
	const uint32_t missionId)
{
	const auto& cache = theServer()->cache();
	const auto dungeonIt = cache.missionQuestDungeon().find(static_cast<int32_t>(missionId));
	if (dungeonIt == cache.missionQuestDungeon().end())
		co_return 0; // Vortex, Frontier Gate, event content — no clear Gem.

	const auto missionsIt = cache.missionsByDungeon().find(dungeonIt->second);
	if (missionsIt == cache.missionsByDungeon().end())
		co_return 0;

	const auto& dungeonMissions = missionsIt->second;

	// One query rather than a per-mission loop: MissionEnd already runs a long
	// chain of awaits inside a transaction on a single-connection SQLite pool,
	// and that is exactly the shape that wedged the pool in §6.14.
	//
	// The ids are QUOTED because user_campaign_missions.mission_id is TEXT (the
	// insert above writes std::to_string(serial_id)).  SQLite would in fact
	// apply the column's TEXT affinity to a bare integer in an IN list and
	// match anyway, but that is a rule worth not depending on.  These are our
	// own MST ints, so there is nothing to escape.
	std::string idList;
	idList.reserve(dungeonMissions.size() * 10);
	for (size_t i = 0; i < dungeonMissions.size(); ++i)
	{
		if (i)
			idList += ',';
		idList += '\'';
		idList += std::to_string(dungeonMissions[i]);
		idList += '\'';
	}

	const auto rows = co_await database->execSqlCoro(
		"SELECT COUNT(DISTINCT mission_id) AS cleared FROM user_campaign_missions"
		" WHERE user_id = $1 AND state = 2 AND mission_id IN (" + idList + ");",
		identity.userId);
	if (rows.empty())
		co_return 0;

	const auto cleared = rows[0]["cleared"].as<int64_t>();
	if (cleared < static_cast<int64_t>(dungeonMissions.size()))
		co_return 0;

	co_await database->execSqlCoro(
		"UPDATE user_info SET gems = MIN(gems + $1, $2)"
		" WHERE gumi_user_id = $3 AND id = $4;",
		kDungeonClearGems,
		kMaxGems,
		identity.gumiUserId,
		identity.userId);

	LOG_INFO << "MissionEnd: quest dungeon " << dungeonIt->second << " fully cleared by "
	         << identity.userId << " (" << dungeonMissions.size()
	         << " missions) — awarded " << kDungeonClearGems << " gem";
	co_return dungeonIt->second;
}

// Fully consume decimal fields; std::stoul accepted suffixes and signed input.
uint32_t dropNumber(const std::string& text)
{
	uint32_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size() || value == 0)
		throw std::runtime_error("Invalid mission drop number: " + text);
	return value;
}

std::vector<UserUnitInfo> parseUnitDrops(const std::string& unitDrops)
{
	std::vector<UserUnitInfo> units;
	std::istringstream drops(unitDrops);
	for (std::string drop; std::getline(drops, drop, ',');)
	{
		if (drop.empty())
		{
			continue;
		}

		std::istringstream parts(drop);
		std::string id;
		std::string level;
		std::string type;
		if (!std::getline(parts, id, ':')
			|| !std::getline(parts, level, ':')
			|| !std::getline(parts, type, ':'))
		{
			throw std::runtime_error("Invalid mission drop unit entry: " + drop);
		}

		const auto dropType = dropNumber(type);
		if (dropType > 6)
			throw std::runtime_error("Invalid mission drop unit type: " + type);
		auto unit = gme::fromArchivedUnit(dropNumber(id), dropType);
		if (!unit)
		{
			throw std::runtime_error("Unknown mission drop unit or type: " + drop);
		}

		// A drop can specify a level above 1, and base_* must follow it.
		// fromArchivedUnit hands back the LEVEL-1 statline, and the client
		// displays user_units.base_* verbatim, so setting the level alone
		// produced a "Lv 30" unit with level-1 stats.
		auto dropLevel = dropNumber(level);
		const auto& catalogue = theServer()->cache().unitMst();
		const auto mst = std::find_if(catalogue.begin(), catalogue.end(),
			[&](const auto& row) { return row.id == static_cast<int32_t>(unit->unit_id); });
		if (mst == catalogue.end() || mst->max_lv < 1)
			throw std::runtime_error("Mission drop unit has no unit master: " + drop);
		// CAPPED, NOT REFUSED.  The level is the mission data's unit_drop_level,
		// echoed by the client, and 156 drop entries (mostly Imps and Crystals,
		// max level 1) ask for Lv 4-9.  Refusing it threw away the whole clear:
		// energy spent, no Zel/Karma/EXP, no units (2026-10-18, 44 failed clears).
		// The unit cannot hold a level above its master's, so it arrives at max.
		if (dropLevel > static_cast<uint32_t>(mst->max_lv))
		{
			LOG_WARN << "MissionEnd: drop " << drop << " asks for Lv " << dropLevel
			         << " but unit " << unit->unit_id << " stops at Lv " << mst->max_lv << "; capped";
			dropLevel = static_cast<uint32_t>(mst->max_lv);
		}

		unit->unit_lvl = dropLevel;
		gme::scaleUnitBaseStats(*unit, static_cast<int>(dropLevel));
		units.push_back(std::move(*unit));
	}

	return units;
}

// Parses the client-reported item drops from MissionBattleResultInfo.item_rewards
// (wire key 4T0Q2Bh5).
//
// APK BattleRewardList::getItemCsv @0x10DFA24 emits itemId:count pairs,
// comma-separated. MissionEndRequest sends them under 4T0Q2Bh5 @0x13A7AE4.
// Reject an invalid batch before any grants are committed. The producer emits
// exactly two positive decimal fields; a bare id is not an observed wire form.
std::vector<std::pair<uint32_t, uint32_t>> parseItemDrops(const std::string& itemDrops)
{
	std::vector<std::pair<uint32_t, uint32_t>> items;
	const auto& catalogue = theServer()->cache().itemMst();
	if (itemDrops.empty()) return items;
	size_t at = 0;
	while (at < itemDrops.size())
	{
		const auto comma = itemDrops.find(',', at);
		const auto entry = itemDrops.substr(at, comma == std::string::npos ? comma : comma - at);
		const auto colon = entry.find(':');
		if (colon == std::string::npos || entry.find(':', colon + 1) != std::string::npos)
			throw std::runtime_error("Invalid mission item drop: " + entry);
		const auto id = dropNumber(entry.substr(0, colon));
		const auto count = dropNumber(entry.substr(colon + 1));
		if (id > INT32_MAX || count > INT32_MAX || std::none_of(catalogue.begin(), catalogue.end(),
			[id](const auto& row) { return row.id == static_cast<int32_t>(id); }))
			throw std::runtime_error("Unknown or oversized mission item drop: " + entry);
		items.emplace_back(id, count);
		if (comma == std::string::npos) break;
		at = comma + 1;
		if (at == itemDrops.size()) throw std::runtime_error("Trailing mission item delimiter");
	}
	return items;
}

std::string encodeUnitDrops(
	const std::vector<UserUnitInfo>& unitDrops,
	const std::vector<bool>& newFlags)
{
	std::string encoded;
	for (size_t idx = 0; idx < unitDrops.size(); ++idx)
	{
		if (!encoded.empty())
		{
			encoded += ',';
		}

		const auto& drop = unitDrops[idx];

		// reward_units is formatted as:
		// unit_id:user_unit_id:play_acquired_animation:show_reward_row
		encoded += std::to_string(drop.unit_id);
		encoded += ':';
		encoded += std::to_string(drop.user_unit_id);
		encoded += ':';
		encoded += (
			idx < newFlags.size() && newFlags[idx]
				? '1'
				: '0');
		encoded += ":1";
	}

	return encoded;
}

/*!
* The player level cap: DefineMst max_team_lv (Kt8H4LN7, 999 in this data).
*
* ⚠ NOT "the last UserLevelMst row".  The level table carries a row for the
* level AFTER the cap: MissionResultScene::updateEvent seeds its EXP counter
* with GameUtils::getNeedUserExp(beforeLv + 1, true) @0x11762A0, which skips
* the max-level check and returns -1 when the row is missing.  At level 999
* that -1 becomes a NEGATIVE per-frame step, and MissionResultBaseScene::countUp
* @0x18B8784 compares `counted + step < total`, which then never becomes false
* -- the result screen froze for every level-999 player.  The row for 1000 is
* the upstream capture's own (see deploy/mst/user_level_mst.json), so the cap
* has to come from the define the client itself compares against.
*/
/*!
* A clear's turn count from MissionEnd's per-wave list ("2,1,2,1,1" -> 7).  0
* when the list is empty or any piece is not a positive number: a list we
* cannot read must not set a record.
*/
int64_t clearTurnCount(const std::string& perWave)
{
	int64_t total = 0;
	std::stringstream stream(perWave);
	for (std::string piece; std::getline(stream, piece, ',');)
	{
		int64_t turns = 0;
		const auto [end, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), turns);
		if (ec != std::errc{} || end != piece.data() + piece.size() || turns <= 0)
			return 0;
		total += turns;
	}
	return total;
}

/*! The level of the race to 999 (21102026_LevelRace). */
constexpr uint32_t kLevelRaceLevel = 999;

uint32_t maxPlayerLevel()
{
	const auto cap = theServer()->cache().initializeResp().defines.max_team_lv;
	return cap > 0 ? static_cast<uint32_t>(cap) : 999u;
}

/*!
* Player EXP Boost (#26): the percent a unit's LEADER SKILL adds to quest EXP.
*
* Passive/process 97 is "Player EXP Boost" (Global wiki, Player EXP Boost rev
* 653737: buff 79, passive 97), and its first parameter is the percent --
* Perpetual Flaw Roglizer's Historical Epilogue (LS 10168) carries "97" with
* "20", which its wiki page notes as "20% EXP" (rev 642770); Holy End
* Roglizer's End of the World (LS 10167) carries 15.  The client has no
* consumer for it: the result screen shows reward_info.inc_exp as the server
* sent it, so the server owns this bonus.
*
* The leader is read from UnitMst by species, as the battle does
* (BattleParty::setPartyPassiveList @0x10B3E10 -> UnitMst::getLeaderSkillID),
* not from user_units.leader_skill_id, which is 0 on units granted before that
* column was written.
*
* LeaderSkillMst.process_param is modelled as a comma list, but its per-process
* separator is '@' ("80,80,80,0,80@15@110,0,0,0,0,0@15" for LS 10167), so the
* list is re-joined and split on '@' to line up with process_id.
*/
constexpr int32_t kPlayerExpBoost = 97;

int32_t leaderExpBoostPercent(const int32_t unitId)
{
	const auto& cache = theServer()->cache();
	const auto unit = std::find_if(cache.unitMst().begin(), cache.unitMst().end(),
		[unitId](const auto& u) { return u.id == unitId; });
	if (unit == cache.unitMst().end() || unit->leader_skill_id <= 0)
		return 0;
	const auto ls = std::find_if(cache.leaderSkillMst().begin(), cache.leaderSkillMst().end(),
		[&](const auto& l) { return l.leader_skill_id == unit->leader_skill_id; });
	if (ls == cache.leaderSkillMst().end())
		return 0;

	std::string joined;
	for (size_t i = 0; i < ls->process_param.size(); ++i)
		joined += (i ? "," : "") + ls->process_param[i];
	std::vector<std::string> params;
	std::stringstream split(joined);
	for (std::string part; std::getline(split, part, '@');)
		params.push_back(part);

	int32_t percent = 0;
	for (size_t i = 0; i < ls->process_id.size() && i < params.size(); ++i)
	{
		if (ls->process_id[i] != kPlayerExpBoost)
			continue;
		const auto first = params[i].substr(0, params[i].find(','));
		int32_t value = 0;
		const auto [end, error] = std::from_chars(first.data(), first.data() + first.size(), value);
		if (error == std::errc{} && value > 0)
			percent += value;
	}
	return percent;
}

/*! The first comma field of a process id or parameter, 0 unless a positive number. */
int32_t leadingNumber(const std::string_view text)
{
	const auto field = text.substr(0, text.find(','));
	int32_t value = 0;
	const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
	return error == std::errc{} && value > 0 ? value : 0;
}

/*!
* Player EXP Boost (#81) an EXTRA SKILL adds: process 97 again, its first
* parameter the percent.  Cheery Bjorn & Linlin's ES 830538 is "1,2@97,2@19,2"
* with "15,15,15,0,15@5@0,0,5,20,10" ("& 5% boost to EXP gained"); BF2
* Tilith's 850808 carries 97 with 5 too.  An Extra Skill works from any squad
* slot, so every squad unit's counts, the helper's included.  Its processes are
* "<id>,<target>"; no shipped 97 entry has a condition (term "1#0,0%0,0..."),
* so none is checked.
*/
int32_t extraSkillExpBoostPercent(const int32_t skillId)
{
	if (skillId <= 0)
		return 0;
	const auto& skills = theServer()->cache().initializeResp().extra_passive_skills;
	const auto skill = std::find_if(skills.begin(), skills.end(),
		[skillId](const auto& s) { return s.id == skillId; });
	if (skill == skills.end())
		return 0;
	const auto processes = gme::FeSkillSet::split(skill->process_id, '@');
	const auto params = gme::FeSkillSet::split(skill->process_param, '@');
	int32_t percent = 0;
	for (size_t i = 0; i < processes.size() && i < params.size(); ++i)
		if (leadingNumber(processes[i]) == kPlayerExpBoost)
			percent += leadingNumber(params[i]);
	return percent;
}

/*!
* Player EXP Boost (#81) the SP enhancements on a LEADER add to its leader
* skill.  Process 97 under effect type 5 (the LS/ES upgrades) is "Enhances LS
* EXP boost": Infinity Starsage Zeruiah's 1000010054 adds 12 to her 20, Perpetual
* Flaw Roglizer's 21001002 adds 5 to his 20 (its text: リーダースキルの「獲得経験値を
* 大幅にアップ」効果をグレードアップ).  Under another effect type 97 is a BB
* process (33006000, SBB damage), not EXP.
*
* Which bought nodes count is the client's rule, UserUnitInfoBase::
* getFeEffectiveSkillInfo @0x12B8358: a node drops out only when another bought
* node of the same series has a HIGHER lv (isFeSkillLvCheck @0x12B86C0 compares
* getSeries, then getLv with b.ge).  Cheery Bjorn & Linlin's 1100000141 (+5)
* and 1100000142 (+10, needs the first) are both series 1100000141 lv 6, so
* both count: 20 + 5 + 10.
*
* @param feSkillInfo user_units.fe_skill_info (gme::FeSkillSet's format).
*/
int32_t spExpBoostPercent(const std::string_view feSkillInfo)
{
	constexpr int32_t kPassiveSkillUpgrade = 5;
	const auto acquired = gme::FeSkillSet::parse(feSkillInfo);
	const auto& all = theServer()->cache().feSkillMst();
	std::vector<const FeSkillMst*> owned;
	for (const auto& group : acquired.groups)
		for (const auto& text : group.second)
		{
			const auto id = gme::FeSkillSet::id(text);
			const auto skill = std::find_if(all.begin(), all.end(),
				[&id](const auto& s) { return id && s.id == *id; });
			if (skill != all.end() && std::find(owned.begin(), owned.end(), &*skill) == owned.end())
				owned.push_back(&*skill);
		}

	int32_t percent = 0;
	for (const auto* skill : owned)
	{
		const auto superseded = std::any_of(owned.begin(), owned.end(), [skill](const auto* other) {
			return other->series == skill->series && other->lv > skill->lv;
		});
		if (superseded)
			continue;
		const auto effects = gme::FeSkillSet::split(skill->effect_type, '@');
		const auto processes = gme::FeSkillSet::split(skill->process_type, '@');
		const auto params = gme::FeSkillSet::split(skill->process_param, '@');
		for (size_t i = 0; i < processes.size() && i < params.size() && i < effects.size(); ++i)
			if (leadingNumber(effects[i]) == kPassiveSkillUpgrade
				&& leadingNumber(processes[i]) == kPlayerExpBoost)
				percent += leadingNumber(params[i]);
	}
	return percent;
}

/*!
* Applies pending EXP to the current user level.
*
* The caller should pass exp after adding the mission reward. This function
* treats UserLevelMst::exp as the per-level chunk required to reach level + 1,
* then mutates level and exp so exp remains the residual progress at the new
* level.  It never levels past maxLevel, and at the cap nothing carries over:
* the client draws an empty bar there (MissionResultScene picks 0 when
* getMaxTeamLv() == beforeLv @0x18CCE4C) and there is no level left to spend
* it on.
*
* @return True if at least one level was gained.
*/
bool levelUp(uint32_t& level, uint32_t& exp, const uint32_t maxLevel)
{
	bool leveled = false;
	while (level < maxLevel)
	{
		const auto mst = gme::getLevelMst(level + 1);
		if (!mst || exp < mst->exp)
			break;

		// Advance a level.
		level++;
		exp -= mst->exp;
		leveled = true;
	}

	if (level >= maxLevel)
		exp = 0;

	return leveled;
}

/*! Longer than any helper id the server hands out (real ids are 8, NPC ids at most 16). */
constexpr size_t kMaxHelperIdLength = 32;

/*!
* Whether every character could appear in a helper id the server hands out.
*
* Real ids come from SecureRandomId's [0-9A-Za-z]; the NPC bands (DCF#####,
* DEV + upper-cased name letters, NEW + 8 digits) stay inside it.
*/
bool helperIdCharacters(const std::string& id)
{
	return std::all_of(id.begin(), id.end(),
		[](const unsigned char c) { return std::isalnum(c) != 0; });
}

/*!
* The helper a MissionStart may record for MissionEnd's Honor, or "".
*
* The id (9Q1Lq5FS.h7eY3sAK) is the client's word, and stored as sent, any
* made-up string earned the borrower the stranger rate on every clear.  It is
* kept only when it names someone the helper picker could have offered:
*   - a REAL player other than the borrower (gme::isRealPlayer);
*   - an NPC -- roster friend, developer or stranger -- that
*     gme::helperLeader resolves to a unit, the lookup MissionEnd's
*     leader-EXP boost already relies on.
* "0" and "" (a solo run) come back "" as before; MissionEnd reads both as no
* helper.
*
* STORY GUESTS EARN NOTHING (decided 2026-10-18).  The client's built-in fixed
* helpers -- FixedReinforcementInfoList::useNoDLCList @0xee02c4: Fencer Vargas,
* Selena, Pikeman Lance and Warrior Eze for missions 10-12, offered only on a
* selective-DLC install that has not finished its first download -- go out as
* their HANDLE NAME (GameUtils::convertToReinforcementInfo @0x1ec0670 makes it
* the user id; the unit id when UnitMst has no name).  Their card promises no
* Honor (friend point 0, no Honor line) and the player does not choose them,
* so the stranger rate they used to earn was a mismatch in the player's
* favour.  Those names are no id this server issues, so they record as solo.
* GuestUnitMst guests already send "0".
*
* The character check comes first for a reason beyond tidiness: SQLite binds
* the text up to its first NUL, so "<my id>\0" would pass the not-yourself
* test here and still find the borrower's own row.
*
* Must not hold a transaction: it reads through theDb() on the one-connection
* pool.  A database failure here is the caller's to swallow.
*
* @param identity The borrower.
* @param helperId MissionStart's h7eY3sAK, unvalidated.
*/
drogon::Task<std::string> recordableHelper(const gme::UserIdentity identity, const std::string helperId)
{
	if (!gme::borrowedHelper(helperId) || helperId.size() > kMaxHelperIdLength
		|| !helperIdCharacters(helperId) || helperId == identity.userId
		|| gme::isOwnHelperId(helperId))   // the player's own unit (FriendGet): solo
		co_return std::string{};

	// A real player only from the borrower's own pool (gme/common/Sandbox.hpp):
	// a friend across the line earns no Honor for a stale picker card.
	if (co_await gme::isRealPlayer(theDb(), helperId))
	{
		if (!(co_await gme::sandbox::samePool(theDb(), identity.userId, helperId)))
			co_return std::string{};
		co_return helperId;
	}
	if ((co_await gme::helperLeader(theDb(), identity, helperId)).unit_id != 0)
		co_return helperId;
	co_return std::string{};
}
}

HANDLEF(MissionEnd)
{
	MissionEndReq req = {};
	const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json);
	if (ec)
	{
		const auto error = glz::format_error(ec, json);
		LOG_ERROR << "MissionEndReq deserialization failed:\n" << error;
		co_return HandleResult::error("Deserialization error", error);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// THE SERIAL NAMES THE BATTLE, NOT THE MISSION (gme/common/MissionRuns.hpp).
	// Resolved once here; everything below uses missionId.  A run serial this
	// user was never issued cannot be settled against any mission.
	const uint32_t wireSerial = req.mission_num.serial_id;
	const bool issuedSerial = wireSerial >= gme::kMissionRunSerialFloor;
	const auto resolvedMission = co_await gme::missionForSerial(theDb(), identity, wireSerial);
	if (!resolvedMission)
	{
		LOG_WARN << "MissionEnd: " << identity.userId << " sent battle serial " << wireSerial
			<< ", which was never issued to them";
		co_return HandleResult::error("Unknown battle", "No battle was started with that serial");
	}
	const uint32_t missionId = *resolvedMission;

	// Same battle-content fallback as MissionStart — without it a mission that
	// STARTS on template content would fail on completion instead, which is a
	// worse failure: the player fights the battle and then loses the result.
	// See the block in MissionStart for why only content is substituted.
	auto missionRecord = MissionArchiver::instance().lookup(missionId);
	if (!missionRecord)
	{
		missionRecord = MissionArchiver::instance().lookup(10);
		if (!missionRecord)
		{
			co_return HandleResult::error("Archive error", "Unable to find mission record");
		}
		// Relabelled for the same reason as MissionStart — the result screen
		// reads the mission back out of the response.
		missionRecord->id = missionId;

		LOG_WARN << "MissionEnd: mission " << missionId
		         << " has no authored battle content; using the template record "
		            "relabelled as this mission";
	}

	MissionEndResp resp{};
	std::string buffer;

	// A Frontier Gate battle sends MissionEnd win or lose, and 2 is a win
	// (UserState::getMissionState — the client subtracts the lost floor on
	// anything else).  A lost battle is not a clear: it pays only what the
	// client picked up, records no clear, and ends the run (below).
	const bool frontierBattle = req.frontier_battle && !req.frontier_battle->empty();
	const bool frontierLost = frontierBattle && req.mission_num.mission_status.has_value()
		&& *req.mission_num.mission_status != 2;

	// The same status decides a NORMAL mission: 103 of the 104 captured ends
	// carry "2", the other "3" (a loss or a give-up).  Honor is paid for a
	// cleared mission only, so a wipe cannot farm the helper.
	const bool missionLost = req.mission_num.mission_status.has_value()
		&& *req.mission_num.mission_status != 2;

	// The helper whose OWNER is owed Honor for this clear, credited once the
	// result below has committed (gme::creditHelperLender).  Left empty by a
	// loss, a solo run and the no-reward replay path.
	std::string lenderToCredit;
	bool lenderIsFriend = false;

	// This needs to be wrapped as a transaction to prevent cases where we issue
	// partial rewards upon mission completion.
	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			auto userInfo = co_await db::DatabaseInterface::read(
				transaction,
				"user_info",
				{
					db::Data("level"),
					db::Data("exp"),
					db::Data("zel"),
					db::Data("karma"),
					db::Data("brave_coin"),
					db::Data("friend_points"),
					db::Data("reinforce_user_id"),
					db::Lookup("gumi_user_id", identity.gumiUserId),
					db::Lookup("id", identity.userId),
			});

			// Fetch the current user state.
			const auto currentZel = userInfo.front<uint64_t>("zel");
			const auto currentKarma = userInfo.front<uint64_t>("karma");
			const auto currentLevel = userInfo.front<uint32_t>("level");
			const auto currentExp = userInfo.front<uint32_t>("exp");
			const auto currentBraveCoin = userInfo.front<int32_t>("brave_coin");
			const auto currentHonor = userInfo.front<int32_t>("friend_points");
			const auto helperUserId = userInfo.front<std::string>("reinforce_user_id");

			// ONE RESULT PER BATTLE.  MissionStart records the battle it opened
			// (user_info.open_mission_id) and this clears it to 0, so a second
			// MissionEnd for the same start -- a client retry after a lost
			// reply, or a replayed request -- finds nothing open.  It must not
			// pay, record, drop or spend anything a second time; it gets the
			// current state with zero rewards instead of an error, because
			// every handler error closes the client's session.  NULL is a
			// battle opened before this column existed and is honoured once.
			//
			// EXACT FOR ISSUED SERIALS.  The client echoes the serial MissionStart
			// gave it, and that serial settles only while it is the open battle's
			// (open_mission_serial) -- so a repeat of a settled result, or a
			// late result from an earlier run of the SAME mission after a new
			// one started, both pay nothing.
			//
			// A MISSION-ID SERIAL SETTLES ONLY A BATTLE THAT PREDATES SERIALS.
			// open_mission_serial is NULL until the first MissionStart after the
			// upgrade (29092026_MissionRunSerials adds it without a default), and
			// every start and every settled result writes it; so NULL, and only
			// NULL, means the open battle -- open_mission_id, or nothing known for
			// a save older than that column too -- was started by a server that
			// sent the mission id as the serial.  That battle may settle once
			// with the mission-id serial its client holds.  Once any run has been
			// issued, the open battle has a serial of its own, and a mission-id
			// serial is at best a delayed result from before the upgrade: letting
			// it through would pay the new run and leave its genuine result
			// nothing to settle (QC 2026-09-30, P1).
			{
				const auto open = co_await transaction->execSqlCoro(
					"SELECT open_mission_id, open_mission_serial FROM user_info WHERE id = $1;",
					identity.userId);
				const bool serialEra = !open.empty() && !open[0]["open_mission_serial"].isNull();
				const bool openUnknown = open.empty() || open[0]["open_mission_id"].isNull();
				const auto openMission = openUnknown ? int64_t{ -1 } : open[0]["open_mission_id"].as<int64_t>();
				const auto openSerial = serialEra ? open[0]["open_mission_serial"].as<int64_t>() : int64_t{ 0 };
				const bool settles = issuedSerial
					? serialEra && openSerial == static_cast<int64_t>(wireSerial)
					: !open.empty() && !serialEra
						&& (openUnknown || openMission == static_cast<int64_t>(missionId));
				if (!settles)
				{
					LOG_WARN << "MissionEnd: " << identity.userId << " ended mission "
						<< missionId << " (serial " << wireSerial << ") but the open battle is "
						<< openMission << " (serial " << openSerial << "); answering with no rewards";

					resp.login_info = std::move((co_await gme::getLoginInfo(transaction, identity)).nonEmpty());
					gme::omitClientScenarioMarkers(resp.login_info);
					resp.team_info = std::move((co_await gme::getTeamInfo(transaction, identity)).nonEmpty());
					resp.energy_recover.action_point_threshold = resp.team_info.max_action_point;
					// Nothing was cleared by THIS reply, so it says so in the
					// client's own sentinels (see "WHAT THIS RESULT NEWLY
					// CLEARED" below): a mission id here would replay that
					// mission's end script, and an empty area id resets the map.
					resp.reward_info.clear_mission_id = 0;
					resp.reward_info.clear_dungeon_id.clear();
					resp.reward_info.clear_area_id = "0";
					resp.reward_info.before_level = currentLevel;
					resp.clear_mission_info = co_await gme::getClearedMissions(transaction, identity);
					// The current badges: a retry after a lost reply still
					// lights a challenge the first one finished.
					resp.badge = co_await gme::achievementBadge(transaction, identity);
					const auto permit = co_await gme::buildPermitPlace(transaction, identity);
					if (const auto error = glz::write_json(resp, buffer); error)
						throw std::runtime_error(glz::format_error(error, buffer));
					gme::injectPermitPlace(buffer, permit);
					// A Frontier Hunter retry: nothing gained, and the result
					// screen's reward lists cleared rather than showing the
					// last run's rewards again (ticket #8).
					gme::frohun::appendMembers(buffer, co_await gme::frohun::replayRun(
						transaction, identity, static_cast<int32_t>(missionId)));
					co_return HandleResult::success(buffer);
				}
				co_await transaction->execSqlCoro(
					"UPDATE user_info SET open_mission_id = 0, open_mission_serial = 0 WHERE id = $1;",
					identity.userId);
			}

			// Parsed before anything is paid: a malformed log fails the whole
			// result inside this transaction rather than being half-applied.
			const auto usedItems = gme::parseUseItemLog(req.battle_result.use_item_log);

			// HONOR for the Summoner Helper this run borrowed.  MissionStart
			// recorded who it was; the amount is the one the card promised
			// (gme::honorForMission picks the friend or stranger rate exactly
			// as ReinforcementInfo::getFriendPoint does), and it is credited
			// straight to the balance rather than paid into the present box,
			// because the result screen counts it off team_info.
			//
			// Cleared and zeroed in the same UPDATE below whatever the outcome,
			// so one start can only ever pay once.
			//
			// The roster decides the rate, and it is read on THIS transaction:
			// the SQLite pool has one connection, so a helper that opened its
			// own would deadlock against the one already held here.
			const auto helperWasFriend =
				co_await gme::isFriend(transaction, identity, helperUserId);
			const auto honorEarned =
				missionLost ? 0 : gme::honorForMission(helperUserId, helperWasFriend);
			// CHAPTER 3 pays that helper in Summoner Training Points instead
			// ("In Brave Frontier's Chapter 3, you will not gain Honor Points"):
			// the same amount goes to user_summoner in the Summoner block below
			// (ticket #143).
			const bool chapter3Quest = gme::isChapter3Quest(static_cast<int32_t>(missionId));
			const auto honorToBalance = chapter3Quest ? 0 : honorEarned;
			// The lender's Honor (gme::creditHelperLender) follows the same
			// rule: no Honor is earned in Chapter 3, on either side.
			if (!missionLost && !chapter3Quest && gme::borrowedHelper(helperUserId))
			{
				lenderToCredit = helperUserId;
				lenderIsFriend = helperWasFriend;
			}

			// THE RESEARCH LAB PAYS ONCE: "Subsequent victories won't award
			// anything" (gme/common/ResearchLab.hpp).  A repeat clear of a lab
			// mission pays no archive Zel, Karma or EXP; its first-clear
			// rewards are already once-only below.  Read before the clear is
			// recorded.
			const auto labReplay = !missionLost
				&& gme::isResearchLabMission(static_cast<int32_t>(missionId))
				&& !(co_await transaction->execSqlCoro(
					"SELECT 1 FROM user_campaign_missions"
					" WHERE user_id = $1 AND mission_id = $2 AND state = 2;",
					identity.userId,
					std::to_string(missionId))).empty();

			// A reported loss earns none of the archive clear rewards.
			const auto payClear = !missionLost && !labReplay;
			const auto rewardZel = req.battle_result.zel + (payClear ? missionRecord->zel : 0);
			const auto rewardKarma = req.battle_result.karma + (payClear ? missionRecord->karma : 0);
			// Nothing is earned at the level cap ("prevent the user from gaining
			// any more experience"): the result shows no EXP obtained, and the
			// stored progress stays at 0 (levelUp below).
			const auto maxLevel = maxPlayerLevel();
			const auto baseExp = (payClear && currentLevel < maxLevel) ? missionRecord->exp : 0;

			// PLAYER EXP BOOST.  Global wiki, Player EXP Boost rev 653737: quest
			// EXP x (leader skill 1 + leader skill 2 + Extra Skill + SP
			// enhancement + ...), every source ADDING ("Zelnite lead and friend
			// ... totaling up to 30%").  Counted here (#26, #81):
			//   - each leader's skill (leaderExpBoostPercent) and the SP
			//     upgrades bought on that leader (spExpBoostPercent): the
			//     player's leader is the party member the client flags
			//     member_type 0 in this very request, the helper's the unit the
			//     picker offered for the helper MissionStart recorded;
			//   - the Extra Skill of every unit in the squad, own or Elgif-given,
			//     and the helper's own (extraSkillExpBoostPercent).
			// The client sends the battle party here (MissionEndRequest::
			// createBody reads UserPartyInfoList), so a unit listed twice counts
			// once.  Rounded down.  NOT modelled: guild / event multipliers the
			// same formula names.
			int32_t expBoostPercent = 0;
			if (baseExp > 0)
			{
				bool leaderCounted = false;
				std::set<uint64_t> squad;
				for (const auto& member : req.party_deck_info)
				{
					if (!squad.insert(member.user_unit_id).second)
						continue;
					const auto unitRows = co_await transaction->execSqlCoro(
						"SELECT unit_id, extra_passive_skill_id2, fe_skill_info FROM user_units"
						" WHERE user_id = $1 AND user_unit_id = $2;",
						identity.userId, static_cast<int64_t>(member.user_unit_id));
					if (unitRows.empty())
						continue;
					const auto& row = unitRows[0];
					const auto species = leadingNumber(row["unit_id"].as<std::string>());
					const auto elgifSkill = leadingNumber(row["extra_passive_skill_id2"].as<std::string>());
					expBoostPercent += extraSkillExpBoostPercent(gme::unitOwnExtraSkill(species))
						+ extraSkillExpBoostPercent(elgifSkill);
					if (member.member_type == 0 && !leaderCounted)
					{
						leaderCounted = true;
						expBoostPercent += leaderExpBoostPercent(species)
							+ spExpBoostPercent(row["fe_skill_info"].as<std::string>());
					}
				}
				if (gme::borrowedHelper(helperUserId))
				{
					const auto helper = co_await gme::helperLeader(transaction, identity, helperUserId);
					expBoostPercent += leaderExpBoostPercent(helper.unit_id)
						+ spExpBoostPercent(helper.fe_skill_info)
						+ extraSkillExpBoostPercent(gme::unitOwnExtraSkill(helper.unit_id))
						+ extraSkillExpBoostPercent(helper.elgif_skill_id);
				}
			}
			// EXP BOOSTERS (ticket #50).  An Experience Token or Badge
			// in the quest-prep bonus bar adds its percent to the same sum (the
			// wiki formula above names item multipliers there too), and one is
			// used up by the clear it paid -- a lost quest, or one at the level
			// cap, keeps it (see gme::findExpBooster).
			std::optional<gme::ExpBooster> expBooster;
			if (baseExp > 0)
			{
				expBooster = co_await gme::findExpBooster(transaction, identity);
				if (expBooster)
					expBoostPercent += expBooster->percent;
			}
			const auto rewardExp = static_cast<uint32_t>(
				static_cast<uint64_t>(baseExp) * static_cast<uint64_t>(100 + expBoostPercent) / 100);
			if (expBoostPercent > 0)
			{
				LOG_INFO << "MissionEnd: EXP boost +" << expBoostPercent << "%"
					<< (expBooster ? " (booster item " + std::to_string(expBooster->itemId) + ")" : std::string{})
					<< " -> " << baseExp << " becomes " << rewardExp;
			}

			// See if we leveled up.  Summed wide: a save-edited exp near the top
			// of the column must not wrap into a small number.
			auto newLevel = currentLevel;
			auto newExp = static_cast<uint32_t>(std::min<uint64_t>(
				static_cast<uint64_t>(currentExp) + rewardExp, UINT32_MAX));
			const auto leveledUp = levelUp(newLevel, newExp, maxLevel);

			// The client expects the post-mission total, including the amount earned
			// during this mission.
			(co_await db::DatabaseInterface::update(
				transaction,
				"user_info",
				{
					db::Data("level", newLevel),
					db::Data("exp", newExp),
					db::Data("zel", currentZel + rewardZel),
					db::Data("karma", currentKarma + rewardKarma),
					db::Data("friend_points", currentHonor + honorToBalance),
					db::Data("reinforce_user_id", std::string{}),
					db::Lookup("gumi_user_id", identity.gumiUserId),
					db::Lookup("id", identity.userId)
				})).nonEmpty();

			if (leveledUp)
			{
				(co_await gme::UserEnergy::refresh(transaction, identity, newLevel, true)).nonEmpty();
				// A level-up fills the Arena Orbs too (2026-10-21, the "leveling up
				// should refill arena orbs" suggestion).  The regeneration clock stops
				// at the cap, as in ShopUse's restoreArenaOrbs.
				co_await transaction->execSqlCoro(
					"UPDATE user_info SET fight_point = max_fight_point WHERE id = $1;",
					identity.userId);
				co_await transaction->execSqlCoro(
					"UPDATE user_arena SET orb_ts = 0 WHERE user_id = $1;", identity.userId);
			}
			// THE RACE TO LEVEL 999: this clear took the player there.  Written in
			// this transaction, so the row commits with the level-up and its seq
			// keeps the order players got there (21102026_LevelRace).  OR IGNORE:
			// one row per account, ever.
			// Sandbox accounts are not in the race (gme/common/Sandbox.hpp).
			if (currentLevel < kLevelRaceLevel && newLevel >= kLevelRaceLevel)
			{
				if (co_await gme::sandbox::isOn(transaction, identity.userId))
				{
					LOG_INFO << "Level race: " << identity.userId << " reached level " << newLevel
					         << " in Sandbox mode; not entered";
				}
				else
				{
					co_await transaction->execSqlCoro(
						"INSERT OR IGNORE INTO level_race (user_id, reached_at) VALUES ($1, $2);",
						identity.userId, gme::unixNow());
					LOG_INFO << "Level race: " << identity.userId << " reached level " << newLevel;
				}
			}
			if (expBooster)
				co_await gme::spendExpBooster(transaction, identity, *expBooster);

			// Persist tutorial checkpoints so leaving and returning mid-tutorial does not
			// replay completed steps.
			if (!missionLost && missionId == kFirstTutorialMission)
			{
				co_await transaction->execSqlCoro(
					"UPDATE user_info SET tutorial_status = $1"
					" WHERE id = $2 AND gumi_user_id = $3 AND tutorial_status < $1;",
					kFirstTutorialCheckpoint, identity.userId, identity.gumiUserId);

			}
			else if (!missionLost && missionId == kSecondTutorialMission)
			{
				// Chapter 10 arms the free-summon tutorial, and tuto15.txt opens
				// with Karl handing over the gems for it ("You received 5 Gems",
				// then "Now summon yourself a strong Unit!").  The script only
				// draws that message — the balance has to come from here, or the
				// player reaches the summon gate unable to pay for it.
				//
				// ⚠ THE GUARD CANNOT BE `tutorial_status < <checkpoint>`.  The
				// chapter ids are not monotonic -- the order is 1..9, 50..54,
				// 10, 11, 12 -- so against a checkpoint of 50 a FINISHED player
				// (chapter 12) still compares "less than", and replaying
				// mission 2 would reset them into the tutorial and pay the gems
				// again.  The old `< 10` hid this only because 10 happened to
				// be low.
				//
				// Advance only a player still inside the opening band, which is
				// the one case this checkpoint is for.  MissionEnd runs again on
				// every replay of mission 2, and everyone else is left alone.
				co_await transaction->execSqlCoro(
					"UPDATE user_info"
					" SET tutorial_status = $1, gems = gems + $2"
					" WHERE gumi_user_id = $3 AND id = $4"
					"   AND tutorial_status BETWEEN 1 AND 9;",
					kSecondTutorialCheckpoint,
					kTutorialSummonGems,
					identity.gumiUserId,
					identity.userId);
			}

			// Filled in below when this turns out to be a first clear; stays
			// empty otherwise, which is what the live server sent and what the
			// result screen's length check expects.
			std::string firstClearBonus;
			// The first-clear Zel and Karma, paid straight into the balance (see
			// the first-clear block) and counted in reward_info.zel / .karma.
			int64_t firstClearZel = 0;
			int64_t firstClearKarma = 0;
			// What this result newly cleared, for the reward block's clear ids
			// (see "WHAT THIS RESULT NEWLY CLEARED").  A loss clears nothing.
			bool newlyCleared = false;
			int32_t completedDungeon = 0;

			// A Frontier Gate battle reports the run's floors and score
			// (eIQ79KO2).  They are stored for the next battle and the end of
			// the run, and a LOST battle ends the run here: FG sends MissionEnd
			// win or lose, and only this reply can carry the result that moves
			// the client on (see gme/common/FrontierGate.hpp).
			std::optional<gme::FrontierRun> frontierRun;
			if (frontierBattle)
			{
				const auto& report = req.frontier_battle->front();
				frontierRun = co_await gme::loadFrontierRun(transaction, identity);
				if (frontierRun)
				{
					frontierRun->score = report.now_score;
					frontierRun->progress = report.progress;
					frontierRun->note = report.note;
					frontierRun->odInfo = report.od_info;
					co_await gme::storeFrontierRun(transaction, identity, *frontierRun);
					// Pay whatever the run has now reached.  Waiting for the
					// run to end loses it all if the player starts the gate
					// again instead of retiring.
					co_await gme::payFrontierRewards(transaction, identity, *frontierRun);
					// Those payouts include the gate's own currencies, so the
					// reply carries the lists they changed rather than leaving
					// the client on a stale balance until the next login.
					if (frontierRun->tokensChanged)
						resp.event_token_info = co_await gme::loadEventTokens(transaction, identity);
					if (frontierRun->ticketsChanged)
						resp.summon_ticket_v2_user = co_await gme::loadSummonTicketsV2(transaction, identity);
				}
				else
				{
					LOG_WARN << "MissionEnd: Frontier Gate battle with no open run for "
						<< identity.userId;
				}
			}

			// Record the clear in the mission clear-history
			// (user_campaign_missions, state=2).  UserInfo reports this set as
			// UT1SVg59 (UserClearMissionInfo) — the list the client evaluates
			// feature unlocks against (F_FUNCTION_RELEASE_MST conditions and
			// the hardcoded town/early-feature gates), so every victorious
			// MissionEnd must land here. A reported loss must not create a clear.
			if (!missionLost)
			{
				const auto clearedAt = static_cast<int64_t>(
					std::chrono::duration_cast<std::chrono::seconds>(
						std::chrono::system_clock::now().time_since_epoch()).count());

				// Whether this is the FIRST clear decides the area Gem below,
				// so it has to be read before the upsert bumps clear_count.
				const auto firstClear = (co_await transaction->execSqlCoro(
					"SELECT 1 FROM user_campaign_missions"
					" WHERE user_id = $1 AND mission_id = $2 AND state = 2;",
					identity.userId,
					std::to_string(missionId))).empty();
				newlyCleared = firstClear;

				co_await transaction->execSqlCoro(
					"INSERT INTO user_campaign_missions"
					" (user_id, mission_id, state, attain_percent, clear_count, last_cleared_at)"
					" VALUES ($1, $2, 2, 100, 1, $3)"
					" ON CONFLICT(user_id, mission_id) DO UPDATE SET"
					" state=2, attain_percent=100,"
					" clear_count=clear_count+1, last_cleared_at=$3;",
					identity.userId,
					std::to_string(missionId),
					clearedAt);

				// FEATURED EVENT TOKENS (gme/common/EventRotation.hpp).  Only
				// this week's event pays, by mission: every clear, the first of
				// a game day, or the first of the featured week.  The reply
				// carries the new balances so the result screen and the Bazaar
				// are current without waiting for the next login.
				if (co_await gme::payFeaturedEventTokens(transaction, identity, missionId, clearedAt))
					resp.event_token_info = co_await gme::loadEventTokens(transaction, identity);

				// DAILY TASKS.  Both quest codes are fed from here because this
				// is the one place that knows a mission was WON: the client
				// reads them in MissionResultScene, but nothing reports the
				// completion back, so the count has to be derived server-side.
				// A Vortex clear is a quest clear too, so QE always advances
				// and VV additionally when the mission is in the Vortex land.
				// advanceDailyTask ignores a code that is not offered today,
				// so both calls are unconditional.
				// ⚠ QUEST EXPLORER IS SCOPED TO TWO AREAS, not to any mission:
				// "Complete 3 Missions in (any 2 randomly selected Areas)".
				// gme::dailyTaskQuestAreas picks the day's pair and
				// fillDailyTaskTables names them on the tile, so the counter
				// has to agree with what the player was told — on the same
				// task day, which turns at 08:00 UTC (gme::dailyTaskToday).
				// One read of the day for both, so a clear on the stroke of
				// 08:00 is checked and counted on the same day.
				const auto taskDay = gme::dailyTaskToday();
				{
					const auto areas = co_await gme::dailyTaskQuestAreas(
						transaction, identity, taskDay);
					if (std::find(areas.begin(), areas.end(),
							gme::missionAreaId(missionId)) != areas.end())
					{
						co_await gme::advanceDailyTask(transaction, identity, "QE", 1, taskDay);
					}
				}
				if (theServer()->cache().vortexMissions().count(missionId))
					co_await gme::advanceDailyTask(transaction, identity, "VV", 1, taskDay);

				// "CLEARED (NO CONTINUES)".  A continue during this run left a
				// row behind; its absence is what earns the flag.  Sticky —
				// once a mission has been done cleanly, a later messy clear
				// must not take it back, so this only ever sets 1.
				const auto continued = co_await transaction->execSqlCoro(
					"SELECT 1 FROM user_mission_continues"
					" WHERE user_id = $1 AND mission_id = $2;",
					identity.userId, std::to_string(missionId));
				if (continued.empty())
				{
					co_await transaction->execSqlCoro(
						"UPDATE user_campaign_missions SET no_continue = 1"
						" WHERE user_id = $1 AND mission_id = $2;",
						identity.userId, std::to_string(missionId));
				}
				co_await transaction->execSqlCoro(
					"DELETE FROM user_mission_continues WHERE user_id = $1 AND mission_id = $2;",
					identity.userId, std::to_string(missionId));

				// "CLEARED IN N TURNS": the fewest turns a won clear took, from
				// the client's per-wave list (Lu0pAk1R).  Only ever lowered.
				// ⚠ Placeholders in order of first use: SQLite numbers $N by
				// where it first appears, and the arguments bind 1, 2, 3.
				if (const auto turns = clearTurnCount(req.battle_result.clear_turns); turns > 0)
				{
					co_await transaction->execSqlCoro(
						"UPDATE user_campaign_missions SET best_turns = $1"
						" WHERE user_id = $2 AND mission_id = $3 AND (best_turns = 0 OR best_turns > $1);",
						turns, identity.userId, std::to_string(missionId));
				}

				if (firstClear)
				{
					completedDungeon = co_await grantDungeonClearGem(transaction, identity, missionId);

					// Per-mission first-clear rewards, queued to the present box.
					const auto& clearRewards = theServer()->cache().missionClearRewards();
					const auto rewardIt = clearRewards.find(
						static_cast<int32_t>(missionId));
					if (rewardIt != clearRewards.end())
					{
						const auto rewards = parseClearRewards(rewardIt->second);
						for (const auto& r : rewards)
						{
							// ZEL AND KARMA ARE PART OF THIS RESULT, not presents.
							// MissionResultScene::updateEvent @0x18CB3C8 shows the
							// quest's own Zel as reward_info.zel minus the clear
							// bonus's Zel (getRewardBonusZel, type 3) minus the
							// guild bonus, and Karma the same way (type 11): the
							// client expects the bonus inside reward_info and in
							// the balance already.  Queued as a present instead,
							// a 120,000 Zel first clear (The Longest Night)
							// showed "-119,xxx Zel" until the next refresh
							// (ticket #134).
							if (r.type == 3 && r.amount > 0)
							{
								firstClearZel += r.amount;
								continue;
							}
							if (r.type == 11 && r.amount > 0)
							{
								firstClearKarma += r.amount;
								continue;
							}
							co_await gme::addUserPresent(
								transaction, identity, r.type, r.id, r.amount,
								0, "First clear reward", r.extraSkill);
						}
						if (firstClearZel > 0 || firstClearKarma > 0)
						{
							// Clamped like the present box's Zel and Karma.
							co_await transaction->execSqlCoro(
								"UPDATE user_info SET zel = MAX(zel, MIN(zel + $1, $2)),"
								" karma = MAX(karma, MIN(karma + $3, $2)) WHERE id = $4;",
								firstClearZel, static_cast<int64_t>(99'999'999), firstClearKarma,
								identity.userId);
						}
						// The result screen shows only the currency half.
						firstClearBonus = encodeClearBonus(rewards);
						LOG_INFO << "MissionEnd: first clear of "
							<< missionId << " -> " << rewards.size()
							<< " reward(s) queued (" << rewardIt->second << ")";
					}
				}
				else if (!labReplay && !frontierBattle)
				{
					// REPEAT-CLEAR REWARDS (Balance Forge, mission_mst S2ChOKo6;
					// forge/RepeatClear.hpp).  A won clear that is not the first
					// pays the list as presents; a "daily:N" list pays the first N
					// repeat clears of a game day (08:00 UTC), counted in the
					// exchange ledger like the featured events' daily claims.  The
					// Research Lab pays once by design and a Frontier Gate run pays
					// its own rewards, so neither takes one.  The reply's own
					// S2ChOKo6 stays empty: its format is unverified and the result
					// screen reads it; the present box badge tells the player.
					if (const auto* repeat = forge::repeatRewards(static_cast<int32_t>(missionId)))
					{
						bool pay = true;
						if (repeat->dailyLimit > 0)
						{
							// ⚠ Placeholders in order of first use ($1, $2, $3).
							const auto counted = co_await transaction->execSqlCoro(
								"INSERT INTO user_exchange_purchases(user_id,shop,offer_id,count)"
								" VALUES($1,'repeat-clear',$2,1)"
								" ON CONFLICT(user_id,shop,offer_id) DO UPDATE SET count = count + 1 WHERE count < $3"
								" RETURNING count;",
								identity.userId,
								std::to_string(missionId) + "@d" + std::to_string(gme::gameDayAt(clearedAt)),
								repeat->dailyLimit);
							pay = !counted.empty();
						}
						if (pay)
						{
							for (const auto& r : repeat->rewards)
								co_await gme::addUserPresent(
									transaction, identity, r.type, r.id, r.amount,
									0, "Repeat clear reward", r.extraSkill);
							LOG_INFO << "MissionEnd: repeat clear of " << missionId << " -> "
								<< repeat->rewards.size() << " reward(s) queued";
						}
						else
						{
							LOG_INFO << "MissionEnd: repeat clear of " << missionId
								<< " pays nothing: today's " << repeat->dailyLimit << " are paid";
						}
					}
				}
			}

			// Hand the run's state back (the client's only source for it), and
			// when the battle was lost, end the run and pay it — before the
			// team info below is read, so the header and the present badge
			// already include what it paid.  The best is read first so the
			// result scene measures the run against the previous best.
			if (frontierRun)
			{
				const auto best = co_await gme::frontierBestScore(transaction, identity, frontierRun->gateId);
				resp.frontier_battle = std::vector<::FrontierBattleInfo>{ gme::frontierBattleInfo(*frontierRun, best) };
				if (frontierLost)
				{
					auto result = co_await gme::finishFrontierRun(transaction, identity, *frontierRun);
					resp.frontier_end = std::vector<::FrontierEndInfo>{ std::move(result.end) };
					resp.frontier_rewards = std::move(result.rewards);
				}
			}
			else if (frontierLost)
			{
				// No run to pay (already ended, or never opened here) — still
				// send a result, or the client never leaves the battle-end scene.
				::FrontierEndInfo end{};
				end.grade_id = gme::frontierGrade(0);
				end.bonus_rate = 1.0f;
				resp.frontier_end = std::vector<::FrontierEndInfo>{ std::move(end) };
				resp.frontier_rewards = std::vector<::FrontierResRewardInfo>{};
			}

			auto loginInfo = std::move((co_await gme::getLoginInfo(transaction, identity)).nonEmpty());
			auto teamInfo = std::move((co_await gme::getTeamInfo(transaction, identity)).nonEmpty());

			auto droppedUnits = parseUnitDrops(req.battle_result.unit_rewards);

			// We need to track which units are already seen by the player.
			std::vector<bool> newFlags;
			newFlags.reserve(droppedUnits.size());
			for (auto& dropped : droppedUnits)
			{
				// Read from unit dictionary to see if the player has already seen this unit.
				newFlags.push_back((co_await db::PacketInterfaceFor<UserUnitDictionary>::read(
					transaction,
					"user_unit_dictionary",
					{
						db::Lookup("user_id", identity.userId),
						db::Lookup("unit_id", dropped.unit_id),
					})).affected == 0);

				// Add the unit and persist it to the dictionary.
				dropped = std::move(
					(co_await gme::addUserUnit(
						transaction,
						identity,
						dropped)).nonEmpty());
				// A caught copy trades in for less Merit than a summoned one
				// (BossDropPolicy::drop_merit_percent, ticket #82).
				co_await transaction->execSqlCoro(
					"UPDATE user_units SET source = 'drop' WHERE user_id = $1 AND user_unit_id = $2;",
					identity.userId, static_cast<int64_t>(dropped.user_unit_id));
			}

			// The COMPLETE dictionary, never just the dropped species: this
			// response rebuilds the reference list the summon lineup reads
			// (UserUnitDictionary in net/user.kdl).  Only when a unit dropped,
			// so a clear without drops sends exactly what it always did.
			if (!droppedUnits.empty())
				resp.unit_dictionary = co_await gme::loadUnitDictionary(transaction, identity);

			// AUTO-SELL (ticket #27, AutoSell.hpp): the clutter it takes
			// is sold now and leaves the unit list and the result rows below.
			if (!droppedUnits.empty() && co_await gme::autosell::isOn(transaction, identity.userId)
				&& co_await gme::autosell::sellDrops(transaction, identity, droppedUnits, newFlags) > 0)
			{
				const auto zel = co_await transaction->execSqlCoro(
					"SELECT zel FROM user_info WHERE id = $1;", identity.userId);
				teamInfo.zel = zel[0]["zel"].as<uint64_t>();
			}

			// Credit item/sphere drops reported by the client to the player's
			// warehouse.  Spheres travel the same path as any other item — they
			// just carry stat params in ItemMst.  The client already renders
			// the obtained-item cards from its own battle-result state, so we
			// only need to persist ownership here.
			for (const auto& [itemId, qty] : parseItemDrops(req.battle_result.item_rewards))
			{
				const auto stack = co_await transaction->execSqlCoro(
					"SELECT item_num FROM user_items WHERE user_id = $1 AND item_id = $2;",
					identity.userId, itemId);
				const auto owned = stack.empty() ? int64_t{0} : stack[0]["item_num"].as<int64_t>();
				if (owned < 0 || owned > INT32_MAX - static_cast<int64_t>(qty))
					throw std::runtime_error("Mission item stack exceeds wire range");
				(co_await gme::addUserItem(transaction, identity, itemId, qty));
			}

			// Items used in the battle leave the OWNED bar, win or lose -- the
			// client has already spent them from its copy (see
			// gme/common/BattleItems.hpp).  A battle that ran on a supplied
			// list (challenge item sets) spent nothing the player owns.
			if (!usedItems.empty() && gme::missionUsesOwnedLoadout(
					static_cast<int32_t>(missionId)))
			{
				co_await gme::consumeLoadoutItems(transaction, identity, usedItems);
			}
			// The bar as it now stands, every battle: the client refills its bar
			// from this (see gme::loadEquipBar), so it can never offer an item the
			// server has already spent.
			resp.equip_info = co_await gme::loadEquipBar(transaction, identity);

			// UserWarehouseInfoResponse @0x14060E0 clears its list. Send every
			// positive stack, preserving instance IDs and unrelated inventory.
			{
				auto warehouse = co_await gme::loadWarehouseSnapshot(transaction, identity);
				resp.warehouse_info = std::move(warehouse.warehouse);
				resp.item_favorite = std::move(warehouse.favorites);
				resp.item_dictionary_info = std::move(warehouse.dictionary);
			}

			resp.reward_info.zel = rewardZel + firstClearZel;
			resp.reward_info.karma = rewardKarma + firstClearKarma;
			resp.reward_info.before_level = currentLevel;
			resp.reward_info.lvlup_flag = leveledUp ? 1 : 0;
			resp.reward_info.inc_exp = rewardExp;
			resp.reward_info.reward_units = encodeUnitDrops(droppedUnits, newFlags);
			resp.reward_info.clear_bonus = firstClearBonus;
			// Tell the client the mission is cleared NOW, rather than leaving
			// it to whenever UserInfo next runs.  Until this was here a
			// freshly beaten mission kept its uncleared marker on the map.
			// PermitPlace below separately refreshes what that clear unlocks.
			//
			// Merges rather than replaces: readParam @0x13FD758 addObject()s
			// without removeAllObjects, so this cannot drop clears the client
			// already knows about.
			// MUST use `transaction`, not theDb(): the SQLite pool is a single
			// connection, so querying theDb() while this transaction holds it
			// deadlocks the request.  Introduced as theDb() in dee3a1f and hung
			// every MissionEnd from then until 2026-08-24 (handbook 6.14).
			resp.clear_mission_info = co_await gme::getClearedMissions(transaction, identity);
			std::set<int32_t> cleared;
			for (const auto& done : resp.clear_mission_info) cleared.insert(done.mission_id);
			// PermitRecipeResponse @0x13E9BB8 replaces the list. Rebuild all
			// unlocked recipes, including ones gated by this newly cleared mission.
			resp.permit_receipes = co_await gme::Town::permittedRecipes(transaction, identity, cleared);
			// THE TOWN TILES, against the same set.  Missions 11 and 12 open
			// the Farm and the Mountain, and the client gates them itself on
			// the UT1SVg59 above -- but a tile locked at the last UserInfo was
			// sent with no taps, and nothing else re-sent this list, so a Farm
			// opened mid-session passed its gate and stayed empty and inert
			// until the next login (reported 2026-10-02).  locationState rolls
			// a newly opened tile's first period, and any tile whose 3 hours
			// are up.  Taps are never lost to it: the client flushes them in
			// one TownUpdate on leaving town.  See town_location_detail in
			// net/handlers.kdl.
			{
				std::vector<::UserTownLocationInfo> unusedInfo;
				std::vector<::UserTownLocationDetail> tiles;
				co_await gme::Town::locationState(transaction, identity, cleared, unusedInfo, tiles);
				resp.town_location_detail = std::move(tiles);
			}

			// WHAT THIS RESULT NEWLY CLEARED -- and nothing on a repeat.
			//
			// MissionResultFriendRequestScene::changeNextScene @0x18C301C routes
			// the player out of the result screens on these three ids, and
			// trusts them completely:
			//
			//   mauD5qZ1 clear_mission_id -- "12" first offers the Randall
			//     presentation; then, if that mission has an END script
			//     (GameUtils::existMissionEvent(id, "1")), it is PLAYED, with no
			//     once-only check of its own.  Mission 10's end script is the
			//     Farm announcement, and it ends on script op 41 = 0: return
			//     point 0, which MissionEventScene::updateEvent maps to Home.
			//   4sQ8vBXm clear_dungeon_id -- the same for the DUNGEON's end
			//     script (map1-ending.txt on dungeon 80), and
			//     GameUtils::getAppearDungeon @0x1196FE4 animates
			//     DungeonMstList::getNext of it on the dungeon list.
			//   NgPQbA46 clear_area_id -- anything but "0", an EMPTY string
			//     included, resets UserState's last area so the map shows the
			//     newly opened one.
			//
			// With all three neutral a story clear goes to scene 61,
			// DungeonSelectScene2 -- the area's dungeon list, where the next
			// stage is picked (scene ids resolved through getGameScene's table).
			//
			// All three used to be sent on EVERY clear, so each repeat of
			// mission 10 replayed the Farm scene and then went Home (reported
			// 2026-10-02).  The client never resets them between results --
			// MissionRewardInfo::init @0x1268830 leaves them alone and readParam
			// stores the raw string -- so "nothing" must be sent explicitly, in
			// the client's own sentinels: "0" for the mission (row 0 has no
			// script) and the area, "" for the dungeon (what
			// GameUtils::deleteAppearDungeon @0x1197188 writes).
			resp.reward_info.clear_mission_id = 0;
			resp.reward_info.clear_dungeon_id.clear();
			resp.reward_info.clear_area_id = "0";
			if (newlyCleared)
			{
				resp.reward_info.clear_mission_id = missionId;
				// The dungeon only when this clear completed it -- the same
				// fact the clear Gem pays on.  Quest dungeons only, as before:
				// special content keeps its end scripts unplayed.
				if (completedDungeon > 0)
					resp.reward_info.clear_dungeon_id = std::to_string(completedDungeon);
				// The area only when this clear OPENED another story area.  Not
				// "every dungeon done": Mistral's EX dungeon opens at mission 365,
				// long after Morgan, and the area presentation is for the
				// opening (85 -> Morgan, 265 -> St. Lamia, ...).
				const auto& cache = theServer()->cache();
				const auto dungeonIt = cache.missionQuestDungeon().find(static_cast<int32_t>(missionId));
				if (dungeonIt != cache.missionQuestDungeon().end()
					&& gme::storyAreaOpenedBy(static_cast<int32_t>(missionId), cleared))
				{
					const auto& dungeons = cache.dungeonMst();
					const auto row = std::find_if(dungeons.begin(), dungeons.end(),
						[&dungeonIt](const ::DungeonMst& d) { return d.dungeon_id == dungeonIt->second; });
					if (row != dungeons.end() && row->area_id > 0)
						resp.reward_info.clear_area_id = std::to_string(row->area_id);
				}
				LOG_INFO << "MissionEnd: first clear of " << missionId
					<< " (dungeon cleared: " << (resp.reward_info.clear_dungeon_id.empty() ? "-" : resp.reward_info.clear_dungeon_id)
					<< ", area opened by: " << resp.reward_info.clear_area_id << ")";
			}


			// THE SUMMONER (ticket #132).  A Chapter 3 quest pays its
			// mission_ep3_mst row; any other cleared quest pays a much smaller
			// share of the player EXP it earned (gme::regularQuestGain).  Levels, Summoning
			// Arts and level-up SP follow in gme::creditSummoner, and the
			// achievements read user_summoner, so the badge below already
			// counts them.  A loss or a lab replay pays nothing, as for EXP.
			if (payClear)
			{
				const auto ep3Gain = gme::ep3QuestGain(static_cast<int32_t>(missionId), newlyCleared);
				auto gain = ep3Gain ? *ep3Gain : gme::regularQuestGain(
					gme::lifetimePlayerExp(currentLevel, currentExp),
					gme::lifetimePlayerExp(newLevel, newExp));
				if (chapter3Quest)
					gain.stPoints = honorEarned;
				if (!gain.empty())
				{
					const auto credit = co_await gme::creditSummoner(transaction, identity, gain);
					resp.summoner_info = std::vector<::UserSummonerInfo>{
						co_await gme::loadSummonerInfo(transaction, identity) };
					// The result screens only exist for Chapter 3, and they look
					// arm levels up in F_SU_ARM_LEVEL_MST, which this server never
					// sends -- so the block goes out for a Chapter 3 quest only.
					if (ep3Gain)
					{
						const auto pair = [](const int64_t earned, const int64_t bonus) {
							return std::to_string(earned) + "," + std::to_string(bonus);
						};
						resp.ep3_reward = std::vector<::MissionEp3Reward>{ ::MissionEp3Reward{
							.lvup_flg = credit.afterLevel > credit.beforeLevel ? 1 : 0,
							.before_lv = credit.beforeLevel,
							.exp = pair(gain.exp, gain.expClearBonus),
							.elem_lvup_flg = credit.afterElemLevel > credit.beforeElemLevel ? 1 : 0,
							.before_elem_lv = credit.beforeElemLevel,
							.elem_exp = pair(gain.elemExp, 0),
							.arm_lvup_flg = 0,
							.before_arm_lv = credit.armLevel,
							.arm_exp = pair(credit.armCredited ? gain.armExp : 0, 0),
							.sp = std::to_string(gain.sp + credit.levelUpSp),
							.exp_guild_summoner = 0,
							.boost_item_id = {},
						} };
					}
				}
			}

			// Fold this battle's statistics into the lifetime trophy archive.
			// The client already reports all eight in rXvA1E5y with the same
			// keys UserTeamArchive stores them under; before this they were
			// parsed and dropped, which is why every battle trophy read 0.
			co_await gme::accumulateBattleArchive(transaction, identity, req.battle_result);

			// DAILY CHALLENGES.  The same battle log, read for the counts the
			// trophies do not keep (overkills, criticals, enemies by element,
			// ...).  Won or lost, the battle happened; only a clear is a
			// "Quest Cleared".  Past the replay guard above, so once per battle.
			co_await gme::advanceDailyChallenges(transaction, identity,
				gme::challenge::progressFromMissionEnd(json, !missionLost));

			// Lifetime totals the handler observes rather than receives.
			// rewardZel/rewardKarma are the server-computed grant, not the
			// client's claim, so these track what we actually paid out.
			co_await gme::bumpArchiveCounters(transaction, identity, {
				{ "zel_get",         static_cast<int64_t>(rewardZel) + firstClearZel },
				{ "karma_get",       static_cast<int64_t>(rewardKarma) + firstClearKarma },
				// Losses in either normal quests or Frontier Gate are not wins.
				{ "quest_clear_cnt", missionLost ? 0 : 1 },
				// TROPHY 200030 (total battle wins).  A cleared mission is a
				// won battle.
				{ "quest_win_cnt",   missionLost ? 0 : 1 },
				// TROPHY 100090 (Honor earned).  Its counterpart 100100 is
				// bumped where Honor is spent, in Gacha.
				{ "friend_p_get",    static_cast<int64_t>(honorToBalance) },
			});

			// THE BATTLE IS OVER, won or lost, so it is no longer resumable.
			// Inside the reward transaction: a rolled-back MissionEnd has to
			// leave the resume offer standing, or a failed result would lose
			// the run outright.
			co_await gme::clearMissionBreak(transaction, identity);

			resp.login_info = std::move(loginInfo);
			// Never echo the cutscene markers here: the client re-parses them
			// from any reply and this one would roll back events it has just
			// played (see omitClientScenarioMarkers).
			gme::omitClientScenarioMarkers(resp.login_info);
			resp.team_info = std::move(teamInfo);
			// A level-up raises max energy, and with it the header's
			// energy-refill threshold (see UserInfo).
			resp.energy_recover.action_point_threshold = resp.team_info.max_action_point;
			resp.unit_info = std::move(droppedUnits);

			// THE BADGES, after every count above (the challenges, the trophy
			// archive, quest_clear_cnt).  A battle that finishes a daily
			// Challenge lights Missions > Challenges only through this: the
			// client asks for BadgeInfo at login and Home/town entry only.
			resp.badge = co_await gme::achievementBadge(transaction, identity);

			// Build and serialize before committing rewards. Any failure rolls
			// back the clear too; all reads use the held SQLite transaction.
			const auto permit = co_await gme::buildPermitPlace(transaction, identity);
			if (const auto error = glz::write_json(resp, buffer); error)
			    throw std::runtime_error(glz::format_error(error, buffer));
			gme::injectPermitPlace(buffer, permit);
			// The used booster leaves the bar on screen at once (BattleItems.hpp).
			if (expBooster)
				gme::appendBonusBar(buffer, co_await gme::loadBonusBar(transaction, identity));

			// FRONTIER HUNTER (ticket #8): a quest of the running event
			// records its score, Frontier Service and point rewards, and the reply
			// carries the result screen's blocks (gme/common/FrontierHunter.hpp).
			// Past the once-per-battle guard and in this transaction, so a
			// replayed result scores nothing and a failed one scores nothing.
			// Only a battle MissionStart issued a serial for: that start is
			// the one that charged the Hunter Orb (a mission-id serial is a
			// pre-serial save's one legacy settlement, never a scored run).
			if (issuedSerial)
				gme::frohun::appendMembers(buffer, co_await gme::frohun::settleRun(
					transaction, identity, json, static_cast<int32_t>(missionId), !missionLost,
					static_cast<int64_t>(rewardZel), static_cast<int64_t>(rewardKarma)));

		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	// THE DIFFICULTY LOG (forge/ClearLog.hpp): one line for this settled run,
	// after it committed (a replayed result returned above and logs nothing).
	forge::clears::ended(identity.userId, static_cast<int32_t>(missionId), wireSerial,
		req.mission_num.mission_status.has_value() ? static_cast<int32_t>(*req.mission_num.mission_status) : -1,
		clearTurnCount(req.battle_result.clear_turns));

	// THE LENDER'S HONOR, after the result above has committed and in a
	// transaction of its own: nothing that goes wrong on the lender's side may
	// roll back the borrower's clear or turn this reply into an error.
	// creditHelperLender decides whether anything is owed (a real account,
	// once per borrower a day, the daily cap).
	if (!lenderToCredit.empty())
	{
		try
		{
			co_await gme::creditHelperLender(identity.userId, lenderToCredit, lenderIsFriend);
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			LOG_WARN << "MissionEnd: lender Honor for " << lenderToCredit << " failed: " << ex.base().what();
		}
		catch (const std::exception& ex)
		{
			LOG_WARN << "MissionEnd: lender Honor for " << lenderToCredit << " failed: " << ex.what();
		}
	}

	co_return HandleResult::success(buffer);
}

// A FIXED-PARTY QUEST FIGHTS WITH ITS OWN PARTY, NOT THE ACTIVE SQUAD.  MissionMst
// 1e6jxQzf (npc_encounter_id here; really the fixed party, "unit:member,...") holds a
// ':' for the 69 quests whose party MissionCheckScene::missionFixPtSet builds from that
// list (isMissionFixPt @0x187a860).  Squad checks against the active squad do not
// apply to them.
static bool usesFixedParty(const uint32_t missionId)
{
	static const auto fixed = []
	{
		std::set<int32_t> missions;
		for (const auto& mission : theServer()->cache().missionMst())
			if (mission.npc_encounter_id.find(':') != std::string::npos)
				missions.insert(mission.id);
		return missions;
	}();
	return fixed.count(static_cast<int32_t>(missionId)) != 0;
}

// ONCE A DAY (ticket #100).  A daily dungeon can be won once per game
// day (08:00 UTC), as in the original game: every DungeonMst of type 100 (the
// "[Daily]" dungeons, Gortius Daily, Treasure Paradise, Defend the Gates, ...)
// and every featured event's "daily" payout mission, which covers Daily Tan,
// whose dungeon carries no type.  Losing or retiring does not use the day up.
static bool isDailyMission(const uint32_t missionId)
{
	static const auto daily = []
	{
		std::set<int32_t> missions;
		const auto& cache = theServer()->cache();
		for (const auto& dungeon : cache.dungeonMst())
		{
			if (dungeon.dungeon_type != 100)
				continue;
			const auto it = cache.missionsByDungeon().find(dungeon.dungeon_id);
			if (it != cache.missionsByDungeon().end())
				missions.insert(it->second.begin(), it->second.end());
		}
		return missions;
	}();
	return daily.contains(static_cast<int32_t>(missionId)) || gme::isDailyPayoutMission(missionId);
}

HANDLEF(MissionStart)
{
	// Before the first co_await: the session carries the CURRENT request's tag.
	const auto startTag = requestTag(session);

	MissionStartReq req = {};
	const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json);
	if (ec)
	{
		const auto error = glz::format_error(ec, json);
		LOG_ERROR << "MissionStartReq deserialization failed:\n" << error;
		co_return HandleResult::error("Deserialization error", error);
	}

	// Enforce the same prerequisites as the map (all-of, gme::storyMissionUnlocked)
	// before spending energy or replacing an in-flight battle. A stale client
	// tile cannot bypass them.
	{
		const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
		std::set<int32_t> cleared;
		for (const auto& done : co_await gme::getClearedMissions(theDb(), identity))
			cleared.insert(done.mission_id);
		// Every unlock gate below (story, rules.json) opens for a Sandbox
		// account (gme/common/Sandbox.hpp), as the map already showed it.
		if (co_await gme::sandbox::isOn(theDb(), identity.userId))
			cleared = gme::sandbox::everyMission();
		// A STALE TILE, NOT A CHEAT: the client drew this quest from a permit
		// list it has not refreshed (#30), so the player gets the reason and
		// Home instead of the app exiting.  Nothing has been spent yet.
		if (!gme::storyMissionUnlocked(req.start_info.mission_id, cleared))
			co_return HandleResult::refuseToHome(
				"This quest is still locked. Clear the quests it requires first.",
				"Mission locked: prerequisites not cleared");

		// Same soft refusal for a daily quest already won today: nothing is
		// spent, and the player is told when it opens again.
		if (isDailyMission(req.start_info.mission_id))
		{
			const auto last = co_await theDb()->execSqlCoro(
				"SELECT last_cleared_at FROM user_campaign_missions"
				" WHERE user_id = $1 AND mission_id = $2 AND state = 2;",
				identity.userId, std::to_string(req.start_info.mission_id));
			const auto now = gme::unixNow();
			if (!last.empty() && gme::gameDayAt(last[0]["last_cleared_at"].as<int64_t>()) == gme::gameDayAt(now))
				co_return HandleResult::refuseToHome(
					"You have already cleared this daily quest today. It opens again at 08:00 UTC.",
					"Daily mission already cleared today");
		}

		// AN OWN-UNIT HELPER ALREADY FIGHTING IN THE SQUAD (ticket #98): the
		// helper card MYUNIT<user_unit_id> is a copy of one of the player's units, so
		// picking a unit that is also in the squad being sent would field it twice
		// (the strongest unit, doubled).  The squad is the one the client names
		// (Z0Y4RoD7, 0-9 = user_decks.deck_num: MissionStartRequest::createBody sends
		// UserTeamInfo::getActiveDeckNum, which MissionCheckScene keeps on the squad
		// shown).  Refused like a stale tile: the message, then Home, before any
		// energy is spent or battle opened.  Not for a fixed-party quest, whose party
		// is not that squad (players were refused there: usesFixedParty).
		// A TRIAL IS NOT FOUGHT WITH THE SQUAD Z0Y4RoD7 NAMES EITHER: the client
		// writes its normal squad there for every quest (createBody @0x13AAEC0), and a
		// trial fights one of its trial squads (user_trial_decks, by dungeon type:
		// forge::trialDeckRange).  So a trial checks the whole group: the server
		// cannot tell which of them fights.
		const auto ownUnit = gme::ownHelperUnitId(req.start_info.reinforce_user_id);
		if (ownUnit && !usesFixedParty(req.start_info.mission_id))
		{
			const auto trialDecks = forge::trialDeckRange(static_cast<int32_t>(req.start_info.mission_id));
			const auto inSquad = trialDecks
				? co_await theDb()->execSqlCoro(
					"SELECT 1 FROM user_trial_decks WHERE user_id = $1 AND deck_num BETWEEN $2 AND $3"
					" AND user_unit_id = $4 LIMIT 1;",
					identity.userId, trialDecks->first, trialDecks->second, *ownUnit)
				: co_await theDb()->execSqlCoro(
					"SELECT 1 FROM user_decks WHERE user_id = $1 AND deck_type = 1 AND deck_num = $2"
					" AND user_unit_id = $3 LIMIT 1;",
					identity.userId, req.start_info.deck_num, *ownUnit);
			if (!inSquad.empty())
			{
				LOG_WARN << "MissionStart: " << identity.userId << " refused: own helper "
					<< req.start_info.reinforce_user_id << " is in squad " << req.start_info.deck_num
					<< " (mission " << req.start_info.mission_id << ")";
				co_return HandleResult::refuseToHome(
					"That helper is already in the squad you are fighting with. "
					"Pick another helper, or change your squad.",
					"Own helper " + req.start_info.reinforce_user_id + " is in squad "
						+ (trialDecks ? "of trial group " + std::to_string(trialDecks->first)
							: std::to_string(req.start_info.deck_num)) + "; start refused");
			}
		}

		// BALANCE FORGE RULES (rules.json; forge/Rules.hpp): schedule windows and
		// unlock rules, then squad rules (not on a fixed-party quest, whose party
		// is not the player's squad).  Same soft refusal as above: the message,
		// then Home, nothing spent.
		const auto forgeRules = forge::rules();
		if (!forgeRules->empty())
		{
			const auto missionId = static_cast<int32_t>(req.start_info.mission_id);
			if (const auto refused = forgeRules->questRefusal(missionId, cleared, gme::unixNow()))
				co_return HandleResult::refuseToHome(refused->message,
					"forge rule " + refused->rule + ": quest " + std::to_string(missionId) + " refused");
			if (!usesFixedParty(req.start_info.mission_id))
				if (const auto refused = co_await forge::checkSquad(forgeRules, identity, missionId,
						static_cast<int32_t>(req.start_info.deck_num), req.start_info.reinforce_user_id))
					co_return HandleResult::refuseToHome(refused->message,
						"forge rule " + refused->rule + ": squad refused for quest " + std::to_string(missionId));
		}
	}

	// A FRONTIER HUNTER QUEST COSTS A HUNTER ORB, and the client never takes it
	// (see gme/common/HunterOrbs.hpp).  Checked here, before anything is
	// spent or recorded, so a start with no orbs changes nothing.  The client
	// checks its own count first (ChallengeConditionScene::startCheck), so this
	// only fires when the two disagree.  refuseToRetry is a soft refusal on
	// this scene: MissionStartScene::noticeOK @0x1825E98 handles -4000 by
	// returning a Frontier Hunter start to the Survey Office (scene 1100),
	// whose refresh shows the real count and offers the 1-Gem restore.  A
	// resend of a start already paid for is let through (it is not charged).
	const bool frontierHunter = gme::isFrontierHunterMission(req.start_info.mission_id);
	constexpr const char* kNoHunterOrbs =
		"You have no Hunter Orbs left. They recover over time, or you can restore them at the Survey Office.";
	if (frontierHunter)
	{
		const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
		const auto orbs = co_await gme::loadHunterOrbRefresh(theDb(), identity);
		if (orbs.aube <= 0)
		{
			const auto alreadyPaid = co_await gme::isHunterOrbResend(
				theDb(), identity, req.start_info.mission_id, startTag);
			if (!alreadyPaid)
				co_return HandleResult::refuseToRetry(kNoHunterOrbs, "Frontier Hunter start with no Hunter Orbs");
		}
	}

	// BATTLE-CONTENT FALLBACK.
	//
	// deploy/archive/mission.json supplies authored waves, monsters, and AIs.
	// Only a mission missing from that archive borrows mission 10's content;
	// an existing but malformed record must fail validation below.
	//
	// CRITICAL: only the CONTENT comes from the template.  resp.start_info is
	// echoed from the request below, so the response still names the mission the
	// client asked for.  Handbook §3.1 is the story of what happens otherwise:
	// answering mission 11 with "mission 10" crashed the client outright.
	//
	// Replace this with real authored content per mission — that is what the
	// mission editor exists for.  When the archive covers a mission, nothing
	// here runs.
	constexpr MissionArchiver::MissionId kTemplateMissionId = 10;

	auto missionRecord = MissionArchiver::instance().lookup(req.start_info.mission_id);
	const bool usingTemplate = !missionRecord;
	if (usingTemplate)
	{
		missionRecord = MissionArchiver::instance().lookup(kTemplateMissionId);
		if (!missionRecord)
		{
			// The template itself is missing, so the archive is unusable.
			co_return HandleResult::error("Archive error", "Unable to find mission record");
		}
		// RELABEL THE TEMPLATE AS THE REQUESTED MISSION.
		//
		// populatePacket stamps record.id into BattleGroupMst::mission_id and
		// MissionNumInfo::serial_id.  Left alone, the response would carry
		// start_info for mission 9010001 while every battle group and the
		// mission_num claimed mission 10 — the client resolves the mission it
		// asked for, finds battle data belonging to a different one, and dies
		// during load.  That is handbook §3.1's mismatch crash exactly, and it
		// is what echoing start_info alone did NOT fix (verified 2026-08-07: FG
		// downloaded its assets and then crashed with a clean server log).
		//
		// Safe because the archive contract is explicit that the SERVER owns
		// these ids: "Client battle group ids and encoded AI/drop wire strings
		// are generated while formatting a mission-start response"
		// (MissionArchiver.hpp).  The client does not require the group ids its
		// own MST lists — only that the response is internally consistent about
		// which mission it describes.
		missionRecord->id = req.start_info.mission_id;
		// Chest policies belong to authored content, not fallback missions.
		missionRecord->mimic_chests.reset();
		missionRecord->random_mimics.reset();
		MissionArchiver::instance().applyCaptureFloor(*missionRecord, forge::tuning().captureFloorPct);

		LOG_WARN << "MissionStart: mission " << req.start_info.mission_id
		         << " has no authored battle content; serving mission "
		         << kTemplateMissionId << "'s waves relabelled as this mission";
	}
	else
	{
		// This run's enemies, for slots that roll (Metal Parades, Idol and Totem
		// fights: MissionArchiver::rollVariants).  Everything below is filled from
		// the rolled record, and MissionEnd pays what the client reports of it.
		MissionArchiver::instance().rollVariants(*missionRecord);
		// The capture floor (tuning.json capture_floor_pct, #164), before the bosses' drops:
		// a boss the floor makes capturable keeps that capture of itself.
		MissionArchiver::instance().applyCaptureFloor(*missionRecord, forge::tuning().captureFloorPct);
		// Then the bosses' base-form capture (ticket #82), so it
		// follows the rolled enemy.  Not in Frontier Gate runs.
		if (!req.frontier_run || req.frontier_run->empty())
			MissionArchiver::instance().applyBossDrops(*missionRecord);
	}
	// Last, the Forge's capture rules (forge/Capture.hpp, ticket #155):
	// which units can be captured here and how often.  No capture.json, no change.
	forge::applyCaptureRules(*missionRecord, *forge::captureRules(),
		[](uint32_t unit) { return UnitArchiver::instance().contains(unit); },
		MissionArchiver::instance().handBuilt(req.start_info.mission_id));

	MissionStartResp resp{};
	resp.signal_key = req.signal_key;
	resp.start_info = req.start_info;

	// A Frontier Gate battle starts from the run's floors and score, which
	// the client holds only when the server sends them (see
	// gme/common/FrontierGate.hpp): zeros on a fresh run, the last
	// MissionEnd's report after that.
	if (req.frontier_run && !req.frontier_run->empty())
	{
		const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
		if (const auto run = co_await gme::loadFrontierRun(theDb(), identity))
		{
			const auto best = co_await gme::frontierBestScore(theDb(), identity, run->gateId);
			resp.frontier_battle = std::vector<::FrontierBattleInfo>{ gme::frontierBattleInfo(*run, best) };
		}
		else
		{
			LOG_WARN << "MissionStart: Frontier Gate battle with no open run for " << identity.userId;
		}
	}

	if (!MissionArchiver::populatePacket(*missionRecord, resp.mission_num)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.ais)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.monsters)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.monster_cgs)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.battle_skills)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.battle_monster_groups)
		|| !MissionArchiver::populatePacket(*missionRecord, resp.battle_groups))
	{
		co_return HandleResult::error("Archive error", "Unable to populate mission start response");
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// THIS BATTLE'S OWN SERIAL, in place of the mission id populatePacket put
	// there (see gme/common/MissionRuns.hpp): the client echoes it in MissionEnd
	// and MissionContinue and seeds the battle from it.  Issued before the
	// preflight below so the reply carries it; it only becomes the OPEN battle
	// once the start has succeeded (open_mission_serial, further down).
	const auto runSerial = co_await gme::issueMissionSerial(
		theDb(), identity, static_cast<uint32_t>(req.start_info.mission_id));
	resp.mission_num.serial_id = runSerial;

	std::string buffer;
	const auto& writeError = glz::write_json(resp, buffer);
	if (writeError)
	{
		const auto& glze = glz::format_error(writeError, buffer);
		LOG_ERROR << "MissionStart response serialization failed: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}

	// Preflight the complete response before spending: malformed archive data
	// or a serialization failure must not consume energy or alter its timer.
	// Energy is only consumed for a mission we actually have data for.  The
	// template's cost belongs to mission 10, not to whatever was requested, and
	// charging a made-up amount is the §3.4 anti-pattern; the real per-mission
	// cost lives in the archive (handbook §6.15 rule 3), which by definition
	// does not have this one.  Frontier Hunter and Frontier Gate cost Hunter
	// Orbs instead, and both are charged by the server: Frontier Hunter just
	// below, Frontier Gate when its run opens (FrontierGateStart).
	//
	// THE HUNTER ORB, charged against this start's own serial so that a resent
	// copy of the same request is recognised and not charged twice.  Losing a
	// race to the last orb leaves an unused serial, which can never settle
	// (MissionRuns.hpp), and nothing else.
	std::optional<::HunterOrbRefresh> hunterOrbs;
	if (frontierHunter)
	{
		hunterOrbs = co_await gme::chargeHunterOrbForStart(
			identity, req.start_info.mission_id, runSerial, startTag);
		if (!hunterOrbs)
			co_return HandleResult::refuseToRetry(kNoHunterOrbs, "Frontier Hunter start with no Hunter Orbs");
	}

	if (!usingTemplate && missionRecord->energy_cost > 0)
	{
		(co_await gme::UserEnergy::consume(
			theDb(),
			identity,
			missionRecord->energy_cost)).nonEmpty();
	}

	// REMEMBER THE BORROWED HELPER so MissionEnd can pay the Honor for it.
	//
	// MissionEnd cannot work it out for itself: MissionEndRequest::createBody
	// @0x13A78F8 sends 75 keys and h7eY3sAK is not one of them.  This request
	// is where the helper is named -- "0" for a solo run, the helper's user id
	// otherwise -- so it is recorded here and consumed (and cleared) there.
	//
	// Written unconditionally and outside the energy branch: a template mission
	// still borrows a helper, an energy-free one still borrows a helper, and a
	// solo start has to CLEAR whatever the last start left, or the next clear
	// would pay for a helper it never took.  A start that fails validation
	// above never reaches this line.
	//
	// ONLY A HELPER THAT EXISTS (recordableHelper): anything else is recorded
	// as a solo run.  Never a refusal -- an error here closes the session, and
	// an unplaceable helper costs the player nothing but that run's Honor.
	// For the log only: bounded, and anything outside an id's characters shown as '?'.
	auto named = req.start_info.reinforce_user_id.substr(0, kMaxHelperIdLength + 1);
	std::replace_if(named.begin(), named.end(),
		[](const unsigned char c) { return std::isalnum(c) == 0; }, '?');
	std::string helper;
	try
	{
		helper = co_await recordableHelper(identity, req.start_info.reinforce_user_id);
		if (helper.empty() && gme::borrowedHelper(named) && !gme::isOwnHelperId(named))
		{
			LOG_INFO << "MissionStart: " << identity.userId << " named helper " << named
				<< ", nobody they could borrow; recorded as solo, no Honor for this run";
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_WARN << "MissionStart: could not check helper " << named << " for "
			<< identity.userId << ", recorded as solo: " << ex.base().what();
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "MissionStart: could not check helper " << named << " for "
			<< identity.userId << ", recorded as solo: " << ex.what();
	}
	co_await theDb()->execSqlCoro(
		"UPDATE user_info SET reinforce_user_id = $1 WHERE id = $2;",
		helper, identity.userId);

	// THE BATTLE THIS START OPENS, so MissionEnd pays its result exactly once
	// (see the ONE RESULT PER BATTLE block there).  A new start supersedes any
	// battle the player walked away from, the same way the break record below
	// does -- its serial can no longer settle.
	co_await theDb()->execSqlCoro(
		"UPDATE user_info SET open_mission_id = $1, open_mission_serial = $2 WHERE id = $3;",
		static_cast<int64_t>(req.start_info.mission_id), static_cast<int64_t>(runSerial),
		identity.userId);

	// A FRESH RUN STARTS WITH A CLEAN CONTINUE RECORD, or a continue spent in an
	// earlier attempt would still be counted against this one and the "Cleared
	// (No Continues)" achievements could never be earned after a single bad run.
	co_await theDb()->execSqlCoro(
		"DELETE FROM user_mission_continues WHERE user_id = $1 AND mission_id = $2;",
		identity.userId, std::to_string(req.start_info.mission_id));

	// A NEW BATTLE SUPERSEDES ANY INTERRUPTED ONE.  The resume path never comes
	// through here -- MissionRestartScene fires MissionRestart and rebuilds from
	// the client's own suspend data -- so a MissionStart means the player walked
	// away from whatever was open.  The statement is a no-op when nothing is.
	co_await gme::clearMissionBreak(theDb(), identity);

	// THE DIFFICULTY LOG (forge/ClearLog.hpp): this run and its squad, kept in
	// memory until MissionEnd settles it or the player's next start abandons
	// it.  Reads only; never fails the start.
	{
		const auto strength = co_await forge::clears::readStrength(identity,
			static_cast<int32_t>(req.start_info.mission_id), static_cast<int32_t>(req.start_info.deck_num),
			req.start_info.reinforce_user_id, helper, usesFixedParty(req.start_info.mission_id));
		forge::clears::started(identity.userId, static_cast<int32_t>(req.start_info.mission_id), runSerial, strength);
	}

	// The count after the charge, so the screens after this battle (a quest
	// repeat checks getAube() before it starts) agree with the server.
	if (hunterOrbs)
		gme::appendHunterOrbs(buffer, *hunterOrbs);

	// The quest's best this week (7B5Ue9kH), so the result screen knows a new
	// record when it sees one (ticket #8).
	if (frontierHunter)
		co_await gme::frohun::appendMissionStart(theDb(), identity.userId, req.start_info.mission_id, buffer);

	co_return HandleResult::success(buffer);
}
