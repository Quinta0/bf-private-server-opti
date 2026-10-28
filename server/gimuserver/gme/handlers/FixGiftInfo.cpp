#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Gifts.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

// FixGiftInfo (gLRIn74v) — the gift screens' ONE write: Receive and Send.
//
// It had been a `{}` acknowledgement since the old tree, which is why claiming a
// gift did nothing.  FixGiftInfoRequest::createBody @0x13A37E4 shows it carries
// three things at once:
//
//   s2WnRw9N  the gift the player is ASKING friends for (UserTeamInfo::
//             getWantGift) — the preference that biases tomorrow's drop
//   23Xi0jom  comma-separated gift_identify_ids being CLAIMED, joined from
//             UserState::getUserGiftInfoListRecieve
//   qD74LPXb  gifts being SENT, as `user_id_to:gift_id` pairs
//
// Sending delivers to real friends (gme::sendGifts); it used to be logged and
// dropped, so a gift to another player never reached their inbox.
//
// THE REPLY CARRIES THE GIFT LIST (30uygM9m).  The client wipes both of its
// local lists when this reply is parsed (UserState::clear("gLRIn74v")
// @0x1246308), so without the server's list a friend gifted a moment ago would
// be offered again.  gme::loadGiftInbox includes today's sent rows for that.
//
// ONE REQUEST PER VISIT.  ConnectRequestList::addObject @0x13914f8 drops a
// second queued gLRIn74v, so every send and claim of one visit to the gift
// screens arrives together, at the next scene change -- not necessarily while
// the screen is open (hence it can follow a MissionEnd).  Everything here is
// idempotent: a resent body claims and sends nothing twice.
//
// Never an error reply: that is GmeErrorCommand::Close, which drops the
// session.  Bad pairs are skipped and the reply is still a success.
HANDLEF(FixGiftInfo)
{
	(void)session;
	LOG_INFO << "FixGiftInfo: " << json;

	FixGiftInfoReq req = {};
	{
		glz::context ctx{};
		if (const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "FixGiftInfo: parse error: " << glz::format_error(ec, json);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	std::string wantGift;
	std::string receiveIds;
	std::string sendPairs;
	for (const auto& node : req.nodes)
	{
		wantGift = node.want_gift;
		receiveIds = node.receive_ids;
		sendPairs = node.send_pairs;
	}

	const auto sends = gme::parseGiftSends(sendPairs);
	int32_t paid = 0;
	int32_t delivered = 0;
	try
	{
		auto transaction = co_await gme::newTransaction(theDb());

		// The preference first, so that even a claim that grants nothing still
		// records what the player asked for.
		if (!wantGift.empty())
		{
			co_await transaction->execSqlCoro(
				"UPDATE user_info SET want_gift = $1 WHERE id = $2;",
				wantGift, identity.userId);
		}

		paid = co_await gme::claimGifts(transaction, identity, receiveIds);
		delivered = co_await gme::sendGifts(transaction, identity, sends);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FixGiftInfo: DB error: " << ex.base().what();
		// Still a SUCCESS: an error reply closes the session, and losing the
		// session is worse for the player than a claim that did not land.
		co_return HandleResult::success("{}");
	}

	LOG_INFO << "FixGiftInfo: " << identity.userId << " claimed " << paid
		<< " gift(s), delivered " << delivered << " of " << sends.size() << " sent"
		<< (wantGift.empty() ? "" : ", wants " + wantGift);

	// Read back AFTER the writes have committed (the transaction above is out
	// of scope, and the one SQLite connection runs its COMMIT first), so a
	// failure while building the list can never undo a claim or a send.
	GetGiftInfoResp resp{};
	try
	{
		resp.gift_info = co_await gme::loadGiftInbox(theDb(), identity);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FixGiftInfo: gift list for the reply failed: " << ex.base().what();
		co_return HandleResult::success("{}");
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "FixGiftInfo: gift list for the reply failed: " << ex.what();
		co_return HandleResult::success("{}");
	}

	std::string buffer{};
	if (const auto& ec = glz::write_json(resp, buffer); ec)
	{
		LOG_ERROR << "FixGiftInfo: serialization error: " << glz::format_error(ec, buffer);
		co_return HandleResult::success("{}");
	}
	co_return HandleResult::success(buffer);
}
