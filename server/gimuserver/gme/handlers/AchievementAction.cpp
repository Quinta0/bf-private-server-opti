#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/gme/common/Achievements.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/MeritShop.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// The Merit Point loop, and the two buttons on an achievement's detail page.
//
//   AchievementRewardReceive  uq69mTtR / cbE74zBZ   claim a finished achievement
//   AchievementTrade          m9LiF6P2 / 0IWC9LVq   buy from the Merit shop
//   AchievementAccept         dx5qvm7L / g9N1y7bc   Start / Give Up
//
// Every request shape comes from its createBody (@0x139E3A8, @0x139E600 and
// @0x139DCA0) -- see net/achievement.kdl.  The last request of this family,
// Deliver (vsaXI4M0), is still unregistered: it trades units and currency for
// points through AchievementDeliverRateMst, which needs its colon-packed band
// decoded first.
//
// The shop's reward vocabulary is the shared present vocabulary, with one
// entry that grants nothing physical: type 15 is ALTERNATE UNIT ART.  The
// illustrations already ship with the client (unit_ills_full_<id>_2.png); what
// is bought is permission to show them, and that permission is one bit on the
// unit-dictionary row -- see gme::loadUnitDictionary.
//
// Merit Points are NOT part of the currency block.  `Bnc4LpM8` is its own
// singleton and only UserAchievementInfoResponse::readParam writes the client's
// copy, so both replies carry it or the number on screen does not move --
// the same gap that made the Frontier Gate look broken on 2026-09-12.

namespace
{

/*!
* Reads the colon-packed reward of a shop offer: "<type>:<id>:<count>:…".
* A unit reward's sixth field is the extra skill an Elgif carries
* ("6:50792:1:1:0:1001301" = Imbued Tablet Elgif with Dark Oracle Lv. 2);
* without it addUserUnit rolls a random one (ticket #167).
*/
struct TradeReward
{
	int32_t type = 0;
	std::string target;
	int32_t count = 1;
	int32_t extraSkill = 0;
};

TradeReward parseReward(const std::string& packed)
{
	TradeReward reward{};
	size_t start = 0, field = 0;
	while (start <= packed.size() && field < 6)
	{
		const auto end = packed.find(':', start);
		const auto part = packed.substr(start, end == std::string::npos ? end : end - start);
		try
		{
			if (field == 0) reward.type = std::stoi(part);
			else if (field == 1) reward.target = part;
			else if (field == 2) reward.count = std::max(std::stoi(part), 1);
			else if (field == 5) reward.extraSkill = std::max(std::stoi(part), 0);
		}
		catch (const std::exception&) { /* leave the default */ }
		if (end == std::string::npos)
			break;
		start = end + 1;
		++field;
	}
	return reward;
}

/*! A comma-separated subject id list, trimmed, empties dropped. */
std::vector<std::string> subjectIdList(const std::string& packed)
{
	std::vector<std::string> ids;
	size_t at = 0;
	while (at <= packed.size())
	{
		const auto comma = packed.find(',', at);
		auto piece = packed.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		// Trim, because a trailing comma is cheap for the client to emit.
		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front())))
			piece.erase(piece.begin());
		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back())))
			piece.pop_back();
		if (!piece.empty())
			ids.push_back(std::move(piece));
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}
	return ids;
}

/*! An id the way the MST keys it, or 0 when it is not a number. */
int32_t subjectIdNumber(const std::string& id)
{
	try { return std::stoi(id); }
	catch (const std::exception&) { return 0; }
}

/*! One line of the Accept log per outcome. */
const char* acceptOutcome(const gme::ChallengeAcceptResult result)
{
	switch (result)
	{
	case gme::ChallengeAcceptResult::Started:     return "started";
	case gme::ChallengeAcceptResult::GaveUp:      return "given up";
	case gme::ChallengeAcceptResult::NotOffered:  return "refused, not in the open window";
	case gme::ChallengeAcceptResult::AlreadyHeld: return "refused, already held";
	case gme::ChallengeAcceptResult::CapReached:  return "refused, the accept cap is reached";
	case gme::ChallengeAcceptResult::NotGivenUp:  return "refused, not in progress";
	}
	return "?";
}

/*!
* Latches one lifetime record's reward, refusing an unfinished one.
*
* Guarded on reward_received = 0, so a replayed body pays once.  Whether it is
* finished is checked AFTER the latch so the two cannot race, and a refusal
* undoes only this row's latch, keeping the rest of a Receive All batch.
*/
drogon::Task<bool> claimRecord(
	const db::Database transaction,
	const gme::UserIdentity& identity,
	const ::AchievementSubjectMst& subject)
{
	const auto subjectId = std::to_string(subject.id);
	const auto claimed = co_await transaction->execSqlCoro(
		"INSERT INTO user_achievement_subjects (user_id, subject_id, reward_received)"
		" VALUES ($1, $2, 1)"
		" ON CONFLICT(user_id, subject_id) DO UPDATE SET reward_received = 1"
		" WHERE user_achievement_subjects.reward_received = 0"
		" RETURNING subject_id;",
		identity.userId, subjectId);
	if (claimed.empty())
		co_return false;          // already claimed

	if (!co_await gme::achievementComplete(transaction, identity, subject))
	{
		co_await transaction->execSqlCoro(
			"UPDATE user_achievement_subjects SET reward_received = 0"
			" WHERE user_id = $1 AND subject_id = $2;",
			identity.userId, subjectId);
		co_return false;
	}
	co_return true;
}

} // namespace

// ---------------------------------------------------------------------------
HANDLEF(AchievementRewardReceive)
{
	(void)session;

	AchievementRewardReceiveReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "AchievementRewardReceive: parse error: " << glz::format_error(ec, json);
	}
	if (req.nodes.empty())
		co_return HandleResult::error("Invalid achievement request", "no achievement named");

	const auto& node = req.nodes.front();
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// RECEIVE ALL SENDS A LIST.  `M7SXoc31` is a COMMA-SEPARATED set of subject
	// ids, not one id -- a live capture on 2026-09-14 carried
	// "25000,25010,26000,27000,27010,27020".  Comparing that whole string
	// against one row's id never matched, so the Receive All button answered
	// "Invalid achievement request" on screen every time.  A single claim is
	// just the one-element case of the same list.
	const auto wanted = subjectIdList(node.subject_id);
	if (wanted.empty())
		co_return HandleResult::error("Invalid achievement request", "no achievement named");

	const auto& catalogue = theServer()->cache().achievementSubjectMst();

	AchievementRewardReceiveResp resp{};
	resp.signal_key.key = "5EdKHavF";
	std::string body;
	int32_t paidCount = 0;
	int32_t queuedPresents = 0;
	int64_t paidPoints = 0;
	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			// EACH ID IS CLAIMED INDEPENDENTLY AND A REFUSAL IS NOT FATAL.
			// Receive All sweeps whatever the screen is showing, so it will
			// routinely include rows that are already claimed or not finished;
			// failing the batch on the first of those would make the button
			// useless.  Skip them and pay the rest.
			for (const auto& subjectId : wanted)
			{
				const auto subject = std::find_if(catalogue.begin(), catalogue.end(),
					[&subjectId](const ::AchievementSubjectMst& s)
					{ return std::to_string(s.id) == subjectId; });
				if (subject == catalogue.end())
				{
					LOG_WARN << "AchievementRewardReceive: unknown achievement " << subjectId;
					continue;
				}

				// A DAILY CHALLENGE IS LATCHED PER ATTEMPT, not per id: the same
				// id comes back every week, so user_achievement_subjects' one
				// permanent latch would pay it once ever.
				if (subject->category == gme::challenge::kCategory)
				{
					if (!co_await gme::claimDailyChallenge(transaction, identity, subject->id))
						continue;   // not finished, or already paid
				}
				else if (!co_await claimRecord(transaction, identity, *subject))
				{
					continue;
				}

				if (subject->point > 0)
				{
					co_await transaction->execSqlCoro(
						"UPDATE user_info SET achieve_point = MIN(achieve_point + $1, 999999),"
						" achieve_point_total = achieve_point_total + $1 WHERE id = $2;",
						subject->point, identity.userId);
					paidPoints += subject->point;
				}

				// THE PACKED REWARD, which is not the same thing as the points.
				// Merit points are what the Randall records pay; the Level Up
				// Campaign's 152 rows pay 0 of them and carry a unit, sphere,
				// material or currency in reward_info instead.  Claiming those
				// without this would latch reward_received and hand the player
				// nothing, permanently -- worse than leaving them unclaimable.
				if (co_await gme::grantAchievementReward(
						transaction, identity, subject->reward_info))
				{
					++queuedPresents;
				}
				++paidCount;
			}

			resp.achievement_info = co_await gme::loadAchievementInfo(transaction, identity);
			resp.subjects = co_await gme::loadAchievementSubjects(
				transaction, identity, node.category, node.cond_type);

			// The badges, recomputed from the state this transaction just
			// wrote.  AchievementBadgeInfo's only writer is parseBadgeData, so
			// a claim that does not re-send this leaves the badge lit on the
			// Randall town tile and the record row until the next login.
			resp.badge.badge_data =
				co_await gme::achievementBadgeData(transaction, identity);

			// SERIALIZE BEFORE THE TRANSACTION COMMITS.  The points are already
			// credited above; if the reply could not be written after the block
			// closed, the player would have paid-out state and an empty screen
			// with no way to tell.  Failing here rolls the whole claim back.
			if (const auto error = glz::write_json(resp, body); error)
				throw std::runtime_error(glz::format_error(error, body));
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	// An empty sweep is a SUCCESS, not an error.  Tapping Receive All with
	// nothing outstanding is an ordinary thing to do, and answering with an
	// error puts a failure dialog on screen for a no-op.
	LOG_INFO << "AchievementRewardReceive: " << identity.userId << " claimed "
		<< paidCount << " of " << wanted.size() << " achievement(s) for "
		<< paidPoints << " merit point(s) and " << queuedPresents
		<< " present(s); balance " << resp.achievement_info.id;

	co_return HandleResult::success(body);
}

// ---------------------------------------------------------------------------
HANDLEF(AchievementTrade)
{
	(void)session;

	// A BODY THAT DOES NOT PARSE IS REFUSED WHOLE.  Every number in it is a
	// quoted string (AchievementTradeRequest::createBody @0x139E600); a value
	// glaze cannot read stops the parse at that field, and acting on whatever
	// half of the request was read could buy the wrong thing.
	AchievementTradeReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
		{
			LOG_WARN << "AchievementTrade: parse error: " << glz::format_error(ec, json);
			co_return HandleResult::refuseToHome("The exchange request could not be read.",
				"parse error");
		}
	}
	// The scene sends exactly one offer per request.
	if (req.nodes.size() != 1)
		co_return HandleResult::refuseToHome("Invalid exchange request.",
			std::to_string(req.nodes.size()) + " offers named");

	const auto& node = req.nodes.front();
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	const auto& shop = theServer()->cache().achievementTradeMst();
	const auto offer = std::find_if(shop.begin(), shop.end(),
		[&node](const ::AchievementTradeMst& o)
		{ return std::to_string(o.id) == node.trade_id; });
	if (offer == shop.end())
		co_return HandleResult::refuseToHome("That exchange is no longer available.",
			"unknown offer " + node.trade_id);

	// HOW MANY, AGAINST THE PER-PLAYER TOTAL.  The quantity is the shop
	// slider's (RandallAchievementShopDetailScene::sliderSet @0x1A62FBC: 1 up
	// to limit_count minus what this player has bought), priced at
	// quantity x price (getBeedPointStr).  limit_count is S8rdp9zk -- 400 on
	// the Ignis Shard offer -- not 9Hau45Jj, which is 1 on every row and has
	// no reader in the shop; reading that one refused every multi-quantity
	// purchase (2026-10-02, offer 91000059 x 400).  The remaining-stock half
	// is enforced atomically by the insert below.
	if (node.count <= 0 || offer->price <= 0 || offer->limit_count <= 0
		|| node.count > offer->limit_count)
	{
		co_return HandleResult::refuseToHome("Invalid quantity for this exchange.",
			"quantity " + std::to_string(node.count) + " of offer " + node.trade_id
			+ " (limit " + std::to_string(offer->limit_count) + ")");
	}
	const int32_t count = node.count;

	const auto reward = parseReward(offer->reward_info);
	const auto cost = static_cast<int64_t>(offer->price) * count;
	// Each purchase pays the offer's own reward count; the quantity multiplies
	// it.  Bounded by limit_count, but kept wide and checked all the same.
	const auto grantCount = static_cast<int64_t>(reward.count) * count;
	if (reward.count <= 0 || grantCount > INT32_MAX)
		co_return HandleResult::refuseToHome("That exchange cannot be completed.",
			"reward count " + std::to_string(reward.count) + " x " + std::to_string(count));

	AchievementTradeResp resp{};
	resp.signal_key.key = "5EdKHavF";
	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			// The purchase limit, enforced in the same statement that records
			// the buy so two taps cannot both pass the check.  A weekly offer's
			// row from an older week (period) starts again from this purchase;
			// see gme/common/MeritShop.hpp.  Parameters are numbered in the
			// order they first appear: SQLite binds $N by first appearance.
			const auto period = gme::merit_shop::periodOf(*offer, gme::unixNow());
			const auto bought = co_await transaction->execSqlCoro(
				"INSERT INTO user_achievement_trades (user_id, trade_id, count, period)"
				" VALUES ($1, $2, $3, $4)"
				" ON CONFLICT(user_id, trade_id) DO UPDATE SET"
				" count = CASE WHEN period = $4 THEN count + $3 ELSE $3 END,"
				" period = $4"
				" WHERE CASE WHEN user_achievement_trades.period = $4"
				" THEN user_achievement_trades.count ELSE 0 END + $3 <= $5"
				" RETURNING count;",
				identity.userId, node.trade_id, count, period, offer->limit_count);
			if (bought.empty())
			{
				transaction->rollback();
				co_return HandleResult::refuseToHome("This exchange is sold out.", "purchase limit reached");
			}

			// The points, debited only if there are enough.
			const auto paid = co_await transaction->execSqlCoro(
				"UPDATE user_info SET achieve_point = achieve_point - $1"
				" WHERE id = $2 AND achieve_point >= $1 RETURNING achieve_point;",
				cost, identity.userId);
			if (paid.empty())
			{
				transaction->rollback();
				LOG_WARN << "AchievementTrade: " << identity.userId
					<< " cannot afford offer " << node.trade_id << " (" << cost << " points)";
				co_return HandleResult::refuseToHome("Not enough Merit Points.", "not enough merit points");
			}

			// The goods.  Same present vocabulary the box uses; the porter has
			// already dropped every offer whose type is not one of these, so an
			// unknown type here is data drift rather than a normal path.
			switch (reward.type)
			{
			case 3:  // zel
			case 8:  // gem
			case 11: // karma
			{
				const char* column = reward.type == 3 ? "zel" : (reward.type == 8 ? "gems" : "karma");
				co_await transaction->execSqlCoro(
					std::string("UPDATE user_info SET ") + column + " = " + column +
					" + $1 WHERE id = $2;",
					grantCount, identity.userId);
				resp.team_info = std::move((co_await gme::getTeamInfo(transaction, identity)).nonEmpty());
				break;
			}
			case 6:  // unit
			{
				uint32_t unitId = 0;
				try { unitId = static_cast<uint32_t>(std::stoul(reward.target)); }
				catch (const std::exception&)
				{
					transaction->rollback();
					co_return HandleResult::refuseToHome("That exchange cannot be completed.", "bad unit id");
				}
				auto unit = gme::fromArchivedUnit(unitId, UnitArchiver::getRandomType());
				if (!unit)
				{
					transaction->rollback();
					co_return HandleResult::refuseToHome("That exchange cannot be completed.", "unit has no archive record");
				}
				if (reward.extraSkill > 0)
					unit->add_extra_passive_skill_id = reward.extraSkill;
				std::vector<::UserUnitInfo> granted;
				for (int64_t i = 0; i < grantCount; ++i)
					granted.push_back(std::move((co_await gme::addUserUnit(transaction, identity, *unit)).nonEmpty()));
				resp.unit_info = std::move(granted);
				resp.unit_dictionary = co_await gme::loadUnitDictionary(transaction, identity);
				break;
			}
			case 4:  // item
			case 5:  // material
			case 7:  // sphere
			{
				uint32_t itemId = 0;
				try { itemId = static_cast<uint32_t>(std::stoul(reward.target)); }
				catch (const std::exception&)
				{
					transaction->rollback();
					co_return HandleResult::refuseToHome("That exchange cannot be completed.", "bad item id");
				}
				(co_await gme::addUserItem(transaction, identity, itemId,
					static_cast<uint32_t>(grantCount))).nonEmpty();
				auto snapshot = co_await gme::loadWarehouseSnapshot(transaction, identity);
				resp.warehouse_info = std::move(snapshot.warehouse);
				resp.item_favorite = std::move(snapshot.favorites);
				resp.item_dictionary_info = std::move(snapshot.dictionary);
				break;
			}
			case 15: // alternate unit art
			{
				uint32_t unitId = 0;
				try { unitId = static_cast<uint32_t>(std::stoul(reward.target)); }
				catch (const std::exception&)
				{
					transaction->rollback();
					co_return HandleResult::refuseToHome("That exchange cannot be completed.", "bad unit id");
				}
				// The unlock is per SPECIES and has no count: the offer's
				// limit_count is what stops it being bought twice.
				co_await transaction->execSqlCoro(
					"INSERT INTO user_unit_alt_art (user_id, unit_id) VALUES ($1, $2)"
					" ON CONFLICT(user_id, unit_id) DO NOTHING;",
					identity.userId, unitId);
				// The dictionary carries the flag that turns the swap on, and
				// it is a full replace, so the whole thing goes back.
				resp.unit_dictionary = co_await gme::loadUnitDictionary(transaction, identity);
				break;
			}
			default:
				transaction->rollback();
				LOG_WARN << "AchievementTrade: offer " << node.trade_id
					<< " pays reward type " << reward.type << ", which this server cannot grant";
				co_return HandleResult::refuseToHome("That exchange cannot be completed.", "unsupported reward");
			}

			resp.achievement_info = co_await gme::loadAchievementInfo(transaction, identity);
			resp.trades = co_await gme::loadAchievementTrades(transaction, identity);
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	LOG_INFO << "AchievementTrade: " << identity.userId << " bought \"" << offer->name
		<< "\" (offer " << node.trade_id << ") x" << count << " for " << cost
		<< " merit point(s); balance " << resp.achievement_info.id;

	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}


// ---------------------------------------------------------------------------
// Start / Give Up.  One request serves both buttons, told apart by mnZ5K4Ii:
// challengeStart @0x1A570CC writes 1, QuitRequestState::updateEvent @0x1A571D8
// writes 2.
//
// In the original, accepting is what starts an achievement counting and what
// starts its VDKB0Y5h countdown, with
// RandallAchievementCommon::getMaxAchievementAcceptCnt capping how many may be
// held at once.
//
// THE DAILY CHALLENGES (category 2) WORK THAT WAY HERE TOO: Start stores an
// attempt for the open window and only a started challenge counts, Give Up
// drops it -- see gme::acceptDailyChallenge.  The client checks the cap of 5
// itself before it sends anything (challengeStart @0x1A5703C); the server
// checks it again.
//
// Every OTHER category stores nothing: their counters are tracked
// unconditionally -- the progress numbers are read off user_team_archive and
// friends on every request -- so there is nothing for accepting to switch on
// and nothing for giving up to lose.  Those rows are reported as In Progress
// (state 1) or Completed (state 2), never as "not started", which is why
// these buttons should not normally be on screen for them at all.
//
// The reply has to carry the list too, because createBody calls
// removeDataObject on the way out and the client has already dropped the page
// it is looking at.
HANDLEF(AchievementAccept)
{
	(void)session;

	AchievementAcceptReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "AchievementAccept: parse error: " << glz::format_error(ec, json);
	}
	if (req.nodes.empty())
		co_return HandleResult::error("Invalid achievement request", "no achievement named");

	const auto& node = req.nodes.front();
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	AchievementAcceptResp resp{};
	resp.signal_key.key = "5EdKHavF";
	std::string outcome;
	std::string body;
	{
		// ONE TRANSACTION, so the cap holds against two Accepts in flight at
		// once: it holds the only SQLite connection from the count to the
		// insert, and the reply is read from the state it wrote.
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			// START ALL SENDS A LIST.  The list's Start button (touchBegan
			// @0x1A5B220, tag 2) joins every not-started row it has room for
			// into one comma-separated M7SXoc31, the way Receive All does; the
			// detail page sends one id.  Each is started in order, so the cap
			// stops the tail.
			std::set<int32_t> seen;
			for (const auto& id : subjectIdList(node.subject_id))
			{
				const auto subjectId = subjectIdNumber(id);
				if (node.category != gme::challenge::kCategory && !gme::challenge::isChallengeId(subjectId))
					continue;
				if (!seen.insert(subjectId).second)
					continue;
				const auto result = co_await gme::acceptDailyChallenge(
					transaction, identity, subjectId, node.accept_flag);
				outcome += (outcome.empty() ? "" : ", ") + id + " " + acceptOutcome(result);
			}

			resp.achievement_info = co_await gme::loadAchievementInfo(transaction, identity);
			resp.subjects = co_await gme::loadAchievementSubjects(
				transaction, identity, node.category, node.cond_type);
			if (const auto error = glz::write_json(resp, body); error)
				throw std::runtime_error(glz::format_error(error, body));
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}
	if (outcome.empty())
		outcome = "nothing stored (progress here is unconditional)";

	LOG_INFO << "AchievementAccept: " << identity.userId
		<< (node.accept_flag == 2 ? " gave up " : " started ") << node.subject_id
		<< " (category " << node.category << "); " << outcome << ", "
		<< resp.subjects.size() << " row(s) re-sent";

	co_return HandleResult::success(body);
}
