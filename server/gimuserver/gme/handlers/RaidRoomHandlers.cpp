#include "RaidHandlerSupport.hpp"

#include <gimuserver/gme/common/RaidBlackList.hpp>
#include <gimuserver/gme/common/RaidRoomSearch.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <optional>
#include <utility>

// Classic Raid round 2 -- shared rooms (RAIDS_ROUND2_MULTIPLAYER.md 4.2 to
// 4.6): browse/search/auto-join, make and edit a room, join it, ready up,
// kick.  The lobby heartbeat (RaidRoomInfo) and leaving (RaidExit) stay in
// Raid.cpp with round 1; chat, blacklist and helpers are in
// RaidSocialHandlers.cpp.
//
// Every change that touches more than one row runs in one transaction, and
// the decisive check sits in the same statement as its write (the room-full
// INSERT): two players racing for the last slot cannot both get it.

using namespace gme::raid;
using namespace gme::raid::handlers;

namespace
{

/*! RaidUpDataRoomInfoRequest OpeType (mnZ5K4Ii). */
constexpr int32_t kOpCreate = 1;
constexpr int32_t kOpEdit = 2;
/*! RaidUpDataUserReady values (uX87tAZB). */
constexpr int32_t kReadyMidway = 2;
/*! Room comment cap in code points: the room list row is one short line. */
constexpr size_t kCommentMaxChars = 40;

/*!
* The code point at `i` and its length in bytes, or nullopt for anything that
* is not shortest-form UTF-8 of a scalar value (overlong forms such as C0 BC
* for '<', surrogates, values past U+10FFFF).
*/
std::optional<std::pair<uint32_t, size_t>> utf8At(const std::string& raw, size_t i)
{
	const auto byte = [&](size_t k) { return static_cast<unsigned char>(raw[i + k]); };
	const auto lead = byte(0);
	if (lead < 0x80)
		return std::make_pair(static_cast<uint32_t>(lead), size_t{ 1 });
	size_t len = 0;
	uint32_t cp = 0;
	if (lead >= 0xC2 && lead <= 0xDF) { len = 2; cp = lead & 0x1F; }
	else if (lead >= 0xE0 && lead <= 0xEF) { len = 3; cp = lead & 0x0F; }
	else if (lead >= 0xF0 && lead <= 0xF4) { len = 4; cp = lead & 0x07; }
	else return std::nullopt;
	if (i + len > raw.size())
		return std::nullopt;
	for (size_t k = 1; k < len; ++k)
	{
		if ((byte(k) & 0xC0) != 0x80)
			return std::nullopt;
		cp = (cp << 6) | (byte(k) & 0x3F);
	}
	const uint32_t minimum[] = { 0, 0, 0x80, 0x800, 0x10000 };
	if (cp < minimum[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return std::nullopt;
	return std::make_pair(cp, len);
}

/*!
* The client renders <color=...>/<br> markup in labels, so a comment is
* stripped of '<', '>' and control characters, trimmed and capped -- never
* rejected for its content (4.2).
*/
std::string sanitizeComment(const std::string& raw)
{
	std::string out;
	size_t chars = 0;
	for (size_t i = 0; i < raw.size() && chars < kCommentMaxChars;)
	{
		const auto lead = static_cast<unsigned char>(raw[i]);
		const auto cp = utf8At(raw, i);
		if (!cp)
			break;   // not UTF-8: keep what was valid so far
		const size_t len = cp->second;
		// C0, DEL and C1 controls, and the line/paragraph separators.
		const bool control = cp->first < 0x20 || (cp->first >= 0x7F && cp->first <= 0x9F)
			|| cp->first == 0x2028 || cp->first == 0x2029;
		if (!control && lead != '<' && lead != '>')
		{
			out.append(raw, i, len);
			++chars;
		}
		i += len;
	}
	const auto first = out.find_first_not_of(' ');
	const auto last = out.find_last_not_of(' ');
	return first == std::string::npos ? std::string{} : out.substr(first, last - first + 1);
}

bool isPassword(const std::string& password)
{
	return password.empty()
		|| (password.size() == 4 && std::all_of(password.begin(), password.end(), [](char c) { return c >= '0' && c <= '9'; }));
}

bool worldExists(const std::string& worldId)
{
	for (const auto& world : theServer()->cache().raidWorldMst())
		if (std::to_string(world.id) == worldId)
			return true;
	return false;
}

std::string knownPlaystyle(const std::string& playstyleId)
{
	for (const auto& style : theServer()->cache().raidPlaystyleMst())
		if (std::to_string(style.id) == playstyleId)
			return playstyleId;
	return "1";   // RaidPlaystyleMst 1 = "Not Specified"
}

/*! The validated 8iBuZa3n node of a make/edit request. */
struct RoomForm
{
	int32_t op = 0;
	std::string worldId;
	std::string roomId;
	std::string playstyleId;
	std::string comment;
	std::string password;
	bool applyPassword = false;
	int32_t minRc = 1;
	int32_t minLv = 1;
	std::string missionId;
	int32_t kick = 0;
	int32_t midway = 0;
	int32_t autoJoin = 0;
};

RoomForm readRoomForm(const RaidRequest& req)
{
	const auto* node = req.node("8iBuZa3n");
	const auto flag = [&](const char* key) { return RaidRequest::numberOf(node, key) == 1 ? 1 : 0; };
	RoomForm form{};
	form.op = static_cast<int32_t>(RaidRequest::numberOf(node, "mnZ5K4Ii"));
	form.worldId = RaidRequest::textOf(node, "43hMuY2I");
	form.roomId = RaidRequest::textOf(node, "8VYd6xSX");
	form.playstyleId = knownPlaystyle(RaidRequest::textOf(node, "NQR1Vd3t"));
	form.comment = sanitizeComment(RaidRequest::textOf(node, "WDUyI85C"));
	form.password = RaidRequest::textOf(node, "4WSu1irc");
	// nXCf2FE9: 1 on create; on edit RaidInfo::getSelPassFlg.  INFERRED:
	// "apply the password field", absent = keep the stored one.
	form.applyPassword = form.op == kOpCreate || RaidRequest::numberOf(node, "nXCf2FE9") == 1;
	form.minRc = std::max<int32_t>(1, static_cast<int32_t>(RaidRequest::numberOf(node, "wYmg52xp", 1)));
	form.minLv = std::max<int32_t>(1, static_cast<int32_t>(RaidRequest::numberOf(node, "0TfXnP3m", 1)));
	form.missionId = RaidRequest::textOf(node, "j28VNcUW");
	form.kick = flag("j3sGA1zt");
	form.midway = flag("1Txpza2u");
	form.autoJoin = flag("KQA1oWj7");
	return form;
}

/*! Why the caller may not set up a room like this ("" = fine). */
std::string formRefusal(const RoomForm& form, const RaidUser& user, int32_t level)
{
	if (form.op != kOpCreate && form.op != kOpEdit)
		return "This Room could not be updated.";
	if (!worldExists(form.worldId))
		return "This Room is no longer available.";
	if (form.minRc > user.rc)
		return "You can't select a class higher than yours.";
	if (form.minLv > level)
		return "You can't require a level higher than yours.";
	if (form.applyPassword && !isPassword(form.password))
		return "The password must be 4 digits.";
	if (!form.missionId.empty())
	{
		const auto mission = findMission(form.missionId);
		if (!mission || mission->rc_id > user.rc)
			return "Your Raid Class is too low for that quest.";
	}
	return {};
}

/*! ROOM + USER, the reply of make/edit and RaidEnter. */
drogon::Task<HandleResult> roomAndUser(const char* name, const std::string userId, const std::string roomId)
{
	auto resp = co_await roomResp(theDb(), userId, roomId);
	if (resp.rooms.empty())
		co_return HandleResult::refuseToRetry("This Room is no longer available.", std::string(name) + ": room gone");
	resp.user = std::vector<::RaidUserInfo>{ (co_await userInfoResp(theDb(), userId)).user };
	co_return reply(name, resp, co_await roomPartyGroup(theDb(), roomId));
}

drogon::Task<std::string> createRoom(const db::Database tx, const std::string userId, const RoomForm form)
{
	const auto user = co_await loadRaidUser(tx, userId);
	if (!user.login_room_id.empty())
		co_await leaveRoom(tx, userId, user.login_room_id);
	const auto now = nowSeconds();
	const auto created = co_await tx->execSqlCoro(
		"INSERT INTO raid_room(world_id, master_user_id, mission_id, quick_start, status, playstyle_id, comment,"
		" password, password_flg, min_rc, min_lv, kick_flg, midway_joinable, quick_matching, created_at, updated_at)"
		" VALUES ($1, $2, $3, 0, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15) RETURNING room_id;",
		form.worldId, userId, form.missionId, kRoomLobby, form.playstyleId, form.comment, form.password,
		form.password.empty() ? 0 : 1, form.minRc, form.minLv, form.kick, form.midway, form.autoJoin, now, now);
	const auto roomId = std::to_string(created[0]["room_id"].as<int64_t>());
	co_await tx->execSqlCoro(
		"INSERT INTO raid_room_member(room_id, user_id, ready, joined_at, last_seen) VALUES ($1, $2, 0, $3, $4);",
		roomId, userId, now, now);
	co_await tx->execSqlCoro(
		"UPDATE raid_user SET login_world_id = $1, login_room_id = $2 WHERE user_id = $3;",
		form.worldId, roomId, userId);
	co_return roomId;
}

/*! Edit an open room the caller masters; "" on success, else the refusal. */
drogon::Task<std::string> editRoom(const db::Database tx, const std::string userId, const RoomForm form)
{
	const auto rooms = co_await tx->execSqlCoro(
		"SELECT master_user_id, status, mission_id, quick_start FROM raid_room WHERE room_id = $1 AND status != $2;",
		form.roomId, kRoomClosed);
	if (rooms.empty() || rooms[0]["quick_start"].as<int32_t>() == 1)
		co_return "This Room is no longer available.";
	if (rooms[0]["master_user_id"].as<std::string>() != userId)
		co_return "Changes can only be made by the Room Master.";
	const bool newMission = form.missionId != rooms[0]["mission_id"].as<std::string>();
	if (rooms[0]["status"].as<int32_t>() == kRoomInField && newMission)
		co_return "The quest cannot be changed while it is under way.";
	if (newMission && !form.missionId.empty())
	{
		// The same checks RaidMissionSelect makes: everybody already in may
		// play it, and it has room for them.
		const auto mission = findMission(form.missionId);
		const auto members = co_await tx->execSqlCoro(
			"SELECT COUNT(*) AS n, COALESCE(SUM(CASE WHEN COALESCE(u.rc, 1) < $1 THEN 1 ELSE 0 END), 0) AS low"
			" FROM raid_room_member m LEFT JOIN raid_user u ON u.user_id = m.user_id"
			" WHERE m.room_id = $2 AND m.kicked = 0;",
			mission ? mission->rc_id : 1, form.roomId);
		if (members[0]["low"].as<int32_t>() > 0)
			co_return "A member's Raid Class is too low for that quest.";
		if (members[0]["n"].as<int32_t>() > roomCapacity(form.missionId))
			co_return "That quest has room for fewer players than are in this Room.";
	}
	// The kick flag is fixed at creation (RAID_ROOMMAKE_WINDOW_CREATE2): not
	// in this UPDATE whatever the client sends.
	co_await tx->execSqlCoro(
		"UPDATE raid_room SET playstyle_id = $1, comment = $2, min_rc = $3, min_lv = $4, mission_id = $5,"
		" midway_joinable = $6, quick_matching = $7, updated_at = $8 WHERE room_id = $9;",
		form.playstyleId, form.comment, form.minRc, form.minLv, form.missionId, form.midway, form.autoJoin,
		nowSeconds(), form.roomId);
	if (newMission)
		co_await tx->execSqlCoro(
			"UPDATE raid_room_member SET ready = 0 WHERE room_id = $1 AND user_id != $2;", form.roomId, userId);
	if (form.applyPassword)
		co_await tx->execSqlCoro(
			"UPDATE raid_room SET password = $1, password_flg = $2 WHERE room_id = $3;",
			form.password, form.password.empty() ? 0 : 1, form.roomId);
	co_return std::string{};
}

/*! The RaidEnter checks of 4.3 steps 1 and 3 to 7 ("" = may join). */
drogon::Task<std::string> enterRefusal(
	const db::Database tx, const RaidUser user, const int32_t level, const std::string roomId,
	const std::string worldId, const std::string password)
{
	const auto rooms = co_await tx->execSqlCoro(
		"SELECT world_id, quick_start, password, min_rc, min_lv, status, midway_joinable, mission_started_at,"
		" mission_id, master_user_id FROM raid_room WHERE room_id = $1 AND status != $2;",
		roomId, kRoomClosed);
	if (rooms.empty() || rooms[0]["quick_start"].as<int32_t>() == 1
		|| (!worldId.empty() && rooms[0]["world_id"].as<std::string>() != worldId))
		co_return "This Room is no longer available.";
	const auto& room = rooms[0];
	// A room across the Sandbox line is not there for this player (Sandbox.hpp).
	if (!(co_await gme::sandbox::samePool(tx, user.user_id, room["master_user_id"].as<std::string>())))
		co_return "This Room is no longer available.";
	const auto kicked = co_await tx->execSqlCoro(
		"SELECT 1 FROM raid_room_member WHERE room_id = $1 AND user_id = $2 AND kicked = 1;", roomId, user.user_id);
	if (!kicked.empty())
		co_return "You cannot rejoin this Room.";
	if (room["password"].as<std::string>() != password)
		co_return "The password is incorrect.";
	const auto links = co_await blackListLinks(tx, user.user_id);
	const auto members = co_await tx->execSqlCoro(
		"SELECT user_id FROM raid_room_member WHERE room_id = $1 AND kicked = 0;", roomId);
	for (const auto& member : members)
		if (links.contains(member["user_id"].as<std::string>()))
			co_return "You cannot join this Room.";
	const auto mission = findMission(room["mission_id"].as<std::string>());
	if (user.rc < room["min_rc"].as<int32_t>() || level < room["min_lv"].as<int32_t>()
		|| (mission && mission->rc_id > user.rc))
		co_return "You do not meet this Room's requirements.";
	if (room["status"].as<int32_t>() == kRoomInField
		&& (room["midway_joinable"].as<int32_t>() != 1
			|| nowSeconds() >= room["mission_started_at"].as<int64_t>() + kMidwayJoinWindowSec))
		co_return "You cannot join that Room's current Quest.";
	co_return std::string{};
}

} // namespace

// RaidRoomList (YF9UQpn6) -- search (1), friends (2), auto-join (3), guild (4).
HANDLEF(RaidRoomList)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto search = readRoomSearch(call.req);
	::RaidRoomListResp resp{};
	// searchRooms prunes every room it judges: one transaction for the lot.
	auto transaction = co_await gme::newTransaction(theDb());
	resp.rooms = co_await searchRooms(transaction, call.identity, search);
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		LOG_ERROR << "RaidRoomList: prune of the searched rooms did not commit";
	LOG_INFO << "RaidRoomList: type " << search.type << " for " << call.identity.userId << ": "
		<< resp.rooms.size() << " row(s)";
	co_return reply("RaidRoomList", resp);
}

// RaidUpDataRoomInfo (52H0dkjv) -- make (op 1) or edit (op 2) a room.
HANDLEF(RaidUpDataRoomInfo)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	const auto form = readRoomForm(call.req);
	const auto user = co_await loadRaidUser(theDb(), userId);
	const auto level = (co_await loadProfile(theDb(), userId)).level;
	if (const auto why = formRefusal(form, user, level); !why.empty())
		co_return HandleResult::refuseToRetry(why, "RaidUpDataRoomInfo: " + userId + ": " + why);

	auto transaction = co_await gme::newTransaction(theDb());
	std::string roomId = form.roomId;
	std::string refusal;
	if (form.op == kOpCreate)
		roomId = co_await createRoom(transaction, userId, form);
	else
		refusal = co_await editRoom(transaction, userId, form);
	if (!refusal.empty())
	{
		transaction->rollback();
		co_return HandleResult::refuseToRetry(refusal, "RaidUpDataRoomInfo: " + userId + ": " + refusal);
	}
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return HandleResult::refuseToRetry("The Room could not be saved. Please try again.",
			"RaidUpDataRoomInfo: commit failed for " + userId);
	LOG_INFO << "RaidUpDataRoomInfo: " << userId << (form.op == kOpCreate ? " opened" : " edited")
		<< " room " << roomId << " (mission '" << form.missionId << "')";
	co_return co_await roomAndUser("RaidUpDataRoomInfo", userId, roomId);
}

// RaidEnter (yZReG75i) -- join a room by id (search list, auto-join, a
// friend's room).  Checks in the order of 4.3; "full" is decided by the
// INSERT itself.
HANDLEF(RaidEnter)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	const auto roomId = call.req.text("8iBuZa3n", "8VYd6xSX");
	const auto refuse = [&](const std::string& message) {
		return HandleResult::refuseToRetry(message, "RaidEnter: " + userId + " refused room '" + roomId + "': " + message);
	};
	if (roomId.empty())
		co_return refuse("This Room is no longer available.");

	auto transaction = co_await gme::newTransaction(theDb());
	co_await pruneRoom(transaction, roomId);
	// A retried RaidEnter must not fail: already in is success.
	if (!(co_await isActiveMember(transaction, roomId, userId)))
	{
		const auto user = co_await loadRaidUser(transaction, userId);
		const auto level = (co_await loadProfile(transaction, userId)).level;
		const auto why = co_await enterRefusal(transaction, user, level, roomId,
			call.req.text("8iBuZa3n", "43hMuY2I"), call.req.text("8iBuZa3n", "4WSu1irc"));
		if (!why.empty())
		{
			transaction->rollback();
			co_return refuse(why);
		}
		if (!user.login_room_id.empty() && user.login_room_id != roomId)
			co_await leaveRoom(transaction, userId, user.login_room_id);
		const auto mission = co_await transaction->execSqlCoro(
			"SELECT mission_id FROM raid_room WHERE room_id = $1;", roomId);
		const auto joined = co_await transaction->execSqlCoro(
			"INSERT INTO raid_room_member(room_id, user_id, ready, joined_at, last_seen)"
			" SELECT $1, $2, 0, $3, $4 WHERE (SELECT COUNT(*) FROM raid_room_member"
			" WHERE room_id = $5 AND kicked = 0) < $6 RETURNING user_id;",
			roomId, userId, nowSeconds(), nowSeconds(), roomId,
			roomCapacity(mission.empty() ? std::string{} : mission[0]["mission_id"].as<std::string>()));
		if (joined.empty())
		{
			transaction->rollback();
			co_return refuse("This Room is full.");
		}
	}
	co_await transaction->execSqlCoro(
		"UPDATE raid_user SET login_world_id = (SELECT world_id FROM raid_room WHERE room_id = $1),"
		" login_room_id = $2 WHERE user_id = $3;",
		roomId, roomId, userId);
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return refuse("The Room could not be joined. Please try again.");
	LOG_INFO << "RaidEnter: " << userId << " is in room " << roomId;
	co_return co_await roomAndUser("RaidEnter", userId, roomId);
}

// RaidUpDataUserReady (vmdA2GD8) -- 0 un-ready, 1 ready, 2 set out now (the
// master is already in the quest: mid-quest join, or a ready member late).
HANDLEF(RaidUpDataUserReady)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	const auto& roomId = call.roomId;
	if (!(co_await isActiveMember(theDb(), roomId, userId)))
	{
		co_await forgetRoom(theDb(), userId, roomId);
		co_return HandleResult::refuseToRetry("This Room is no longer available.",
			"RaidUpDataUserReady: " + userId + " is not in room '" + roomId + "'");
	}
	const auto target = call.req.text("uwH62EZy", "h7eY3sAK");
	const auto ready = RaidRequest::numberOf(call.req.node("uwH62EZy"), "uX87tAZB", -1);
	if ((!target.empty() && target != userId) || ready < 0 || ready > kReadyMidway)
		co_return HandleResult::refuseToRetry("Your status could not be changed.",
			"RaidUpDataUserReady: " + userId + " sent ready " + std::to_string(ready) + " for '" + target + "'");

	// Readying up commits to setting out, which costs a raid orb: a member
	// with none is told now, not when the master starts.
	const auto orbs = (co_await loadRaidUser(theDb(), userId)).aube;
	if (ready >= 1 && orbs <= 0)
		co_return HandleResult::refuseToRetry("You have no Raid Orbs left.",
			"RaidUpDataUserReady: " + userId + " has no aube");
	if (ready == kReadyMidway)
	{
		const auto why = co_await midwayRefusal(theDb(), roomId, userId);
		if (!why.empty())
			co_return HandleResult::refuseToRetry("You cannot join this Quest.", "RaidUpDataUserReady: " + why);
		co_await theDb()->execSqlCoro(
			"UPDATE raid_room_member SET ready = $1, mission_status = $2 WHERE room_id = $3 AND user_id = $4;",
			kReadyMidway, kRunRunning, roomId, userId);
	}
	else
	{
		co_await theDb()->execSqlCoro(
			"UPDATE raid_room_member SET ready = $1 WHERE room_id = $2 AND user_id = $3 AND mission_status = 0;",
			ready, roomId, userId);
	}
	LOG_INFO << "RaidUpDataUserReady: " << userId << " ready " << ready << " in room " << roomId;
	co_return reply("RaidUpDataUserReady", co_await roomResp(theDb(), userId, roomId));
}

// RaidRoomKick (Xbgen5J8) -- the master expels a lobby member.  The target
// keeps a kicked row (no rejoin) and loses their login ids at once; they see
// the expel notice on their next poll and answer it with RaidExit.
HANDLEF(RaidRoomKick)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	const auto& roomId = call.roomId;
	const auto target = call.req.text("uwH62EZy", "h7eY3sAK");
	const auto refuse = [&](const std::string& message) {
		return HandleResult::refuseToRetry(message, "RaidRoomKick: " + userId + " -> " + target + ": " + message);
	};
	auto transaction = co_await gme::newTransaction(theDb());
	const auto rooms = co_await transaction->execSqlCoro(
		"SELECT master_user_id, kick_flg FROM raid_room WHERE room_id = $1 AND status != $2;", roomId, kRoomClosed);
	const auto member = co_await transaction->execSqlCoro(
		"SELECT mission_status FROM raid_room_member WHERE room_id = $1 AND user_id = $2 AND kicked = 0;",
		roomId, target);
	std::string why;
	if (rooms.empty() || rooms[0]["master_user_id"].as<std::string>() != userId)
		why = "Changes can only be made by the Room Master.";
	else if (rooms[0]["kick_flg"].as<int32_t>() != 1)
		why = "Kick function unavailable.";
	else if (target.empty() || target == userId || member.empty())
		why = "That player is no longer in this Room.";
	else if (member[0]["mission_status"].as<int32_t>() != 0)
		why = "That player is on a Quest.";
	if (why.empty())
	{
		// The decisive conditions are repeated in the write itself (3.3 rule 5).
		const auto kicked = co_await transaction->execSqlCoro(
			"UPDATE raid_room_member SET kicked = 1, ready = 0 WHERE room_id = $1 AND user_id = $2 AND kicked = 0"
			" AND mission_status = 0 AND EXISTS (SELECT 1 FROM raid_room WHERE room_id = $3 AND master_user_id = $4"
			" AND kick_flg = 1 AND status != $5);",
			roomId, target, roomId, userId, kRoomClosed);
		if (kicked.affectedRows() == 0)
			why = "That player is no longer in this Room.";
	}
	if (!why.empty())
	{
		transaction->rollback();
		co_return refuse(why);
	}
	co_await forgetRoom(transaction, target, roomId);
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return refuse("The player could not be expelled. Please try again.");
	LOG_INFO << "RaidRoomKick: " << userId << " expelled " << target << " from room " << roomId;
	co_return reply("RaidRoomKick", co_await roomResp(theDb(), userId, roomId));
}
