#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Exchange.hpp>
#include <gimuserver/gme/common/GuildSocial.hpp>
#include <gimuserver/gme/common/Guilds.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <ctime>

// Guilds between real players: search, a friend's guild, the Requests screen,
// accepting and declining, settings and the board.  Wire shapes: net/guild.kdl;
// design and evidence: docs/features/GUILDS_SOCIAL_2026-10-17.md.
//
// Every refusal answers like a success.  These screens read nothing back but a
// success (or a message the session survives, which nothing here needs), and
// a handler error closes the session.  The reason is in the log.

namespace
{

template <typename Req>
Req parseGuildRequest(const char* name, const std::string& json)
{
	Req req{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
		LOG_WARN << name << ": parse error: " << glz::format_error(ec, json);
	return req;
}

// A display string for the board and the Requests screen; the client draws it
// verbatim and parses nothing.  UTC, since the players are not in one zone.
std::string displayTime(int64_t at)
{
	const std::time_t t = static_cast<std::time_t>(at);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%m/%d %H:%M UTC", &tm);
	return buf;
}

} // namespace

// GuildSearch (R38ba9M3) -- the search box on the recommended list.
//
// GuildSearchListScene::initConnect @0x1e63154 sends the keyword; the client
// never sends an empty one.  Matches names case-insensitively, and a guild id
// typed as digits.  An empty result is GUILD_SEARCH_NOT_FOUND, which is safe.
HANDLEF(GuildSearch)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildSearchReq>("GuildSearch", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const std::string text = req.search.empty() ? std::string{} : req.search.front().name;
	::GuildSearchResp resp{};
	resp.guilds = co_await gme::guildInfoList(theDb(),
		co_await gme::searchGuilds(theDb(), identity.userId, text));

	LOG_INFO << "GuildSearch: \"" << text << "\" from " << identity.userId << ": "
		<< resp.guilds.size() << " guild(s)";
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildMemberGuildInfo (38bSeq81) -- a profile's View Guild button.
//
// UNREGISTERED UNTIL 2026-10-17, so the tap closed the session; it was only
// unreachable because every friend card sent guild id 0.  The reply becomes
// GuildUserGuildInfo's search target and the profile opens GuildDetailScene on
// it, where Apply is.  Nothing to show is {}, which leaves the previous target:
// the button only appears for a profile whose guild id is not 0, so that is a
// player who left between the two requests.
HANDLEF(GuildMemberGuildInfo)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildMemberGuildInfoReq>("GuildMemberGuildInfo", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const std::string target = req.members.empty() ? std::string{} : req.members.front().user_id;
	const auto guild = target.empty() ? std::nullopt : co_await gme::guildOfUser(theDb(), target);
	if (!guild || !(co_await gme::isRealPlayer(theDb(), target)))
	{
		LOG_INFO << "GuildMemberGuildInfo: " << target << " is in no guild (asked by " << identity.userId << ")";
		co_return HandleResult::success("{}");
	}

	::GuildMemberGuildInfoResp resp{};
	resp.guild.push_back(co_await gme::guildInfoFor(theDb(), *guild, identity.userId, true));
	LOG_INFO << "GuildMemberGuildInfo: " << identity.userId << " viewed guild " << guild->guild_id
		<< " of " << target;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildJoinedList (3890ab5j) -- the guild's Requests screen.
//
// Sent ONLY by GuildRequestsListScene::initConnect @0x1e48b7c.  Until
// 2026-10-17 it shared GuildInfo's handler, so the screen was always empty and
// no application could ever be approved.  The rows are every open application
// to the caller's guild (type 1) and every invite it has outstanding (type 2);
// accepting or declining one sends GuildInviteManage with its invite id.
HANDLEF(GuildJoinedList)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildInfoReq>("GuildJoinedList", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto guild = co_await gme::guildOfUser(theDb(), identity.userId);
	if (!guild)
	{
		LOG_INFO << "GuildJoinedList: " << identity.userId << " is in no guild";
		co_return HandleResult::success("{}");
	}

	auto requests = co_await gme::pendingApplications(theDb(), guild->guild_id);
	for (const auto& r : co_await theDb()->execSqlCoro(
		"SELECT request_id, created_at, user_id FROM user_guild_requests"
		" WHERE guild_id = $1 AND kind = $2 ORDER BY request_id;",
		guild->guild_id, gme::kGuildRequestInvite))
	{
		gme::GuildRequestRow row{};
		row.request_id = r["request_id"].as<int32_t>();
		row.guild_id = guild->guild_id;
		row.user_id = r["user_id"].as<std::string>();
		row.kind = gme::kGuildRequestInvite;
		row.created_at = r["created_at"].as<int64_t>();
		requests.push_back(std::move(row));
	}

	::GuildJoinedListResp resp{};
	for (const auto& request : requests)
	{
		const auto card = co_await gme::loadPlayerCard(theDb(), request.user_id);
		if (!card)
			continue;
		::GuildJoinRequestInfo row{};
		row.invite_id = request.request_id;
		row.user_id = request.user_id;
		row.request_date = displayTime(request.created_at);
		// ⚠ NOT this guild's id.  setRequestsList(int,bool) @0x1e4a734 gives a
		// type-1 row Accept/Refuse only while sD73jd20 is 0 -- the applicant's
		// own guild, which is none -- and Cancel only otherwise.
		row.guild_id = request.kind == gme::kGuildRequestApply ? 0 : guild->guild_id;
		row.handle_name = card->handle_name;
		row.team_lv = card->level;
		row.friend_id = card->friend_id;
		row.unit_id = std::to_string(card->unit_id);
		row.unit_img_type = 1; // base artwork, as on every guild card
		row.unit_lv = card->unit_lv;
		row.request_type = request.kind;
		row.base_hp = card->base_hp;
		row.base_atk = card->base_atk;
		row.base_def = card->base_def;
		row.base_heal = card->base_rec;
		row.ext_hp = card->ext_hp;
		row.ext_atk = card->ext_atk;
		row.ext_def = card->ext_def;
		row.ext_heal = card->ext_rec;
		row.skill_id = std::to_string(card->bb_id);
		row.skill_lv = card->bb_lvl;
		row.extra_skill_id = std::to_string(card->sbb_id);
		row.extra_skill_lv = card->sbb_lvl;
		row.equipitem_id = std::to_string(card->sphere_1);
		row.equipitem_id2 = std::to_string(card->sphere_2);
		resp.requests.push_back(std::move(row));
	}

	LOG_INFO << "GuildJoinedList: " << resp.requests.size() << " open request(s) for guild "
		<< guild->guild_id << " (" << identity.userId << ")";
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildUpdate (92bDoqBi) -- the Hall's Save and Disband.
//
// One node: guild id, description, crest and an action (0 save, -1 disband).
// The client cannot rename.  It reads nothing back; on a disband it sets its
// own guild id to 0 and goes Home, and the next GuildInfo reports no guild.
HANDLEF(GuildUpdate)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildUpdateReq>("GuildUpdate", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	constexpr int32_t kGuildUpdateDisband = -1;
	for (const auto& node : req.guild)
	{
		if (node.action == kGuildUpdateDisband)
		{
			const bool done = co_await gme::disbandGuild(theDb(), identity.userId);
			LOG_INFO << "GuildUpdate: " << identity.userId << " disband of guild " << node.guild_id
				<< (done ? " done" : " refused");
			continue;
		}
		const bool done = co_await gme::updateGuildSettings(theDb(), identity.userId,
			std::nullopt, node.description, node.guild_art_id);
		LOG_INFO << "GuildUpdate: " << identity.userId << " edit of guild " << node.guild_id
			<< (done ? " saved" : " refused");
	}
	co_return HandleResult::success("{}");
}

namespace
{

drogon::Task<::GuildBoardResp> boardReply(const gme::GuildRow& guild)
{
	::GuildBoardResp resp{};
	// The thumbnail must be a unit whose files the scene already fetched:
	// GuildBoardScene::downloadFiles requests every member's unit on open but
	// nothing after a post.  So a current member's lead unit, else the
	// default picture ("1").
	const auto roster = co_await gme::guildRosterOf(theDb(), guild);
	auto posts = co_await gme::guildBoard(theDb(), guild.guild_id);
	// Newest first: the scene draws row 0 at the top and never scrolls.
	for (auto it = posts.rbegin(); it != posts.rend(); ++it)
	{
		::GuildBoardInfo row{};
		row.post_id = it->post_id;
		row.guild_id = guild.guild_id;
		row.user_id = it->user_id;
		row.message = it->message;
		row.created = displayTime(it->created_at);
		row.handle_name = it->handle_name;
		row.unit_id = "1";
		for (const auto& member : roster)
		{
			if (member.user_id == it->user_id)
			{
				row.friend_id = member.friend_id;
				row.team_lv = member.team_lv;
				if (member.unit_id > 0)
					row.unit_id = std::to_string(member.unit_id);
				break;
			}
		}
		resp.posts.push_back(std::move(row));
	}
	co_return resp;
}

} // namespace

// GuildBoardInfo (J93ki3Bw) -- opening the board.  Identity only on the wire.
// Opening it marks every post seen, which clears the Hall's unread badge
// (GuildInfo b6D19bg9) for this member.
HANDLEF(GuildBoardInfo)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildInfoReq>("GuildBoardInfo", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto guild = co_await gme::guildOfUser(theDb(), identity.userId);
	if (!guild)
		co_return HandleResult::success("{}");

	const auto resp = co_await boardReply(*guild);
	co_await gme::markGuildBoardSeen(theDb(), guild->guild_id, identity.userId);
	LOG_INFO << "GuildBoardInfo: " << resp.posts.size() << " post(s) in guild " << guild->guild_id
		<< " for " << identity.userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildBoardPost (X89bDai1) -- posting.  The reply MUST carry the refreshed
// board: GuildBoardScene::checkConnectResult @0x1D9F280 only redraws from the
// list and does not ask again.
HANDLEF(GuildBoardPost)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildBoardPostReq>("GuildBoardPost", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto guild = co_await gme::guildOfUser(theDb(), identity.userId);
	if (!guild)
		co_return HandleResult::success("{}");

	// AUTHORED: a burst limit, so one player cannot flood the board.
	if (!gme::allowRate("guild_board_post", identity.userId, 10, 60))
		LOG_WARN << "GuildBoardPost: " << identity.userId << " is posting too fast; post dropped";
	else if (!req.posts.empty())
		co_await gme::postToGuildBoard(theDb(), identity.userId, req.posts.front().message);

	const auto resp = co_await boardReply(*guild);
	co_await gme::markGuildBoardSeen(theDb(), guild->guild_id, identity.userId);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildInviteManage (1D8bba8D) -- accepting or declining, by invite id.
//
// TWO SENDERS, ONE SHAPE: {7ad38bad invite id, ad8D1kqP 1 accept / 0 decline}.
//   - GuildRequestsListScene (the guild's Requests screen): Accept on an
//     application, or Refuse / Refuse all / Cancel (0) on applications and the
//     guild's own invites.  noticeOK @0x1e4c504 then raises the member count
//     and drops the rows ITSELF and reads nothing, so the reply is {}: a guild
//     block would count the new member twice, and a roster would free members
//     the Hall underneath may still point at.
//   - GuildDetailScene opened from the message box: Accept on an invite (decline
//     goes through InboxMessageManage).  The client drops the message and pops
//     back to the inbox without asking for the guild, so the reply carries the
//     guild just joined -- GuildTownScene reads GuildUserGuildInfo directly.
// Stale or unauthorised ids answer {} as well: neither list can be emptied over
// the wire, so the client can hold rows the server has already dropped.
HANDLEF(GuildInviteManage)
{
	(void)session;
	const auto req = parseGuildRequest<::GuildInviteManageReq>("GuildInviteManage", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// Only the invitee can join through this request; the guild's side never
	// changes its own membership here.
	const bool wasGuildless = !(co_await gme::guildOfUser(theDb(), identity.userId)).has_value();
	for (const auto& node : req.nodes)
	{
		const bool accept = node.action == 1;
		const auto outcome = co_await gme::resolveGuildRequest(theDb(), identity.userId, node.user_id, accept);
		LOG_INFO << "GuildInviteManage: " << identity.userId << (accept ? " accepted" : " declined")
			<< " request " << node.user_id << " -- outcome " << static_cast<int>(outcome);
	}

	const auto guild = wasGuildless ? co_await gme::loadGuild(theDb(), identity) : std::nullopt;
	if (!guild)
		co_return HandleResult::success("{}");

	// The invitee just joined: hand their client the guild it is now in.
	::GuildInfoResp resp{};
	resp.guild.push_back(co_await gme::guildInfoFor(theDb(), *guild, identity.userId));
	resp.members = co_await gme::guildRoster(theDb(), identity);
	resp.exchange_mst = theServer()->cache().guildPointExchangeMst();
	resp.exchange_stock = co_await gme::loadGuildExchangeStock(theDb(), identity);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

namespace
{

// The message box's action vocabulary (MessageInboxInfo.message_action and
// InboxMessageManageNode.action), read off every caller.
constexpr int32_t kInboxFetch = 2;          // InboxMessageManage: list everything
constexpr int32_t kInboxDelete = 0;         // InboxMessageManage: delete / decline
constexpr int32_t kInboxCancelRequest = 91; // InboxMessageManage: withdraw my application
constexpr int32_t kMessageGuild = 1;        // message_type: the only type drawn
constexpr int32_t kMessageInvite = 90;      // message_action: Accept / Decline
constexpr int32_t kMessageOwnRequest = 91;  // message_action: Cancel request / Delete

drogon::Task<std::optional<::MessageInboxInfo>> guildMessage(
	const gme::GuildRequestRow& request, int32_t action)
{
	const auto guild = co_await gme::loadGuildById(theDb(), request.guild_id);
	if (!guild)
		co_return std::nullopt;
	const auto block = co_await gme::guildInfoFor(theDb(), *guild, "");

	::MessageInboxInfo m{};
	// One id space: request ids are unique across kinds, so the message id IS
	// the request id, and so is the invite id GuildInviteManage sends back.
	m.message_id = request.request_id;
	m.guild_invite_id = request.request_id;
	m.message_date = displayTime(request.created_at);
	m.message_type = kMessageGuild;
	m.message_action = action;
	// Neither the list (createGuildNotice) nor the detail screen built from a
	// message (getGuildInfoObject @0x1e77578) shows the guild's name, so the
	// text carries it.
	if (action == kMessageInvite)
	{
		const auto from = co_await theDb()->execSqlCoro(
			"SELECT username FROM user_info WHERE id = $1;", request.from_user_id);
		const auto inviter = from.empty() ? std::string{"A guild member"} : from[0]["username"].as<std::string>();
		m.message_title = "Guild invitation: " + guild->name;
		m.message_content = inviter + " invited you to join " + guild->name + ".";
	}
	else
	{
		m.message_title = "Join request: " + guild->name;
		m.message_content = "You asked to join " + guild->name + ". Waiting for an answer.";
	}
	m.guild_id = block.id;
	m.guild_name = block.name;
	m.guild_description = block.description;
	m.guild_art_id = block.guild_art_id;
	m.guild_level = block.level;
	m.guild_rank = block.rank;
	m.guild_prestige_point = block.prestige_point;
	m.guild_members_count = block.members_count;
	m.guild_master_name = block.master_name;
	m.guild_create_date = block.create_date;
	co_return m;
}

} // namespace

// InboxMessageManage (rYSfaC4P) -- the message box.
//
// InboxListScene::initConnect @0x1e75350 FETCHES with {message 0, action 2};
// later the scene sends every Delete / Decline (0) and Cancel request (91) the
// player made in one deferred request from onSceneInvisible, having already
// removed those messages itself.
//
// The messages are the player's open guild requests: an invite to them
// (action 90, Accept / Decline) and their own applications (action 91, Cancel).
// D9dkXSPY is a full replace and GuildDetailScene looks its message up by id
// with no null check, so the list goes out ONLY on a fetch, and always whole.
HANDLEF(InboxMessageManage)
{
	(void)session;
	const auto req = parseGuildRequest<::InboxMessageManageReq>("InboxMessageManage", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// Only an explicit fetch (action 2) gets the list: anything else may arrive
	// while GuildDetailScene holds a message by id.
	bool fetch = false;
	for (const auto& node : req.nodes)
	{
		if (node.action == kInboxFetch)
		{
			fetch = true;
			continue;
		}
		if ((node.action == kInboxDelete || node.action == kInboxCancelRequest) && node.message_id > 0)
		{
			co_await gme::dropOwnRequest(theDb(), identity.userId, node.message_id);
			continue;
		}
		LOG_INFO << "InboxMessageManage: " << identity.userId << " sent action " << node.action
			<< " for message " << node.message_id << "; nothing to do";
	}
	if (!fetch)
		co_return HandleResult::success("{}");

	::MessageInboxResp resp{};
	for (const auto& invite : co_await gme::pendingInvites(theDb(), identity.userId))
		if (auto m = co_await guildMessage(invite, kMessageInvite))
			resp.messages.push_back(std::move(*m));
	for (const auto& application : co_await gme::pendingApplicationsBy(theDb(), identity.userId))
		if (auto m = co_await guildMessage(application, kMessageOwnRequest))
			resp.messages.push_back(std::move(*m));

	LOG_INFO << "InboxMessageManage: " << resp.messages.size() << " message(s) for " << identity.userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
