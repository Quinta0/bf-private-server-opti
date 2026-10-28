#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/gme/common/Arena.hpp>
#include <gimuserver/gme/common/Colosseum.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The Colosseum as asynchronous PvP by class.  A match is two client-simulated
// fights against one opponent: the player's Attack squad against their
// Defence squad, then the player's Defence squad against their Attack squad
// (ArenaBattleScene runs both, ColosseumBattleEnd reports both).  Opponents
// are other players' saved squads for the same class, or a generated squad
// when nobody fits.  Each fight won pays Colosseum Battle Points (CBP), a
// clean sweep pays a bonus, and the tickets put on the match multiply it all.
// CBP buys rewards in the exchange and ranks the player for the month's
// season.  Wire shapes: net/colosseum.kdl; rules: docs/features/COLOSSEUM.md.
//
// Refusals use refuseToHome and never throw: a thrown error closes the
// session, which the client reads as a crash.

namespace
{

namespace colo = gme::colosseum;
using gme::arena::JsonObject;

template <typename Req>
Req parseRequest(const std::string& json, const char* name)
{
	Req req{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
		LOG_WARN << name << ": parse error: " << glz::format_error(ec, json);
	return req;
}

std::optional<int64_t> parseInt(const std::optional<std::string>& raw)
{
	if (!raw || raw->empty())
		return std::nullopt;
	int64_t value = 0;
	const auto [ptr, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
	if (ec != std::errc{} || ptr != raw->data() + raw->size())
		return std::nullopt;
	return value;
}

/// The class a request names, 0 when it names none we know.
int32_t classOf(const std::optional<std::vector<::ColosseumClassEntry>>& entries)
{
	if (!entries || entries->empty())
		return 0;
	const auto id = parseInt(entries->front().class_id).value_or(0);
	return id >= 1 && id <= colo::kClassCount ? static_cast<int32_t>(id) : 0;
}

// CBP per fight and for winning both, before the ticket multiplier.  Ours:
// the original numbers were never dumped.
constexpr int32_t kFightWinCbp = 40;
constexpr int32_t kFightLossCbp = 10;
constexpr int32_t kSweepBonusCbp = 20;
constexpr int32_t kMaxCbp = 999'999'999;

// Fights are at most 5 a side; crystals and damage are only logged.
constexpr int64_t kMaxDefeatUnits = 5;

// Squad slots 0..4 fight.  ColosseumDeckEdit also sends the class's other
// selected units on the attack squad at positions 5+ (createBodyPartyDeck);
// they are stored so the squad screens get them back, never fielded.
constexpr int32_t kSquadSize = 5;

constexpr const char* kBotPrefix = "COLOBOT";

drogon::Task<::UserTeamInfo> teamInfo(db::Database database, const gme::UserIdentity& identity)
{
	auto team = (co_await gme::getTeamInfo(database, identity)).nonEmpty();
	team.fight_rest_timer = co_await gme::arena::orbRestSeconds(database, identity.userId);
	co_return team;
}

std::vector<JsonObject> parseParty(const std::string& json)
{
	std::vector<JsonObject> party;
	if (json.empty() || glz::read_json(party, json))
		return {};
	return party;
}

int32_t dispOrder(const JsonObject& member)
{
	const auto it = member.find("P_DISPORDER");
	return it == member.end() ? 0 : std::atoi(it->second.c_str());
}

/// The fighting part of a stored squad, with a leader: the first slot leads
/// when the client marked none.
std::vector<JsonObject> fieldedSquad(const std::vector<JsonObject>& party)
{
	std::vector<JsonObject> squad;
	for (const auto& member : party)
	{
		if (dispOrder(member) < kSquadSize && member.count("P_USER_UNIT_ID"))
			squad.push_back(member);
	}
	std::sort(squad.begin(), squad.end(),
		[](const JsonObject& a, const JsonObject& b) { return dispOrder(a) < dispOrder(b); });
	const auto hasLeader = std::any_of(squad.begin(), squad.end(), [](const JsonObject& m) {
		const auto it = m.find("P_MEMBER_TYPE");
		return it != m.end() && it->second == "0";
	});
	if (!hasLeader && !squad.empty())
		squad.front()["P_MEMBER_TYPE"] = "0";
	return squad;
}

/// One side of a match as both reply rows carry it.
struct Side
{
	std::string userId;
	std::string name;
	std::string friendId;
	int32_t level = 1;
	int32_t points = 0;
	int32_t battleCnt = 0;
	int32_t winCnt = 0;
	int32_t position = 0;
	std::string team;
	std::string atkParty;
	std::string defParty;
	std::string units;
	int32_t atkFormation = 101;
	int32_t atkSupport = 600;
	int32_t defFormation = 201;
	int32_t defSupport = 400;
	bool isBot = false;
};

std::string teamJson(const std::string& userId, int32_t level)
{
	return glz::write_json(JsonObject{
		{ "P_USER_ID", userId },
		{ "P_FRIEND_MESSAGE", "" },
		{ "P_LV", std::to_string(level) },
		{ "P_GUILD_ID", "" },
	}).value_or("{}");
}

/// A player's saved squads for a class, with their units as they are now.
/// std::nullopt when either squad is empty or no longer owned.
drogon::Task<std::optional<Side>> playerSide(db::Database database, const std::string& userId, int32_t classId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT c.*, i.username AS username, i.level AS level, COALESCE(i.friend_id, '') AS fid"
		" FROM user_colosseum_class c JOIN user_info i ON i.id = c.user_id"
		" WHERE c.user_id = $1 AND c.class_id = $2;",
		userId, classId);
	if (rows.empty())
		co_return std::nullopt;
	const auto& row = rows[0];

	auto atk = fieldedSquad(parseParty(row["atk_party"].as<std::string>()));
	auto def = fieldedSquad(parseParty(row["def_party"].as<std::string>()));
	std::vector<uint32_t> ids;
	for (const auto* squad : { &atk, &def })
	{
		for (const auto& member : *squad)
		{
			const auto id = static_cast<uint32_t>(std::strtoul(member.at("P_USER_UNIT_ID").c_str(), nullptr, 10));
			if (id > 0 && std::find(ids.begin(), ids.end(), id) == ids.end())
				ids.push_back(id);
		}
	}
	const auto units = co_await gme::arena::unitRows(database, userId, ids);
	std::set<std::string> owned;
	for (const auto& unit : units)
		owned.insert(unit.at("P_USER_UNIT_ID"));
	for (auto* squad : { &atk, &def })
	{
		std::erase_if(*squad, [&](const JsonObject& m) { return !owned.count(m.at("P_USER_UNIT_ID")); });
		*squad = fieldedSquad(*squad);
	}
	if (atk.empty() || def.empty())
		co_return std::nullopt;

	Side side{};
	side.userId = userId;
	side.name = row["username"].as<std::string>();
	side.friendId = row["fid"].as<std::string>();
	side.level = row["level"].as<int32_t>();
	side.points = row["points"].as<int32_t>();
	side.battleCnt = row["battle_cnt"].as<int32_t>();
	side.winCnt = row["win_cnt"].as<int32_t>();
	side.position = co_await colo::livePosition(database, userId, classId, side.points,
		row["season_battle_cnt"].as<int32_t>());
	side.team = teamJson(userId, side.level);
	side.atkParty = glz::write_json(atk).value_or("[]");
	side.defParty = glz::write_json(def).value_or("[]");
	side.units = glz::write_json(units).value_or("[]");
	side.atkFormation = row["atk_formation"].as<int32_t>();
	side.atkSupport = row["atk_support"].as<int32_t>();
	side.defFormation = row["def_formation"].as<int32_t>();
	side.defSupport = row["def_support"].as<int32_t>();
	co_return side;
}

/// Units a generated squad may field in each class: ordinary 7* and Omni
/// units within the class's Cost cap that have an archive record (art and
/// Brave Bursts).  Built once; the unit table never changes at runtime.
const std::vector<int32_t>& botPool(int32_t classId)
{
	static const auto pools = [] {
		std::array<std::vector<int32_t>, colo::kClassCount> out{};
		for (int32_t c = 1; c <= colo::kClassCount; ++c)
		{
			const auto cap = colo::maxUnitCost(c);
			for (const auto& unit : theServer()->cache().unitMst())
			{
				if (unit.kind != 0 || unit.rarity < 7 || unit.cost > cap || unit.ai_id <= 0)
					continue;
				const auto record = UnitArchiver::instance().lookup(static_cast<UnitArchiver::UnitId>(unit.id));
				if (record && !record->bb_id.empty())
					out[c - 1].push_back(unit.id);
			}
		}
		return out;
	}();
	return pools[std::clamp(classId, 1, colo::kClassCount) - 1];
}

constexpr std::array kBotNames{
	"Iron Gladiator", "Crimson Duelist", "Azure Sentinel", "Golden Champion",
	"Shadow Fencer", "Storm Warden", "Ember Knight", "Frost Valkyrie",
};

/// A generated opponent: ten different units from the class's pool at max
/// level, random formations and supports, the attacker's own team level.
std::optional<Side> botSide(int32_t classId, int32_t level, int32_t points)
{
	auto pool = botPool(classId);
	if (pool.size() < 2 * kSquadSize)
		return std::nullopt;
	for (size_t i = 0; i < 2 * kSquadSize; ++i)
		std::swap(pool[i], pool[i + RandomUInt(0, static_cast<uint32_t>(pool.size() - i - 1))]);

	std::vector<JsonObject> units;
	std::vector<JsonObject> atk;
	std::vector<JsonObject> def;
	for (int32_t i = 0; i < 2 * kSquadSize; ++i)
	{
		const auto userUnitId = std::to_string(900001 + i);
		auto unit = gme::arena::mstUnitRow(pool[i], 999, userUnitId);
		if (!unit)
			return std::nullopt;
		units.push_back(std::move(*unit));
		auto& squad = i < kSquadSize ? atk : def;
		squad.push_back({
			{ "P_USER_UNIT_ID", userUnitId },
			{ "P_MEMBER_TYPE", squad.empty() ? "0" : "1" },
			{ "P_DISPORDER", std::to_string(squad.size()) },
		});
	}

	static constexpr int32_t kSupports[] = { 400, 600, 700, 800 };
	Side side{};
	side.userId = kBotPrefix + std::to_string(RandomUInt(1000, 9999));
	side.name = kBotNames[RandomUInt(0, kBotNames.size() - 1)];
	side.level = std::clamp(level, 1, 999);
	side.points = std::max(0, points + static_cast<int32_t>(RandomUInt(0, 200)) - 100);
	side.team = teamJson(side.userId, side.level);
	side.atkParty = glz::write_json(atk).value_or("[]");
	side.defParty = glz::write_json(def).value_or("[]");
	side.units = glz::write_json(units).value_or("[]");
	side.atkFormation = 101 + static_cast<int32_t>(RandomUInt(0, 2));
	side.defFormation = 201 + static_cast<int32_t>(RandomUInt(0, 2));
	side.atkSupport = kSupports[RandomUInt(0, 3)];
	side.defSupport = kSupports[RandomUInt(0, 3)];
	side.isBot = true;
	return side;
}

/// Another player's squads for the class: someone near our season CBP if
/// possible, anyone otherwise, the last opponent only when nobody else fits.
drogon::Task<std::optional<Side>> pickPlayer(
	db::Database database, const std::string& userId, int32_t classId, int32_t myPoints, const std::string& lastEnemy)
{
	const auto band = std::max(500, myPoints / 4);
	// Only the player's own pool (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await gme::sandbox::isOn(database, userId);
	const auto pool = gme::sandbox::poolSql("user_id", sandboxed);
	for (const bool banded : { true, false })
	{
		const auto rows = co_await database->execSqlCoro(
			"SELECT user_id FROM user_colosseum_class"
			" WHERE class_id = $1 AND user_id != $2 AND atk_party != '[]' AND def_party != '[]'"
			" AND ($3 = 0 OR ABS(points - $4) <= $5)" + pool +
			" ORDER BY (user_id = $6), RANDOM() LIMIT 8;",
			classId, userId, banded ? 1 : 0, myPoints, band, lastEnemy);
		for (const auto& row : rows)
		{
			if (auto side = co_await playerSide(database, row["user_id"].as<std::string>(), classId))
				co_return side;
		}
	}
	co_return std::nullopt;
}

::ColosseumBattleEnemyInfo enemyRow(const Side& side, const std::string& battleNum)
{
	::ColosseumBattleEnemyInfo row{};
	row.user_id = side.userId;
	row.battle_num = battleNum;
	row.handle_name = side.name;
	row.lv = side.level;
	row.win_cnt = side.winCnt;
	row.battle_cnt = side.battleCnt;
	row.team = side.team;
	row.atk_party = side.atkParty;
	row.def_party = side.defParty;
	row.units = side.units;
	row.atk_formation_id = side.atkFormation;
	row.atk_support_id = side.atkSupport;
	row.def_formation_id = side.defFormation;
	row.def_support_id = side.defSupport;
	row.friend_id = side.friendId;
	row.total_cbp = side.points;
	row.position = side.position;
	return row;
}

::ColosseumBattlePartyInfo partyRow(const Side& side)
{
	::ColosseumBattlePartyInfo row{};
	row.atk_party = side.atkParty;
	row.def_party = side.defParty;
	row.units = side.units;
	row.atk_formation_id = side.atkFormation;
	row.atk_support_id = side.atkSupport;
	row.def_formation_id = side.defFormation;
	row.def_support_id = side.defSupport;
	return row;
}

drogon::Task<std::optional<drogon::orm::Row>> findBattle(
	db::Database database, const std::string& userId, std::optional<int64_t> battleId)
{
	const auto rows = battleId
		? co_await database->execSqlCoro(
			"SELECT * FROM colosseum_battle WHERE battle_id = $1 AND attacker_id = $2;", *battleId, userId)
		: co_await database->execSqlCoro(
			"SELECT * FROM colosseum_battle WHERE attacker_id = $1 AND status = 0"
			" ORDER BY battle_id DESC LIMIT 1;", userId);
	if (rows.empty())
		co_return std::nullopt;
	co_return rows[0];
}

drogon::Task<std::string> battleStartBody(
	db::Database database, const gme::UserIdentity& identity, const drogon::orm::Row& battle)
{
	::ColosseumBattleStartResp resp{};
	::ColosseumBattleEnemyInfo enemy{};
	::ColosseumBattlePartyInfo party{};
	if (glz::read_json(enemy, battle["enemy_json"].as<std::string>())
		|| glz::read_json(party, battle["party_json"].as<std::string>()))
	{
		co_return std::string{};
	}
	resp.enemy_info = { std::move(enemy) };
	resp.party_info = { std::move(party) };
	resp.team_info = co_await teamInfo(database, identity);
	// Both sides fight on AI; without these rows nobody casts a BB (#45).
	resp.ais = MissionArchiver::unitAiRows();
	co_return glz::write_json(resp).value_or("{}");
}

/// Sum of a comma list of numbers ("" and junk count 0).
int64_t sumList(const std::optional<std::string>& raw)
{
	int64_t sum = 0;
	if (!raw)
		return 0;
	size_t start = 0;
	while (start <= raw->size())
	{
		const auto end = std::min(raw->find(',', start), raw->size());
		int64_t value = 0;
		std::from_chars(raw->data() + start, raw->data() + end, value);
		sum += std::clamp<int64_t>(value, 0, 1'000'000'000);
		start = end + 1;
	}
	return sum;
}

struct MatchResult
{
	int32_t wins = 0;
	int32_t fights = 0;
	bool overallWin = false;
	int32_t defeated = 0;
};

/// Both fights won is a win and none a loss.  One each goes to the side with
/// more HP left over the two fights, the ColosseumResultJudgeScene's crown
/// (INFERRED: the original verdict rule was never dumped).
MatchResult judge(const std::vector<::ColosseumBattleEndEntry>& entries)
{
	MatchResult match{};
	int64_t ourHp = 0;
	int64_t theirHp = 0;
	for (const auto type : { "1", "2" })
	{
		const auto entry = std::find_if(entries.begin(), entries.end(),
			[type](const ::ColosseumBattleEndEntry& e) { return e.battle_type.value_or("") == type; });
		++match.fights;
		if (entry == entries.end())
			continue;
		if (entry->result.value_or("0") == "1")
			++match.wins;
		match.defeated += static_cast<int32_t>(std::clamp<int64_t>(parseInt(entry->defeat_unit_cnt).value_or(0), 0, kMaxDefeatUnits));
		ourHp += sumList(entry->hp_party_rest);
		theirHp += sumList(entry->hp_enemy_rest);
	}
	match.overallWin = match.wins == 2 || (match.wins == 1 && ourHp > theirHp);
	return match;
}

drogon::Task<std::string> settleInTransaction(
	db::Database database, const gme::UserIdentity& identity, const drogon::orm::Row& battle,
	const std::vector<::ColosseumBattleEndEntry>& entries)
{
	const auto battleId = battle["battle_id"].as<int64_t>();
	const auto claimed = co_await database->execSqlCoro(
		"UPDATE colosseum_battle SET status = 1, settled_at = $1 WHERE battle_id = $2 AND status = 0;",
		gme::unixNow(), battleId);
	if (claimed.affectedRows() == 0)
		co_return std::string{};

	const auto classId = battle["class_id"].as<int32_t>();
	const auto tickets = std::max(1, battle["tickets"].as<int32_t>());
	const auto match = judge(entries);
	const auto losses = match.fights - match.wins;
	const auto bonus = match.wins == 2 ? kSweepBonusCbp * tickets : 0;
	const auto gained = tickets * (match.wins * kFightWinCbp + losses * kFightLossCbp) + bonus;

	const auto before = co_await database->execSqlCoro(
		"SELECT points FROM user_colosseum_class WHERE user_id = $1 AND class_id = $2;",
		identity.userId, classId);
	const auto beforeCbp = before.empty() ? 0 : before[0]["points"].as<int32_t>();
	const auto afterCbp = static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(beforeCbp) + gained, kMaxCbp));

	const auto win = match.overallWin ? 1 : 0;
	const auto sweep = match.wins == 2 ? 1 : 0;
	co_await database->execSqlCoro(
		"UPDATE user_colosseum_class SET points = $1, season_battle_cnt = season_battle_cnt + 1,"
		" battle_cnt = battle_cnt + 1, win_cnt = win_cnt + $2,"
		" atk_series_win = CASE WHEN $2 = 1 THEN atk_series_win + 1 ELSE 0 END,"
		" max_atk_series_win = MAX(max_atk_series_win, CASE WHEN $2 = 1 THEN atk_series_win + 1 ELSE 0 END),"
		" all_win_cnt = all_win_cnt + $3,"
		" all_series_win = CASE WHEN $3 = 1 THEN all_series_win + 1 ELSE 0 END,"
		" max_all_series_win = MAX(max_all_series_win, CASE WHEN $3 = 1 THEN all_series_win + 1 ELSE 0 END),"
		" updated_at = $4 WHERE user_id = $5 AND class_id = $6;",
		afterCbp, win, sweep, gme::unixNow(), identity.userId, classId);
	co_await database->execSqlCoro(
		"UPDATE user_colosseum SET cbp = MIN(cbp + $1, $2), total_cbp = MIN(total_cbp + $1, $2) WHERE user_id = $3;",
		gained, kMaxCbp, identity.userId);
	if (battle["is_bot"].as<int32_t>() == 0)
	{
		co_await database->execSqlCoro(
			"UPDATE user_colosseum_class SET battle_cnt_accept = battle_cnt_accept + 1,"
			" win_cnt_accept = win_cnt_accept + $1 WHERE user_id = $2 AND class_id = $3;",
			1 - win, battle["defender_id"].as<std::string>(), classId);
	}

	const auto battleNum = std::to_string(battleId);
	::ColosseumBattleEndResp resp{};
	for (const auto type : { 1, 2 })
	{
		const auto entry = std::find_if(entries.begin(), entries.end(), [type](const ::ColosseumBattleEndEntry& e) {
			return e.battle_type.value_or("") == std::to_string(type);
		});
		::ColosseumBattleResult row{};
		row.battle_num = battleNum;
		row.battle_type = type;
		row.result = entry != entries.end() && entry->result.value_or("0") == "1" ? 1 : 0;
		row.defeat_unit_cnt = entry != entries.end()
			? static_cast<int32_t>(std::clamp<int64_t>(parseInt(entry->defeat_unit_cnt).value_or(0), 0, kMaxDefeatUnits))
			: 0;
		row.before_cbp = beforeCbp;
		row.after_cbp = afterCbp;
		row.battle_cnt = tickets;
		row.bonus_type = battle["bonus_type"].as<std::string>();
		row.bonus_param = battle["bonus_param"].as<std::string>();
		resp.results.push_back(std::move(row));
	}
	::ColosseumBattleResult summary{};
	summary.battle_num = battleNum;
	summary.battle_type = 3;
	summary.result = win;
	summary.defeat_unit_cnt = match.defeated;
	summary.before_cbp = beforeCbp;
	summary.after_cbp = afterCbp;
	summary.bonus_point = bonus;
	summary.battle_cnt = tickets;
	summary.bonus_type = battle["bonus_type"].as<std::string>();
	summary.bonus_param = battle["bonus_param"].as<std::string>();
	resp.results.push_back(std::move(summary));
	resp.classes = co_await colo::loadClasses(database, identity.userId);
	resp.user_info = co_await colo::loadUserInfo(database, identity.userId);
	resp.team_info = co_await teamInfo(database, identity);
	const auto body = glz::write_json(resp).value_or("{}");

	co_await database->execSqlCoro(
		"UPDATE colosseum_battle SET result_json = $1 WHERE battle_id = $2;", body, battleId);
	LOG_INFO << "Colosseum: battle " << battleId << " settled for " << identity.userId << " class " << classId
	         << " fights won " << match.wins << "/2 x" << tickets << " tickets, CBP "
	         << beforeCbp << " -> " << afterCbp;
	co_return body;
}

/// Settles once, in one transaction from the claim to the cached reply, as
/// Arena does: every helper inside gets the transaction (SQLite has a single
/// connection, which the open transaction holds).
drogon::Task<std::string> settleBattle(
	db::Database database, const gme::UserIdentity& identity, const drogon::orm::Row& battle,
	const std::vector<::ColosseumBattleEndEntry>& entries)
{
	const auto transaction = co_await gme::newTransaction(database);
	try
	{
		co_return co_await settleInTransaction(transaction, identity, battle, entries);
	}
	catch (const drogon::orm::DrogonDbException&)
	{
		throw;
	}
	catch (...)
	{
		transaction->rollback();
		throw;
	}
}

drogon::Task<::FriendUserArenaInfo> rankingRow(
	db::Database database, const drogon::orm::Row& row, int32_t infoType, int32_t position)
{
	::FriendUserArenaInfo info{};
	info.user_id = row["user_id"].as<std::string>();
	info.handle_name = row["username"].as<std::string>();
	info.lv = row["level"].as<int32_t>();
	info.arena_rank_id = 1;
	info.ranking_point = row["points"].as<int32_t>();
	info.battle_cnt = row["battle_cnt"].as<int32_t>();
	info.win_cnt = row["win_cnt"].as<int32_t>();
	info.battle_cnt_accept = row["battle_cnt_accept"].as<int32_t>();
	info.win_cnt_accept = row["win_cnt_accept"].as<int32_t>();
	info.info_type = infoType;
	info.position = position;

	const auto squad = fieldedSquad(parseParty(row["atk_party"].as<std::string>()));
	for (const auto& member : squad)
	{
		if (member.at("P_MEMBER_TYPE") != "0")
			continue;
		const auto id = static_cast<uint32_t>(std::strtoul(member.at("P_USER_UNIT_ID").c_str(), nullptr, 10));
		const auto units = co_await gme::arena::unitRows(database, info.user_id, { id });
		if (!units.empty())
		{
			info.unit_id = units[0].at("P_UNIT_ID");
			info.unit_lv = std::atoi(units[0].at("P_LV").c_str());
			info.unit_img_type = std::atoi(units[0].at("P_IMG_TYPE").c_str());
		}
	}
	co_return info;
}

} // namespace

HANDLEF(ColosseumDeckEdit)
{
	(void)session;
	const auto req = parseRequest<::ColosseumDeckEditReq>(json, "ColosseumDeckEdit");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await colo::sync(database, identity);

	const auto classId = classOf(req.class_info);
	if (classId > 0 && req.members && !req.members->empty())
	{
		// Only units the player owns, each once per squad.
		std::vector<uint32_t> ids;
		for (const auto& m : *req.members)
		{
			const auto id = parseInt(m.user_unit_id).value_or(0);
			if (id > 0)
				ids.push_back(static_cast<uint32_t>(id));
		}
		const auto units = co_await gme::arena::unitRows(database, identity.userId, ids);
		std::set<std::string> owned;
		for (const auto& unit : units)
			owned.insert(unit.at("P_USER_UNIT_ID"));

		std::vector<JsonObject> atk;
		std::vector<JsonObject> def;
		for (const auto& m : *req.members)
		{
			const auto id = std::to_string(parseInt(m.user_unit_id).value_or(0));
			if (!owned.count(id))
				continue;
			const auto isDef = m.deck_type ? *m.deck_type == "2" : m.deck_num.value_or("0") == "1";
			auto& squad = isDef ? def : atk;
			if (std::any_of(squad.begin(), squad.end(), [&](const JsonObject& o) { return o.at("P_USER_UNIT_ID") == id; }))
				continue;
			squad.push_back({
				{ "P_USER_UNIT_ID", id },
				{ "P_MEMBER_TYPE", std::to_string(parseInt(m.member_type).value_or(1)) },
				{ "P_DISPORDER", std::to_string(parseInt(m.disp_order).value_or(static_cast<int64_t>(squad.size()))) },
			});
		}

		const auto stored = co_await database->execSqlCoro(
			"SELECT atk_formation, atk_support, def_formation, def_support FROM user_colosseum_class"
			" WHERE user_id = $1 AND class_id = $2;", identity.userId, classId);
		int32_t ids4[4] = { 101, 600, 201, 400 };
		if (!stored.empty())
		{
			ids4[0] = stored[0]["atk_formation"].as<int32_t>();
			ids4[1] = stored[0]["atk_support"].as<int32_t>();
			ids4[2] = stored[0]["def_formation"].as<int32_t>();
			ids4[3] = stored[0]["def_support"].as<int32_t>();
		}
		const auto& formations = theServer()->cache().colosseumFormationMst();
		const auto& supports = theServer()->cache().colosseumSupportMst();
		for (const auto& s : req.supports.value_or(std::vector<::ColosseumDeckSupport>{}))
		{
			const auto slot = s.deck_type.value_or("1") == "2" ? 2 : 0;
			const auto formation = static_cast<int32_t>(parseInt(s.formation_id).value_or(0));
			const auto support = static_cast<int32_t>(parseInt(s.support_id).value_or(0));
			if (std::any_of(formations.begin(), formations.end(), [&](const auto& f) { return f.id == formation; }))
				ids4[slot] = formation;
			if (std::any_of(supports.begin(), supports.end(), [&](const auto& f) { return f.id == support; }))
				ids4[slot + 1] = support;
		}

		co_await database->execSqlCoro(
			"UPDATE user_colosseum_class SET atk_party = $1, def_party = $2, atk_formation = $3, atk_support = $4,"
			" def_formation = $5, def_support = $6, updated_at = $7 WHERE user_id = $8 AND class_id = $9;",
			glz::write_json(atk).value_or("[]"), glz::write_json(def).value_or("[]"),
			ids4[0], ids4[1], ids4[2], ids4[3], gme::unixNow(), identity.userId, classId);
		LOG_INFO << "ColosseumDeckEdit: " << identity.userId << " class " << classId
		         << " atk " << atk.size() << " def " << def.size() << " units";
	}

	::ColosseumDeckEditResp resp{};
	resp.user_info = co_await colo::loadUserInfo(database, identity.userId);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ColosseumBattleStart)
{
	(void)session;
	const auto req = parseRequest<::ColosseumBattleStartReq>(json, "ColosseumBattleStart");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await colo::sync(database, identity);

	const auto classId = classOf(req.class_info);
	if (classId == 0)
		co_return HandleResult::refuseToHome("This Colosseum class could not be found.");
	const auto info = (req.battle_info && !req.battle_info->empty()) ? req.battle_info->front() : ::ColosseumBattleInfoEntry{};
	const auto tickets = static_cast<int32_t>(std::clamp<int64_t>(parseInt(info.ticket_cnt).value_or(1), 1, colo::kMaxTickets));

	const auto mine = co_await playerSide(database, identity.userId, classId);
	if (!mine)
		co_return HandleResult::refuseToHome("Set up both an Attack and a Defence squad for this class first.");

	// A match left open (the app closed before ColosseumBattleEnd) is
	// abandoned: its tickets stay spent and it pays nothing.
	co_await database->execSqlCoro(
		"UPDATE colosseum_battle SET status = 3, settled_at = $1 WHERE attacker_id = $2 AND status = 0;",
		gme::unixNow(), identity.userId);

	const auto me = co_await database->execSqlCoro(
		"SELECT last_enemy FROM user_colosseum WHERE user_id = $1;", identity.userId);
	const auto lastEnemy = me.empty() ? std::string{} : me[0]["last_enemy"].as<std::string>();
	auto enemy = co_await pickPlayer(database, identity.userId, classId, mine->points, lastEnemy);
	if (!enemy)
		enemy = botSide(classId, mine->level, mine->points);
	if (!enemy)
		co_return HandleResult::refuseToHome("No opponent could be found right now. Please try again later.");

	const auto spent = co_await database->execSqlCoro(
		"UPDATE user_info SET colosseum_tickets = colosseum_tickets - $1 WHERE id = $2 AND colosseum_tickets >= $1;",
		tickets, identity.userId);
	if (spent.affectedRows() == 0)
		co_return HandleResult::refuseToHome("You don't have enough Colosseum Tickets.");

	const auto inserted = co_await database->execSqlCoro(
		"INSERT INTO colosseum_battle (attacker_id, defender_id, class_id, season_id, is_bot, tickets,"
		" enemy_json, party_json, bonus_type, bonus_param, created_at)"
		" VALUES ($1, $2, $3, $4, $5, $6, '', $7, $8, $9, $10);",
		identity.userId, enemy->userId, classId, colo::currentSeason(), enemy->isBot ? 1 : 0, tickets,
		glz::write_json(partyRow(*mine)).value_or("{}"),
		info.bonus_type.value_or(""), info.bonus_param.value_or(""), gme::unixNow());
	const auto battleId = static_cast<int64_t>(inserted.insertId());
	co_await database->execSqlCoro(
		"UPDATE colosseum_battle SET enemy_json = $1 WHERE battle_id = $2;",
		glz::write_json(enemyRow(*enemy, std::to_string(battleId))).value_or("{}"), battleId);
	co_await database->execSqlCoro(
		"UPDATE user_colosseum SET last_enemy = $1 WHERE user_id = $2;", enemy->userId, identity.userId);

	const auto battle = co_await findBattle(database, identity.userId, battleId);
	const auto body = battle ? co_await battleStartBody(database, identity, *battle) : std::string{};
	if (body.empty())
		co_return HandleResult::refuseToHome("The match could not be started. Please try again.");
	LOG_INFO << "ColosseumBattleStart: battle " << battleId << " " << identity.userId << " class " << classId
	         << " vs " << enemy->userId << (enemy->isBot ? " (generated)" : "") << " x" << tickets;
	co_return HandleResult::success(body);
}

HANDLEF(ColosseumBattleRestart)
{
	(void)session;
	const auto req = parseRequest<::ColosseumClassReq>(json, "ColosseumBattleRestart");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	const auto battle = co_await findBattle(database, identity.userId, std::nullopt);
	const auto body = battle ? co_await battleStartBody(database, identity, *battle) : std::string{};
	if (body.empty())
		co_return HandleResult::refuseToHome("This Colosseum match has already ended.");
	co_return HandleResult::success(body);
}

HANDLEF(ColosseumBattleEnd)
{
	(void)session;
	const auto req = parseRequest<::ColosseumBattleEndReq>(json, "ColosseumBattleEnd");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	const auto entries = req.results.value_or(std::vector<::ColosseumBattleEndEntry>{});
	std::optional<int64_t> battleId;
	for (const auto& entry : entries)
	{
		if (!battleId)
			battleId = parseInt(entry.battle_num);
	}
	const auto battle = battleId ? co_await findBattle(database, identity.userId, battleId) : std::nullopt;
	if (!battle)
		co_return HandleResult::refuseToHome("This Colosseum match could not be found.");

	// Settled already: the stored reply, so a resend pays nothing.
	if ((*battle)["status"].as<int32_t>() != 0)
	{
		const auto stored = (*battle)["result_json"].as<std::string>();
		if (!stored.empty())
			co_return HandleResult::success(stored);
		co_return HandleResult::refuseToHome("This Colosseum match has already ended.");
	}

	co_await colo::sync(database, identity);
	auto body = co_await settleBattle(database, identity, *battle, entries);
	if (body.empty())
	{
		const auto again = co_await findBattle(database, identity.userId, battleId);
		body = again ? (*again)["result_json"].as<std::string>() : std::string{};
	}
	if (body.empty())
		co_return HandleResult::refuseToHome("This Colosseum match has already ended.");
	co_return HandleResult::success(body);
}

HANDLEF(ColosseumFriendList)
{
	(void)session;
	const auto req = parseRequest<::ColosseumClassReq>(json, "ColosseumFriendList");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await colo::sync(database, identity);

	const auto classId = std::max(1, classOf(req.class_info));
	const auto asked = (req.class_info && !req.class_info->empty())
		? req.class_info->front().duration_id.value_or("") : std::string{};
	const auto season = colo::currentSeason();
	// "Overall Ranking" asks for duration "0": all seasons' CBP in the class.
	const auto overall = asked == "0";
	const auto past = !overall && !asked.empty() && asked != season;

	// ColosseumRankScene (emulator 2026-10-23): info type 1 is the everyone
	// list and 0 the Show Friends list, the reverse of the guess in
	// ArenaFriendList; a past season reads list 1 as well.  The player's own
	// row goes in both: getMyInfo copies it from the lists for the header,
	// which otherwise shows 0 CBP and no rank.  Every query binds $1 class and
	// $2 season, so the overall one carries a no-op "$2 = $2".
	const std::string select = past
		? "SELECT s.user_id AS user_id, i.username AS username, i.level AS level, s.points AS points,"
		  " COALESCE(c.battle_cnt, 0) AS battle_cnt, COALESCE(c.win_cnt, 0) AS win_cnt,"
		  " COALESCE(c.battle_cnt_accept, 0) AS battle_cnt_accept, COALESCE(c.win_cnt_accept, 0) AS win_cnt_accept,"
		  " COALESCE(c.atk_party, '[]') AS atk_party"
		  " FROM user_colosseum_season s JOIN user_info i ON i.id = s.user_id"
		  " LEFT JOIN user_colosseum_class c ON c.user_id = s.user_id AND c.class_id = s.class_id"
		  " WHERE s.class_id = $1 AND s.season_id = $2"
		: overall
		? "SELECT c.user_id AS user_id, i.username AS username, i.level AS level,"
		  " c.points + COALESCE((SELECT SUM(a.points) FROM user_colosseum_season a"
		  " WHERE a.user_id = c.user_id AND a.class_id = c.class_id), 0) AS points,"
		  " c.battle_cnt AS battle_cnt, c.win_cnt AS win_cnt, c.battle_cnt_accept AS battle_cnt_accept,"
		  " c.win_cnt_accept AS win_cnt_accept, c.atk_party AS atk_party"
		  " FROM user_colosseum_class c JOIN user_info i ON i.id = c.user_id"
		  " WHERE c.class_id = $1 AND c.battle_cnt > 0 AND $2 = $2"
		: "SELECT c.user_id AS user_id, i.username AS username, i.level AS level, c.points AS points,"
		  " c.battle_cnt AS battle_cnt, c.win_cnt AS win_cnt, c.battle_cnt_accept AS battle_cnt_accept,"
		  " c.win_cnt_accept AS win_cnt_accept, c.atk_party AS atk_party"
		  " FROM user_colosseum_class c JOIN user_info i ON i.id = c.user_id"
		  " WHERE c.class_id = $1 AND c.season_id = $2 AND c.season_battle_cnt > 0";
	const auto userCol = past ? "s.user_id" : "c.user_id";
	const auto seasonArg = past ? asked : season;

	// Both lists hold the player's own pool only (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await gme::sandbox::isOn(database, identity.userId);
	const auto pool = gme::sandbox::poolSql(userCol, sandboxed);
	::ColosseumFriendListResp resp{};
	const auto everyone = co_await database->execSqlCoro(
		select + pool + " ORDER BY points DESC, " + userCol + " LIMIT 50;", classId, seasonArg);
	int32_t position = 0;
	for (const auto& row : everyone)
		resp.list.push_back(co_await rankingRow(database, row, 1, ++position));
	const auto friends = co_await database->execSqlCoro(
		select + pool + " AND (" + userCol + " = $3 OR " + userCol + " IN (SELECT friend_id FROM user_friends WHERE user_id = $3))"
		" ORDER BY points DESC, " + userCol + " LIMIT 100;",
		classId, seasonArg, identity.userId);
	position = 0;
	for (const auto& row : friends)
		resp.list.push_back(co_await rankingRow(database, row, 0, ++position));

	resp.durations = co_await colo::loadDurations(database, identity.userId);
	resp.past_rankings = co_await colo::loadPastRankings(database, identity.userId);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ColosseumGetRewardInfo)
{
	(void)session;
	const auto req = parseRequest<::ColosseumGetRewardInfoReq>(json, "ColosseumGetRewardInfo");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await colo::sync(database, identity);

	auto stage = co_await colo::currentStage(database, identity.userId);
	const auto asked = (req.filter && !req.filter->empty()) ? parseInt(req.filter->front().stage_id).value_or(0) : 0;
	// The shop's "Go to Next Stage" (after its own ARENA_COLOSSEUM_SHOP_NEXT_CHECK
	// confirm) asks for the stage after this one: the player moves on, and what
	// they did not buy here stays behind.  Anything else answers the stage they
	// are on, so a vault left behind cannot be reopened (ticket #133:
	// answering stage 1 again made the button look broken).
	if (asked > 0 && asked == colo::nextStage(stage))
	{
		co_await colo::moveToStage(database, identity.userId, static_cast<int32_t>(asked));
		LOG_INFO << "ColosseumGetRewardInfo: " << identity.userId << " moved on to stage " << asked;
		stage = static_cast<int32_t>(asked);
	}

	::ColosseumRewardResp resp{};
	resp.rewards = co_await colo::rewardRows(database, identity.userId, stage);
	resp.stages = colo::stageRows(stage);
	resp.user_info = co_await colo::loadUserInfo(database, identity.userId);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ColosseumRewardReceipt)
{
	(void)session;
	const auto req = parseRequest<::ColosseumRewardReceiptReq>(json, "ColosseumRewardReceipt");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await colo::sync(database, identity);

	const auto current = co_await colo::currentStage(database, identity.userId);
	const auto takenRows = co_await database->execSqlCoro(
		"SELECT reward_id FROM user_colosseum_reward WHERE user_id = $1;", identity.userId);
	std::set<int32_t> taken;
	for (const auto& row : takenRows)
		taken.insert(row["reward_id"].as<int32_t>());
	std::vector<const colo::Reward*> picks;
	int64_t price = 0;
	for (const auto& entry : req.rewards.value_or(std::vector<::ColosseumReceiptEntry>{}))
	{
		const auto* reward = colo::findReward(static_cast<int32_t>(parseInt(entry.reward_id).value_or(0)));
		// Only the vault the player is on: later ones are not open yet, earlier
		// ones were left behind.
		if (!reward || reward->stageId != current || std::find(picks.begin(), picks.end(), reward) != picks.end())
			continue;
		if (taken.count(reward->id) && !reward->repeatable)
			continue;
		picks.push_back(reward);
		price += reward->needCbp;
	}
	if (picks.empty())
		co_return HandleResult::refuseToHome("That reward is no longer available.");

	// The block ends the transaction (drogon commits on release) before the
	// reply reads on the plain connection, which the open transaction holds.
	{
		const auto transaction = co_await gme::newTransaction(database);
		try
		{
			const auto paid = co_await transaction->execSqlCoro(
				"UPDATE user_colosseum SET cbp = cbp - $1 WHERE user_id = $2 AND cbp >= $1;", price, identity.userId);
			if (paid.affectedRows() == 0)
			{
				transaction->rollback();
				co_return HandleResult::refuseToHome("You don't have enough CBP for that.");
			}
			for (const auto* reward : picks)
			{
				if (!reward->repeatable)
				{
					co_await transaction->execSqlCoro(
						"INSERT OR IGNORE INTO user_colosseum_reward (user_id, reward_id, received_at) VALUES ($1, $2, $3);",
						identity.userId, reward->id, gme::unixNow());
				}
				co_await gme::addUserPresent(transaction, identity, reward->presentType, reward->targetId,
					reward->count, 0, std::string("Colosseum exchange: ") + reward->name);
			}
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			LOG_ERROR << "ColosseumRewardReceipt: " << ex.base().what();
			co_return HandleResult::refuseToHome("The exchange failed. Please try again.");
		}
	}
	LOG_INFO << "ColosseumRewardReceipt: " << identity.userId << " exchanged " << picks.size()
	         << " reward(s) for " << price << " CBP";

	::ColosseumRewardResp resp{};
	resp.rewards = co_await colo::rewardRows(database, identity.userId, picks.front()->stageId);
	resp.stages = colo::stageRows(current);
	resp.user_info = co_await colo::loadUserInfo(database, identity.userId);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
