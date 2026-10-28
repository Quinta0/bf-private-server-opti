#pragma once

#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <cstdlib>
#include <optional>
#include <string>

// Shared plumbing for the round-2 raid field and battle handlers
// (RaidField.cpp, RaidBattle.cpp).
//
// REFUSALS IN THE FIELD ARE RETRIES.  Every classic raid scene's noticeOK
// calls RaidSceneBase::connectErrorInit @0x19CF898, which re-enters the raid
// through scene 1201: RaidUserInfo, then RaidInitScene resumes the login room
// (or lands on the world list once the server has blanked it).  That is the
// recovery path the client was built around, so every refusal below is
// HandleResult::refuseToRetry and leaves the server consistent with what the
// next RaidUserInfo will say (RAIDS_ROUND2_CORE.md 1.2).
namespace gme::raid::handlers
{

/*! The caller, the loose body and the run it names. */
struct FieldCall
{
	UserIdentity identity;
	RaidRequest req;
	std::optional<RaidRun> run;
};

inline drogon::Task<FieldCall> openFieldCall(const std::string& json)
{
	::RaidBareReq bare{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(bare, json, ctx); ec)
		LOG_WARN << "Raid field: login parse error: " << glz::format_error(ec, json);
	FieldCall call{ (co_await gme::getUserIdentity(theDb(), bare.login_info)).nonEmpty(), RaidRequest(json), {} };
	call.run = co_await requestRun(theDb(), call.req, call.identity.userId);
	if (call.run)
	{
		co_await theDb()->execSqlCoro(
			"UPDATE raid_room_member SET last_seen = $1 WHERE room_id = $2 AND user_id = $3;",
			nowSeconds(), call.run->roomId, call.identity.userId);
	}
	co_return call;
}

/*! A lobby-side request: the caller, the loose body and the room it names (or the login room). */
struct RoomCall
{
	UserIdentity identity;
	RaidRequest req;
	std::string roomId;
};

inline drogon::Task<RoomCall> openRoomCall(const std::string& json)
{
	::RaidBareReq bare{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(bare, json, ctx); ec)
		LOG_WARN << "Raid room: login parse error: " << glz::format_error(ec, json);
	RoomCall call{ (co_await gme::getUserIdentity(theDb(), bare.login_info)).nonEmpty(), RaidRequest(json), {} };
	call.roomId = call.req.text("8iBuZa3n", "8VYd6xSX");
	if (call.roomId.empty())
		call.roomId = (co_await loadRaidUser(theDb(), call.identity.userId)).login_room_id;
	// MULTIPLAYER 3.3 rule 1: any raid request naming a room refreshes the
	// caller's presence in it.
	if (!call.roomId.empty())
		co_await theDb()->execSqlCoro(
			"UPDATE raid_room_member SET last_seen = $1 WHERE room_id = $2 AND user_id = $3;",
			nowSeconds(), call.roomId, call.identity.userId);
	co_return call;
}

/*!
* May `userId` set out into the room's running quest now (ReadyUp 2, then
* their own MissionStart)?  "" = yes.  Either the master started it with
* them ready (mission_status already 1), or the room takes mid-quest joins and
* the window (kMidwayJoinWindowSec) is still open.
*/
inline drogon::Task<std::string> midwayRefusal(
	const db::Database database, const std::string roomId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT r.status, r.midway_joinable, r.mission_started_at, r.mission_id, m.mission_status"
		" FROM raid_room r JOIN raid_room_member m ON m.room_id = r.room_id"
		" WHERE r.room_id = $1 AND m.user_id = $2 AND m.kicked = 0;",
		roomId, userId);
	if (rows.empty() || rows[0]["status"].as<int32_t>() != kRoomInField)
		co_return "room " + roomId + " is not on a quest";
	const auto run = co_await roomRun(database, roomId);
	if (!run || run->result != kRunRunning)
		co_return "the quest of room " + roomId + " is over";
	// A member who retired from this run cannot ready back into it: their
	// MissionStart would be refused ("You have left this raid") forever.
	const auto left = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_mission_user WHERE room_mission_id = $1 AND user_id = $2 AND ended = 1;",
		run->id, userId);
	if (!left.empty())
		co_return userId + " already left run " + std::to_string(run->id);
	const auto& row = rows[0];
	if (row["mission_status"].as<int32_t>() == kRunRunning)
		co_return std::string{};
	const auto mission = findMission(row["mission_id"].as<std::string>());
	const auto user = co_await loadRaidUser(database, userId);
	if (row["midway_joinable"].as<int32_t>() != 1
		|| nowSeconds() >= row["mission_started_at"].as<int64_t>() + kMidwayJoinWindowSec
		|| !mission || mission->rc_id > user.rc)
		co_return userId + " may not join room " + roomId + "'s quest midway";
	co_return std::string{};
}

/*!
* By value on purpose: the generated bool-as-string fields
* (pkgen_glaze_helpers bool_as_string) bind a non-const reference, so glaze
* cannot write a const object that carries one (MonsterMst does).
*/
template <class T>
std::optional<std::string> serialize(const char* name, T resp)
{
	std::string buffer{};
	if (const auto ec = glz::write_json(resp, buffer); ec)
	{
		LOG_ERROR << name << ": serialization error: " << glz::format_error(ec, buffer);
		return std::nullopt;
	}
	return buffer;
}

template <class T>
HandleResult reply(const char* name, const T& resp, const std::string& extra = {})
{
	const auto buffer = serialize(name, resp);
	if (!buffer)
		return HandleResult::error("Serialization error", name);
	return HandleResult::success(extra.empty() ? *buffer : mergeJson(*buffer, extra));
}

/*!
* RaidUserUnitInfo (CEVQ4p7A) for the caller's own party units of `runId`,
* as a JSON object to splice into a reply ("" when there is nothing to send).
*
* WITHOUT IT EVERY RAID BATTLE CRASHES.  While RaidInfo::isQuest is set,
* UserUnitInfoList::shared @0x12B8B44 returns RaidUserUnitInfoList instead of
* the account's unit list, and PlayerParty::entryRaid @0x10C7024 looks each
* own wc2DiMY1 row up there by user unit id and passes the result unchecked
* to PlayerUnit::initialize (SIGSEGV in BattleUnit::initializeBaseStatus,
* seen on emulator-5558).  RaidUserUnitInfoResponse::readParam reads the
* UserUnitInfo key set (resp_keys.tsv: CEVQ4p7A), so the login read's rows
* (readUserUnits) are what it gets.
*/
inline drogon::Task<std::string> raidUnitsGroup(const UserIdentity identity, const int64_t runId)
{
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT user_unit_id FROM raid_mission_party WHERE room_mission_id = $1 AND user_id = $2 AND friend_id = ''"
		" ORDER BY disp_order;",
		runId, identity.userId);
	std::vector<uint32_t> ids;
	for (const auto& row : rows)
	{
		const auto id = std::strtoul(row["user_unit_id"].as<std::string>().c_str(), nullptr, 10);
		if (id > 0)
			ids.push_back(static_cast<uint32_t>(id));
	}
	auto units = co_await gme::readUserUnits(theDb(), identity, ids);   // not const: glaze binds bool-as-string fields by reference
	if (units.empty())
		co_return std::string{};
	std::string list{};
	if (glz::write_json(units, list))
		co_return std::string{};
	co_return "{\"CEVQ4p7A\":" + list + "}";
}

inline HandleResult noRun(const char* name, const std::string& userId)
{
	return HandleResult::refuseToRetry("This raid has already ended.",
		std::string(name) + ": no live run for " + userId);
}

} // namespace gme::raid::handlers
