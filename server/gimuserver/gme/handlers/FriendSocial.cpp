#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/AccountReset.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Gifts.hpp>   // giftToday(), the NPC strangers' day key
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <cctype>
#include <set>

// The Social menu between real players: requests, search and favorites.
// docs/features/FRIENDS_SPEC.md §4 is the behaviour; §1.4 is why most of these
// never refuse.  Every reply is the COMPLETE social list (gme::socialReply),
// because FriendInfoList is a full replace on the client.
//
// A DB failure still answers success with "{}": an error reply closes the
// session, and an empty body cannot clear what the client already holds.

namespace
{

/*! Lenient read: unknown keys are normal, a malformed body means "nothing named". */
template <typename T>
T readRequest(const char* name, const std::string& json)
{
	T req{};
	glz::context ctx{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
		LOG_WARN << name << ": parse error: " << glz::format_error(ec, json);
	return req;
}

/*! Every id the request names: h7eY3sAK plus FriendRefuse's comma list, deduplicated and capped. */
std::vector<std::string> namedIds(const FriendTargetReq& req)
{
	std::set<std::string> ids;
	for (const auto& node : req.nodes)
	{
		if (!node.user_id.empty())
			ids.insert(node.user_id);
		std::string item;
		for (const char ch : node.user_ids_csv + ",")
		{
			if (ch != ',')
			{
				item += ch;
				continue;
			}
			if (!item.empty())
				ids.insert(item);
			item.clear();
		}
	}
	std::vector<std::string> out(ids.begin(), ids.end());
	if (out.size() > 100)
		out.resize(100);
	return out;
}

/*! The typed public ID if it is plausibly one (1-12 ASCII digits), else "". */
std::string typedDigits(const FriendSearchReq& req)
{
	if (req.nodes.empty())
		return {};
	std::string text = req.nodes.front().friend_id;
	text.erase(std::remove_if(text.begin(), text.end(),
		[](unsigned char c) { return std::isspace(c); }), text.end());
	const bool digits = !text.empty() && text.size() <= 12
		&& std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
	return digits ? text : std::string{};
}

/*! The reply every handler here ends with; "{}" if the DB fails mid-build. */
drogon::Task<HandleResult> replyWithList(const char* name, const gme::UserIdentity& identity,
	std::vector<::FriendInfo> extra = {})
{
	try
	{
		co_return HandleResult::success(co_await gme::socialReply(theDb(), identity, std::move(extra)));
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << name << ": DB error building the list: " << ex.base().what();
		co_return HandleResult::success("{}");
	}
}

} // namespace

// FriendGetAgree (aVqJrR08) -- opening the Requests screen.  The client lists
// the type 2 and 3 rows of whatever this returns, so it is just the list,
// with requests older than two weeks dropped first.
HANDLEF(FriendGetAgree)
{
	(void)session;
	auto req = readRequest<FriendGetAgreeReq>("FriendGetAgree", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	try { co_await gme::expireStaleRequests(theDb(), identity.userId); }
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_WARN << "FriendGetAgree: expiry failed: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendGetAgree", identity);
}

// FriendAgree (NYKBH4V6) -- accept a request.  The ONLY friend reply allowed
// to refuse: FriendAgreeConnectScene's noticeOK handles -4000 by returning to
// the Requests list (FRIENDS_SPEC §1.4), and a full list must not be overfilled.
HANDLEF(FriendAgree)
{
	(void)session;
	auto req = readRequest<FriendTargetReq>("FriendAgree", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	try
	{
		for (const auto& node : req.nodes)
		{
			const auto outcome = co_await gme::acceptRequest(theDb(), identity.userId, node.user_id);
			if (outcome == gme::AgreeOutcome::SelfFull)
				co_return HandleResult::refuseToRetry("Your friend list is full.");
			if (outcome == gme::AgreeOutcome::TargetFull)
				co_return HandleResult::refuseToRetry("That Summoner's friend list is full.");
			LOG_INFO << "FriendAgree: " << identity.userId << " <- " << node.user_id
				<< (outcome == gme::AgreeOutcome::Accepted ? " accepted" : " (no pending request)");
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendAgree: DB error: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendAgree", identity);
}

// FriendRefuse (N5SQd7oA) -- refuse one incoming request, cancel one I sent
// (the delete button on a type 2 row), or reject all.  Only pending pairs are
// touched, so the client's possibly stale reject-all list is harmless.
HANDLEF(FriendRefuse)
{
	(void)session;
	auto req = readRequest<FriendTargetReq>("FriendRefuse", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	try
	{
		const auto dropped = co_await gme::refuseOrCancel(theDb(), identity.userId, namedIds(req));
		LOG_INFO << "FriendRefuse: " << identity.userId << " dropped " << dropped << " request(s)";
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendRefuse: DB error: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendRefuse", identity);
}

// FriendSearch (umNt8M7i) -- find a player by public ID.  A match goes back as
// a type 4 row; the client itself decides "that's you" (same user_id) and
// "not found" (no type 4 row), so neither needs an error.
HANDLEF(FriendSearch)
{
	(void)session;
	auto req = readRequest<FriendSearchReq>("FriendSearch", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	if (!gme::allowRate("search", identity.userId, 20, 600))
		co_return HandleResult::refuseToRetry("Too many searches. Try again in a few minutes.");

	const auto digits = typedDigits(req);
	// The in-game account reset rides on this box (gme/common/AccountReset.hpp).
	if (!digits.empty())
	{
		try
		{
			if (const auto popup = co_await gme::accountResetSearch(theDb(), identity, digits))
				co_return HandleResult::refuseToRetry(*popup);
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			LOG_ERROR << "FriendSearch: account reset: DB error: " << ex.base().what();
			co_return HandleResult::refuseToRetry(
				"Something went wrong, so nothing was reset. Search 000000 to try again.");
		}
	}

	std::vector<::FriendInfo> found;
	try
	{
		std::optional<std::string> who;
		if (!digits.empty())
			who = co_await gme::userIdForPublicId(theDb(), digits);
		// Sandbox and normal accounts cannot find each other (gme/common/Sandbox.hpp).
		if (who && !(co_await gme::sandbox::samePool(theDb(), identity.userId, *who)))
			who.reset();
		if (who)
		{
			if (const auto card = co_await gme::loadPlayerCard(theDb(), *who))
				found.push_back(gme::friendInfoFromCard(*card, gme::kRelSearchRow, nullptr));
		}
		LOG_INFO << "FriendSearch: " << identity.userId << " searched '" << digits << "': "
			<< found.size() << " match";
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendSearch: DB error: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendSearch", identity, std::move(found));
}

// FriendRandomSearch (mE5gUQOp) -- "suggest some players".  Never refuses:
// FriendRandomSearchScene has no noticeOK of its own.  Real players closest in
// level come first; NPC strangers (whose NEW ids FriendApply already resolves)
// top the list up so a near-empty server still offers someone.
HANDLEF(FriendRandomSearch)
{
	(void)session;
	auto req = readRequest<FriendRandomSearchReq>("FriendRandomSearch", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	std::vector<::FriendInfo> rows;
	try
	{
		if (gme::allowRate("random", identity.userId, 30, 600))
		{
			for (const auto& card : co_await gme::unrelatedPlayers(
					theDb(), identity.userId, gme::kRandomSearchCount, false))
				rows.push_back(gme::friendInfoFromCard(card, gme::kRelSearchRow, nullptr));
		}
		if (rows.size() < 5)
		{
			const auto peak = co_await gme::playerPeak(theDb(), identity);
			const auto roster = co_await gme::loadFriendRoster(theDb(), identity, false);
			auto npc = gme::socialList(
				gme::strangerSuggestions(roster, identity.userId, gme::giftToday(), peak), peak);
			for (auto& row : npc)
			{
				row.friend_type = gme::kRelSearchRow;
				rows.push_back(std::move(row));
			}
		}
		LOG_INFO << "FriendRandomSearch: " << rows.size() << " candidate(s) for " << identity.userId;
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendRandomSearch: DB error: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendRandomSearch", identity, std::move(rows));
}

// FriendFavorite (ps6Vgx4R) -- star a friend.  Real friends keep the flag on
// this side's relation row; NPC friends on their roster row.
HANDLEF(FriendFavorite)
{
	(void)session;
	auto req = readRequest<FriendTargetReq>("FriendFavorite", json);
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	try
	{
		for (const auto& node : req.nodes)
		{
			const bool on = node.favorite == 1;
			if (!(co_await gme::setRelationFavorite(theDb(), identity.userId, node.user_id, on)))
			{
				co_await theDb()->execSqlCoro(
					"UPDATE user_friends SET favorite = $1 WHERE user_id = $2 AND friend_id = $3;",
					on ? 1 : 0, identity.userId, node.user_id);
			}
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << "FriendFavorite: DB error: " << ex.base().what();
	}
	co_return co_await replyWithList("FriendFavorite", identity);
}
