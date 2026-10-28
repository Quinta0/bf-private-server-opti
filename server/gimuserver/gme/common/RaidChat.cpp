#include "App.hpp"

#include <gimuserver/gme/common/RaidChat.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <map>
#include <optional>

namespace gme::raid
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (see Raids.cpp): every statement
// below numbers its $N left to right and never repeats one.

namespace
{

/*!
* Every StampMst id (FfZ30yBt), copied from the client's F_STAMP_MST_Ver8.json
* (mst-decoded/3-mst-translated; deploy/mst has no stamp table).  100 rows:
* stamp sets 1..6 hold 1..101 except 49 and 66..69, and set 1 also holds
* 8000..8003.  Sets 4..6 are bought with gems (F_STAMP_SET_MST), but the server
* tracks no stamp ownership, so every id is accepted (MULTIPLAYER 8, unknown 9).
*/
constexpr std::array<int32_t, 100> kStampIds{
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
	31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 50, 51,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
	62, 63, 64, 65, 70, 71, 72, 73, 74, 75,
	76, 77, 78, 79, 80, 81, 82, 83, 84, 85,
	86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
	96, 97, 98, 99, 100, 101, 8000, 8001, 8002, 8003,
};
static_assert(std::is_sorted(kStampIds.begin(), kStampIds.end()), "isStampId binary-searches kStampIds");

/*!
* The two types the client sends (touchStamp "2", touchUserStamp "5").  "1"
* is a text balloon and "3"/"4" are log lines drawn verbatim
* (RaidChatScene::setChatLogLayout @0x1952A3C): never accepted from a client.
*/
constexpr const char* kChatStamp = "2";
constexpr const char* kChatMemberStamp = "5";
/*! The rolling window of kChatPerMinute. */
constexpr int64_t kChatRateWindowMs = 60'000;
/*!
* Sender art in the balloon: the base artwork, as the guild cards send it
* (Guilds.cpp: growth types >= 2 append _N and can name missing thumbnails).
*/
constexpr int32_t kUnitImgBase = 1;
/*! Longest client text echoed into a log line. */
constexpr size_t kLogTextMax = 16;

int64_t nowMillis()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

/*! raid_chat.room_id is an INTEGER: a room id that is not a plain positive decimal names no room. */
std::optional<int64_t> roomKey(const std::string& roomId)
{
	int64_t key = 0;
	const auto* first = roomId.data();
	const auto* last = first + roomId.size();
	const auto [end, ec] = std::from_chars(first, last, key);
	if (roomId.empty() || ec != std::errc{} || end != last || key <= 0)
		return std::nullopt;
	return key;
}

/*!
* The stamp id `content` names, accepted only in the exact form the client
* writes (CommonUtils::IntToString): "7", never "07", "+7" or " 7".  Anything
* else would be a second spelling of a stamp that the client's own
* checkSendSuccess (same type AND content) would never match.
*/
std::optional<int32_t> stampOf(const std::string& content)
{
	if (content.empty() || content.size() > 9)
		return std::nullopt;
	int32_t id = 0;
	const auto* last = content.data() + content.size();
	const auto [end, ec] = std::from_chars(content.data(), last, id);
	if (ec != std::errc{} || end != last || std::to_string(id) != content)
		return std::nullopt;
	return id;
}

/*! Client text fit for one log line: alphanumerics only, short. */
std::string logSafe(const std::string& text)
{
	std::string out;
	for (const char c : text)
	{
		if (out.size() >= kLogTextMax)
			break;
		if (std::isalnum(static_cast<unsigned char>(c)))
			out.push_back(c);
	}
	return out;
}

/*! ToSxkU34, drawn verbatim by RaidChatScene::drawTime: "HH:MM" UTC (width INFERRED, 4.8). */
std::string clockText(int64_t createdAtMs)
{
	const auto t = static_cast<std::time_t>(createdAtMs / 1000);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char buf[8]{};
	std::strftime(buf, sizeof(buf), "%H:%M", &tm);
	return buf;
}

::RaidChatLog chatRow(const drogon::orm::Row& row, const RaidProfile& sender)
{
	::RaidChatLog out{};
	out.log_id = row["log_id"].as<int64_t>();
	out.date = clockText(row["created_at"].as<int64_t>());
	out.user_id = row["user_id"].as<std::string>();
	out.user_name = sender.name;
	out.leader_unit_id = sender.leader_unit_id;
	out.chat_type = row["chat_type"].as<std::string>();
	out.content = row["content"].as<std::string>();
	out.unit_img_type = kUnitImgBase;
	return out;
}

drogon::Task<drogon::orm::Result> deltaRows(const db::Database database, const int64_t room, const int64_t afterLogId)
{
	if (afterLogId > 0)
	{
		co_return co_await database->execSqlCoro(
			"SELECT log_id, user_id, chat_type, content, created_at FROM raid_chat"
			" WHERE room_id = $1 AND log_id > $2 ORDER BY log_id LIMIT $3;",
			room, afterLogId, kChatDeltaMax);
	}
	// A client holding nothing (a fresh lobby) gets the tail of the log, still
	// oldest first: the client sorts anyway, but the contract is ascending.
	co_return co_await database->execSqlCoro(
		"SELECT log_id, user_id, chat_type, content, created_at FROM ("
		"SELECT log_id, user_id, chat_type, content, created_at FROM raid_chat"
		" WHERE room_id = $1 ORDER BY log_id DESC LIMIT $2) ORDER BY log_id;",
		room, kChatDeltaMax);
}

/*! The same stamp from the same user in this room within kChatResendWindowMs. */
drogon::Task<bool> isResend(const db::Database database, const int64_t room, const std::string userId,
	const std::string chatType, const std::string content, const int64_t now)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_chat WHERE room_id = $1 AND user_id = $2 AND chat_type = $3"
		" AND content = $4 AND created_at >= $5 LIMIT 1;",
		room, userId, chatType, content, now - kChatResendWindowMs);
	co_return !rows.empty();
}

/*!
* Insert the row unless the user is over the rate limit.  The limit and the
* write are ONE statement, so two sends racing on the shared connection cannot
* both pass the check (MULTIPLAYER 3.3 rule 5).  Per user, not per room: a
* player is in one room at a time, and hopping rooms must not reset the limit.
* @return True when the row was stored.
*/
drogon::Task<bool> insertWithinRate(const db::Database database, const int64_t room, const std::string userId,
	const std::string chatType, const std::string content, const int64_t now)
{
	const auto rows = co_await database->execSqlCoro(
		"INSERT INTO raid_chat(room_id, user_id, chat_type, content, created_at)"
		" SELECT $1, $2, $3, $4, $5"
		" WHERE NOT EXISTS (SELECT 1 FROM raid_chat WHERE user_id = $6 AND created_at > $7)"
		" AND (SELECT COUNT(*) FROM raid_chat WHERE user_id = $8 AND created_at > $9) < $10"
		" RETURNING log_id;",
		room, userId, chatType, content, now,
		userId, now - kChatMinGapMs,
		userId, now - kChatRateWindowMs, kChatPerMinute);
	co_return !rows.empty();
}

/*! Keep the newest kChatKeepPerRoom rows: the client never shows more (checkNum). */
drogon::Task<void> pruneChat(const db::Database database, const int64_t room)
{
	// The subquery is the newest row that falls OUTSIDE the kept window; NULL
	// (nothing deleted) while the room has kChatKeepPerRoom rows or fewer.
	co_await database->execSqlCoro(
		"DELETE FROM raid_chat WHERE room_id = $1 AND log_id <= (SELECT log_id FROM raid_chat"
		" WHERE room_id = $2 ORDER BY log_id DESC LIMIT 1 OFFSET $3);",
		room, room, kChatKeepPerRoom);
}

} // namespace

bool isStampId(int32_t stampId)
{
	return std::binary_search(kStampIds.begin(), kStampIds.end(), stampId);
}

ChatRequest readChatRequest(const RaidRequest& req)
{
	ChatRequest out{};
	out.roomId = req.text("8iBuZa3n", "8VYd6xSX");
	out.chatType = req.text("JQ6hW0Ma", "8Fj06n3K");
	out.content = req.text("JQ6hW0Ma", "3kQUaGx0");
	out.afterLogId = RaidRequest::numberOf(req.node("pw38qdui"), "NLKce95n");
	return out;
}

drogon::Task<bool> isChatMember(const db::Database database, const std::string roomId, const std::string userId)
{
	const auto room = roomKey(roomId);
	if (!room)
		co_return false;
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_room_member m JOIN raid_room r ON r.room_id = m.room_id"
		" WHERE m.room_id = $1 AND m.user_id = $2 AND m.kicked = 0 AND r.status != $3;",
		*room, userId, kRoomClosed);
	co_return !rows.empty();
}

drogon::Task<std::vector<::RaidChatLog>> chatDelta(
	const db::Database database, const std::string roomId, const int64_t afterLogId)
{
	std::vector<::RaidChatLog> out;
	const auto room = roomKey(roomId);
	if (!room)
		co_return out;
	const auto rows = co_await deltaRows(database, *room, afterLogId);

	// A room has at most a handful of senders: load each profile once, not once
	// per row (this runs on every 4 s lobby poll of every member).
	std::map<std::string, RaidProfile> senders;
	for (const auto& row : rows)
	{
		const auto userId = row["user_id"].as<std::string>();
		if (!senders.contains(userId))
		{
			const auto profile = co_await loadProfile(database, userId);
			senders.emplace(userId, profile);
		}
		out.push_back(chatRow(row, senders.at(userId)));
	}
	co_return out;
}

drogon::Task<ChatVerdict> postStamp(const db::Database database, const std::string roomId,
	const std::string userId, const std::string chatType, const std::string content)
{
	const auto room = roomKey(roomId);
	const auto stamp = stampOf(content);
	const bool stampType = chatType == kChatStamp || chatType == kChatMemberStamp;
	if (!room || !stampType || !stamp || !isStampId(*stamp))
	{
		// The client only ever sends a stamp id; anything else is a modified
		// client or a test, so it is dropped without a refusal.
		LOG_WARN << "Raid chat: dropped a non-stamp message from " << userId << " in room " << logSafe(roomId)
			<< " (type '" << logSafe(chatType) << "', content '" << logSafe(content) << "')";
		co_return ChatVerdict::Dropped;
	}
	if (!(co_await isChatMember(database, roomId, userId)))
	{
		// A kick or a closed room can race the chat scene; harmless, no rows.
		LOG_INFO << "Raid chat: " << userId << " is not a member of room " << *room << "; stamp dropped";
		co_return ChatVerdict::Dropped;
	}

	const auto now = nowMillis();
	// Before the rate limit: a resend arrives >= 1 s after the original, which
	// is inside kChatMinGapMs, and must still read as "already stored".
	if (co_await isResend(database, *room, userId, chatType, content, now))
		co_return ChatVerdict::Resend;
	if (!(co_await insertWithinRate(database, *room, userId, chatType, content, now)))
	{
		LOG_INFO << "Raid chat: " << userId << " is over the rate limit; stamp " << *stamp << " dropped";
		co_return ChatVerdict::Dropped;
	}
	co_await pruneChat(database, *room);
	co_return ChatVerdict::Stored;
}

drogon::Task<void> forgetRoomChat(const db::Database database, const std::string roomId)
{
	const auto room = roomKey(roomId);
	if (!room)
		co_return;
	co_await database->execSqlCoro("DELETE FROM raid_chat WHERE room_id = $1;", *room);
}

} // namespace gme::raid
