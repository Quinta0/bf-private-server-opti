#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>

// FriendDelete (SfMN9w4p) — unfriend.
//
// Body, from FriendDeleteRequest::createBody @0x13A3EC0: one node `BPz1e5tU`
// carrying `h7eY3sAK`, the user id to drop.  FriendApply and FriendAgree send
// the identical shape, which is worth knowing before those are written.
//
// WHY THIS MATTERS BEYOND TIDINESS: a friend whose evolution chain tops out
// below the player stops appearing in the helper picker (see Friends.hpp).  The
// roster still holds them, on purpose, so the player can see who has fallen
// behind and decide.  Without a working unfriend that list would only ever grow
// and the stale entries could never be cleared -- the mechanic needs this
// handler to be the way out.
//
// THE REPLY IS THE FULL LIST.  It used to be `{}` on the theory that the
// client refreshes with FriendGet -- it does not: FriendDetailScene neither
// removes the row locally nor sends FriendGet, so the deleted friend stayed in
// the Friend List until the next login (FRIENDS_SPEC §4.6).
//
// A real player is removed on BOTH sides; an NPC from the roster only.
// Never refuse: FriendDetailScene ignores -4000.
HANDLEF(FriendDelete)
{
	(void)session;
	LOG_INFO << "FriendDelete: " << json;

	FriendDeleteReq req = {};
	{
		glz::context ctx{};
		if (const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "FriendDelete: parse error: " << glz::format_error(ec, json);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	int32_t removed = 0;
	try
	{
		for (const auto& node : req.nodes)
		{
			if (node.user_id.empty())
				continue;
			const bool gone = co_await gme::isRealPlayer(theDb(), node.user_id)
				? co_await gme::removeRelation(theDb(), identity.userId, node.user_id)
				: co_await gme::removeFriend(theDb(), identity, node.user_id);
			if (gone)
				++removed;
			else
			{
				// Not an error: the client can resend, and a friend already
				// gone is the state the player asked for.
				LOG_INFO << "FriendDelete: " << node.user_id
					<< " was not on " << identity.userId << "'s roster";
			}
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendDelete: DB error: " << ex.base().what();
		// SUCCESS regardless: an error reply is GmeErrorCommand::Close, and
		// dropping the session is worse for the player than a failed unfriend.
		co_return HandleResult::success("{}");
	}

	LOG_INFO << "FriendDelete: removed " << removed << " friend(s) for " << identity.userId;
	try
	{
		co_return HandleResult::success(co_await gme::socialReply(theDb(), identity));
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendDelete: DB error building the list: " << ex.base().what();
		co_return HandleResult::success("{}");
	}
}
