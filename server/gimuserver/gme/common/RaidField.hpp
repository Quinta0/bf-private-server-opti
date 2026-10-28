#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidBattles.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

#include <optional>
#include <string>
#include <vector>

// Classic Raid round 2, band "core": the field of a running raid mission.
//
// A RUN is one raid_room_mission row: one mission played by the members of one
// room.  Everything the players share (time, result, boss HP) hangs off the
// run, everything personal (point, hearts, open battle, bag, party) off the
// (run, member) pair, so the solo quick start and a later shared room use the
// same code.  As in round 1 nothing ticks: the time-out is derived whenever a
// request reads the run.  Wire shapes and the client flow behind every rule
// are in docs/features/RAIDS_ROUND2_CORE.md.
namespace gme::raid
{

/*! raid_room_mission.result, which is also what g97TWISb.3tWuc7de sends. */
inline constexpr int32_t kRunRunning = 1;
inline constexpr int32_t kRunWon = 2;
inline constexpr int32_t kRunLost = 3;
inline constexpr int32_t kRunTimeUp = 4;
/*! DefineMst raid_max_p: hearts per run (RaidMissionUesrInfo::getPlayerRestMax @0x12F6110). */
inline constexpr int32_t kHeartsMax = 3;
/*! DefineMst raid_bag_count: mission bag slots. */
inline constexpr int32_t kBagCapacity = 50;

/*! One running or finished raid mission. */
struct RaidRun
{
	std::string roomId;
	int64_t id = 0;
	std::string missionId;
	int32_t result = kRunRunning;
	int64_t startedAt = 0;
	int32_t timeLimit = 0;
	bool quick = false;
};

/*! A member's personal state in a run. */
struct RunMember
{
	int32_t nowPoint = 0;
	int32_t dstPoint = 0;
	int32_t hearts = kHeartsMax;
	std::string searchInfo;
	int64_t battleSerial = 0;
	bool battleBoss = false;
	std::string battleBossId;
	int32_t battlePoint = 0;
	int64_t lastEventId = 0;
	bool ended = false;
	std::string drops;
	std::string resultJson;
};

/*! One wc2DiMY1 row as the client sends it. */
struct PartyRow
{
	std::string userUnitId;
	std::string unitId;
	std::string friendId;
	std::string unitTypeId;
	int32_t unitLv = 1;
	int32_t memberType = 1;
	int32_t dispOrder = 0;
	int32_t imgType = 0;
	int64_t hp = 0;
	int64_t hel = 0;
	int32_t bbGauge = 0;
	int32_t sbGauge = 0;
};

/*! One client event (QP4cwt7n row). */
struct ClientEvent
{
	int64_t id = 0;
	int32_t type = 0;
	std::string params;
};

std::vector<PartyRow> readPartyRows(const RaidRequest& req);
std::vector<ClientEvent> readEvents(const RaidRequest& req);

int64_t nowSeconds();

/*! The mission row behind a run, or nullopt for an unknown id. */
std::optional<::RaidMissionMst> runMission(const RaidRun& run);

/*!
* The room's current run (time-out applied), or nullopt when the room has
* none.  Finished runs are returned too until MissionEnd collects them.
*/
drogon::Task<std::optional<RaidRun>> roomRun(const db::Database database, const std::string roomId);

/*! The run a request names: the room (or the caller's login room) and, when sent, ja39HMLq must match. */
drogon::Task<std::optional<RaidRun>> requestRun(
	const db::Database database, const RaidRequest& req, const std::string userId);

drogon::Task<bool> isRoomMember(const db::Database database, const std::string roomId, const std::string userId);

/*! Open a run of the room's mission: bosses at their start points, room flagged in mission. */
drogon::Task<RaidRun> startRun(const db::Database database, const std::string roomId, const ::RaidMissionMst mission);

/*! Put a member into a run (first MissionStart): start point, full hearts, the party as sent. */
drogon::Task<void> joinRun(
	const db::Database database, const RaidRun run, const std::string userId,
	const std::vector<PartyRow> party, const ::RaidMissionMst mission);

drogon::Task<std::optional<RunMember>> loadMember(const db::Database database, const int64_t runId, const std::string userId);

/*! Finish a run for everyone still in it (won, lost or timed out). */
drogon::Task<void> finishRun(const db::Database database, const RaidRun run, const int32_t result);

/*!
* A run by id, whatever the room's current run is (time-out NOT applied):
* MissionEnd uses it for a member collecting an older run of their room.
*/
drogon::Task<std::optional<RaidRun>> loadRun(const db::Database database, const int64_t runId);

/*! A RaidUserRouteMst route from `from` to `to` in the mission (no Teleport). */
bool hasRoute(const std::string& missionId, int32_t from, int32_t to);

/*! Walk to `target` when a route joins it to the stored point; otherwise keep the stored point. */
drogon::Task<bool> moveTo(const db::Database database, const RaidRun run, const std::string userId, const int32_t target);

/*!
* The member, after taking `point` as their pending walk when it differs from
* the stored one (the client acts on arrival before its next poll reports the
* move).  The move is only taken when a route allows it (moveTo).
*/
drogon::Task<std::optional<RunMember>> catchUpMove(
	const db::Database database, const RaidRun run, const std::string userId, const int32_t point);

/*! Store the HP and gauges the client reports for its own rows. */
drogon::Task<void> saveParty(
	const db::Database database, const RaidRun run, const std::string userId, const std::vector<PartyRow> rows);

/*!
* Apply client events at most once each (boss damage only; items are logged
* elsewhere) and return every received id as an ack.  May finish the run as
* won when the clear-condition bosses are down.
*/
drogon::Task<std::vector<::RaidEventAck>> applyEvents(
	const db::Database database, const RaidRun run, const std::string userId,
	const std::vector<ClientEvent> events);

/*! Defeated monsters one BattleEnd may report (6RxkwBv7 rows and count per row). */
inline constexpr int64_t kMaxKillsPerBattle = 50;

/*!
* Add one closed battle's defeated monsters (BattleEnd zS9s5bEI.6RxkwBv7,
* "monsterId:count,...") to the run's tally, and finish the run as won when
* that meets the mission's clear conditions (type-2 targets count kills).
* The caller guarantees once per battle (the serial guard).
* @return True when this battle won the run.
*/
drogon::Task<bool> recordKills(const db::Database database, const RaidRun run, const std::string userId,
	const std::string defeated);

/*! Boss rows of a run. */
struct RunBoss
{
	std::string missionBossId;
	int32_t bossId = 0;
	int32_t point = 0;
	bool visible = true;
	std::vector<BossPart> parts;
	int64_t bodyHp() const;
};
drogon::Task<std::vector<RunBoss>> loadBosses(const db::Database database, const int64_t runId);

/*! The field state block every field request answers with. */
/*!
* @param withSet Attach the raid item set (Fihk73ZR).  Only where the
* server's copy must win (MissionStart, battle start, item use): the item and
* mix overlays edit the set locally and poll InfoLight meanwhile, and a set in
* that poll would overwrite the unsent edit.
*/
drogon::Task<::RaidFieldResp> fieldResp(const db::Database database, const std::string userId, const RaidRun run,
	const bool withSet = false);

/*! Splice extra top-level groups (a JSON object) into a serialized reply object. */
std::string mergeJson(std::string base, const std::string& extra);

} // namespace gme::raid
