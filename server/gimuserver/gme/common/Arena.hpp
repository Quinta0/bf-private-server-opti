#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Classic Arena state shared by the arena handlers (gme/handlers/Arena.cpp)
// and the login reply (UserInfo / GetPlayerInfo).  Wire shapes and the scene
// flow they serve: docs/features/ARENA_SPEC.md.

namespace gme::arena
{

/*! One npc_mst-style P_* object: every value is a string on the wire. */
using JsonObject = std::map<std::string, std::string>;

/*!
* One player's arena row.  A player who never fought has no row; every reader
* goes through loadStanding, which answers the defaults (rank 1, 0 points)
* instead, so accounts that predate the arena need no backfill.
*/
struct Standing
{
	int32_t rankId = 1;
	int32_t maxRankId = 1;
	int32_t points = 0;
	int32_t battleCnt = 0;
	int32_t winCnt = 0;
	int32_t battleCntAccept = 0;
	int32_t winCntAccept = 0;
	int32_t atkSeriesWin = 0;
	int32_t maxAtkSeriesWin = 0;
	int32_t defSeriesWin = 0;
	int32_t maxDefSeriesWin = 0;
	int32_t defeatUnitCnt = 0;
	int32_t maxTurnDamage = 0;
	int32_t maxTurnSpark = 0;
	int32_t sumBCrystal = 0;
	int32_t sumHCrystal = 0;
	std::string offerUserId;
	int64_t orbTs = 0;
};

/*!
* A side of a fight exactly as the client parses it: three JSON texts in the
* npc_mst P_* format (RivalParty::entry), plus the card the Matching screen
* draws.  Bots and players produce the same shape, so the battle code never
* needs to know which it is fighting.
*/
struct Snapshot
{
	std::string userId;
	std::string name;
	std::string message;
	int32_t level = 1;
	int32_t rankId = 1;
	int32_t points = 0;
	std::string leaderUnitId;
	int32_t leaderImgType = 0;
	int32_t leaderLv = 1;
	std::string team;
	std::string party;
	std::string units;
	bool isBot = false;
};

/*! Seconds per regenerated Arena Orb (DefineMst recover_time_fight, 3600). */
int32_t orbRecoverSeconds();

/*! Unix time now, in seconds. */
int64_t nowSeconds();

/*! The ArenaRankMst row whose point band holds `points` (the top rank above it). */
int32_t rankForPoints(int32_t points);

/*!
* Points for a win.  The original formula was never dumped, so this is ours:
* 100 for an even match, more for beating a stronger player, clamped to
* 50..300.  A loss is 0 and a rank never drops, because the result screens
* only animate rank-ups.
*/
int32_t pointsForWin(int32_t myPoints, int32_t enemyPoints);

/*! Karma for a win: 100 per rank held after the battle. */
int32_t karmaForWin(int32_t rankAfter);

drogon::Task<Standing> loadStanding(db::Database database, const std::string& userId);

/*! Creates the row with defaults if missing.  Every writer calls this first. */
drogon::Task<void> ensureStanding(db::Database database, const std::string& userId);

/*!
* Brings user_info.fight_point up to date with the orbs regenerated since the
* last spend.  The client regenerates orbs on its own clock; without this the
* server would refuse a battle the client shows as affordable.
*/
drogon::Task<void> syncOrbs(db::Database database, const std::string& userId);

/*! Seconds until the next orb (0 when full), for UserTeamInfo.fight_rest_timer. */
drogon::Task<int32_t> orbRestSeconds(db::Database database, const std::string& userId);

/*! 8jBJ7uKR: exactly one row (the parser is a singleton). */
drogon::Task<std::vector<::UserArenaInfo>> loadArenaInfo(db::Database database, const UserIdentity& identity);

/*! PQ56vbkI: exactly one row (the parser is a singleton). */
drogon::Task<std::vector<::UserTeamArenaArchive>> loadArenaArchive(db::Database database, const UserIdentity& identity);

/*!
* Snapshot of a player's arena deck (falls back to the active deck when the
* arena deck is empty).  std::nullopt when the player has no usable deck.
*/
drogon::Task<std::optional<Snapshot>> playerSnapshot(db::Database database, const std::string& userId);

/*!
* P_* unit rows (RivalParty / NpcUnitParseList format) for these of a player's
* units, in the order asked.  Units the player no longer owns are skipped.
* Shared with the Colosseum, whose squads are any units, not one deck.
*/
drogon::Task<std::vector<JsonObject>> unitRows(
	db::Database database, const std::string& userId, const std::vector<uint32_t>& userUnitIds);

/*!
* A P_* unit row for UnitMst `unitId` at `level` (capped at its max level), with
* its own BB and SBB, for generated opponents.  std::nullopt for an unknown unit.
*/
std::optional<JsonObject> mstUnitRow(int32_t unitId, int32_t level, const std::string& userUnitId);

/*! Bot ids are never real user ids: user ids are 8 hex characters. */
bool isBotId(const std::string& userId);

/*! Snapshot of bot `userId`, scaled to the attacker's level; std::nullopt for an unknown id. */
std::optional<Snapshot> botSnapshot(const std::string& userId, int32_t attackerLevel, int32_t attackerPoints);

/*! A random bot id. */
std::string randomBotId();

} // namespace gme::arena
