#include "App.hpp"

#include <gimuserver/gme/common/GuildSocial.hpp>
#include <gimuserver/gme/common/GameDay.hpp>   // utcDateToday(): founding / join dates
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace gme
{

namespace
{

int64_t nowSec()
{
	return static_cast<int64_t>(std::time(nullptr));
}

// Every guild query reads the same columns, with the live member count.
constexpr const char* kGuildSelect =
	"SELECT g.guild_id, g.owner_user_id, g.name, g.description, g.guild_art_id,"
	" g.experience, g.prestige_point, g.created_day,"
	" (SELECT COUNT(*) FROM user_guild_members m WHERE m.guild_id = g.guild_id) AS members_count"
	" FROM user_guilds g";

GuildRow guildFromRow(const drogon::orm::Row& r)
{
	GuildRow row{};
	row.guild_id = r["guild_id"].as<int32_t>();
	row.owner_user_id = r["owner_user_id"].as<std::string>();
	row.name = r["name"].as<std::string>();
	row.description = r["description"].as<std::string>();
	row.guild_art_id = r["guild_art_id"].as<int32_t>();
	row.experience = r["experience"].as<int32_t>();
	row.prestige_point = r["prestige_point"].as<int32_t>();
	row.created_day = r["created_day"].as<std::string>();
	row.members_count = r["members_count"].as<int32_t>();
	return row;
}

GuildRequestRow requestFromRow(const drogon::orm::Row& r)
{
	GuildRequestRow row{};
	row.request_id = r["request_id"].as<int32_t>();
	row.guild_id = r["guild_id"].as<int32_t>();
	row.user_id = r["user_id"].as<std::string>();
	row.kind = r["kind"].as<int32_t>();
	row.from_user_id = r["from_user_id"].as<std::string>();
	row.created_at = r["created_at"].as<int64_t>();
	return row;
}

bool hasRoom(const GuildRow& guild)
{
	return guild.members_count < guildMaxMembers(guildLevelFor(guild.experience));
}

// AUTHORED (GuildSocial.hpp): Guild Master, Vice and Officers recruit -- the
// client enables the Invite button for member_type 1-3 (GameUtils::
// setGuildInviteButton @0x1ebe3f4), so the same ranks send invites and decide
// applications.
bool recruits(int32_t rank)
{
	return rank >= kGuildRankMaster && rank <= kGuildRankOfficer;
}

// The real member who takes over when the Guild Master leaves: highest rank,
// then longest in the guild.  Computer-run friends cannot run a guild.
drogon::Task<std::optional<std::string>> successorOf(db::Database database, const GuildRow& guild)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT m.member_id FROM user_guild_members m JOIN user_info i ON i.id = m.member_id"
		" WHERE m.guild_id = $1 AND m.member_id != $2"
		" ORDER BY m.member_type ASC, m.joined_day ASC, m.member_id ASC LIMIT 1;",
		guild.guild_id, guild.owner_user_id);
	if (rows.empty())
		co_return std::nullopt;
	co_return rows[0]["member_id"].as<std::string>();
}

drogon::Task<void> dissolve(db::Database database, int32_t guildId)
{
	co_await database->execSqlCoro("DELETE FROM user_guild_members WHERE guild_id = $1;", guildId);
	co_await database->execSqlCoro("DELETE FROM user_guild_requests WHERE guild_id = $1;", guildId);
	co_await database->execSqlCoro("DELETE FROM user_guild_board WHERE guild_id = $1;", guildId);
	co_await database->execSqlCoro("DELETE FROM user_guilds WHERE guild_id = $1;", guildId);
}

std::string trimmed(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

// Cut to at most `limit` bytes without splitting a UTF-8 sequence.
std::string clipUtf8(const std::string& text, size_t limit)
{
	if (text.size() <= limit)
		return text;
	size_t end = limit;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
		--end;
	return text.substr(0, end);
}

} // namespace

drogon::Task<void> dropComputerRunMembers(db::Database database, int32_t guildId)
{
	const auto dropped = co_await database->execSqlCoro(
		"DELETE FROM user_guild_members WHERE guild_id = $1"
		" AND member_id NOT IN (SELECT id FROM user_info);", guildId);
	if (dropped.affectedRows() > 0)
		LOG_INFO << "Guilds: " << dropped.affectedRows() << " computer-run member(s) left guild "
			<< guildId << " with its old Guild Master";
}

std::string cleanGuildText(const std::string& text, size_t limit)
{
	return clipUtf8(trimmed(text), limit);
}

int32_t cleanGuildArtId(int32_t artId)
{
	// GuildSelectInsigniaScene only hands back ids >= 1, and locks crests above
	// the guild's level itself; GuildInsigniaMst is not loaded server-side.
	return artId >= 1 && artId < 1000 ? artId : kDefaultGuildArtId;
}

int32_t shownRank(const GuildRow& guild, const std::string& memberId, int32_t stored)
{
	if (memberId == guild.owner_user_id)
		return kGuildRankMaster;
	// Anything outside the named ranks would draw an unnamed rank.
	return (stored >= kGuildRankVice && stored <= kGuildRankMember) ? stored : kGuildRankMember;
}

drogon::Task<::GuildInfo> guildInfoFor(
	db::Database database, const GuildRow guild, const std::string viewerId, bool withMaxMembers)
{
	const auto master = co_await database->execSqlCoro(
		"SELECT username FROM user_info WHERE id = $1;", guild.owner_user_id);
	auto info = guildInfoBlock(guild, master.empty() ? std::string{} : master[0]["username"].as<std::string>());
	info.rank = co_await guildLeaderboardPlace(database, guild.guild_id);
	if (!viewerId.empty() && (co_await rankIn(database, guild, viewerId)) != 0)
		info.board_flag = co_await guildBoardUnread(database, guild.guild_id, viewerId);
	if (withMaxMembers)
		info.max_members = guildMaxMembers(guildLevelFor(guild.experience));
	co_return info;
}

drogon::Task<std::vector<::GuildInfo>> guildInfoList(db::Database database, const std::vector<GuildRow> guilds)
{
	std::vector<::GuildInfo> out;
	if (guilds.empty())
		co_return out;
	const auto board = co_await guildLeaderboard(database);
	for (const auto& guild : guilds)
	{
		const auto master = co_await database->execSqlCoro(
			"SELECT username FROM user_info WHERE id = $1;", guild.owner_user_id);
		auto info = guildInfoBlock(guild, master.empty() ? std::string{} : master[0]["username"].as<std::string>());
		info.rank = 0;
		for (size_t i = 0; i < board.size(); ++i)
		{
			if (board[i].guild_id == guild.guild_id)
			{
				info.rank = static_cast<int32_t>(i + 1);
				break;
			}
		}
		out.push_back(std::move(info));
	}
	co_return out;
}

drogon::Task<std::optional<GuildRow>> loadGuildById(db::Database database, int32_t guildId)
{
	const auto rows = co_await database->execSqlCoro(
		std::string(kGuildSelect) + " WHERE g.guild_id = $1;", guildId);
	if (rows.empty())
		co_return std::nullopt;
	co_return guildFromRow(rows[0]);
}

drogon::Task<std::optional<GuildRow>> guildOfUser(db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT guild_id FROM user_guild_members WHERE member_id = $1 ORDER BY joined_day LIMIT 1;",
		userId);
	if (rows.empty())
		co_return std::nullopt;
	co_return co_await loadGuildById(database, rows[0]["guild_id"].as<int32_t>());
}

drogon::Task<int32_t> rankIn(db::Database database, const GuildRow& guild, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT member_type FROM user_guild_members WHERE guild_id = $1 AND member_id = $2;",
		guild.guild_id, userId);
	if (rows.empty())
		co_return 0;
	co_return shownRank(guild, userId, rows[0]["member_type"].as<int32_t>());
}

drogon::Task<std::vector<GuildRow>> searchGuilds(
	db::Database database, const std::string& viewerId, const std::string& text)
{
	// Longer than any guild name can be is not a search, just work.
	const auto query = clipUtf8(trimmed(text), kGuildNameMaxBytes);
	const auto own = co_await guildOfUser(database, viewerId);
	const int32_t ownId = own ? own->guild_id : 0;

	std::vector<GuildRow> out;
	if (query.empty())
	{
		// The recommended list: guilds a player could actually join, busiest
		// first, so a new player lands somewhere with people in it.
		for (const auto& r : co_await database->execSqlCoro(
			std::string(kGuildSelect) + " WHERE g.guild_id != $1"
			" ORDER BY members_count DESC, g.guild_id DESC LIMIT 200;", ownId))
		{
			auto row = guildFromRow(r);
			if (!hasRoom(row))
				continue;
			out.push_back(std::move(row));
			if (static_cast<int32_t>(out.size()) >= kGuildSearchLimit)
				break;
		}
		co_return out;
	}

	// instr(lower(...)) rather than LIKE, so % and _ in a typed name are just
	// characters.  A run of digits may also be a guild id.
	const bool digits = std::all_of(query.begin(), query.end(),
		[](unsigned char c) { return std::isdigit(c) != 0; }) && query.size() <= 9;
	const int32_t asId = digits ? std::stoi(query) : -1;
	for (const auto& r : co_await database->execSqlCoro(
		std::string(kGuildSelect) + " WHERE g.guild_id = $1 OR instr(lower(g.name), lower($2)) > 0"
		" ORDER BY (g.guild_id = $1) DESC, members_count DESC, g.guild_id DESC LIMIT $3;",
		asId, query, kGuildSearchLimit))
	{
		out.push_back(guildFromRow(r));
	}
	co_return out;
}

drogon::Task<GuildApplyOutcome> applyToGuild(
	db::Database database, const std::string& applicantId, int32_t guildId)
{
	auto tx = co_await gme::newTransaction(database);
	if (const auto current = co_await guildOfUser(tx, applicantId))
	{
		tx->rollback();
		co_return current->guild_id == guildId ? GuildApplyOutcome::AlreadyMember
			: GuildApplyOutcome::InAnotherGuild;
	}
	const auto guild = co_await loadGuildById(tx, guildId);
	if (!guild)
	{
		tx->rollback();
		co_return GuildApplyOutcome::NoSuchGuild;
	}
	if (!hasRoom(*guild))
	{
		tx->rollback();
		co_return GuildApplyOutcome::GuildFull;
	}
	co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE created_at < $1;",
		nowSec() - kGuildRequestExpirySec);
	const auto open = (co_await tx->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_guild_requests WHERE user_id = $1 AND kind = $2;",
		applicantId, kGuildRequestApply))[0]["n"].as<int32_t>();
	if (open >= kMaxOpenApplications)
	{
		tx->rollback();
		co_return GuildApplyOutcome::Limited;
	}
	const auto inserted = co_await tx->execSqlCoro(
		"INSERT OR IGNORE INTO user_guild_requests (guild_id, user_id, kind, from_user_id, created_at)"
		" VALUES ($1, $2, $3, '', $4);",
		guildId, applicantId, kGuildRequestApply, nowSec());
	if (inserted.affectedRows() == 0)
	{
		tx->rollback();
		co_return GuildApplyOutcome::AlreadyApplied;
	}
	if (!(co_await CommitTransaction(std::move(tx))))
		co_return GuildApplyOutcome::Limited;
	LOG_INFO << "Guilds: " << applicantId << " applied to guild " << guildId;
	co_return GuildApplyOutcome::Applied;
}

drogon::Task<GuildInviteOutcome> inviteToGuild(
	db::Database database, const std::string& inviterId, int32_t guildId, const std::string& inviteeId)
{
	if (inviteeId.empty() || inviteeId == inviterId)
		co_return GuildInviteOutcome::NotFound;

	auto tx = co_await gme::newTransaction(database);
	const auto guild = co_await guildOfUser(tx, inviterId);
	if (!guild || guild->guild_id != guildId || !recruits(co_await rankIn(tx, *guild, inviterId)))
	{
		tx->rollback();
		co_return GuildInviteOutcome::NotAllowed;
	}
	if (!(co_await isRealPlayer(tx, inviteeId)))
	{
		tx->rollback();
		co_return GuildInviteOutcome::NotFound;
	}
	if (co_await guildOfUser(tx, inviteeId))
	{
		tx->rollback();
		co_return GuildInviteOutcome::AlreadyInGuild;
	}
	if (!hasRoom(*guild))
	{
		tx->rollback();
		co_return GuildInviteOutcome::GuildFull;
	}
	co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE created_at < $1;",
		nowSec() - kGuildRequestExpirySec);
	const auto open = (co_await tx->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_guild_requests WHERE guild_id = $1 AND kind = $2;",
		guildId, kGuildRequestInvite))[0]["n"].as<int32_t>();
	if (open >= kMaxOpenGuildInvites)
	{
		tx->rollback();
		co_return GuildInviteOutcome::Limited;
	}
	const auto inserted = co_await tx->execSqlCoro(
		"INSERT OR IGNORE INTO user_guild_requests (guild_id, user_id, kind, from_user_id, created_at)"
		" VALUES ($1, $2, $3, $4, $5);",
		guildId, inviteeId, kGuildRequestInvite, inviterId, nowSec());
	if (inserted.affectedRows() == 0)
	{
		tx->rollback();
		co_return GuildInviteOutcome::AlreadyInvited;
	}
	if (!(co_await CommitTransaction(std::move(tx))))
		co_return GuildInviteOutcome::Limited;
	LOG_INFO << "Guilds: " << inviterId << " invited " << inviteeId << " to guild " << guildId;
	co_return GuildInviteOutcome::Invited;
}

drogon::Task<GuildRequestOutcome> resolveGuildRequest(
	db::Database database, const std::string& actorId, int32_t requestId, bool accept)
{
	auto tx = co_await gme::newTransaction(database);
	// Expired requests are purged lazily, so the age is checked here too.
	const auto rows = co_await tx->execSqlCoro(
		"SELECT request_id, guild_id, user_id, kind, from_user_id, created_at"
		" FROM user_guild_requests WHERE request_id = $1 AND created_at >= $2;",
		requestId, nowSec() - kGuildRequestExpirySec);
	if (rows.empty())
	{
		tx->rollback();
		co_return GuildRequestOutcome::NotFound;
	}
	const auto request = requestFromRow(rows[0]);
	const auto guild = co_await loadGuildById(tx, request.guild_id);
	if (!guild)
	{
		co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE request_id = $1;", requestId);
		co_await CommitTransaction(std::move(tx));
		co_return GuildRequestOutcome::NotFound;
	}

	const bool allowed = request.kind == kGuildRequestInvite
		? actorId == request.user_id
		: recruits(co_await rankIn(tx, *guild, actorId));
	// A guild may also withdraw an invite it sent (the Requests screen's Cancel).
	const bool withdrawsInvite = !accept && request.kind == kGuildRequestInvite
		&& recruits(co_await rankIn(tx, *guild, actorId));
	if (!allowed && !withdrawsInvite)
	{
		tx->rollback();
		LOG_WARN << "Guilds: " << actorId << " may not decide request " << requestId;
		co_return GuildRequestOutcome::NotAllowed;
	}

	if (!accept)
	{
		co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE request_id = $1;", requestId);
		if (!(co_await CommitTransaction(std::move(tx))))
			co_return GuildRequestOutcome::NotFound;
		LOG_INFO << "Guilds: " << actorId << " declined request " << requestId
			<< " (" << request.user_id << ", guild " << request.guild_id << ")";
		co_return GuildRequestOutcome::Declined;
	}

	if (co_await guildOfUser(tx, request.user_id))
	{
		// Joined somewhere else meanwhile: this request is spent.
		co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE request_id = $1;", requestId);
		co_await CommitTransaction(std::move(tx));
		co_return GuildRequestOutcome::InAnotherGuild;
	}
	if (!hasRoom(*guild))
	{
		tx->rollback();
		co_return GuildRequestOutcome::GuildFull;
	}

	co_await tx->execSqlCoro(
		"INSERT INTO user_guild_members (guild_id, member_id, joined_day, member_type)"
		" VALUES ($1, $2, $3, $4);",
		guild->guild_id, request.user_id, utcDateToday(), kGuildRankMember);
	// In a guild now: every other open request for them is moot.
	co_await tx->execSqlCoro("DELETE FROM user_guild_requests WHERE user_id = $1;", request.user_id);
	if (!(co_await CommitTransaction(std::move(tx))))
		co_return GuildRequestOutcome::NotFound;
	LOG_INFO << "Guilds: " << request.user_id << " joined guild " << guild->guild_id
		<< " (request " << requestId << " accepted by " << actorId << ")";
	co_return GuildRequestOutcome::Joined;
}

drogon::Task<std::vector<GuildRequestRow>> pendingApplications(db::Database database, int32_t guildId)
{
	co_await database->execSqlCoro("DELETE FROM user_guild_requests WHERE created_at < $1;",
		nowSec() - kGuildRequestExpirySec);
	std::vector<GuildRequestRow> out;
	for (const auto& r : co_await database->execSqlCoro(
		"SELECT request_id, guild_id, user_id, kind, from_user_id, created_at"
		" FROM user_guild_requests WHERE guild_id = $1 AND kind = $2 ORDER BY request_id;",
		guildId, kGuildRequestApply))
	{
		out.push_back(requestFromRow(r));
	}
	co_return out;
}

drogon::Task<std::vector<GuildRequestRow>> pendingInvites(db::Database database, const std::string& userId)
{
	co_await database->execSqlCoro("DELETE FROM user_guild_requests WHERE created_at < $1;",
		nowSec() - kGuildRequestExpirySec);
	std::vector<GuildRequestRow> out;
	for (const auto& r : co_await database->execSqlCoro(
		"SELECT request_id, guild_id, user_id, kind, from_user_id, created_at"
		" FROM user_guild_requests WHERE user_id = $1 AND kind = $2 ORDER BY request_id DESC;",
		userId, kGuildRequestInvite))
	{
		out.push_back(requestFromRow(r));
	}
	co_return out;
}

drogon::Task<std::vector<GuildRequestRow>> pendingApplicationsBy(db::Database database, const std::string& userId)
{
	std::vector<GuildRequestRow> out;
	for (const auto& r : co_await database->execSqlCoro(
		"SELECT request_id, guild_id, user_id, kind, from_user_id, created_at"
		" FROM user_guild_requests WHERE user_id = $1 AND kind = $2 AND created_at >= $3"
		" ORDER BY request_id DESC;",
		userId, kGuildRequestApply, nowSec() - kGuildRequestExpirySec))
	{
		out.push_back(requestFromRow(r));
	}
	co_return out;
}

drogon::Task<bool> dropOwnRequest(db::Database database, const std::string& userId, int32_t requestId)
{
	const auto removed = co_await database->execSqlCoro(
		"DELETE FROM user_guild_requests WHERE request_id = $1 AND user_id = $2;", requestId, userId);
	if (removed.affectedRows() > 0)
		LOG_INFO << "Guilds: " << userId << " withdrew or declined request " << requestId;
	co_return removed.affectedRows() > 0;
}

drogon::Task<GuildLeaveOutcome> leaveGuild(db::Database database, const std::string& userId)
{
	auto tx = co_await gme::newTransaction(database);
	const auto guild = co_await guildOfUser(tx, userId);
	if (!guild)
	{
		tx->rollback();
		co_return GuildLeaveOutcome::NotMember;
	}

	if (guild->owner_user_id != userId)
	{
		co_await tx->execSqlCoro(
			"DELETE FROM user_guild_members WHERE guild_id = $1 AND member_id = $2;",
			guild->guild_id, userId);
		if (!(co_await CommitTransaction(std::move(tx))))
			co_return GuildLeaveOutcome::NotMember;
		LOG_INFO << "Guilds: " << userId << " left guild " << guild->guild_id;
		co_return GuildLeaveOutcome::Left;
	}

	const auto heir = co_await successorOf(tx, *guild);
	if (!heir)
	{
		co_await dissolve(tx, guild->guild_id);
		if (!(co_await CommitTransaction(std::move(tx))))
			co_return GuildLeaveOutcome::NotMember;
		LOG_INFO << "Guilds: " << userId << " left guild " << guild->guild_id
			<< " (\"" << guild->name << "\") as its last real member; dissolved";
		co_return GuildLeaveOutcome::Dissolved;
	}

	co_await tx->execSqlCoro(
		"DELETE FROM user_guild_members WHERE guild_id = $1 AND member_id = $2;",
		guild->guild_id, userId);
	co_await dropComputerRunMembers(tx, guild->guild_id);
	co_await tx->execSqlCoro(
		"UPDATE user_guilds SET owner_user_id = $1 WHERE guild_id = $2;", *heir, guild->guild_id);
	co_await tx->execSqlCoro(
		"UPDATE user_guild_members SET member_type = $1 WHERE guild_id = $2 AND member_id = $3;",
		kGuildRankMaster, guild->guild_id, *heir);
	if (!(co_await CommitTransaction(std::move(tx))))
		co_return GuildLeaveOutcome::NotMember;
	LOG_INFO << "Guilds: Guild Master " << userId << " left guild " << guild->guild_id
		<< "; " << *heir << " is the new Guild Master";
	co_return GuildLeaveOutcome::HandedOver;
}

drogon::Task<bool> updateGuildSettings(
	db::Database database, const std::string& actorId,
	const std::optional<std::string>& name, const std::optional<std::string>& description,
	const std::optional<int32_t>& artId)
{
	const auto guild = co_await guildOfUser(database, actorId);
	if (!guild || guild->owner_user_id != actorId)
	{
		LOG_WARN << "Guilds: " << actorId << " is not a Guild Master; settings edit refused";
		co_return false;
	}

	auto newName = guild->name;
	if (name)
	{
		const auto typed = cleanGuildText(*name, kGuildNameMaxBytes);
		if (!typed.empty())
			newName = typed;
	}
	const auto newDescription = description
		? cleanGuildText(*description, kGuildDescriptionMaxBytes) : guild->description;
	const auto newArt = artId && *artId >= 1 ? cleanGuildArtId(*artId) : guild->guild_art_id;

	co_await database->execSqlCoro(
		"UPDATE user_guilds SET name = $1, description = $2, guild_art_id = $3 WHERE guild_id = $4;",
		newName, newDescription, newArt, guild->guild_id);
	LOG_INFO << "Guilds: " << actorId << " updated guild " << guild->guild_id
		<< " (name \"" << newName << "\", art " << newArt << ")";
	co_return true;
}

drogon::Task<bool> disbandGuild(db::Database database, const std::string& actorId)
{
	auto tx = co_await gme::newTransaction(database);
	const auto guild = co_await guildOfUser(tx, actorId);
	if (!guild || guild->owner_user_id != actorId)
	{
		tx->rollback();
		LOG_WARN << "Guilds: " << actorId << " is not a Guild Master; disband refused";
		co_return false;
	}
	co_await dissolve(tx, guild->guild_id);
	if (!(co_await CommitTransaction(std::move(tx))))
		co_return false;
	LOG_INFO << "Guilds: " << actorId << " disbanded guild " << guild->guild_id
		<< " (\"" << guild->name << "\")";
	co_return true;
}

drogon::Task<std::vector<GuildBoardPost>> guildBoard(db::Database database, int32_t guildId)
{
	std::vector<GuildBoardPost> out;
	for (const auto& r : co_await database->execSqlCoro(
		"SELECT b.post_id, b.user_id, COALESCE(i.username, b.user_id) AS handle_name,"
		" b.message, b.created_at FROM user_guild_board b"
		" LEFT JOIN user_info i ON i.id = b.user_id"
		" WHERE b.guild_id = $1 ORDER BY b.post_id DESC LIMIT $2;",
		guildId, kGuildBoardKeep))
	{
		GuildBoardPost post{};
		post.post_id = r["post_id"].as<int32_t>();
		post.user_id = r["user_id"].as<std::string>();
		post.handle_name = r["handle_name"].as<std::string>();
		post.message = r["message"].as<std::string>();
		post.created_at = r["created_at"].as<int64_t>();
		out.push_back(std::move(post));
	}
	std::reverse(out.begin(), out.end());
	co_return out;
}

drogon::Task<bool> postToGuildBoard(db::Database database, const std::string& userId, const std::string& text)
{
	const auto message = clipUtf8(trimmed(text), kGuildBoardPostMaxBytes);
	if (message.empty())
		co_return false;
	const auto guild = co_await guildOfUser(database, userId);
	if (!guild)
	{
		LOG_WARN << "Guilds: " << userId << " is in no guild; board post refused";
		co_return false;
	}
	co_await database->execSqlCoro(
		"INSERT INTO user_guild_board (guild_id, user_id, message, created_at) VALUES ($1, $2, $3, $4);",
		guild->guild_id, userId, message, nowSec());
	// Keep the board bounded: only the newest kGuildBoardKeep posts are shown.
	co_await database->execSqlCoro(
		"DELETE FROM user_guild_board WHERE guild_id = $1 AND post_id NOT IN"
		" (SELECT post_id FROM user_guild_board WHERE guild_id = $1 ORDER BY post_id DESC LIMIT $2);",
		guild->guild_id, kGuildBoardKeep);
	LOG_INFO << "Guilds: " << userId << " posted to the board of guild " << guild->guild_id;
	co_return true;
}

drogon::Task<bool> guildBoardUnread(db::Database database, int32_t guildId, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT EXISTS (SELECT 1 FROM user_guild_board b WHERE b.guild_id = $1 AND b.user_id != $2"
		" AND b.post_id > COALESCE((SELECT board_seen_post FROM user_guild_members"
		"  WHERE guild_id = $1 AND member_id = $2), 0)) AS unread;",
		guildId, userId);
	co_return !rows.empty() && rows[0]["unread"].as<int32_t>() != 0;
}

drogon::Task<void> markGuildBoardSeen(db::Database database, int32_t guildId, const std::string& userId)
{
	co_await database->execSqlCoro(
		"UPDATE user_guild_members SET board_seen_post ="
		" COALESCE((SELECT MAX(post_id) FROM user_guild_board WHERE guild_id = $1), 0)"
		" WHERE guild_id = $1 AND member_id = $2;",
		guildId, userId);
}

drogon::Task<std::vector<GuildRow>> guildLeaderboard(db::Database database)
{
	std::vector<GuildRow> out;
	for (const auto& r : co_await database->execSqlCoro(
		std::string(kGuildSelect) +
		" ORDER BY g.prestige_point DESC, g.experience DESC, members_count DESC, g.guild_id ASC;"))
	{
		out.push_back(guildFromRow(r));
	}
	co_return out;
}

drogon::Task<int32_t> guildLeaderboardPlace(db::Database database, int32_t guildId)
{
	const auto board = co_await guildLeaderboard(database);
	for (size_t i = 0; i < board.size(); ++i)
	{
		if (board[i].guild_id == guildId)
			co_return static_cast<int32_t>(i + 1);
	}
	co_return 0;
}

} // namespace gme
