#include "App.hpp"

#include <gimuserver/gme/common/RaidChat.hpp>
#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidRooms.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdlib>

namespace
{

/*!
* One 9PS8BLiC row (RaidRoomPartyInfoResponse::readParam @0x144077C).  Every
* value as a string, as the client's own JsonNode::addParam writes them; the
* summoner-avatar keys it also reads are left out (it keeps its defaults).
*/
struct RoomPartyRow
{
	std::string user_id, user_unit_id, unit_id, img_type, deck_no, member_type, disp_order, unit_lv,
		base_hp, add_hp, ext_hp, sphere_1, sphere_2, unit_type_id, friend_user_id;
};

} // namespace

template <>
struct glz::meta<RoomPartyRow>
{
	using T = RoomPartyRow;
	static constexpr auto value = object(
		"h7eY3sAK", &T::user_id, "edy7fq3L", &T::user_unit_id, "pn16CNah", &T::unit_id,
		"2pAyFjmZ", &T::img_type, "zsiAn9P1", &T::deck_no, "gr48vsdJ", &T::member_type,
		"XuJL4pc5", &T::disp_order, "4A6LzBxr", &T::unit_lv, "e7DK0FQT", &T::base_hp,
		"cuIWp89g", &T::add_hp, "TokWs1B3", &T::ext_hp, "Ge8Yo32T", &T::sphere_1,
		"mZA7fH2v", &T::sphere_2, "nBTx56W9", &T::unit_type_id, "98WfKiyA", &T::friend_user_id);
};

namespace gme::raid
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (see Raids.cpp): every statement
// below numbers its $N left to right and never repeats one.

namespace
{

/*! Text of a helper field whether the client quoted it or not. */
std::string helperText(const glz::generic& node, const char* key)
{
	if (!node.is_object() || !node.contains(key))
		return {};
	const auto& value = node[key];
	if (value.is_string())
		return value.get<std::string>();
	if (value.is_number())
		return std::to_string(static_cast<int64_t>(value.get<double>()));
	return {};
}

int32_t helperInt(const glz::generic& node, const char* key)
{
	const auto text = helperText(node, key);
	return text.empty() ? 0 : std::atoi(text.c_str());
}

/*!
* Fill the room row's friend columns from the stored quick-start helper.  The
* helper arrives as a FriendInfo group (ZX7gzQ6s) and is kept verbatim, so this
* reads it back loosely: numbers may come quoted or not.
*/
void applyHelper(::RaidRoomInfo& row, const std::string& helperJson)
{
	if (helperJson.empty())
		return;
	glz::generic parsed{};
	if (glz::read_json(parsed, helperJson))
		return;
	const glz::generic* node = &parsed;
	if (parsed.is_array())
	{
		const auto& list = parsed.get_array();
		if (list.empty())
			return;
		node = &list.front();
	}
	row.friend_user_id = helperText(*node, "h7eY3sAK");
	row.friend_user_name = helperText(*node, "B5JQyV8j");
	row.friend_user_lv = helperInt(*node, "2Fh3J7ng");
	row.friend_leader_unit_id = helperText(*node, "pn16CNah");
	row.friend_leader_unit_lv = helperInt(*node, "4A6LzBxr");
	row.friend_unit_img_type = helperInt(*node, "2pAyFjmZ");
}

/*!
* The master is gone: hand the room to the earliest-joined member who has not
* been kicked, or close it when nobody is left.  RaidUtils::roomInfoUpDate
* @0x11CC208 reports the change and roomMasterCheck @0x19CA92C shows the new
* master RAID_ROOM_MASTER_CHANGE, so promotion is a supported event.
*/
drogon::Task<void> handOver(const db::Database database, const std::string& roomId)
{
	const auto next = co_await database->execSqlCoro(
		"SELECT user_id FROM raid_room_member WHERE room_id = $1 AND kicked = 0 ORDER BY joined_at, user_id LIMIT 1;",
		roomId);
	if (next.empty())
	{
		co_await closeRoom(database, roomId);
		co_return;
	}
	const auto masterId = next[0]["user_id"].as<std::string>();
	co_await database->execSqlCoro(
		"UPDATE raid_room SET master_user_id = $1, updated_at = $2 WHERE room_id = $3;",
		masterId, nowSeconds(), roomId);
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET ready = 0 WHERE room_id = $1 AND user_id = $2;", roomId, masterId);
	LOG_INFO << "Raid: room " << roomId << " handed to " << masterId;
}

/*!
* Drop lobby members silent for kLobbyAbsentSec.  Members on a run are the
* run's business (its own time-out), and kicked rows stay as the rejoin block.
*/
drogon::Task<void> dropAbsent(const db::Database database, const std::string& roomId)
{
	// A member holding an uncollected result (mission_status 2..4) is kept
	// much longer, so a player who stepped away during the fight can still
	// come back to the result scene and collect it (settleRoomAfterRun).
	const auto absent = co_await database->execSqlCoro(
		"SELECT user_id FROM raid_room_member WHERE room_id = $1 AND kicked = 0"
		" AND ((mission_status = 0 AND last_seen < $2) OR (mission_status >= $3 AND last_seen < $4));",
		roomId, nowSeconds() - kLobbyAbsentSec, kRunWon, nowSeconds() - kUncollectedHoldSec);
	for (const auto& row : absent)
	{
		const auto userId = row["user_id"].as<std::string>();
		co_await database->execSqlCoro(
			"DELETE FROM raid_room_member WHERE room_id = $1 AND user_id = $2;", roomId, userId);
		co_await forgetRoom(database, userId, roomId);
		LOG_INFO << "Raid: " << userId << " dropped from room " << roomId << " (absent)";
	}
}

/*! Seconds left to join the room's running quest midway, 0 when it cannot be joined. */
int32_t midwayRest(int32_t status, int32_t midwayJoinable, int64_t startedAt)
{
	if (status != kRoomInField || midwayJoinable != 1)
		return 0;
	return static_cast<int32_t>(std::clamp<int64_t>(startedAt + kMidwayJoinWindowSec - nowSeconds(), 0,
		kMidwayJoinWindowSec));
}

drogon::Task<::RaidRoomUserInfo> memberRow(
	const db::Database database, const drogon::orm::Row& member, const std::string& roomId,
	const std::string& missionId, const int32_t runId)
{
	const auto memberId = member["user_id"].as<std::string>();
	const auto profile = co_await loadProfile(database, memberId);
	const auto raider = co_await loadRaidUser(database, memberId);
	::RaidRoomUserInfo out{};
	out.room_id = roomId;
	out.user_id = memberId;
	out.user_name = profile.name;
	out.leader_unit_id = profile.leader_unit_id;
	out.rc = raider.rc;
	out.user_lv = profile.level;
	out.aube = raider.aube;
	out.ready = member["ready"].as<int32_t>();
	out.mission_status = member["mission_status"].as<int32_t>();
	// questStartInfoCheck @0x19CABD0 needs a non-empty CurrentRoomMissionID
	// while the member is in the run; every later request echoes it as ja39HMLq.
	// A member still holding an older run's result (the room moved on while
	// they were away) is given THAT run, so their MissionEnd collects it.
	out.current_room_mission_id = out.mission_status != 0 ? runId : 0;
	if (out.mission_status >= kRunWon)
	{
		const auto own = co_await database->execSqlCoro(
			"SELECT u.room_mission_id FROM raid_mission_user u JOIN raid_room_mission m"
			" ON m.room_mission_id = u.room_mission_id WHERE m.room_id = $1 AND u.user_id = $2 AND u.ended = 0"
			" AND m.result != $3 ORDER BY u.room_mission_id DESC LIMIT 1;",
			roomId, memberId, kRunRunning);
		if (!own.empty())
			out.current_room_mission_id = static_cast<int32_t>(own[0]["room_mission_id"].as<int64_t>());
	}
	out.kicked_flg = member["kicked"].as<int32_t>();
	out.active_status = 1;
	out.mission_id = missionId;
	out.guild_id = profile.guild_id;
	co_return out;
}

} // namespace

drogon::Task<void> forgetRoom(const db::Database database, const std::string userId, const std::string roomId)
{
	co_await database->execSqlCoro(
		"UPDATE raid_user SET login_world_id = '', login_room_id = '' WHERE user_id = $1 AND login_room_id = $2;",
		userId, roomId);
}

drogon::Task<void> touchLoginRoom(const db::Database database, const std::string userId)
{
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET last_seen = $1 WHERE user_id = $2"
		" AND room_id = (SELECT login_room_id FROM raid_user WHERE user_id = $3);",
		nowSeconds(), userId, userId);
}

int32_t roomCapacity(const std::string& missionId)
{
	const auto mission = findMission(missionId);
	return mission && mission->unk1 > 0 ? mission->unk1 : kDefaultRoomCapacity;
}

drogon::Task<bool> roomIsLive(const db::Database database, const std::string roomId)
{
	if (roomId.empty())
		co_return false;
	// A battle sends nothing between turns and a finished run must stay
	// resumable until its MissionEnd is collected, so a room on a run is live
	// whatever its members' last_seen says.
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_room r WHERE r.room_id = $1 AND r.status != $2"
		" AND (r.mission_start_flg = 1 OR EXISTS (SELECT 1 FROM raid_room_member m"
		" WHERE m.room_id = r.room_id AND m.last_seen >= $3));",
		roomId, kRoomClosed, nowSeconds() - kRoomIdleSec);
	co_return !rows.empty();
}

drogon::Task<bool> roomInGrace(const db::Database database, const std::string roomId, const std::string userId)
{
	// RaidFieldQuestResultConScene sends RaidRoomInfo right after MissionEnd
	// and only then moves on to the result scenes.
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_room r JOIN raid_room_member m ON m.room_id = r.room_id"
		" WHERE r.room_id = $1 AND r.status = $2 AND r.closed_at >= $3 AND m.user_id = $4;",
		roomId, kRoomClosed, nowSeconds() - kClosedRoomGraceSec, userId);
	co_return !rows.empty();
}

drogon::Task<void> closeRoom(const db::Database database, const std::string roomId)
{
	co_await database->execSqlCoro(
		"UPDATE raid_room SET status = $1, mission_start_flg = 0, updated_at = $2, closed_at = $3"
		" WHERE room_id = $4;",
		kRoomClosed, nowSeconds(), nowSeconds(), roomId);
	co_await database->execSqlCoro(
		"UPDATE raid_room_mission SET result = $1, ended_at = $2 WHERE room_id = $3 AND result = $4;",
		kRunLost, nowSeconds(), roomId, kRunRunning);
	co_await database->execSqlCoro("DELETE FROM raid_room_member WHERE room_id = $1;", roomId);
	co_await database->execSqlCoro(
		"UPDATE raid_user SET login_world_id = '', login_room_id = '' WHERE login_room_id = $1;", roomId);
	co_await forgetRoomChat(database, roomId);
	LOG_INFO << "Raid: room " << roomId << " closed";
}

drogon::Task<bool> isActiveMember(const db::Database database, const std::string roomId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_room_member WHERE room_id = $1 AND user_id = $2 AND kicked = 0;", roomId, userId);
	co_return !rows.empty();
}

drogon::Task<void> pruneRoom(const db::Database database, const std::string roomId)
{
	const auto rooms = co_await database->execSqlCoro(
		"SELECT master_user_id, quick_start, status, current_room_mission_id FROM raid_room"
		" WHERE room_id = $1 AND status != $2;",
		roomId, kRoomClosed);
	if (rooms.empty() || rooms[0]["quick_start"].as<int32_t>() == 1)
		co_return;

	co_await dropAbsent(database, roomId);
	if (!(co_await isActiveMember(database, roomId, rooms[0]["master_user_id"].as<std::string>())))
		co_await handOver(database, roomId);
	if (rooms[0]["status"].as<int32_t>() == kRoomInField)
		co_await settleRoomAfterRun(database, roomId, rooms[0]["current_room_mission_id"].as<int64_t>());
}

drogon::Task<void> leaveRoom(const db::Database database, const std::string userId, const std::string roomId)
{
	co_await forgetRoom(database, userId, roomId);
	const auto room = co_await database->execSqlCoro(
		"SELECT master_user_id, quick_start FROM raid_room WHERE room_id = $1 AND status != $2;",
		roomId, kRoomClosed);
	if (room.empty())
		co_return;
	// Leaving ends the player's part in the room's run: a stale ended = 0 row
	// would keep the run open after the last real player retired.
	co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET ended = 1, battle_serial = 0 WHERE room_mission_id ="
		" (SELECT current_room_mission_id FROM raid_room WHERE room_id = $1) AND user_id = $2 AND ended = 0;",
		roomId, userId);
	// A kicked member's row stays as the rejoin block (4.10); only the login
	// ids above go.
	const auto gone = co_await database->execSqlCoro(
		"DELETE FROM raid_room_member WHERE room_id = $1 AND user_id = $2 AND kicked = 0;", roomId, userId);
	if (gone.affectedRows() == 0)
		co_return;   // not (or no longer) in it: someone else's room stays as it is

	// A solo room has nobody to hand over to.
	if (room[0]["quick_start"].as<int32_t>() == 1)
	{
		co_await closeRoom(database, roomId);
		co_return;
	}
	if (const auto run = co_await roomRun(database, roomId); run && run->result == kRunRunning)
	{
		const auto left = co_await database->execSqlCoro(
			"SELECT COUNT(*) AS n FROM raid_mission_user WHERE room_mission_id = $1 AND ended = 0;", run->id);
		if (left[0]["n"].as<int32_t>() == 0)
			co_await finishRun(database, *run, kRunLost);
	}
	if (room[0]["master_user_id"].as<std::string>() == userId)
		co_await handOver(database, roomId);
}

drogon::Task<std::optional<::RaidRoomInfo>> roomRow(
	const db::Database database, const std::string roomId, const std::string viewerId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT r.world_id, r.master_user_id, r.mission_id, r.quick_start, r.playstyle_id, r.comment, r.password,"
		" r.min_rc, r.min_lv, r.kick_flg, r.midway_joinable, r.quick_matching, r.helper_json, r.status,"
		" r.mission_start_flg, r.mission_started_at,"
		" (SELECT COUNT(*) FROM raid_room_member m WHERE m.room_id = r.room_id AND m.kicked = 0) AS members"
		" FROM raid_room r WHERE r.room_id = $1;",
		roomId);
	if (rows.empty())
		co_return std::nullopt;
	const auto& room = rows[0];
	const auto masterId = room["master_user_id"].as<std::string>();
	const auto master = co_await loadProfile(database, masterId);

	::RaidRoomInfo row{};
	row.room_id = roomId;
	row.world_id = room["world_id"].as<std::string>();
	row.master_user_id = masterId;
	row.master_user_name = master.name;
	row.master_user_lv = master.level;
	row.master_leader_unit_id = master.leader_unit_id;
	row.master_leader_unit_lv = master.leader_unit_lv;
	row.playstyle_id = room["playstyle_id"].as<std::string>();
	row.mission_id = room["mission_id"].as<std::string>();
	row.member_num = room["members"].as<int32_t>();
	row.quick_start_flg = room["quick_start"].as<int32_t>();
	row.quick_matching_flg = room["quick_matching"].as<int32_t>();
	row.midway_joinable_flg = room["midway_joinable"].as<int32_t>();
	row.kick_flg = room["kick_flg"].as<int32_t>();
	row.min_rc = room["min_rc"].as<int32_t>();
	row.min_lv = std::to_string(room["min_lv"].as<int32_t>());
	row.comment = room["comment"].as<std::string>();
	// The client reads the password only in setSelRoomData, the master's edit
	// form: nobody else is ever sent it.
	const auto password = room["password"].as<std::string>();
	row.password = viewerId == masterId ? password : std::string{};
	row.password_flg = password.empty() ? 0 : 1;
	row.dissolve_time = kRoomIdleSec;

	const auto status = room["status"].as<int32_t>();
	const bool inField = status == kRoomInField || room["mission_start_flg"].as<int32_t>() == 1;
	row.mission_start_flg = inField ? 1 : 0;
	row.mission_joinable_rest_time = midwayRest(status, row.midway_joinable_flg,
		room["mission_started_at"].as<int64_t>());
	const bool full = row.member_num >= roomCapacity(row.mission_id);
	const bool open = status != kRoomClosed && row.quick_start_flg == 0 && !full;
	row.enterable_flg = open && (!inField || row.mission_joinable_rest_time > 0) ? 1 : 0;
	applyHelper(row, room["helper_json"].as<std::string>());
	co_return row;
}

drogon::Task<::RaidRoomResp> roomResp(const db::Database database, const std::string userId, const std::string roomId)
{
	::RaidRoomResp resp{};
	// This request is the viewer's own heartbeat: refresh it BEFORE the
	// absence rule runs, or a player back from party edit drops themself.
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET last_seen = $1 WHERE room_id = $2 AND user_id = $3;",
		nowSeconds(), roomId, userId);
	co_await pruneRoom(database, roomId);
	// Only members see a room (4.1 rule 1).  Kicked rows count: the expelled
	// player keeps getting the room with their own D2ZKtGR9 row (rule 2).
	if (!(co_await isRoomMember(database, roomId, userId)))
		co_return resp;
	if (!(co_await roomIsLive(database, roomId)) && !(co_await roomInGrace(database, roomId, userId)))
		co_return resp;
	// Applies a pending time-out before the member rows are read, so a
	// resuming player sees MissionStatus 4 rather than a run that is over.
	co_await roomRun(database, roomId);

	auto row = co_await roomRow(database, roomId, userId);
	if (!row)
		co_return resp;
	const auto run = co_await database->execSqlCoro(
		"SELECT current_room_mission_id FROM raid_room WHERE room_id = $1;", roomId);
	const auto runId = static_cast<int32_t>(run[0]["current_room_mission_id"].as<int64_t>());
	// Master first, then join order (RaidRoomUserInfoResponse keeps slots by
	// user id, so order only places newcomers).  A kicked row goes to the
	// kicked player alone, who needs D2ZKtGR9 to see the expel notice.
	const auto members = co_await database->execSqlCoro(
		"SELECT user_id, ready, kicked, mission_status FROM raid_room_member WHERE room_id = $1"
		" AND (kicked = 0 OR user_id = $2) ORDER BY CASE WHEN user_id = $3 THEN 0 ELSE 1 END, joined_at;",
		roomId, userId, row->master_user_id);
	for (const auto& member : members)
		resp.members.push_back(co_await memberRow(database, member, roomId, row->mission_id, runId));
	resp.rooms.push_back(std::move(*row));
	co_return resp;
}

drogon::Task<std::string> roomPartyGroup(const db::Database database, const std::string roomId)
{
	// A member on the room's run shows the party they actually fielded
	// (raid_mission_party, helper included): in the field RaidUnitScene::
	// setUnitInfoParty @0x19E8800 matches every row against the client's
	// RaidUserPartyDeckInfoList by (user id, user unit id) and drops the rest.
	// Anyone else shows their active raid squad (deck type 3, the squad
	// user_info.active_deck names, as RaidUserInfo reports it).
	const auto rows = co_await database->execSqlCoro(
		"SELECT m.user_id, d.deck_num, d.member_type, d.disp_order, u.user_unit_id, u.unit_id, u.unit_lvl,"
		" u.base_hp, u.add_hp, u.ext_hp, u.eqip_item_id, u.eqip_item_id2, u.unit_type_id, '' AS friend_id,"
		" m.joined_at FROM raid_room_member m JOIN user_info i ON i.id = m.user_id"
		" JOIN user_decks d ON d.user_id = m.user_id AND d.deck_type = $1 AND d.deck_num = i.active_deck"
		" JOIN user_units u ON u.user_id = d.user_id AND u.user_unit_id = d.user_unit_id"
		" WHERE m.room_id = $2 AND m.kicked = 0 AND NOT EXISTS (SELECT 1 FROM raid_mission_party p"
		" JOIN raid_room r ON r.room_id = m.room_id WHERE p.room_mission_id = r.current_room_mission_id"
		" AND p.user_id = m.user_id)"
		" UNION ALL"
		" SELECT m.user_id, 0, p.member_type, p.disp_order, p.user_unit_id, p.unit_id, p.unit_lv,"
		" p.max_hp, 0, 0, 0, 0, p.unit_type_id, p.friend_id, m.joined_at"
		" FROM raid_room_member m JOIN raid_room r ON r.room_id = m.room_id"
		" JOIN raid_mission_party p ON p.room_mission_id = r.current_room_mission_id AND p.user_id = m.user_id"
		" WHERE m.room_id = $3 AND m.kicked = 0 ORDER BY 15, 1, 4;",
		kRaidDeckType, roomId, roomId);
	std::vector<RoomPartyRow> party;
	for (const auto& row : rows)
	{
		const auto text = [&](const char* column) { return row[column].as<std::string>(); };
		party.push_back({ text("user_id"), text("user_unit_id"), text("unit_id"), "0", text("deck_num"),
			text("member_type"), text("disp_order"), text("unit_lvl"), text("base_hp"), text("add_hp"),
			text("ext_hp"), text("eqip_item_id"), text("eqip_item_id2"), text("unit_type_id"), text("friend_id") });
	}
	std::string list{};
	if (party.empty() || glz::write_json(party, list))
		co_return std::string{};
	co_return "{\"9PS8BLiC\":" + list + "}";
}

drogon::Task<void> retireQuickRoom(const db::Database database, const std::string roomId)
{
	co_await database->execSqlCoro(
		"UPDATE raid_room SET status = $1, mission_start_flg = 0, updated_at = $2, closed_at = $3"
		" WHERE room_id = $4;",
		kRoomClosed, nowSeconds(), nowSeconds(), roomId);
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET mission_status = 0 WHERE room_id = $1;", roomId);
	co_await database->execSqlCoro(
		"UPDATE raid_user SET login_world_id = '', login_room_id = '' WHERE login_room_id = $1;", roomId);
}

drogon::Task<int32_t> memberMissionStatus(const db::Database database, const std::string roomId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT mission_status FROM raid_room_member WHERE room_id = $1 AND user_id = $2 AND kicked = 0;",
		roomId, userId);
	co_return rows.empty() ? -1 : rows[0]["mission_status"].as<int32_t>();
}

drogon::Task<int32_t> sendReadyMembers(const db::Database database, const std::string roomId, const std::string masterId)
{
	// A member with no raid orb is left in the lobby: their own MissionStart
	// would be refused.  questStartStand @0x19DC530 moves a non-master whose row says
	// MissionStatus 1 to RaidFieldReady, which then sends their own
	// MissionStart: flipping the row is all the server does for them.
	const auto sent = co_await database->execSqlCoro(
		"UPDATE raid_room_member SET mission_status = $1 WHERE room_id = $2 AND user_id != $3"
		" AND kicked = 0 AND ready = 1 AND mission_status = 0"
		" AND NOT EXISTS (SELECT 1 FROM raid_user u WHERE u.user_id = raid_room_member.user_id"
		" AND u.aube < 1 AND NOT (u.aube_ts > 0 AND u.aube_ts <= $4));",
		kRunRunning, roomId, masterId, nowSeconds() - kAubeRecoverSec);
	co_return static_cast<int32_t>(sent.affectedRows());
}

drogon::Task<void> settleRoomAfterRun(const db::Database database, const std::string roomId, const int64_t runId)
{
	const auto run = co_await roomRun(database, roomId);
	if (!run || run->id != runId || run->result == kRunRunning)
		co_return;
	// A member who never comes back must not hold the room on a finished
	// run forever: only members seen recently are waited for.  The absent
	// one still collects later (MissionEnd names its run explicitly).
	const auto pending = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM raid_mission_user u JOIN raid_room_member m"
		" ON m.room_id = $1 AND m.user_id = u.user_id"
		" WHERE u.room_mission_id = $2 AND u.ended = 0 AND m.last_seen >= $3;",
		roomId, runId, nowSeconds() - kLobbyAbsentSec);
	if (pending[0]["n"].as<int32_t>() > 0)
		co_return;
	const auto back = co_await database->execSqlCoro(
		"UPDATE raid_room SET status = $1, mission_start_flg = 0, updated_at = $2 WHERE room_id = $3 AND status = $4;",
		kRoomLobby, nowSeconds(), roomId, kRoomInField);
	if (back.affectedRows() == 0)
		co_return;
	// Members still holding this run's result keep it (2..4: the resume path
	// sends them to the result scene, MissionEnd pays them later); everyone
	// else is back in the lobby.
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET ready = 0, mission_status = CASE WHEN EXISTS (SELECT 1 FROM raid_mission_user u"
		" WHERE u.room_mission_id = $1 AND u.user_id = raid_room_member.user_id AND u.ended = 0)"
		" THEN mission_status ELSE 0 END WHERE room_id = $2;",
		runId, roomId);
	LOG_INFO << "Raid: room " << roomId << " back in the lobby after run " << runId;
}

} // namespace gme::raid
