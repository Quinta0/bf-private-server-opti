#include "RaidHandlerSupport.hpp"

#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/RaidBlackList.hpp>
#include <gimuserver/gme/common/RaidChat.hpp>
#include <gimuserver/gme/common/RaidStats.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

// Classic Raid round 2 -- the social side of a room (RAIDS_ROUND2_MULTIPLAYER.md
// 4.7 to 4.11): stamp chat, the blacklist, the player's own raid records and
// the helper pick.
//
// SOME OF THESE MUST NEVER REFUSE.  RaidChatScene answers -4000 by throwing
// the player back through 1201, and FriendRemove is also sent from the
// generic party editor, whose -4000 handling knows nothing of raids.  So chat,
// chat log, friend remove and player info answer success whatever happens;
// a non-member simply gets no rows.

using namespace gme::raid;
using namespace gme::raid::handlers;
using gme::UserIdentity;

namespace
{

/*! The caller's live room membership, for the helper pick (lobby only). */
drogon::Task<bool> inLobby(const std::string roomId, const std::string userId)
{
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT 1 FROM raid_room_member m JOIN raid_room r ON r.room_id = m.room_id"
		" WHERE m.room_id = $1 AND m.user_id = $2 AND m.kicked = 0 AND m.mission_status = 0 AND r.status != $3;",
		roomId, userId, kRoomClosed);
	co_return !rows.empty();
}

/*! A friend's helper as the server builds it (NPC roster first, then real friends); "" when not a friend. */
drogon::Task<std::string> friendHelper(const UserIdentity identity, const std::string friendId)
{
	const auto peak = co_await gme::playerPeak(theDb(), identity);
	const auto roster = co_await gme::loadFriendRoster(theDb(), identity, false);
	for (const auto& entry : gme::socialList(roster, peak))
		if (entry.user_id == friendId)
			co_return serialize("RaidFrlendSelect", std::vector<::FriendInfo>{ entry }).value_or("");

	for (const auto& rel : co_await gme::loadRelations(theDb(), identity.userId))
	{
		if (rel.other_id != friendId || rel.state != gme::kRelFriend)
			continue;
		// No helper across the Sandbox line (gme/common/Sandbox.hpp).
		if (!(co_await gme::sandbox::samePool(theDb(), identity.userId, friendId)))
			co_return std::string{};
		const auto card = co_await gme::loadPlayerCard(theDb(), friendId);
		if (!card)
			co_return std::string{};
		const auto row = gme::friendInfoFromCard(*card, gme::kRelFriend, &rel);
		co_return serialize("RaidFrlendSelect", std::vector<::FriendInfo>{ row }).value_or("");
	}
	co_return std::string{};
}

/*!
* One of the caller's OWN units, offered by RaidUnitSelectionScene for
* elemental-lock quests (UserUnitInfoList::getNotParty, wrapped in a
* FriendInfo with the caller's id).  Only a unit the caller owns and is not
* fielding in the active party, and the card is rebuilt from user_units:
* nothing the client put in the FriendInfo is trusted.
*/
drogon::Task<std::string> ownUnitHelper(const UserIdentity identity, const RaidRequest& req)
{
	const auto userUnitId = req.text("ZX7gzQ6s", "edy7fq3L");
	// Not a unit of the raid squad the player fields (deck type 3, the
	// squad number UserInfo's active deck names).
	const auto inSquad = co_await theDb()->execSqlCoro(
		"SELECT 1 FROM user_decks d JOIN user_info i ON i.id = d.user_id WHERE d.user_id = $1"
		" AND d.user_unit_id = $2 AND d.deck_type = $3 AND d.deck_num = i.active_deck;",
		identity.userId, userUnitId, kRaidDeckType);
	if (userUnitId.empty() || !inSquad.empty())
		co_return std::string{};
	const auto card = co_await gme::loadUnitCard(theDb(), identity.userId, userUnitId);
	if (!card)
		co_return std::string{};
	// friend type 0: the caller's own unit is nobody's friend.
	co_return serialize("RaidFrlendSelect", std::vector<::FriendInfo>{ gme::friendInfoFromCard(*card, 0, nullptr) })
		.value_or("");
}

} // namespace

// RaidSendChat (dYIy6Eu8) -- a stamp.  The reply MUST carry the delta with
// the sender's own new row: RaidChatScene::checkSendSuccess @0x1957E7C keeps
// re-sending until it sees it.
HANDLEF(RaidSendChat)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto chat = readChatRequest(call.req);
	::RaidChatResp resp{};
	if (co_await isChatMember(theDb(), chat.roomId, call.identity.userId))
	{
		co_await postStamp(theDb(), chat.roomId, call.identity.userId, chat.chatType, chat.content);
		auto rows = co_await chatDelta(theDb(), chat.roomId, chat.afterLogId);
		if (!rows.empty())
			resp.chat = std::move(rows);
	}
	co_return reply("RaidSendChat", resp);
}

// RaidGetChatLog (k5pMN68B) -- the field's chat poll (>= 5 s).  The client
// appends rows without de-duplication, so only rows past its cursor go out.
HANDLEF(RaidGetChatLog)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto chat = readChatRequest(call.req);
	::RaidChatResp resp{};
	if (co_await isChatMember(theDb(), chat.roomId, call.identity.userId))
	{
		auto rows = co_await chatDelta(theDb(), chat.roomId, chat.afterLogId);
		if (!rows.empty())
			resp.chat = std::move(rows);
	}
	co_return reply("RaidGetChatLog", resp);
}

// RaidUpdateBlackList (4igfcyIm) -- op 1 add (member card), op 3 remove
// (blacklist detail).  Replies with the caller's whole list.
HANDLEF(RaidUpdateBlackList)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto edit = readBlackListEdit(call.req);
	const auto result = co_await updateBlackList(theDb(), call.identity, edit.targetUserId, edit.op);
	if (result == BlackListResult::Full)
		co_return HandleResult::refuseToRetry("Your Black List is full.",
			"RaidUpdateBlackList: list of " + call.identity.userId + " is full");
	if (result == BlackListResult::Invalid)
		co_return HandleResult::refuseToRetry("That player could not be found.",
			"RaidUpdateBlackList: invalid target '" + edit.targetUserId + "'");
	::RaidBlackListResp resp{};
	resp.list = co_await blackListRows(theDb(), call.identity);
	co_return reply("RaidUpdateBlackList", resp);
}

// RaidGetPlayerInfo (81zkMjfN) -- the player's own raid records
// (RaidPlayerInfoResultScene 1209).  Read only; never refuses.
HANDLEF(RaidGetPlayerInfo)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	::RaidPlayerInfoResp resp{};
	resp.info = co_await archiveInfo(theDb(), call.identity.userId);
	co_return reply("RaidGetPlayerInfo", resp);
}

// RaidFrlendSelect (u59zdKCw, sic) -- pick the helper for the room's quest:
// a friend's unit, or one of the caller's own units outside the party.  The
// server builds (or checks) the FriendInfo and stores it on the member row.
HANDLEF(RaidFrlendSelect)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	if (!(co_await inLobby(call.roomId, userId)))
		co_return HandleResult::refuseToRetry("This Room is no longer available.",
			"RaidFrlendSelect: " + userId + " is not in the lobby of room '" + call.roomId + "'");
	const auto picked = call.req.text("ZX7gzQ6s", "h7eY3sAK");
	// No co_await inside ?: -- GCC 13 ICEs on it (see RaidFieldHandlers useItem).
	std::string helper;
	if (picked == userId)
		helper = co_await ownUnitHelper(call.identity, call.req);
	else
		helper = co_await friendHelper(call.identity, picked);
	if (helper.empty())
		co_return HandleResult::refuseToRetry("That helper is not available.",
			"RaidFrlendSelect: " + userId + " picked unknown helper '" + picked + "'");

	co_await theDb()->execSqlCoro(
		"UPDATE raid_room_member SET helper_json = $1 WHERE room_id = $2 AND user_id = $3;",
		helper, call.roomId, userId);
	LOG_INFO << "RaidFrlendSelect: " << userId << " picked helper " << picked << " in room " << call.roomId;
	co_return reply("RaidFrlendSelect", co_await roomResp(theDb(), userId, call.roomId), "{\"ZX7gzQ6s\":" + helper + "}");
}

// RaidFriendRemove (u59zdKCi) -- drop the helper.  Always succeeds: the party
// editor sends it too, outside any raid scene.
HANDLEF(RaidFriendRemove)
{
	(void)session;
	auto call = co_await openRoomCall(json);
	const auto& userId = call.identity.userId;
	if (call.roomId.empty())
		co_return HandleResult::success("{}");
	// No co_await inside || (GCC 13 ICE).
	const bool member = co_await isActiveMember(theDb(), call.roomId, userId);
	if (!member)
		co_return HandleResult::success("{}");
	co_await theDb()->execSqlCoro(
		"UPDATE raid_room_member SET helper_json = '' WHERE room_id = $1 AND user_id = $2;", call.roomId, userId);
	const auto room = co_await roomResp(theDb(), userId, call.roomId);
	if (room.rooms.empty())
		co_return HandleResult::success("{}");
	co_return reply("RaidFriendRemove", room);
}
