#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Classic Raid room chat (RAIDS_ROUND2_MULTIPLAYER.md 4.8).
//
// CHAT IS STAMPS ONLY.  RaidChatScene has no text input: the only sendChat
// callers are touchStamp @0x1956900 (type "2", content = the StampMst id in
// decimal) and touchUserStamp @0x1956CE4 (type "5", a stamp from the member
// palette), plus the scene's own resend of the same pair.  So the server
// stores nothing but a known stamp id, and no player can put text (or label
// markup) on another player's screen.
//
// The client's log is APPEND-ONLY: UserChatLogInfoList::addObject does not
// de-duplicate, and after every reply the list is sorted by log id and trimmed
// to 200.  Every reply therefore carries only rows newer than the cursor the
// client sends (pw38qdui.NLKce95n).  Rows live in raid_chat under a global
// AUTOINCREMENT log id, so ids also rise within each room.
//
// Expected handler use (SendChat / GetChatLog, and RaidRoomInfo's WCA9VhL3):
//   member  = isChatMember(...)          -- no: reply success with no rows
//   SendChat: postStamp(...)             -- verdict is for logs and tests only
//   rows    = chatDelta(..., afterLogId) -- includes the sender's own new row,
//                                           which ends the client's resend loop
// Never refuse: -4000 in RaidChatScene throws the player back to scene 1201.
namespace gme::raid
{

/*! Rows per reply; a client asking from log id 0 gets the newest this many. */
inline constexpr int32_t kChatDeltaMax = 50;
/*! Rows kept per room: the client keeps 200 (UserChatLogInfoList::checkNum @0x1288074). */
inline constexpr int32_t kChatKeepPerRoom = 200;
/*! Rate limit per user: one stored stamp per this many milliseconds ... */
inline constexpr int64_t kChatMinGapMs = 1500;
/*! ... and at most this many per rolling minute. */
inline constexpr int32_t kChatPerMinute = 20;
/*!
* An identical stamp from the same user in the same room this recent is the
* client's resend, not a new message: RaidChatScene::checkSendSuccess
* @0x1957E7C resends after >= 1 s, up to 3 attempts, until a reply carries it.
*/
inline constexpr int64_t kChatResendWindowMs = 10000;

/*! StampMst ids (embedded from the client's F_STAMP_MST_Ver8.json; see RaidChat.cpp). */
bool isStampId(int32_t stampId);

/*! The chat keys of a SendChat / GetChatLog / RaidRoomInfo body ("" / 0 when absent). */
struct ChatRequest
{
	std::string roomId;      // 8iBuZa3n.8VYd6xSX
	std::string chatType;    // JQ6hW0Ma.8Fj06n3K (SendChat only)
	std::string content;     // JQ6hW0Ma.3kQUaGx0 (SendChat only)
	int64_t afterLogId = 0;  // pw38qdui.NLKce95n: newest log id the client holds
};
ChatRequest readChatRequest(const RaidRequest& req);

/*!
* True when `userId` may read and post in `roomId`: a non-kicked member of a
* room that is not closed (lobby or field).
*/
drogon::Task<bool> isChatMember(const db::Database database, const std::string roomId, const std::string userId);

/*!
* WCA9VhL3 rows with log_id > afterLogId, oldest first, at most kChatDeltaMax;
* afterLogId <= 0 -> the newest kChatDeltaMax (still oldest first).  Does not
* check membership (see isChatMember).
*/
drogon::Task<std::vector<::RaidChatLog>> chatDelta(
	const db::Database database, const std::string roomId, const int64_t afterLogId);

enum class ChatVerdict { Stored, Resend, Dropped };

/*!
* Validate (member, type "2"/"5", content a stamp id), dedupe a client resend,
* rate-limit, store and prune the room to kChatKeepPerRoom.  Never throws for
* bad input: anything unacceptable is Dropped.
*/
drogon::Task<ChatVerdict> postStamp(const db::Database database, const std::string roomId,
	const std::string userId, const std::string chatType, const std::string content);

/*!
* Delete a room's chat.  Call when the room closes: nothing reads a closed
* room's log, and without this raid_chat keeps up to kChatKeepPerRoom rows for
* every room ever opened.
*/
drogon::Task<void> forgetRoomChat(const db::Database database, const std::string roomId);

} // namespace gme::raid
