#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/RaidBlackList.hpp>
#include <gimuserver/gme/common/RaidChat.hpp>
#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidItems.hpp>
#include <gimuserver/gme/common/Raids.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

// Classic Raid Battle, slice 1 -- the Home Raid button through to a solo room.
//
// The client walks RaidInitConnect (RaidUserInfo) -> RaidInitScene (terms via
// RaidAgreement) -> RaidWorldScene (RaidWorloInfo) -> quest lists (no request,
// filtered by the permit list) -> helper pick (RaidRoomFriendGet) ->
// RaidQuickCheckScene (queued RoonItemEdit/UpDateCash, then RaidQuickStart) ->
// RaidFieldReady, whose RaidMissionStart opens the run (round 2).  Scene
// ids and every key are in docs/features/RAIDS_SPEC.md; the state lives in
// gme/common/Raids.cpp.

namespace
{

/*!
* The quick-start helper (ZX7gzQ6s), kept as raw JSON.  It is echoed back
* exactly as the client built it -- re-serialising it through FriendInfo would
* re-type fields the client's FriendInfo createBody may quote differently, and
* this slice only needs to show it, not interpret it.
*/
struct RaidHelperPeek
{
	glz::raw_json helper{};
};

/*! True when the raw helper group names nobody (absent, or an empty list). */
bool noHelper(const std::string& raw)
{
	return raw.empty() || raw == "[]" || raw == "[{}]";
}

template <class T>
T readRaidRequest(const char* name, const std::string& json)
{
	T req{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
		LOG_WARN << name << ": parse error: " << glz::format_error(ec, json);
	return req;
}

/*! The room a request names, falling back to the one the player is logged into. */
drogon::Task<std::string> requestedRoom(const ::RaidRoomReq& req, const std::string userId)
{
	if (!req.room.empty() && !req.room.front().room_id.empty())
		co_return req.room.front().room_id;
	co_return (co_await gme::raid::loadRaidUser(theDb(), userId)).login_room_id;
}

template <class T>
HandleResult writeRaidReply(const char* name, const T& resp)
{
	std::string buffer{};
	if (const auto ec = glz::write_json(resp, buffer); ec)
	{
		const auto detail = glz::format_error(ec, buffer);
		LOG_ERROR << name << ": serialization error: " << detail;
		return HandleResult::error("Serialization error", detail);
	}
	return HandleResult::success(buffer);
}

/*!
* The raid blacklist for replies that carry it (MULTIPLAYER 4.9): the World
* screen opens the blacklist scene (1225) with no request of its own, so the
* list must already be loaded.  Omitted while empty: an empty array reaches
* no row and would change nothing anyway.
*/
drogon::Task<std::optional<std::vector<::FriendInfo>>> blackListGroup(const gme::UserIdentity identity)
{
	auto rows = co_await gme::raid::blackListRows(theDb(), identity);
	if (rows.empty())
		co_return std::nullopt;
	co_return rows;
}

/*! A stored raid item list (CAMP_ITEMS 4.5), omitted while empty. */
drogon::Task<std::optional<std::vector<::UserEquipItemInfo>>> itemGroup(const std::string userId, const int32_t kind)
{
	auto rows = gme::raid::setRows(co_await gme::raid::loadSet(theDb(), userId, kind));
	if (rows.empty())
		co_return std::nullopt;
	co_return rows;
}

} // namespace

template <>
struct glz::meta<RaidHelperPeek>
{
	using T = RaidHelperPeek;
	static constexpr auto value = object("ZX7gzQ6s", &T::helper);
};

// RaidUserInfo (jJZ01VQo) -- the first request after tapping Raid.
HANDLEF(RaidUserInfo)
{
	(void)session;
	const auto req = readRaidRequest<::RaidBareReq>("RaidUserInfo", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	auto resp = co_await gme::raid::userInfoResp(theDb(), identity.userId);
	resp.black_list = co_await blackListGroup(identity);
	// The room screens show the stored set after a restart (CAMP_ITEMS 4.5).
	resp.item_set = co_await itemGroup(identity.userId, gme::raid::kItemSetKind);
	resp.item_reserve = co_await itemGroup(identity.userId, gme::raid::kItemReserveKind);
	LOG_INFO << "RaidUserInfo: " << identity.userId << " rc " << resp.user.rc
		<< " agreement " << resp.user.agreement << " room '" << resp.user.login_room_id
		<< "', " << resp.permit.size() << " permitted mission(s)";
	co_return writeRaidReply("RaidUserInfo", resp);
}

// RaidAgreement (Cu94xU8j) -- the terms screen's accept button.
HANDLEF(RaidAgreement)
{
	(void)session;
	const auto req = readRaidRequest<::RaidBareReq>("RaidAgreement", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	co_await gme::raid::loadRaidUser(theDb(), identity.userId);   // row exists first
	co_await gme::raid::acceptAgreement(theDb(), identity.userId);
	LOG_INFO << "RaidAgreement: " << identity.userId << " accepted the raid terms";
	co_return writeRaidReply("RaidAgreement", co_await gme::raid::userInfoResp(theDb(), identity.userId));
}

// RaidWorloInfo (H7GRp01L, the client's own spelling) -- the world list.
HANDLEF(RaidWorloInfo)
{
	(void)session;
	const auto req = readRaidRequest<::RaidBareReq>("RaidWorloInfo", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	auto resp = co_await gme::raid::worldInfoResp(theDb(), identity.userId);
	resp.black_list = co_await blackListGroup(identity);
	co_return writeRaidReply("RaidWorloInfo", resp);
}

// RaidRoomFriendGet (Xe2UB3A8) -- the helper picker.  The same Social list
// FriendGet sends, under the raid key: the raid picker reads FriendInfo, not
// ReinforcementInfo, so strangers are not offered here.
HANDLEF(RaidRoomFriendGet)
{
	(void)session;
	const auto req = readRaidRequest<::RaidRoomReq>("RaidRoomFriendGet", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	::RaidRoomFriendGetResp resp{};
	try
	{
		const auto peak = co_await gme::playerPeak(theDb(), identity);
		const auto roster = co_await gme::loadFriendRoster(theDb(), identity, false);
		resp.friends = gme::socialList(roster, peak);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		// An empty list is a valid answer (RAID_NO_FRIEND + the no-helper button).
		LOG_WARN << "RaidRoomFriendGet: roster query failed: " << ex.base().what();
	}
	LOG_INFO << "RaidRoomFriendGet: " << resp.friends.size() << " helper(s) for " << identity.userId;
	co_return writeRaidReply("RaidRoomFriendGet", resp);
}

// RaidQuickStart (CWeTY8BQ) -- a solo room of one.  Aube is not spent here: the
// roadmap charges it at MissionStart, which this slice does not run.
HANDLEF(RaidQuickStart)
{
	(void)session;
	const auto req = readRaidRequest<::RaidRoomReq>("RaidQuickStart", json);
	const auto peek = readRaidRequest<RaidHelperPeek>("RaidQuickStart(helper)", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto missionId = req.room.empty() ? std::string{} : req.room.front().mission_id;
	const auto mission = gme::raid::findMission(missionId);
	if (!mission)
		co_return HandleResult::refuseToHome("That raid quest does not exist.",
			"RaidQuickStart: unknown mission '" + missionId + "'");

	const auto user = co_await gme::raid::loadRaidUser(theDb(), identity.userId);
	if (mission->rc_id > user.rc)
		co_return HandleResult::refuseToHome("Your Raid Class is too low for that quest.",
			"RaidQuickStart: mission " + missionId + " needs rc " + std::to_string(mission->rc_id));

	const auto helper = noHelper(peek.helper.str) ? std::string{} : peek.helper.str;
	const auto roomId = co_await gme::raid::createSoloRoom(theDb(), identity.userId, missionId, helper);
	const auto room = co_await gme::raid::roomResp(theDb(), identity.userId, roomId);

	::RaidQuickStartResp resp{};
	resp.rooms = room.rooms;
	resp.members = room.members;
	resp.user = (co_await gme::raid::userInfoResp(theDb(), identity.userId)).user;

	auto reply = writeRaidReply("RaidQuickStart", resp);
	// Echo the helper verbatim (see RaidHelperPeek): it lands in
	// RaidInfo::setSelFriendInfo.  Omitted entirely when there is none.
	if (!reply.isError() && !helper.empty() && reply.successJson.back() == '}')
	{
		reply.successJson.pop_back();
		reply.successJson += ",\"ZX7gzQ6s\":" + helper + "}";
	}
	LOG_INFO << "RaidQuickStart: " << identity.userId << " opened solo room " << roomId
		<< " for mission " << missionId << (helper.empty() ? " without" : " with") << " a helper";
	co_return reply;
}

// RaidRoonItemEdit (bv9xt3m6, sic) and RaidUpDateCash (XTban70i) -- queued by
// the room scenes (RaidQuickCheckScene before QuickStart, RaidRoomTopScene's
// updateEvent).  If either fails the queue aborts and QuickStart never goes
// out, so they always acknowledge.
//
// RoonItemEdit carries the whole raid item set (Fihk73ZR) and reserve
// (GvV1w4yM) the player filled from the warehouse: the per-item difference
// against the stored lists moves out of / back into user_items, all or
// nothing (CAMP_ITEMS 4.5, the ItemEdit.cpp rule).  An edit the warehouse
// cannot cover stores nothing; the next RaidUserInfo shows the stored set.
HANDLEF(RaidRoonItemEdit)
{
	(void)session;
	const auto req = readRaidRequest<::RaidBareReq>("RaidRoonItemEdit", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	const gme::raid::RaidRequest body(json);
	co_await gme::raid::touchLoginRoom(theDb(), identity.userId);

	bool stored = false;
	{
		auto transaction = co_await gme::newTransaction(theDb());
		stored = co_await gme::raid::storeRoomItems(transaction, identity,
			gme::raid::readSetRows(body, "Fihk73ZR"), gme::raid::readSetRows(body, "GvV1w4yM"));
		if (!stored)
			transaction->rollback();
		else if (!(co_await gme::CommitTransaction(std::move(transaction))))
			LOG_ERROR << "RaidRoonItemEdit: item move of " << identity.userId << " did not commit";
	}
	if (stored)
		co_return HandleResult::success("{}");
	// Rejected (the warehouse could not cover it): answer with what the
	// server holds, so the room screens show the real lists (global readers).
	std::string reply = "{";
	for (const auto& [key, kind] : { std::pair{ "Fihk73ZR", gme::raid::kItemSetKind },
		std::pair{ "GvV1w4yM", gme::raid::kItemReserveKind } })
	{
		const auto rows = co_await itemGroup(identity.userId, kind);
		std::string list{};
		if (!rows || glz::write_json(*rows, list))
			continue;
		reply += (reply.size() > 1 ? ",\"" : "\"") + std::string(key) + "\":" + list;
	}
	co_return HandleResult::success(reply + "}");
}

HANDLEF(RaidUpDateCash)
{
	(void)session;
	const auto req = readRaidRequest<::RaidBareReq>("RaidUpDateCash", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	co_await gme::raid::touchLoginRoom(theDb(), identity.userId);
	// Its 79PaHFBh.X6WBc9kJ is the constant 1 (RaidUpDateCashRequest::
	// createBody @0x13C8198), not a squad number: nothing to store.
	LOG_INFO << "RaidUpDateCash: acknowledged for " << identity.userId;
	co_return HandleResult::success("{}");
}

// RaidExit (jqi71nWu) -- leave the room.
HANDLEF(RaidExit)
{
	(void)session;
	const auto req = readRaidRequest<::RaidRoomReq>("RaidExit", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto roomId = co_await requestedRoom(req, identity.userId);
	if (!roomId.empty())
	{
		// One transaction: leaving can promote a master or close the room
		// (MULTIPLAYER 3.3 rule 5).
		auto transaction = co_await gme::newTransaction(theDb());
		co_await gme::raid::leaveRoom(transaction, identity.userId, roomId);
		if (!(co_await gme::CommitTransaction(std::move(transaction))))
			LOG_ERROR << "RaidExit: leaving room " << roomId << " did not commit for " << identity.userId;
	}
	LOG_INFO << "RaidExit: " << identity.userId << " left room '" << roomId << "'";
	co_return writeRaidReply("RaidExit", co_await gme::raid::userInfoResp(theDb(), identity.userId));
}

// RaidRoomDissolve (H8N1R0TE) -- the master closes the room; also what
// RaidInitScene sends after repeated errors.
HANDLEF(RaidRoomDissolve)
{
	(void)session;
	const auto req = readRaidRequest<::RaidRoomReq>("RaidRoomDissolve", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto roomId = co_await requestedRoom(req, identity.userId);
	if (!roomId.empty())
	{
		// MULTIPLAYER 3.3 rule 3: the master closes the room for everyone; anybody
		// else (RaidInitScene sends it after repeated errors too) just leaves.
		auto transaction = co_await gme::newTransaction(theDb());
		const auto master = co_await transaction->execSqlCoro(
			"SELECT 1 FROM raid_room WHERE room_id = $1 AND master_user_id = $2 AND status != $3;",
			roomId, identity.userId, gme::raid::kRoomClosed);
		if (master.empty())
			co_await gme::raid::leaveRoom(transaction, identity.userId, roomId);
		else
			co_await gme::raid::closeRoom(transaction, roomId);
		if (!(co_await gme::CommitTransaction(std::move(transaction))))
			LOG_ERROR << "RaidRoomDissolve: room " << roomId << " change did not commit for " << identity.userId;
	}
	LOG_INFO << "RaidRoomDissolve: " << identity.userId << " dissolved room '" << roomId << "'";
	co_return writeRaidReply("RaidRoomDissolve", co_await gme::raid::userInfoResp(theDb(), identity.userId));
}

// RaidRoomInfo (0VxE6kTW) -- the 4 s lobby heartbeat and the resume path.
//
// A GONE ROOM IS A REFUSAL, NOT AN EMPTY LIST.  An empty 8iBuZa3n never
// reaches RaidRoomInfoResponse::readParam, so the lobby would keep the stale
// room and poll forever (MULTIPLAYER 1.3).  refuseToRetry sends every lobby
// scene through connectErrorInit -> 1201 -> RaidUserInfo, which lands on the
// world list once the login ids are blank.  A kicked member still gets the
// room with their own row flagged (D2ZKtGR9), on every poll until their
// RaidExit, so the expel notice shows (4.1 rule 2).
HANDLEF(RaidRoomInfo)
{
	(void)session;
	const auto req = readRaidRequest<::RaidRoomReq>("RaidRoomInfo", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	const gme::raid::RaidRequest body(json);

	const auto roomId = co_await requestedRoom(req, identity.userId);
	auto resp = co_await gme::raid::roomResp(theDb(), identity.userId, roomId);
	if (resp.rooms.empty())
	{
		co_await gme::raid::forgetRoom(theDb(), identity.userId, roomId);
		co_return HandleResult::refuseToRetry("This Room is no longer available.",
			"RaidRoomInfo: room '" + roomId + "' is gone for " + identity.userId);
	}
	const auto cursor = gme::raid::RaidRequest::numberOf(body.node("pw38qdui"), "NLKce95n");
	if (co_await gme::raid::isChatMember(theDb(), roomId, identity.userId))
	{
		auto chat = co_await gme::raid::chatDelta(theDb(), roomId, cursor);
		if (!chat.empty())
			resp.chat = std::move(chat);
	}
	// The member card draws each member's squad from 9PS8BLiC.
	auto reply = writeRaidReply("RaidRoomInfo", resp);
	if (!reply.isError())
		reply.successJson = gme::raid::mergeJson(reply.successJson, co_await gme::raid::roomPartyGroup(theDb(), roomId));
	co_return reply;
}

// RaidMissionStart and the rest of the run: RaidFieldHandlers.cpp and
// RaidBattleHandlers.cpp (round 2).
