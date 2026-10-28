#include "Arena.hpp"

#include "App.hpp"

#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/gme/common/ExtraSkills.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdlib>
#include <map>

namespace gme::arena
{

namespace
{

constexpr const char* kBotPrefix = "ARENABOT";

constexpr const char* kStandingColumns =
	"rank_id, max_rank_id, points, battle_cnt, win_cnt, battle_cnt_accept,"
	" win_cnt_accept, atk_series_win, max_atk_series_win, def_series_win,"
	" max_def_series_win, defeat_unit_cnt, max_turn_damage, max_turn_spark,"
	" sum_b_crystal, sum_h_crystal, offer_user_id, orb_ts";

/// Counters are read as 64-bit and saturated.  as<int32_t> is std::stoi,
/// which throws past INT_MAX, and loadStanding feeds login, rankings and
/// matching: one bad row would take those screens down for everyone who is
/// shown this player, not just for the player.
int32_t counter(const drogon::orm::Field& field)
{
	return static_cast<int32_t>(std::clamp<int64_t>(field.as<int64_t>(),
		std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

Standing standingFromRow(const drogon::orm::Row& row)
{
	Standing s{};
	s.rankId = counter(row["rank_id"]);
	s.maxRankId = counter(row["max_rank_id"]);
	s.points = counter(row["points"]);
	s.battleCnt = counter(row["battle_cnt"]);
	s.winCnt = counter(row["win_cnt"]);
	s.battleCntAccept = counter(row["battle_cnt_accept"]);
	s.winCntAccept = counter(row["win_cnt_accept"]);
	s.atkSeriesWin = counter(row["atk_series_win"]);
	s.maxAtkSeriesWin = counter(row["max_atk_series_win"]);
	s.defSeriesWin = counter(row["def_series_win"]);
	s.maxDefSeriesWin = counter(row["max_def_series_win"]);
	s.defeatUnitCnt = counter(row["defeat_unit_cnt"]);
	s.maxTurnDamage = counter(row["max_turn_damage"]);
	s.maxTurnSpark = counter(row["max_turn_spark"]);
	s.sumBCrystal = counter(row["sum_b_crystal"]);
	s.sumHCrystal = counter(row["sum_h_crystal"]);
	s.offerUserId = row["offer_user_id"].as<std::string>();
	s.orbTs = row["orb_ts"].as<int64_t>();
	return s;
}

const ::UnitMst* findUnitMst(int32_t unitId)
{
	for (const auto& u : theServer()->cache().unitMst())
	{
		if (u.id == unitId)
			return &u;
	}
	return nullptr;
}

/// UnitMst min..max interpolated over 1..max_lv: the curve scaleUnitBaseStats
/// uses, so a bot's statline matches what the same unit shows in a box.
int32_t statAt(const ::UnitMst& mst, int32_t level, int32_t minV, int32_t maxV)
{
	if (mst.max_lv <= 1 || level <= 1)
		return minV;
	if (level >= mst.max_lv)
		return maxV;
	return minV + static_cast<int32_t>(
		static_cast<double>(maxV - minV) * (level - 1) / (mst.max_lv - 1));
}

std::string orZero(const std::string& value)
{
	return value.empty() ? "0" : value;
}

bool missing(const JsonObject& unit, const char* key)
{
	const auto it = unit.find(key);
	return it == unit.end() || it->second.empty() || it->second == "0";
}

/// npc_mst's second team (Kevin) ships with an empty P_SKILL_ID on every
/// unit, and a unit with no BB never fills a gauge: that bot could never
/// burst (ticket #45).  Missing ids get the unit's own BB and SBB
/// from the unit archive; ids the row does carry are left alone.
void fillMissingSkills(JsonObject& unit, int32_t unitId)
{
	const auto record = UnitArchiver::instance().lookup(static_cast<UnitArchiver::UnitId>(unitId));
	if (!record)
		return;
	if (missing(unit, "P_SKILL_ID") && !record->bb_id.empty())
	{
		unit["P_SKILL_ID"] = record->bb_id;
		if (missing(unit, "P_SKILL_LV"))
			unit["P_SKILL_LV"] = "1";
	}
	if (missing(unit, "P_EXTRA_SKILL_ID") && !record->sbb_id.empty())
	{
		unit["P_EXTRA_SKILL_ID"] = record->sbb_id;
		if (missing(unit, "P_EXTRA_SKILL_LV"))
			unit["P_EXTRA_SKILL_LV"] = "1";
	}
}

JsonObject unitJson(const ::UserUnitInfo& u)
{
	// P_EXT_* as ext + limit-over: both are flat bonuses the owner's unit
	// screen adds on top of base + imps (INFERRED, ARENA_SPEC 3.4).
	return {
		{ "P_USER_UNIT_ID", std::to_string(u.user_unit_id) },
		{ "P_UNIT_ID", std::to_string(u.unit_id) },
		{ "P_UNIT_TYPE_ID", std::to_string(u.unit_type_id) },
		{ "P_LV", std::to_string(u.unit_lvl) },
		{ "P_HP", std::to_string(u.base_hp) },
		{ "P_ATK", std::to_string(u.base_atk) },
		{ "P_DEF", std::to_string(u.base_def) },
		{ "P_HEL", std::to_string(u.base_rec) },
		{ "P_ADD_HP", std::to_string(u.add_hp) },
		{ "P_ADD_ATK", std::to_string(u.add_atk) },
		{ "P_ADD_DEF", std::to_string(u.add_def) },
		{ "P_ADD_HEL", std::to_string(u.add_rec) },
		{ "P_EXT_HP", std::to_string(static_cast<int64_t>(u.ext_hp) + u.limit_over_hp) },
		{ "P_EXT_ATK", std::to_string(static_cast<int64_t>(u.ext_atk) + u.limit_over_atk) },
		{ "P_EXT_DEF", std::to_string(static_cast<int64_t>(u.ext_def) + u.limit_over_def) },
		{ "P_EXT_HEL", std::to_string(static_cast<int64_t>(u.ext_rec) + u.limit_over_rec) },
		{ "P_SKILL_ID", orZero(u.bb_id) },
		{ "P_SKILL_LV", std::to_string(u.bb_lvl) },
		{ "P_EXTRA_SKILL_ID", orZero(u.sbb_id) },
		{ "P_EXTRA_SKILL_LV", std::to_string(u.sbb_lvl) },
		{ "P_EXTRA_PASSIVE_SKILL_ID", std::to_string(u.extra_passive_skill_id) },
		{ "P_EXTRA_PASSIVE_SKILL_ID2", std::to_string(u.extra_passive_skill_id2) },
		{ "P_EQP_ITEM_ID", std::to_string(u.equipitem_id) },
		{ "P_EQP_ITEM_ID2", std::to_string(u.equipitem_id2) },
		{ "P_IMG_TYPE", std::to_string(u.unit_img_type) },
		// The unit's SP picks (ticket #136).  NpcUnitParseList::parse
		// hands P_FE_INFO to UserUnitInfoBase::setFeSkillInfo, the setter
		// UserUnitInfo.fe_skill_info uses, so it is the same string; sent as ""
		// before, Arena and Colosseum squads fought without their SP.
		{ "P_FE_INFO", u.fe_skill_info },
		{ "P_OMNI_LV", std::to_string(u.omni_level) },
	};
}

struct DeckRow
{
	uint32_t userUnitId = 0;
	int32_t memberType = 1;
	int32_t dispOrder = 0;
};

drogon::Task<std::vector<DeckRow>> readDeck(db::Database database, const std::string& userId, int32_t deckNum)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT user_unit_id, member_type, disp_order FROM user_decks"
		" WHERE user_id = $1 AND deck_type = 1 AND deck_num = $2 AND user_unit_id > 0"
		" ORDER BY disp_order;",
		userId, deckNum);

	std::vector<DeckRow> deck;
	for (const auto& row : rows)
	{
		deck.push_back({
			.userUnitId = row["user_unit_id"].as<uint32_t>(),
			.memberType = row["member_type"].as<int32_t>(),
			.dispOrder = row["disp_order"].as<int32_t>(),
		});
	}
	co_return deck;
}

} // namespace

int32_t orbRecoverSeconds()
{
	const auto configured = theServer()->cache().initializeResp().defines.recover_time_fight;
	return configured > 0 ? configured : 3600;
}

int64_t nowSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

int32_t rankForPoints(int32_t points)
{
	const auto& ranks = theServer()->cache().initializeResp().arena_ranks;
	int32_t best = 1;
	for (const auto& rank : ranks)
	{
		if (points >= rank.rank_point_start && rank.id > best)
			best = rank.id;
	}
	return best;
}

int32_t pointsForWin(int32_t myPoints, int32_t enemyPoints)
{
	return std::clamp(100 + (enemyPoints - myPoints) / 50, 50, 300);
}

int32_t karmaForWin(int32_t rankAfter)
{
	return 100 * std::max(rankAfter, 1);
}

drogon::Task<Standing> loadStanding(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		std::string("SELECT ") + kStandingColumns + " FROM user_arena WHERE user_id = $1;",
		userId);
	if (rows.empty())
		co_return Standing{};
	co_return standingFromRow(rows[0]);
}

drogon::Task<void> ensureStanding(db::Database database, const std::string& userId)
{
	co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO user_arena (user_id, updated_at) VALUES ($1, $2);",
		userId, nowSeconds());
}

drogon::Task<void> syncOrbs(db::Database database, const std::string& userId)
{
	co_await ensureStanding(database, userId);
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.fight_point AS fp, i.max_fight_point AS maxfp, a.orb_ts AS ts"
		" FROM user_info i JOIN user_arena a ON a.user_id = i.id WHERE i.id = $1;",
		userId);
	if (rows.empty())
		co_return;

	const auto fp = rows[0]["fp"].as<int32_t>();
	const auto maxFp = rows[0]["maxfp"].as<int32_t>();
	const auto ts = rows[0]["ts"].as<int64_t>();
	const auto now = nowSeconds();
	if (fp >= maxFp && ts != 0)
	{
		// Full by some other route (a gem refill): a stale clock left here
		// would hand out an orb the moment the next one is spent.
		co_await database->execSqlCoro(
			"UPDATE user_arena SET orb_ts = 0 WHERE user_id = $1;", userId);
		co_return;
	}
	if (fp >= maxFp || ts <= 0)
	{
		// Full (or never spent): the clock only runs while below the cap.
		co_return;
	}

	const auto step = orbRecoverSeconds();
	const auto gained = static_cast<int32_t>((now - ts) / step);
	if (gained <= 0)
		co_return;

	const auto newFp = std::min(maxFp, fp + gained);
	const auto newTs = newFp >= maxFp ? 0 : ts + static_cast<int64_t>(gained) * step;
	co_await database->execSqlCoro(
		"UPDATE user_info SET fight_point = $1 WHERE id = $2;", newFp, userId);
	co_await database->execSqlCoro(
		"UPDATE user_arena SET orb_ts = $1 WHERE user_id = $2;", newTs, userId);
}

drogon::Task<int32_t> orbRestSeconds(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.fight_point AS fp, i.max_fight_point AS maxfp, a.orb_ts AS ts"
		" FROM user_info i JOIN user_arena a ON a.user_id = i.id WHERE i.id = $1;",
		userId);
	if (rows.empty())
		co_return 0;
	const auto ts = rows[0]["ts"].as<int64_t>();
	if (rows[0]["fp"].as<int32_t>() >= rows[0]["maxfp"].as<int32_t>() || ts <= 0)
		co_return 0;
	const auto left = ts + orbRecoverSeconds() - nowSeconds();
	co_return static_cast<int32_t>(std::max<int64_t>(left, 0));
}

drogon::Task<std::vector<::UserArenaInfo>> loadArenaInfo(db::Database database, const UserIdentity& identity)
{
	const auto s = co_await loadStanding(database, identity.userId);
	// The place among the player's own pool (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await sandbox::isOn(database, identity.userId);
	const auto ahead = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_arena WHERE points > $1" + sandbox::poolSql("user_id", sandboxed) + ";",
		s.points);

	::UserArenaInfo info{};
	info.user_id = identity.userId;
	// "1", never "" or "0": "0" starts the arena tutorial (isArenaTutoPlaying)
	// and "" names no ArenaRankMst row on the rank screen.
	info.arena_rank_id = std::to_string(s.rankId);
	info.max_arena_rank_id = std::to_string(s.maxRankId);
	info.ranking_point = s.points;
	info.battle_cnt = s.battleCnt;
	info.win_cnt = s.winCnt;
	info.battle_cnt_accept = s.battleCntAccept;
	info.win_cnt_accept = s.winCntAccept;
	info.current_position = ahead[0]["n"].as<int32_t>() + 1;
	co_return std::vector<::UserArenaInfo>{ std::move(info) };
}

drogon::Task<std::vector<::UserTeamArenaArchive>> loadArenaArchive(db::Database database, const UserIdentity& identity)
{
	const auto s = co_await loadStanding(database, identity.userId);

	::UserTeamArenaArchive archive{};
	archive.user_id = identity.userId;
	archive.arena_defeat_unit_cnt = s.defeatUnitCnt;
	archive.atk_series_win_cnt = s.atkSeriesWin;
	archive.max_atk_series_win_cnt = s.maxAtkSeriesWin;
	archive.max_def_series_win_cnt = s.maxDefSeriesWin;
	archive.max_turn_damage = s.maxTurnDamage;
	archive.max_turn_spark = s.maxTurnSpark;
	archive.sum_b_crystal_cnt = s.sumBCrystal;
	archive.sum_h_crystal_cnt = s.sumHCrystal;
	co_return std::vector<::UserTeamArenaArchive>{ std::move(archive) };
}

drogon::Task<std::optional<Snapshot>> playerSnapshot(db::Database database, const std::string& userId)
{
	const auto info = co_await database->execSqlCoro(
		"SELECT username, level, active_deck, active_arena_deck FROM user_info WHERE id = $1;",
		userId);
	if (info.empty())
		co_return std::nullopt;

	auto deck = co_await readDeck(database, userId, info[0]["active_arena_deck"].as<int32_t>());
	if (deck.empty())
		deck = co_await readDeck(database, userId, info[0]["active_deck"].as<int32_t>());
	if (deck.empty())
		co_return std::nullopt;

	std::vector<uint32_t> ids;
	for (const auto& d : deck)
		ids.push_back(d.userUnitId);
	const auto units = co_await readUserUnits(database, UserIdentity{ .gumiUserId = {}, .userId = userId }, ids);
	if (units.empty())
		co_return std::nullopt;

	Snapshot snap{};
	snap.userId = userId;
	snap.name = info[0]["username"].as<std::string>();
	snap.level = info[0]["level"].as<int32_t>();

	std::vector<JsonObject> party;
	std::vector<JsonObject> unitRows;
	for (const auto& d : deck)
	{
		const auto unit = std::find_if(units.begin(), units.end(),
			[&](const ::UserUnitInfo& u) { return u.user_unit_id == d.userUnitId; });
		if (unit == units.end())
			continue;
		party.push_back({
			{ "P_USER_UNIT_ID", std::to_string(d.userUnitId) },
			{ "P_MEMBER_TYPE", std::to_string(d.memberType) },
			{ "P_DISPORDER", std::to_string(d.dispOrder) },
		});
		unitRows.push_back(unitJson(*unit));
		if (d.memberType == 0 || snap.leaderUnitId.empty())
		{
			snap.leaderUnitId = std::to_string(unit->unit_id);
			snap.leaderImgType = unit->unit_img_type;
			snap.leaderLv = static_cast<int32_t>(unit->unit_lvl);
		}
	}

	const auto standing = co_await loadStanding(database, userId);
	snap.rankId = standing.rankId;
	snap.points = standing.points;
	snap.team = glz::write_json(JsonObject{
		{ "P_USER_ID", userId },
		{ "P_FRIEND_MESSAGE", "" },
		{ "P_LV", std::to_string(snap.level) },
		{ "P_GUILD_ID", "" },
	}).value_or("{}");
	snap.party = glz::write_json(party).value_or("[]");
	snap.units = glz::write_json(unitRows).value_or("[]");
	co_return snap;
}

drogon::Task<std::vector<JsonObject>> unitRows(
	db::Database database, const std::string& userId, const std::vector<uint32_t>& userUnitIds)
{
	const auto units = co_await readUserUnits(database, UserIdentity{ .gumiUserId = {}, .userId = userId }, userUnitIds);
	std::vector<JsonObject> rows;
	for (const auto id : userUnitIds)
	{
		const auto unit = std::find_if(units.begin(), units.end(),
			[id](const ::UserUnitInfo& u) { return u.user_unit_id == id; });
		if (unit != units.end())
			rows.push_back(unitJson(*unit));
	}
	co_return rows;
}

std::optional<JsonObject> mstUnitRow(int32_t unitId, int32_t level, const std::string& userUnitId)
{
	const auto* mst = findUnitMst(unitId);
	if (!mst)
		return std::nullopt;
	const auto lv = std::clamp(level, 1, std::max(mst->max_lv, 1));
	JsonObject unit{
		{ "P_USER_UNIT_ID", userUnitId },
		{ "P_UNIT_ID", std::to_string(unitId) },
		// Lord..Oracle: the type's stat bias is part of what makes a squad.
		{ "P_UNIT_TYPE_ID", std::to_string(RandomUInt(1, 5)) },
		{ "P_LV", std::to_string(lv) },
		{ "P_HP", std::to_string(statAt(*mst, lv, mst->min_hp, mst->max_hp)) },
		{ "P_ATK", std::to_string(statAt(*mst, lv, mst->min_atk, mst->max_atk)) },
		{ "P_DEF", std::to_string(statAt(*mst, lv, mst->min_def, mst->max_def)) },
		{ "P_HEL", std::to_string(statAt(*mst, lv, mst->min_rec, mst->max_rec)) },
		{ "P_ADD_HP", "0" }, { "P_ADD_ATK", "0" }, { "P_ADD_DEF", "0" }, { "P_ADD_HEL", "0" },
		{ "P_EXT_HP", "0" }, { "P_EXT_ATK", "0" }, { "P_EXT_DEF", "0" }, { "P_EXT_HEL", "0" },
		{ "P_SKILL_ID", "0" }, { "P_SKILL_LV", "0" },
		{ "P_EXTRA_SKILL_ID", "0" }, { "P_EXTRA_SKILL_LV", "0" },
		// The unit's own Extra Skill (MST data, see ExtraSkills.hpp).
		{ "P_EXTRA_PASSIVE_SKILL_ID", std::to_string(unitOwnExtraSkill(static_cast<uint32_t>(unitId))) },
		{ "P_EXTRA_PASSIVE_SKILL_ID2", "0" },
		{ "P_EQP_ITEM_ID", "0" }, { "P_EQP_ITEM_ID2", "0" },
		{ "P_IMG_TYPE", "0" }, { "P_FE_INFO", "" }, { "P_OMNI_LV", "0" },
	};
	fillMissingSkills(unit, unitId);
	return unit;
}

bool isBotId(const std::string& userId)
{
	return userId.rfind(kBotPrefix, 0) == 0;
}

std::string randomBotId()
{
	const auto& npcs = theServer()->cache().initializeResp().npcs;
	if (npcs.empty())
		return {};
	const auto pick = RandomUInt(0, static_cast<uint32_t>(npcs.size() - 1));
	return kBotPrefix + std::to_string(npcs[pick].id);
}

std::optional<Snapshot> botSnapshot(const std::string& userId, int32_t attackerLevel, int32_t attackerPoints)
{
	if (!isBotId(userId))
		return std::nullopt;

	const auto& npcs = theServer()->cache().initializeResp().npcs;
	const auto npc = std::find_if(npcs.begin(), npcs.end(), [&](const ::NpcMst& n) {
		return kBotPrefix + std::to_string(n.id) == userId;
	});
	if (npc == npcs.end())
		return std::nullopt;

	// The two npc_mst teams are fixed level-12 squads; re-level them to the
	// attacker so a new player is not flattened and a veteran not bored.
	std::vector<JsonObject> units;
	std::vector<JsonObject> party;
	if (glz::read_json(units, npc->units) || glz::read_json(party, npc->parties))
		return std::nullopt;

	const auto botLevel = std::clamp(attackerLevel, 1, 100);
	Snapshot snap{};
	snap.userId = userId;
	snap.name = npc->handle_name;
	snap.level = botLevel;
	snap.points = attackerPoints;
	snap.rankId = rankForPoints(attackerPoints);
	snap.isBot = true;

	for (auto& unit : units)
	{
		const auto* mst = findUnitMst(std::atoi(unit["P_UNIT_ID"].c_str()));
		if (!mst)
			continue;
		const auto lv = std::min(botLevel, std::max(mst->max_lv, 1));
		unit["P_LV"] = std::to_string(lv);
		unit["P_HP"] = std::to_string(statAt(*mst, lv, mst->min_hp, mst->max_hp));
		unit["P_ATK"] = std::to_string(statAt(*mst, lv, mst->min_atk, mst->max_atk));
		unit["P_DEF"] = std::to_string(statAt(*mst, lv, mst->min_def, mst->max_def));
		unit["P_HEL"] = std::to_string(statAt(*mst, lv, mst->min_rec, mst->max_rec));
		fillMissingSkills(unit, mst->id);
	}

	for (const auto& member : party)
	{
		const auto it = member.find("P_MEMBER_TYPE");
		if (it == member.end() || it->second != "0")
			continue;
		const auto partyId = member.at("P_USER_UNIT_ID");
		for (const auto& unit : units)
		{
			if (unit.at("P_USER_UNIT_ID") == partyId)
			{
				snap.leaderUnitId = unit.at("P_UNIT_ID");
				snap.leaderLv = std::atoi(unit.at("P_LV").c_str());
			}
		}
	}

	snap.team = glz::write_json(JsonObject{
		{ "P_USER_ID", userId },
		{ "P_FRIEND_MESSAGE", "" },
		{ "P_LV", std::to_string(botLevel) },
		{ "P_GUILD_ID", "" },
	}).value_or("{}");
	snap.party = glz::write_json(party).value_or("[]");
	snap.units = glz::write_json(units).value_or("[]");
	return snap;
}

} // namespace gme::arena
