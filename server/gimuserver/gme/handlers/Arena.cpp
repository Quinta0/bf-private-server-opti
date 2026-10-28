#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/gme/common/Achievements.hpp>
#include <gimuserver/gme/common/Arena.hpp>
#include <gimuserver/gme/common/Colosseum.hpp>
#include <gimuserver/gme/common/DailyChallenges.hpp>
#include <gimuserver/gme/common/DailyTask.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

// Classic Arena as asynchronous PvP: the opponent is another player's saved
// arena deck (or an npc_mst bot when nobody is in range), the client
// simulates the whole fight, and the server only hands out the enemy
// snapshot and settles the reported result.  Flow, wire shapes and what is
// still unverified: docs/features/ARENA_SPEC.md.
//
// Refusals use refuseToHome (message + back Home) and never throw: a thrown
// error closes the session, which the client reads as a crash.  ArenaRetire
// is on the BOOT path and must succeed whatever it is sent.

namespace
{

using gme::arena::Snapshot;
using gme::arena::Standing;

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

// Per-battle ceilings for the client-reported stats.  The client simulates
// the fight, so these are whatever it says; the caps keep a forged report
// from inflating the running totals.  An arena party is at most six units;
// a single fight drops crystals in the tens, so 9999 is generous.  Damage
// and sparks are only kept as a MAX, so they just need to fit an int32.
constexpr int64_t kMaxDefeatUnits = 6;
constexpr int64_t kMaxCrystals = 9999;
constexpr int64_t kMaxTurnStat = 1'000'000'000;

int32_t parseCount(const std::optional<std::string>& raw, int64_t ceiling)
{
	const auto value = parseInt(raw).value_or(0);
	return static_cast<int32_t>(std::clamp<int64_t>(value, 0, ceiling));
}

drogon::Task<::UserTeamInfo> teamInfo(db::Database database, const gme::UserIdentity& identity)
{
	auto team = (co_await gme::getTeamInfo(database, identity)).nonEmpty();
	team.fight_rest_timer = co_await gme::arena::orbRestSeconds(database, identity.userId);
	co_return team;
}

drogon::Task<std::string> standingBody(db::Database database, const gme::UserIdentity& identity)
{
	::ArenaStandingResp resp{};
	resp.arena_info = co_await gme::arena::loadArenaInfo(database, identity);
	co_return glz::write_json(resp).value_or("{}");
}

drogon::Task<int32_t> userLevel(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT level FROM user_info WHERE id = $1;", userId);
	co_return rows.empty() ? 1 : rows[0]["level"].as<int32_t>();
}

/// Players with a deck whose points sit within max(1000, 25%) of ours, in
/// random order.  The previous offer goes last so "search again" shows
/// someone new whenever there is anyone new.
drogon::Task<std::vector<std::string>> candidateIds(
	db::Database database, const std::string& userId, const Standing& me)
{
	const auto band = std::max(1000, me.points / 4);
	// Only the player's own pool (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await gme::sandbox::isOn(database, userId);
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.id AS id FROM user_info i LEFT JOIN user_arena a ON a.user_id = i.id"
		" WHERE i.id != $1"
		" AND EXISTS (SELECT 1 FROM user_decks d WHERE d.user_id = i.id AND d.deck_type = 1 AND d.user_unit_id > 0)"
		" AND ABS(COALESCE(a.points, 0) - $2) <= $3"
		+ gme::sandbox::poolSql("i.id", sandboxed) +
		" ORDER BY (i.id = $4), RANDOM() LIMIT 8;",
		userId, me.points, band, me.offerUserId);

	std::vector<std::string> ids;
	for (const auto& row : rows)
		ids.push_back(row["id"].as<std::string>());
	co_return ids;
}

drogon::Task<std::optional<Snapshot>> pickOpponent(
	db::Database database, const std::string& userId, const Standing& me, int32_t myLevel)
{
	for (const auto& id : co_await candidateIds(database, userId, me))
	{
		if (auto snap = co_await gme::arena::playerSnapshot(database, id))
			co_return snap;
	}
	co_return gme::arena::botSnapshot(gme::arena::randomBotId(), myLevel, me.points);
}

drogon::Task<std::optional<Snapshot>> snapshotFor(
	db::Database database, const std::string& enemyId, int32_t myLevel, int32_t myPoints)
{
	if (gme::arena::isBotId(enemyId))
		co_return gme::arena::botSnapshot(enemyId, myLevel, myPoints);
	co_return co_await gme::arena::playerSnapshot(database, enemyId);
}

/// Spends one Arena Orb atomically.  Starts the regeneration clock when the
/// spend takes the player below the cap.
drogon::Task<bool> spendOrb(db::Database database, const std::string& userId)
{
	co_await gme::arena::syncOrbs(database, userId);
	const auto spent = co_await database->execSqlCoro(
		"UPDATE user_info SET fight_point = fight_point - 1 WHERE id = $1 AND fight_point >= 1;",
		userId);
	if (spent.affectedRows() == 0)
		co_return false;
	co_await database->execSqlCoro(
		"UPDATE user_arena SET orb_ts = $1 WHERE user_id = $2 AND orb_ts = 0;",
		gme::arena::nowSeconds(), userId);
	co_return true;
}

::ArenaBattleEnemyInfo enemyInfoRow(const drogon::orm::Row& battle)
{
	::ArenaBattleEnemyInfo row{};
	row.user_id = battle["defender_id"].as<std::string>();
	row.battle_id = battle["battle_id"].as<int32_t>();
	row.team = battle["enemy_team"].as<std::string>();
	row.party = battle["enemy_party"].as<std::string>();
	row.units = battle["enemy_units"].as<std::string>();
	row.friend_message = battle["enemy_message"].as<std::string>();
	row.rule = 0;
	return row;
}

struct BattleStats
{
	bool win = false;
	int32_t bCrystal = 0;
	int32_t hCrystal = 0;
	int32_t maxTurnDamage = 0;
	int32_t maxTurnSpark = 0;
	int32_t defeatUnits = 0;
	/// What the daily Challenges count; empty on a retire, which is no battle.
	gme::challenge::Progress challenge{};
};

/// Rank-up rewards for every rank in (before, after].  Paid only the first
/// time a rank is reached (above max_rank_id); gems go straight to the
/// wallet, items to the present box so the existing claim path stores them.
drogon::Task<std::vector<::ArenaBattleReward>> grantRankRewards(
	db::Database database, const gme::UserIdentity& identity, const Standing& before, int32_t afterRank)
{
	std::vector<::ArenaBattleReward> rewards;
	const auto& ranks = theServer()->cache().initializeResp().arena_ranks;
	for (int32_t rankId = before.rankId + 1; rankId <= afterRank; ++rankId)
	{
		const auto rank = std::find_if(ranks.begin(), ranks.end(),
			[rankId](const ::ArenaRankMst& r) { return r.id == rankId; });
		::ArenaBattleReward reward{};
		reward.first_flag = rankId > before.maxRankId ? 1 : 0;
		if (rank != ranks.end() && reward.first_flag == 1 && rank->reward_param.size() >= 2)
		{
			const auto target = rank->reward_param[0];
			const auto count = rank->reward_param[1];
			if (rank->reward_type == "1")
			{
				reward.dia = count;
				co_await database->execSqlCoro(
					"UPDATE user_info SET gems = gems + $1 WHERE id = $2;", count, identity.userId);
			}
			else if (rank->reward_type == "2" && target > 0)
			{
				reward.items = std::to_string(target) + ":" + std::to_string(count);
				co_await gme::addUserPresent(database, identity, 4, std::to_string(target), count, 0,
					"Arena rank-up reward: " + rank->name);
			}
		}
		rewards.push_back(std::move(reward));
	}
	co_return rewards;
}

drogon::Task<void> updateAttacker(
	db::Database database, const std::string& userId, const Standing& before,
	int32_t points, int32_t rank, const BattleStats& stats)
{
	const auto series = stats.win ? before.atkSeriesWin + 1 : 0;
	co_await database->execSqlCoro(
		"UPDATE user_arena SET points = $1, rank_id = $2, max_rank_id = MAX(max_rank_id, $2),"
		" battle_cnt = battle_cnt + 1, win_cnt = win_cnt + $3,"
		" atk_series_win = $4, max_atk_series_win = MAX(max_atk_series_win, $4),"
		// Running totals saturate at INT_MAX: they go out as int32 wire fields.
		" defeat_unit_cnt = MIN(defeat_unit_cnt + $5, 2147483647), max_turn_damage = MAX(max_turn_damage, $6),"
		" max_turn_spark = MAX(max_turn_spark, $7), sum_b_crystal = MIN(sum_b_crystal + $8, 2147483647),"
		" sum_h_crystal = MIN(sum_h_crystal + $9, 2147483647), updated_at = $10 WHERE user_id = $11;",
		points, rank, stats.win ? 1 : 0, series, stats.defeatUnits, stats.maxTurnDamage,
		stats.maxTurnSpark, stats.bCrystal, stats.hCrystal, gme::arena::nowSeconds(), userId);
}

/// Defenders never lose points; they only collect the accept counters and a
/// defence streak when the attacker fails.
drogon::Task<void> updateDefender(db::Database database, const std::string& defenderId, bool attackerWon)
{
	co_await gme::arena::ensureStanding(database, defenderId);
	co_await database->execSqlCoro(
		"UPDATE user_arena SET battle_cnt_accept = battle_cnt_accept + 1,"
		" win_cnt_accept = win_cnt_accept + $1,"
		" def_series_win = CASE WHEN $1 = 1 THEN def_series_win + 1 ELSE 0 END,"
		" max_def_series_win = MAX(max_def_series_win, CASE WHEN $1 = 1 THEN def_series_win + 1 ELSE 0 END)"
		" WHERE user_id = $2;",
		attackerWon ? 0 : 1, defenderId);
}

/// The body of settleBattle; `database` is its transaction.
drogon::Task<std::string> settleInTransaction(
	db::Database database, const gme::UserIdentity& identity, const drogon::orm::Row& battle,
	const BattleStats& stats, int32_t finalStatus)
{
	const auto battleId = battle["battle_id"].as<int64_t>();
	// Claim the battle first: if two settles race, only one sees status 0.
	const auto claimed = co_await database->execSqlCoro(
		"UPDATE arena_battle SET status = $1, settled_at = $2 WHERE battle_id = $3 AND status = 0;",
		finalStatus, gme::arena::nowSeconds(), battleId);
	if (claimed.affectedRows() == 0)
		co_return std::string{};

	co_await gme::arena::ensureStanding(database, identity.userId);
	const auto before = co_await gme::arena::loadStanding(database, identity.userId);
	const auto gained = stats.win
		? gme::arena::pointsForWin(before.points, battle["enemy_points"].as<int32_t>())
		: 0;
	const auto points = before.points + gained;
	const auto rank = std::max(before.rankId, gme::arena::rankForPoints(points));
	const auto karma = stats.win ? gme::arena::karmaForWin(rank) : 0;

	const auto rewards = co_await grantRankRewards(database, identity, before, rank);
	co_await updateAttacker(database, identity.userId, before, points, rank, stats);
	if (karma > 0)
	{
		co_await database->execSqlCoro(
			"UPDATE user_info SET karma = karma + $1 WHERE id = $2;", karma, identity.userId);
	}
	if (battle["is_bot"].as<int32_t>() == 0)
		co_await updateDefender(database, battle["defender_id"].as<std::string>(), stats.win);
	if (stats.win)
		co_await gme::advanceDailyTask(database, identity, "AV");
	// After the claim above, so a resent BattleEnd counts nothing.
	co_await gme::advanceDailyChallenges(database, identity, stats.challenge);

	::ArenaBattleResult result{};
	result.battle_id = std::to_string(battleId);
	result.result = stats.win ? "1" : "0";
	result.before_rank_id = std::to_string(before.rankId);
	result.after_rank_id = std::to_string(rank);
	result.before_point = before.points;
	result.after_point = points;
	result.karma = karma;
	// The friend-request prompt (scene 409) is not wired up yet.
	result.friend_req_state = 0;
	result.bonus_point = 0;
	result.bonus_type = battle["bonus_type"].as<std::string>();
	result.bonus_param = battle["bonus_param"].as<std::string>();
	result.battle_cnt = before.battleCnt + 1;

	::ArenaBattleEndResp resp{};
	resp.result = { std::move(result) };
	resp.rewards = rewards;
	resp.arena_info = co_await gme::arena::loadArenaInfo(database, identity);
	resp.arena_archive = co_await gme::arena::loadArenaArchive(database, identity);
	resp.team_info = co_await teamInfo(database, identity);
	auto body = glz::write_json(resp).value_or("{}");

	co_await database->execSqlCoro(
		"UPDATE arena_battle SET result_json = $1 WHERE battle_id = $2;", body, battleId);

	// THE BADGES, on the live reply only: an Arena Challenge this battle
	// finished lights Missions > Challenges at once.  The stored copy above
	// leaves them out, because a resend replaying this count would re-light a
	// challenge claimed since; resends get a fresh one instead
	// (gme::withCurrentBadge).  A retire (3) answers nobody, so it skips them.
	if (finalStatus != 3)
	{
		resp.badge = co_await gme::achievementBadge(database, identity);
		if (resp.badge)
			body = glz::write_json(resp).value_or(body);
	}
	LOG_INFO << "Arena: battle " << battleId << " settled for " << identity.userId
	         << (stats.win ? " WIN " : " LOSS ") << before.points << " -> " << points
	         << " pts, rank " << before.rankId << " -> " << rank;
	co_return body;
}

/// Settles an open battle exactly once and returns the BattleEnd reply, which
/// is also cached on the battle row so a resent BattleEnd gets the same answer.
/// One transaction from the claim to the cached reply: a failure part-way
/// would otherwise leave the battle claimed but unpaid (a resend is refused)
/// or paid without max_rank_id raised (the next win pays the rank-up again).
/// Every helper inside gets the transaction, never theDb(): SQLite runs on a
/// single connection, which the open transaction holds.
drogon::Task<std::string> settleBattle(
	db::Database database, const gme::UserIdentity& identity, const drogon::orm::Row& battle,
	const BattleStats& stats, int32_t finalStatus)
{
	const auto transaction = co_await gme::newTransaction(database);
	try
	{
		co_return co_await settleInTransaction(transaction, identity, battle, stats, finalStatus);
	}
	catch (const drogon::orm::DrogonDbException&)
	{
		// A failed statement already rolled the transaction back (drogon does
		// that itself); a second rollback only logs "roll back error".
		throw;
	}
	catch (...)
	{
		transaction->rollback();
		throw;
	}
}

drogon::Task<std::optional<drogon::orm::Row>> findBattle(
	db::Database database, const std::string& userId, const std::optional<int64_t> battleId)
{
	const auto rows = battleId
		? co_await database->execSqlCoro(
			"SELECT * FROM arena_battle WHERE battle_id = $1 AND attacker_id = $2;", *battleId, userId)
		: co_await database->execSqlCoro(
			"SELECT * FROM arena_battle WHERE attacker_id = $1 AND status = 0"
			" ORDER BY battle_id DESC LIMIT 1;", userId);
	if (rows.empty())
		co_return std::nullopt;
	co_return rows[0];
}

/// A new battle forfeits any battle still open (the app died before
/// BattleEnd and Retire never ran): it counts as a loss, like a retire.
drogon::Task<void> forfeitOpenBattles(db::Database database, const gme::UserIdentity& identity)
{
	while (const auto open = co_await findBattle(database, identity.userId, std::nullopt))
		co_await settleBattle(database, identity, *open, BattleStats{}, 3);
}

drogon::Task<::FriendUserArenaInfo> rankingRow(
	db::Database database, const drogon::orm::Row& row, int32_t infoType, int32_t position)
{
	::FriendUserArenaInfo info{};
	info.user_id = row["id"].as<std::string>();
	info.handle_name = row["username"].as<std::string>();
	info.lv = row["level"].as<int32_t>();
	info.arena_rank_id = row["rank_id"].as<int32_t>();
	info.ranking_point = row["points"].as<int32_t>();
	info.battle_cnt = row["battle_cnt"].as<int32_t>();
	info.win_cnt = row["win_cnt"].as<int32_t>();
	info.battle_cnt_accept = row["battle_cnt_accept"].as<int32_t>();
	info.win_cnt_accept = row["win_cnt_accept"].as<int32_t>();
	info.info_type = infoType;
	info.position = position;

	if (const auto snap = co_await gme::arena::playerSnapshot(database, info.user_id))
	{
		info.unit_id = snap->leaderUnitId;
		info.unit_lv = snap->leaderLv;
		info.unit_img_type = snap->leaderImgType;
	}
	co_return info;
}

constexpr const char* kRankingSelect =
	"SELECT i.id AS id, i.username AS username, i.level AS level,"
	" COALESCE(a.rank_id, 1) AS rank_id, COALESCE(a.points, 0) AS points,"
	" COALESCE(a.battle_cnt, 0) AS battle_cnt, COALESCE(a.win_cnt, 0) AS win_cnt,"
	" COALESCE(a.battle_cnt_accept, 0) AS battle_cnt_accept,"
	" COALESCE(a.win_cnt_accept, 0) AS win_cnt_accept"
	" FROM user_info i LEFT JOIN user_arena a ON a.user_id = i.id";

} // namespace

HANDLEF(ArenaInfo)
{
	(void)session;
	const auto req = parseRequest<::ArenaPlainReq>(json, "ArenaInfo");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await gme::arena::syncOrbs(database, identity.userId);

	// Also the Colosseum hub (ColosseumTopScene re-sends ArenaInfo on every visit).
	co_return HandleResult::success(co_await gme::colosseum::hubBody(database, identity));
}

HANDLEF(ArenaStart)
{
	(void)session;
	const auto req = parseRequest<::ArenaPlainReq>(json, "ArenaStart");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
	co_await gme::arena::ensureStanding(database, identity.userId);
	co_return HandleResult::success(co_await standingBody(database, identity));
}

HANDLEF(ArenaMatching)
{
	(void)session;
	const auto req = parseRequest<::ArenaMatchingReq>(json, "ArenaMatching");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	co_await gme::arena::ensureStanding(database, identity.userId);
	const auto me = co_await gme::arena::loadStanding(database, identity.userId);
	const auto opponent = co_await pickOpponent(
		database, identity.userId, me, co_await userLevel(database, identity.userId));
	if (!opponent)
		co_return HandleResult::refuseToHome("No opponent could be found right now. Please try again later.");

	co_await database->execSqlCoro(
		"UPDATE user_arena SET offer_user_id = $1 WHERE user_id = $2;", opponent->userId, identity.userId);

	const auto enemy = opponent->isBot ? Standing{} : co_await gme::arena::loadStanding(database, opponent->userId);
	::ArenaSuggestEnemy row{};
	row.user_id = opponent->userId;
	row.handle_name = opponent->name;
	row.lv = opponent->level;
	row.arena_rank_id = opponent->rankId;
	row.unit_id = opponent->leaderUnitId;
	row.battle_cnt = enemy.battleCnt;
	row.win_cnt = enemy.winCnt;
	row.battle_cnt_accept = enemy.battleCntAccept;
	row.win_cnt_accept = enemy.winCntAccept;
	row.ranking_point = opponent->points;
	row.unit_img_type = opponent->leaderImgType;

	LOG_INFO << "ArenaMatching: " << identity.userId << " offered " << opponent->userId
	         << (opponent->isBot ? " (bot)" : "");
	::ArenaMatchingResp resp{};
	resp.enemy = { std::move(row) };
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ArenaBattleStart)
{
	(void)session;
	const auto req = parseRequest<::ArenaBattleStartReq>(json, "ArenaBattleStart");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	const auto enemyId = (req.enemy && !req.enemy->empty())
		? req.enemy->front().enemy_user_id.value_or("") : std::string{};
	co_await gme::arena::ensureStanding(database, identity.userId);
	const auto me = co_await gme::arena::loadStanding(database, identity.userId);
	if (enemyId.empty() || enemyId != me.offerUserId)
		co_return HandleResult::refuseToHome("That opponent is no longer available. Please search again.");

	const auto myLevel = co_await userLevel(database, identity.userId);
	const auto enemy = co_await snapshotFor(database, enemyId, myLevel, me.points);
	if (!enemy)
		co_return HandleResult::refuseToHome("That opponent is no longer available. Please search again.");

	co_await forfeitOpenBattles(database, identity);
	if (!co_await spendOrb(database, identity.userId))
		co_return HandleResult::refuseToHome("You have no Arena Orbs left.");

	// Our own side is only needed if the client restarts the battle.
	const auto mine = co_await gme::arena::playerSnapshot(database, identity.userId);
	const auto& deck = (req.deck && !req.deck->empty()) ? req.deck->front() : ::ArenaDeckEntry{};
	// Named lvalues: a `cond ? a : std::string(..)` prvalue is a const
	// std::string, which drogon's SqlBinder template cannot bind.
	const std::string myParty = mine ? mine->party : std::string("[]");
	const std::string myUnits = mine ? mine->units : std::string("[]");
	const auto inserted = co_await database->execSqlCoro(
		"INSERT INTO arena_battle (attacker_id, defender_id, is_bot, enemy_points, enemy_team,"
		" enemy_party, enemy_units, enemy_message, my_party, my_units, bonus_type, bonus_param, created_at)"
		" VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13);",
		identity.userId, enemy->userId, enemy->isBot ? 1 : 0, enemy->points, enemy->team,
		enemy->party, enemy->units, enemy->message,
		myParty, myUnits,
		deck.bonus_type.value_or(""), deck.bonus_param.value_or(""), gme::arena::nowSeconds());
	co_await database->execSqlCoro(
		"UPDATE user_arena SET offer_user_id = '' WHERE user_id = $1;", identity.userId);

	const auto battle = co_await findBattle(database, identity.userId, static_cast<int64_t>(inserted.insertId()));
	if (!battle)
		co_return HandleResult::refuseToHome("The battle could not be started. Please try again.");

	::ArenaBattleStartResp resp{};
	resp.enemy_info = { enemyInfoRow(*battle) };
	resp.team_info = co_await teamInfo(database, identity);
	// Both sides fight on AI; without these rows nobody casts a BB.
	resp.ais = MissionArchiver::unitAiRows();
	LOG_INFO << "ArenaBattleStart: battle " << inserted.insertId() << " " << identity.userId
	         << " vs " << enemy->userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ArenaBattleRestart)
{
	(void)session;
	const auto req = parseRequest<::ArenaPlainReq>(json, "ArenaBattleRestart");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	const auto battle = co_await findBattle(database, identity.userId, std::nullopt);
	if (!battle)
		co_return HandleResult::refuseToHome("This Arena battle has already ended.");

	::ArenaBattlePartyInfo mine{};
	mine.party = (*battle)["my_party"].as<std::string>();
	mine.units = (*battle)["my_units"].as<std::string>();

	::ArenaBattleStartResp resp{};
	resp.enemy_info = { enemyInfoRow(*battle) };
	resp.party_info = std::vector<::ArenaBattlePartyInfo>{ std::move(mine) };
	resp.team_info = co_await teamInfo(database, identity);
	resp.ais = MissionArchiver::unitAiRows();
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

HANDLEF(ArenaBattleEnd)
{
	(void)session;
	const auto req = parseRequest<::ArenaBattleEndReq>(json, "ArenaBattleEnd");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	const auto entry = (req.result && !req.result->empty()) ? req.result->front() : ::ArenaBattleEndEntry{};
	const auto battleId = parseInt(entry.battle_id);
	const auto battle = battleId ? co_await findBattle(database, identity.userId, battleId) : std::nullopt;
	if (!battle)
		co_return HandleResult::refuseToHome("This Arena battle could not be found.");

	// Settled already: answer with the stored reply so a resend pays nothing,
	// and with the badges as they are now (the stored copy has none).
	if ((*battle)["status"].as<int32_t>() != 0)
	{
		const auto stored = (*battle)["result_json"].as<std::string>();
		if (!stored.empty())
			co_return HandleResult::success(co_await gme::withCurrentBadge(database, identity, stored));
		co_return HandleResult::refuseToHome("This Arena battle has already ended.");
	}

	BattleStats stats{};
	stats.win = entry.result.value_or("0") == "1";
	stats.bCrystal = parseCount(entry.b_crystal_cnt, kMaxCrystals);
	stats.hCrystal = parseCount(entry.h_crystal_cnt, kMaxCrystals);
	stats.maxTurnDamage = parseCount(entry.max_turn_damage, kMaxTurnStat);
	stats.maxTurnSpark = parseCount(entry.max_turn_spark, kMaxTurnStat);
	stats.defeatUnits = parseCount(entry.defeat_unit_cnt, kMaxDefeatUnits);
	stats.challenge = gme::challenge::progressFromArenaEnd(json, stats.win);

	auto body = co_await settleBattle(database, identity, *battle, stats, stats.win ? 1 : 2);
	if (body.empty())
	{
		// Lost a race with a concurrent settle; that one stored the reply.
		const auto again = co_await findBattle(database, identity.userId, battleId);
		body = again ? (*again)["result_json"].as<std::string>() : std::string{};
		if (!body.empty())
			body = co_await gme::withCurrentBadge(database, identity, std::move(body));
	}
	if (body.empty())
		co_return HandleResult::refuseToHome("This Arena battle has already ended.");
	co_return HandleResult::success(body);
}

HANDLEF(ArenaRetire)
{
	(void)session;
	const auto req = parseRequest<::ArenaBattleEndReq>(json, "ArenaRetire");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	// Boot path: whatever happens here, login must go on.
	try
	{
		const auto entry = (req.result && !req.result->empty()) ? req.result->front() : ::ArenaBattleEndEntry{};
		const auto battleId = parseInt(entry.battle_id);
		const auto battle = battleId ? co_await findBattle(database, identity.userId, battleId) : std::nullopt;
		if (battle && (*battle)["status"].as<int32_t>() == 0)
			co_await settleBattle(database, identity, *battle, BattleStats{}, 3);
		co_return HandleResult::success(co_await standingBody(database, identity));
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "ArenaRetire: " << ex.what();
	}
	co_return HandleResult::success("{}");
}

HANDLEF(ArenaFriendList)
{
	(void)session;
	const auto req = parseRequest<::ArenaPlainReq>(json, "ArenaFriendList");
	const auto database = theDb();
	const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();

	// Tab mapping is UNVERIFIED (ArenaFriendListScene switches getList(0/1)):
	// the global top 50 goes out as type 0 and the friends as type 1.
	// Both lists hold the player's own pool only (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await gme::sandbox::isOn(database, identity.userId);
	const auto pool = gme::sandbox::poolSql("i.id", sandboxed);
	::ArenaFriendListResp resp{};
	const auto global = co_await database->execSqlCoro(
		std::string(kRankingSelect) +
		" WHERE i.id != $1 AND a.user_id IS NOT NULL" + pool + " ORDER BY points DESC, i.id LIMIT 50;",
		identity.userId);
	int32_t position = 0;
	for (const auto& row : global)
		resp.list.push_back(co_await rankingRow(database, row, 0, ++position));

	const auto friends = co_await database->execSqlCoro(
		std::string(kRankingSelect) +
		" WHERE i.id IN (SELECT friend_id FROM user_friends WHERE user_id = $1)" + pool +
		" ORDER BY points DESC, i.id LIMIT 100;",
		identity.userId);
	position = 0;
	for (const auto& row : friends)
		resp.list.push_back(co_await rankingRow(database, row, 1, ++position));

	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
