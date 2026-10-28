#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Guilds.hpp>

#include <optional>
#include <string>
#include <vector>

// Guilds between REAL players: search, applications, invites, the rules for
// who may manage whom, settings and the board.  The wire shapes live in
// net/guild.kdl; docs/features/GUILDS_SOCIAL_2026-10-17.md is the design.
//
// ONE GUILD PER PLAYER.  Every path that puts a real player into a guild runs
// in one transaction that first checks they are in none, so two accepted
// requests cannot leave them in two.  Computer-run friends are exempt: their
// ids are per-owner roster entries and the same id may sit in several guilds.
//
// AUTHORED POLICY (the live server's rules are not recovered):
//   - The Guild Master, Vice and Officers recruit: they send invites and
//     decide applications (the client enables Invite for ranks 1-3 only).
//   - Applications and invites wait; the other side decides.
//   - Ranks: the Guild Master manages everyone; a Vice manages Officers and
//     Members and may give ranks 3-4; nobody else manages anyone.
//   - One Vice at a time.  Making someone Guild Master hands the guild over
//     (they become 1, the old master Vice) -- the client offers no button for
//     it today.
//   - A Guild Master who leaves hands the guild to the highest-ranked,
//     longest-serving real member; with none left, the guild is dissolved.
//     Their computer-run friends leave with them (dropComputerRunMembers).
namespace gme
{

/*! user_guild_requests.kind */
inline constexpr int32_t kGuildRequestApply = 1;
inline constexpr int32_t kGuildRequestInvite = 2;

/*! Open requests a player may have outstanding.  AUTHORED. */
inline constexpr int32_t kMaxOpenApplications = 10;
/*! Open invites one guild may have outstanding.  AUTHORED. */
inline constexpr int32_t kMaxOpenGuildInvites = 30;
/*! Requests older than this are dropped.  AUTHORED, like FRIENDS_SPEC §6.5. */
inline constexpr int64_t kGuildRequestExpirySec = 14 * 24 * 3600;
/*! How many guilds a search or the recommended list returns.  AUTHORED. */
inline constexpr int32_t kGuildSearchLimit = 30;
/*! Name and description limits.  AUTHORED, matching what the edit boxes take. */
inline constexpr size_t kGuildNameMaxBytes = 48;
inline constexpr size_t kGuildDescriptionMaxBytes = 400;
/*! Board posts kept per guild, and the longest post.  AUTHORED. */
inline constexpr int32_t kGuildBoardKeep = 100;
inline constexpr size_t kGuildBoardPostMaxBytes = 400;

/*! One open application or invite. */
struct GuildRequestRow
{
	int32_t request_id = 0;
	int32_t guild_id = 0;
	std::string user_id;       // applicant, or the player invited
	int32_t kind = 0;
	std::string from_user_id;  // the inviter (invites only)
	int64_t created_at = 0;
};

/*! One board post. */
struct GuildBoardPost
{
	int32_t post_id = 0;
	std::string user_id;
	std::string handle_name;
	std::string message;
	int64_t created_at = 0;
};

enum class GuildApplyOutcome { Applied, AlreadyApplied, AlreadyMember, InAnotherGuild, NoSuchGuild, GuildFull, Limited };
enum class GuildInviteOutcome { Invited, AlreadyInvited, AlreadyInGuild, NotAllowed, GuildFull, NotFound, Limited };
enum class GuildRequestOutcome { Joined, Declined, NotFound, NotAllowed, GuildFull, InAnotherGuild };
enum class GuildLeaveOutcome { Left, HandedOver, Dissolved, NotMember };

/*!
* The GuildInfo block for a guild as a given player sees it: the real Guild
* Master's name, the guild's leaderboard place (P_RANK) and, for a member, the
* unread board badge.  withMaxMembers adds Nt38aDqi (GuildMemberGuildInfo only).
*/
drogon::Task<::GuildInfo> guildInfoFor(
	db::Database database, const GuildRow guild, const std::string viewerId, bool withMaxMembers = false);

/*! GuildInfo blocks for a list of guilds (search / recommended): no board badge. */
drogon::Task<std::vector<::GuildInfo>> guildInfoList(db::Database database, const std::vector<GuildRow> guilds);

/*!
* Remove a guild's computer-run members.  They come from the OWNER's friend
* roster (their ids are per-owner roster slots, DCF00001...), so when the
* guild changes hands they leave with the old owner rather than turning into
* the new owner's friends of the same slot.
*/
drogon::Task<void> dropComputerRunMembers(db::Database database, int32_t guildId);

/*! Trim whitespace and cut to at most `limit` bytes without splitting a UTF-8 character. */
std::string cleanGuildText(const std::string& text, size_t limit);

/*! A crest id the server keeps: 1..999, else the default. */
int32_t cleanGuildArtId(int32_t artId);

/*! The rank a member is SHOWN with: the owner is always 1, others 2-4. */
int32_t shownRank(const GuildRow& guild, const std::string& memberId, int32_t stored);

/*! The guild with this id, with its live member count. */
drogon::Task<std::optional<GuildRow>> loadGuildById(db::Database database, int32_t guildId);

/*! The guild a player belongs to, by user id. */
drogon::Task<std::optional<GuildRow>> guildOfUser(db::Database database, const std::string& userId);

/*! A member's shown rank, or 0 when they are not in this guild. */
drogon::Task<int32_t> rankIn(db::Database database, const GuildRow& guild, const std::string& userId);

/*!
* Guilds matching a search.  Digits match a guild id exactly or a name; other
* text matches names, case-insensitively.  Empty text is the recommended list:
* guilds with room, busiest first.  The viewer's own guild is left out.
*/
drogon::Task<std::vector<GuildRow>> searchGuilds(
	db::Database database, const std::string& viewerId, const std::string& text);

/*! Ask to join a guild.  Waits for a recruiting member (rank 1-3) to decide. */
drogon::Task<GuildApplyOutcome> applyToGuild(
	db::Database database, const std::string& applicantId, int32_t guildId);

/*! Invite a real player into the inviter's guild.  The invitee decides. */
drogon::Task<GuildInviteOutcome> inviteToGuild(
	db::Database database, const std::string& inviterId, int32_t guildId, const std::string& inviteeId);

/*!
* Accept or decline one request by its id.
*
* An APPLICATION is decided by a recruiting member (rank 1-3) of that guild; an
* INVITE by the player invited, or withdrawn by a recruiting member.  Accepting checks the guild still has room
* and the player is still in no guild, then drops every other request they
* had open.
*/
drogon::Task<GuildRequestOutcome> resolveGuildRequest(
	db::Database database, const std::string& actorId, int32_t requestId, bool accept);

/*! Withdraw your own application, or decline an invite to you (the message box). */
drogon::Task<bool> dropOwnRequest(db::Database database, const std::string& userId, int32_t requestId);

/*! Open applications to a guild, oldest first (stale ones are dropped first). */
drogon::Task<std::vector<GuildRequestRow>> pendingApplications(db::Database database, int32_t guildId);

/*! Open invites to a player, newest first. */
drogon::Task<std::vector<GuildRequestRow>> pendingInvites(db::Database database, const std::string& userId);

/*! Guilds a player has applied to and not yet heard back from. */
drogon::Task<std::vector<GuildRequestRow>> pendingApplicationsBy(db::Database database, const std::string& userId);

/*!
* Leave the guild.  A Guild Master hands it on (see the header) or, as its
* last real member, dissolves it.
*/
drogon::Task<GuildLeaveOutcome> leaveGuild(db::Database database, const std::string& userId);

/*!
* Change the description and crest -- all GuildUpdate can send (the client has
* no rename).  Guild Master only, as the Hall only offers the edit to rank 1.
* A name, when given, renames (not reachable from the client today).
*/
drogon::Task<bool> updateGuildSettings(
	db::Database database, const std::string& actorId,
	const std::optional<std::string>& name, const std::optional<std::string>& description,
	const std::optional<int32_t>& artId);

/*! Dissolve the guild.  Guild Master only. */
drogon::Task<bool> disbandGuild(db::Database database, const std::string& actorId);

/*! The board, oldest first, at most kGuildBoardKeep posts. */
drogon::Task<std::vector<GuildBoardPost>> guildBoard(db::Database database, int32_t guildId);

/*! Post to the poster's own guild's board.  False when refused. */
drogon::Task<bool> postToGuildBoard(db::Database database, const std::string& userId, const std::string& text);

/*! Whether the guild board has a post by someone else newer than this member last saw. */
drogon::Task<bool> guildBoardUnread(db::Database database, int32_t guildId, const std::string& userId);

/*! Record that this member has seen every post so far. */
drogon::Task<void> markGuildBoardSeen(db::Database database, int32_t guildId, const std::string& userId);

/*!
* Every guild in leaderboard order: prestige, then experience, then members,
* then the oldest first.  AUTHORED: prestige is only earned by guild raids,
* which do not run yet, so today the order is experience and size.
*/
drogon::Task<std::vector<GuildRow>> guildLeaderboard(db::Database database);

/*! A guild's 1-based place on guildLeaderboard, 0 when it is not there. */
drogon::Task<int32_t> guildLeaderboardPlace(db::Database database, int32_t guildId);

} // namespace gme
