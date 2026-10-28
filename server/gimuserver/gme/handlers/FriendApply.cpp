#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>

// FriendApply (WUNi08YL) — "add this Summoner", and the moment a developer
// easter egg is actually caught.
//
// The flow it completes:
//   1. FriendGet drops today's developer into the helper PICKER only, as a
//      stranger (friend_type 0).
//   2. The player borrows them for a mission.
//   3. MissionResultFriendRequestScene::initialize @0x18C16BC asks
//      FriendInfoList::exist() whether that helper is already a friend.  They
//      are not — which is exactly why the encounter is kept out of the Social
//      list — so the client offers to add them.
//   4. Accepting sends this request, and the roster keeps them for good.
//
// Body is identical to FriendDelete's (`BPz1e5tU` → `h7eY3sAK`), read straight
// off FriendApplyRequest::createBody @0x13A3DA0.
//
// TWO KINDS OF FAR SIDE.
//
// A REAL PLAYER (an id in user_info) gets a real two-step request: a type 2
// row for me, a type 3 row for them (gme::sendRequest, FRIENDS_SPEC §4.2).
// If they had already asked me, asking back makes us friends at once.
//
// AN NPC (a developer encounter or a suggested stranger, whose ids name
// themselves in a form gme::recruitFriend can decode) has nobody to approve,
// so the request is completed on their behalf and they are on the roster when
// the reply lands.  Anything else is logged and ignored rather than invented
// into a friend.
//
// NEVER REFUSE.  MissionResultFriendRequestScene sends this too and ignores
// -4000 (FRIENDS_SPEC §1.4), so a refusal would strand the result screen.
HANDLEF(FriendApply)
{
	(void)session;
	LOG_INFO << "FriendApply: " << json;

	FriendApplyReq req = {};
	{
		glz::context ctx{};
		if (const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "FriendApply: parse error: " << glz::format_error(ec, json);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	FriendApplyResp resp{};
	int32_t added = 0;
	try
	{
		for (const auto& node : req.nodes)
		{
			if (node.user_id.empty() || node.user_id == identity.userId)
				continue;
			if (co_await gme::isRealPlayer(theDb(), node.user_id))
			{
				const auto outcome = co_await gme::sendRequest(theDb(), identity.userId, node.user_id);
				LOG_INFO << "FriendApply: " << identity.userId << " -> " << node.user_id
					<< " outcome " << static_cast<int>(outcome);
				if (outcome == gme::ApplyOutcome::Sent || outcome == gme::ApplyOutcome::AutoAccepted)
					++added;
			}
			else if (co_await gme::recruitFriend(theDb(), identity, node.user_id))
				++added;
			else
			{
				LOG_INFO << "FriendApply: " << node.user_id
					<< " was not addable (unknown id, chain already taken, or roster full)";
			}
		}

		// REFRESH THE LIST WE JUST CHANGED.  FriendInfoList is a client
		// singleton; nothing re-reads it after a friend request, so replying
		// with an empty body left the new friend on the server and absent from
		// the Friend List until the next FriendGet -- which is exactly what a
		// client test showed.  Sent unconditionally: even when nothing was
		// added the player has just been through the request flow, and a
		// correct list is never the wrong answer.
		resp.friend_info = co_await gme::buildSocialList(theDb(), identity);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendApply: DB error: " << ex.base().what();
		// SUCCESS regardless: an error reply is GmeErrorCommand::Close, and
		// losing the session is worse than a friend request that did not land.
		// An empty body is safe here -- a zero-row list never reaches readParam,
		// so it cannot clear what the client already holds.
		co_return HandleResult::success("{}");
	}

	// The rates the refreshed cards read their Honor off; the singleton's ctor
	// defaults both to "0", so any reply that redraws them has to resend it.
	resp.friend_point_info = gme::friendPointInfo();

	LOG_INFO << "FriendApply: " << added << " request(s)/friend(s) added for " << identity.userId
		<< "; returning " << resp.friend_info.size() << "-row roster";

	std::string buffer{};
	if (const auto& ec = glz::write_json(resp, buffer); ec)
	{
		const auto& glze = glz::format_error(ec, buffer);
		LOG_ERROR << "FriendApply: serialization error: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}

	co_return HandleResult::success(buffer);
}
