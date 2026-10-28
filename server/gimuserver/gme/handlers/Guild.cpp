#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Guilds.hpp>
#include <gimuserver/gme/common/GuildSocial.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Exchange.hpp>

// The guild create-and-invite slice.
//
// Four handlers, all of whose wire shapes are Binary-confirmed in
// net/guild.kdl -- the requests from their producing createBody, the responses
// from the class GameResponseParser::getResponseObject dispatches the key to.
//
// WHY THESE FOUR.  They are the whole loop a player can complete here: look at
// the guild screen (which quotes the founding fee when you have none), found
// one, see who you could bring in, bring them in.  The other forty guild
// requests are raids, boards, rankings and research, none of which mean
// anything until a guild exists.

// GuildInfo (138ba8d4) and GuildJoinedList (3890ab5j) -- both identity-only
// requests that answer "what guild am I in?".  They share this handler because
// they share the question; the reply is the same singleton either way.
//
// A player with no guild gets the FOUNDING FEE and no guild block at all --
// the field is optional precisely so it can be absent, because GuildInfoResponse
// is a singleton with no clear and a zeroed row would read as "guild 0".
HANDLEF(GuildInfo)
{
	(void)session;
	::GuildInfoReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildInfo: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto guild = co_await gme::loadGuild(theDb(), identity);
	if (!guild)
	{
		// No guild: send the FOUNDING FEE instead.  Nothing else can -- there is
		// no GuildCreateCostRequest in the binary, so this reply is the only
		// place the cost reaches the player.
		::GuildInfoResp cost{};
		::GuildCreationInfo info{};
		info.currency_type = gme::kGuildCreateCurrency;
		info.required_amount = static_cast<int32_t>(gme::kGuildCreateCost);
		cost.creation_info.push_back(std::move(info));

		// GUILD ID 0 IS THE CLIENT'S OWN "NO GUILD".  GuildInfoConnectScene's
		// updateEvent @0x1DD2070 routes on GuildUserGuildInfo's guild id: 0 runs
		// clearToDefault and opens the recommended list, anything else the guild
		// town.  The singleton is never cleared, so a player dismissed, or whose
		// guild was disbanded, while their game was open still holds the old id
		// and would be sent into a guild they are no longer in.  An explicit
		// zero row resets it (the Hall writes the same 0 itself after a leave).
		cost.guild.push_back(::GuildInfo{});

		// The recommended guilds ride on this reply: GuildRecommendedListScene
		// (102002) sends nothing of its own and draws csI7dh30.
		auto recommended = co_await gme::guildInfoList(theDb(),
			co_await gme::searchGuilds(theDb(), identity.userId, ""));
		const auto offered = recommended.size();
		if (!recommended.empty())
			cost.recommended = std::move(recommended);

		LOG_INFO << "GuildInfo: " << identity.userId
			<< " is not in a guild; sent the founding fee and " << offered << " recommended guild(s)";
		co_return HandleResult::success(glz::write_json(cost).value_or("{}"));
	}

	::GuildInfoResp resp{};
	resp.guild.push_back(co_await gme::guildInfoFor(theDb(), *guild, identity.userId));
	// The Hall's roster rides along.  Without it the member list is empty and
	// tapping a member is a null dereference -- an access violation with no
	// dialog, which no empty-body stub can prevent.
	resp.members = co_await gme::guildRoster(theDb(), identity);
	resp.exchange_mst = theServer()->cache().guildPointExchangeMst();
	resp.exchange_stock = co_await gme::loadGuildExchangeStock(theDb(), identity);

	LOG_INFO << "GuildInfo: " << identity.userId << " is in \"" << guild->name
		<< "\" (" << guild->members_count << " member(s))";
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildCreate (g298Da10) -- found one.
//
// The fee is charged BEFORE the guild is written and in the same statement that
// checks it, so a player who cannot afford it cannot create one by racing two
// requests: the UPDATE only matches while the balance still covers it.
HANDLEF(GuildCreate)
{
	(void)session;
	::GuildCreateReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildCreate: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// req.guild is the node itself, not a list: `[T]::size(1)` generates an
	// embedded object that serialises as a one-element array.  It and `[T]` are
	// not interchangeable, and the difference is invisible on the wire.
	const auto& node = req.guild;

	// Other players see the name and blurb now (search, recommended, ranking,
	// invites), so they get the same limits as GuildUpdate.
	const auto name = gme::cleanGuildText(node.name, gme::kGuildNameMaxBytes);
	const auto description = gme::cleanGuildText(node.description, gme::kGuildDescriptionMaxBytes);
	if (name.empty())
	{
		// Before anything is charged; the message and a way Home, not a
		// closed session.
		co_return HandleResult::refuseToHome("A guild needs a name.",
			"GuildCreate: " + identity.userId + " sent an empty guild name");
	}

	// guild_art_id arrives QUOTED even though GuildInfo reads it back as an int.
	int32_t artId = gme::kDefaultGuildArtId;
	try { artId = gme::cleanGuildArtId(std::stoi(node.guild_art_id)); }
	catch (const std::exception&)
	{
		LOG_WARN << "GuildCreate: unparseable guild_art_id "
			<< node.guild_art_id << "; defaulting to " << artId;
	}

	const auto charged = co_await theDb()->execSqlCoro(
		"UPDATE user_info SET zel = zel - $1 WHERE id = $2 AND zel >= $1;",
		static_cast<int64_t>(gme::kGuildCreateCost), identity.userId);
	if (charged.affectedRows() == 0)
	{
		LOG_INFO << "GuildCreate: " << identity.userId << " cannot afford the "
			<< gme::kGuildCreateCost << " zel fee";
		co_return HandleResult::error("Insufficient funds", "Not enough Zel to found a guild");
	}

	const auto guild = co_await gme::createGuild(
		theDb(), identity, name, description, artId);
	if (!guild)
	{
		// Refund: the charge landed but the guild did not, so the player must
		// not be left paying for nothing.
		co_await theDb()->execSqlCoro(
			"UPDATE user_info SET zel = zel + $1 WHERE id = $2;",
			static_cast<int64_t>(gme::kGuildCreateCost), identity.userId);
		LOG_WARN << "GuildCreate: " << identity.userId << " could not create a guild; fee refunded";

		// Reachable now that others can put a player into a guild: an
		// application accepted while they sat on the recommended list.  Hand
		// them the guild they are in rather than closing the session.
		const auto current = co_await gme::loadGuild(theDb(), identity);
		if (!current)
			co_return HandleResult::refuseToHome("The guild could not be founded. Please try again.",
				"GuildCreate: createGuild failed for " + identity.userId);
		::GuildInfoResp already{};
		already.guild.push_back(co_await gme::guildInfoFor(theDb(), *current, identity.userId));
		already.members = co_await gme::guildRoster(theDb(), identity);
		already.exchange_mst = theServer()->cache().guildPointExchangeMst();
		already.exchange_stock = co_await gme::loadGuildExchangeStock(theDb(), identity);
		co_return HandleResult::success(glz::write_json(already).value_or("{}"));
	}

	::GuildInfoResp resp{};
	resp.guild.push_back(co_await gme::guildInfoFor(theDb(), *guild, identity.userId));
	resp.members = co_await gme::guildRoster(theDb(), identity);
	resp.exchange_mst = theServer()->cache().guildPointExchangeMst();
	resp.exchange_stock = co_await gme::loadGuildExchangeStock(theDb(), identity);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildRecomendedMember (ja5Enusw) -- who the player could invite.
//
// ⚠ A FULL REPLACE: readParam @0x1C64A38 calls removeAllObjects, so this must
// always carry the COMPLETE candidate list.
//
// The candidates are the player's FRIENDS, which is recovered rather than
// chosen: GuildRecomendedMemberInfo's own setters are setFriendUserUnitID,
// setFriendUserUnitLevel and setFriendUserImageType.
HANDLEF(GuildRecomendedMember)
{
	(void)session;
	::GuildInfoReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildRecomendedMember: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// Both lists: the cards (fRaBu6et) and the profiles a tapped card opens
	// (8lAroepR).  Sending only the cards crashed the Hall on the first tap.
	const auto resp = co_await gme::invitableFriends(theDb(), identity);

	LOG_INFO << "GuildRecomendedMember: " << resp.members.size()
		<< " invitable friend(s) for " << identity.userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildMemberUpdate (ad81b8at) -- rank changes, dismissals and leaving.
//
// NOT the invite (that is GuildJoin, below).  The member screen
// (GuildMemberInfoScene::updateEvent @0x1DD36B8) sends the member's id with
// their current type - 1 to promote, + 1 to demote, or 0 to dismiss; the Hall
// and GuildTeamScene2 send the player's OWN id with 0 to leave.  Ranks: see
// kGuildRank* in Guilds.hpp.
//
// ⚠ THE REPLY MUST NOT CARRY THE ROSTER.  After the reply the client updates
// its own copy: noticeOK @0x1DD3DD4 looks the member up again by id and sets
// type - 1 / + 1 on it, and a dismissal removes the object and lowers the
// member count itself.  A roster in the reply (csIuech30) replaces the member
// list first, so the lookup came back null and the promote crashed with
// `write 0x284` (2026-09-27 dump; 0x284 is GuildMemberInfo's member type).
HANDLEF(GuildMemberUpdate)
{
	(void)session;
	::GuildMemberUpdateReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildMemberUpdate: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	for (const auto& node : req.members)
	{
		if (node.user_id.empty())
			continue;
		int32_t memberType = -1;
		try { memberType = std::stoi(node.value); }
		catch (const std::exception&)
		{
			LOG_WARN << "GuildMemberUpdate: unparseable member type \"" << node.value
				<< "\" for " << node.user_id;
			continue;
		}
		co_await gme::updateGuildMember(theDb(), identity, node.user_id, memberType);
	}

	// Refusals answer the same way: every handler error closes the session.
	co_return HandleResult::success("{}");
}

// GuildJoin (bfa2D1bp) -- Apply (type 1) and Invite to Guild (type 2).
//
// TYPE 1, APPLY: GuildDetailScene::touchBegan @0x1da99d8 sends the target guild
// and the player's OWN id.  The client expects a PENDING request: it shows
// GUILD_REQUEST_FINISH and goes back to the list, reading nothing from the
// reply.  The application waits on the guild's Requests screen (GuildJoinedList)
// for a Guild Master or Vice.
//
// TYPE 2, INVITE: every friend profile's invite button sends the sender's guild
// and the profile's id -- the Hall's candidate profile, the Social list, friend
// search and the post-battle helper screens.  A REAL player gets an invite in
// their message box (InboxMessageManage) and decides there.  A computer-run
// friend accepts at once, the rule Guilds.hpp sets out -- only the owner's,
// because their cards come from the owner's roster.
//
// Refusals answer like successes: every handler error closes the session, and
// these screens read nothing back but a success.  The reason is in the log.
//
// The reply is the caller's refreshed guild when they have one: the profile's
// back button rebuilds the Hall, whose initConnect re-requests ONLY
// GuildRecomendedMember, so without it a computer-run friend just invited would
// not show until the next GuildInfo.  Never search or recommended rows here:
// GuildDetailScene holds its guild by a raw pointer into those lists.
HANDLEF(GuildJoin)
{
	(void)session;
	::GuildJoinReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildJoin: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// setRequestValues' third argument; see GuildJoinNode.request_type.
	constexpr int32_t kGuildJoinApply = 1;
	constexpr int32_t kGuildJoinInvite = 2;
	for (const auto& node : req.nodes)
	{
		if (node.request_type == kGuildJoinApply)
		{
			const auto outcome = co_await gme::applyToGuild(theDb(), identity.userId, node.guild_id);
			LOG_INFO << "GuildJoin: " << identity.userId << " applied to guild " << node.guild_id
				<< " -- outcome " << static_cast<int>(outcome);
			continue;
		}
		if (node.request_type != kGuildJoinInvite)
		{
			LOG_WARN << "GuildJoin: " << identity.userId << " sent request type "
				<< node.request_type << " for guild " << node.guild_id << "; ignored";
			continue;
		}

		if (co_await gme::isRealPlayer(theDb(), node.user_id))
		{
			const auto outcome = co_await gme::inviteToGuild(theDb(), identity.userId, node.guild_id, node.user_id);
			LOG_INFO << "GuildJoin: " << identity.userId << " invited " << node.user_id
				<< " to guild " << node.guild_id << " -- outcome " << static_cast<int>(outcome);
			continue;
		}

		const auto guild = co_await gme::loadGuild(theDb(), identity);
		if (!guild || guild->guild_id != node.guild_id || guild->owner_user_id != identity.userId)
		{
			LOG_WARN << "GuildJoin: " << identity.userId << " may not add computer-run friend "
				<< node.user_id << " to guild " << node.guild_id << " (only its Guild Master can)";
			continue;
		}
		const auto joined = co_await gme::inviteFriends(theDb(), identity, { node.user_id });
		LOG_INFO << "GuildJoin: " << identity.userId << " invited " << node.user_id
			<< (joined > 0 ? " -- joined" : " -- did not join");
	}

	const auto refreshed = co_await gme::loadGuild(theDb(), identity);
	if (!refreshed)
		co_return HandleResult::success("{}");

	::GuildInfoResp resp{};
	resp.guild.push_back(co_await gme::guildInfoFor(theDb(), *refreshed, identity.userId));
	resp.members = co_await gme::guildRoster(theDb(), identity);
	resp.exchange_mst = theServer()->cache().guildPointExchangeMst();
	resp.exchange_stock = co_await gme::loadGuildExchangeStock(theDb(), identity);
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// GuildRanking (2b9D01b4) and GuildRankingDetail (7ekSBz2y).
//
// BOTH CRASHED AS EMPTY-BODY STUBS, and for the same reason the Guild Hall did:
// the stub keeps the session alive where an unregistered GroupId would close it,
// but a scene that needs rows still dereferences the rows it did not get.  Both
// responses are REPLACE lists (removeAllObjects in their readParam), so a reply
// is always the whole board.
//
// ⚠ THE LAST ROW IS "YOUR GUILD".  Both ranking scenes draw rows 0..count-2 in
// the list and row count-1 in the fixed own-guild frame
// (GuildRaidRankingResultScene::setRankingLayout @0x1E1C574,
// GuildRankingScene::setScrollArea @0x1E41E54 / 0x1E423CC); nothing filters by
// type, season or guild.  The old reply sent only the board, so with one guild
// the list was empty and the guild sat in the own frame, and with several the
// lowest guild would have been drawn as the player's.  So: the board, then the
// player's own guild again, with its real place.
//
// WHAT IS RANKED (gme::guildLeaderboard): prestige, then experience, members,
// age.  Prestige comes only from guild raids, which do not run yet.
HANDLEF(GuildRanking)
{
	(void)session;
	::GuildRankingReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildRanking: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// Echo the tab and season the screen asked for rather than assuming 0/1 --
	// createBody @0x1C63E2C sends both, and the season it sends comes from
	// GuildRaidRoundInfo::getSeasonID(), i.e. from what we told it last time.
	const int32_t askedType = req.ranking.empty() ? 0 : req.ranking.front().ranking_type;
	const int32_t askedSeason = req.ranking.empty() || req.ranking.front().season_id <= 0
		? gme::kGuildRaidSeason : req.ranking.front().season_id;

	// AUTHORED: how many guilds the list shows above the player's own row.
	constexpr size_t kRankingRows = 50;
	const auto board = co_await gme::guildLeaderboard(theDb());
	const auto toRow = [&](const gme::GuildRow& g, int32_t place) {
		::GuildRankingInfo row{};
		row.ranking          = place;
		row.ranking_type     = askedType;
		row.guild_id         = g.guild_id;
		row.name             = g.name;
		row.description      = g.description;
		row.guild_art_id     = g.guild_art_id;
		row.guild_level      = gme::guildLevelFor(g.experience);
		row.prestige_points  = g.prestige_point;
		row.season_id        = askedSeason;
		return row;
	};

	::GuildRankingResp resp{};
	for (size_t i = 0; i < board.size() && i < kRankingRows; ++i)
		resp.ranking.push_back(toRow(board[i], static_cast<int32_t>(i + 1)));

	const auto own = co_await gme::loadGuild(theDb(), identity);
	if (own)
	{
		int32_t place = 0;
		for (size_t i = 0; i < board.size(); ++i)
			if (board[i].guild_id == own->guild_id)
				place = static_cast<int32_t>(i + 1);
		resp.ranking.push_back(toRow(*own, place));
	}
	else if (!resp.ranking.empty())
	{
		// No guild (the Frontier Gate entry can be reached without one): the
		// own frame still takes the last row, so it says so instead of
		// claiming the lowest guild.  Rank 0 draws "--"; type-1 rows cannot be
		// tapped, and the guild-raid screen needs a guild to reach.
		::GuildRankingInfo none{};
		none.ranking = 0;
		none.ranking_type = askedType;
		none.guild_id = 0;
		none.name = "-";
		none.guild_art_id = gme::kDefaultGuildArtId;
		none.season_id = askedSeason;
		resp.ranking.push_back(std::move(none));
	}

	// THE SEASON LIST IS LOAD-BEARING.  GuildRaidRankingResultScene indexes
	// GuildRaidSeasonDataInfoList with no null check, so an empty list is the
	// crash at PE +0x2FEEB1 (`mov eax,[eax+0x14]` on a nulled eax).  One row is
	// enough and one row is honest: this server has run exactly one season.
	// Index 0 is the NEWEST (the Previous button increments the index).
	for (int32_t season = gme::kGuildRaidSeason; season >= 1; --season)
	{
		::GuildRaidSeasonDataInfo row{};
		row.season_id = season;
		resp.seasons.push_back(std::move(row));
	}
	resp.round.push_back(gme::guildRaidRound());

	LOG_INFO << "GuildRanking: " << board.size() << " guild(s) ranked for " << identity.userId
		<< (own ? " (own guild " + std::to_string(own->guild_id) + " last)" : " (no guild)")
		<< ", type " << askedType << ", season " << askedSeason;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// The per-member breakdown behind a TAPPED board row.
//
// GuildRankingDetailScene::initConnect @0x1E3DF64 sends the tapped row's guild
// id and season, not the viewer's guild: the old handler ignored them and
// always answered with the viewer's own members.  The members come from the
// SAME roster the Guild Hall draws.  Contribution (BCP) is not tracked yet, so
// every member scores 0.
//
// ⚠ s35idar9 AND B5JQyV8j BOTH LAND ON setName, and keys arrive sorted so
// s35idar9 wins: it carries the MEMBER's name too, or every row reads as the
// guild's name.  R6YwbeCB is always sent, even empty: an absent key would leave
// the previous guild's members on screen.  Never g7hx43Pq here: its
// constructor frees the row the detail scene still points at.
HANDLEF(GuildRankingDetail)
{
	(void)session;
	::GuildRankingDetailReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "GuildRankingDetail: parse error: " << glz::format_error(ec, json);
	}
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	::GuildRankingDetailResp resp{};
	const int32_t asked = req.detail.empty() ? 0 : req.detail.front().guild_id;
	const auto guild = asked > 0 ? co_await gme::loadGuildById(theDb(), asked)
		: co_await gme::loadGuild(theDb(), identity);
	if (!guild)
	{
		LOG_INFO << "GuildRankingDetail: guild " << asked << " not found for " << identity.userId
			<< "; sending an empty board";
		co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
	}

	const auto roster = co_await gme::guildRosterOf(theDb(), *guild);
	int32_t place = 0;
	for (const auto& member : roster)
	{
		::GuildRankingDetailInfo row{};
		row.ranking       = ++place;
		row.ranking_type  = 0;
		row.guild_id      = guild->guild_id;
		row.guild_name    = member.handle_name; // s35idar9 -> setName: see above
		row.guild_level   = gme::guildLevelFor(guild->experience);
		row.member_name   = member.handle_name;
		row.user_id       = member.user_id;
		row.bcp_points    = 0;
		row.unit_id       = member.unit_id;
		row.unit_lvl      = member.unit_lv;
		row.unit_img_type = member.unit_img_type;
		resp.detail.push_back(std::move(row));
	}

	LOG_INFO << "GuildRankingDetail: " << resp.detail.size()
		<< " member(s) of guild " << guild->guild_id << " for " << identity.userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// Everything else in the guild set.
//
// Answers an empty body so the session survives.  An unregistered GroupId and a
// handler error both produce GmeErrorCommand::Close, which reaches the player as
// a crash with nothing in deploy/log to explain it; an empty reply gets them a
// blank panel they can back out of instead.
//
// ⚠ NOT a claim that these are done.  Each is a feature still to be written, and
// the GroupId is logged so a capture shows which screen asked for what.
HANDLEF(GuildUnimplemented)
{
	(void)session;
	LOG_INFO << "Guild: unimplemented guild request, answering empty: " << json;
	co_return HandleResult::success("{}");
}
