#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/archive/GachaArchiver.hpp>
#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/forge/Rules.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/SelectorRotation.hpp>
#include <gimuserver/gme/common/SelectorBanners.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>
#include <gimuserver/gme/common/SummonRotation.hpp>
#include <gimuserver/gme/common/SummonGates.hpp>
#include <gimuserver/gme/common/SummonTickets.hpp>
#include <gimuserver/gme/common/UnitPick.hpp>
#include <gimuserver/utils/Random.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace
{

/*!
* Picks a summon animation effect id for a summoned unit rarity.
*
* @param rarity Unit rarity used to choose the animation effect.
* @return Gacha effect id to send, or std::nullopt if none is available.
*/
inline std::optional<uint32_t> getGachaEffect(uint32_t rarity)
{
	const auto& mst = theServer()->cache().initializeResp().gacha_effects;

	uint32_t totalWeight = 0;
	std::vector<const GachaEffectMst*> effects;
	// The MST is already sorted by rarity.
	for (auto it = mst.begin(); it != mst.end() && it->rarity <= rarity; ++it)
	{
		if (it->rarity == rarity && it->weight > 0)
		{
			effects.push_back(&*it);
			totalWeight += it->weight;
		}
	}

	if (effects.empty())
	{
		LOG_ERROR << "Unable to find gacha effect for rarity " << rarity;
		return std::nullopt;
	}

	auto roll = RandomUInt(1, totalWeight);
	for (const auto& effect : effects)
	{
		if (roll <= effect->weight)
		{
			return effect->id;
		}
		roll -= effect->weight;
	}

	return std::nullopt;
}

} // namespace

HANDLEF(GachaAction)
{
	GachaActionReq req{};
	const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json);
	if (ec)
	{
		const auto& fmte = glz::format_error(ec, json);
		LOG_DEBUG << "Gme GachaAction Error during JSON read: " << fmte;
		co_return HandleResult::error("Deserialization error", fmte);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// What pays for the pull (gme::SummonPayment).  `324b023k` is set by
	// SummonsDetailScene::touchBegan @0x16A80BC after it has satisfied itself
	// that the player holds a ticket for this gate: 2 when the gate has V2
	// types of its own (it sums the held amounts of the types whose
	// SummonTicketV2Mst row targets the gate, @0x16A7FA8), 1 when it has none
	// and the plain counter off team_info is not zero (@0x16A7F1C), 0 for the
	// gate's currency.  It used to be read as a bool, so the 2 a real client
	// sends to spend a V2 ticket failed to parse.
	const auto paymentCode = req.gacha_action_info.using_gacha_ticket;
	if (paymentCode > static_cast<uint32_t>(gme::SummonPayment::GateTicket))
	{
		co_return HandleResult::error("Invalid gacha request", "Unknown summon payment");
	}
	const auto payment = static_cast<gme::SummonPayment>(paymentCode);
	const bool usingTicket = payment != gme::SummonPayment::Currency;

	// A count of 0 means one pull, not a bad request.
	//
	// `a329kbl8` is written straight from UserState::noOfGachaRequests.  The
	// summon screen sets it when a button is pressed: 1 for Summon, and
	// min(affordable pulls, 10) for Multi Summon (SummonsDetailScene::
	// setGachaInfo @0x16A6894, shown only while FeatureCheck multisummon is
	// on).  GachaActionScene::checkConnectResult and GachaSummaryScene reset it
	// to 0.  The tutorial never goes through a press —
	// tuto15.txt drives `change_gacha_action_scene` from the script — so the
	// count arrives as the 0 it was last reset to:
	//
	//     "1IR86sAv":[{"7Ffmi96v":"2000","a329kbl8":"0","324b023k":"0"}]
	//
	// The gate id is correct in that payload, so this is not a parse failure
	// and not a client bug to route around: no response field influences
	// noOfGachaRequests, it is pure client state.  Rejecting 0 makes the
	// free-summon tutorial unfinishable, so clamp it to a single pull.  The
	// currency check below still applies, so this cannot be used to summon
	// for free.
	//
	// Anything above what the client can send is refused: the client never
	// offers more than 10, and an unbounded count let one request bypass the
	// Multi Summon cap and roll an arbitrary number of units.
	const auto count = std::max<uint32_t>(req.gacha_action_info.request_count, 1);
	const auto maxCount = theServer()->cache().feature().multi_summon
		? gme::kMaxSummonsPerRequest
		: 1u;
	if (count > maxCount)
	{
		co_return HandleResult::error("Invalid gacha request", "Too many summons in one request");
	}

	// Find the summon gate and determine the cost.
	const auto gachaId = req.gacha_action_info.gacha_id;
	const auto gachaRecord = GachaArchiver::instance().lookup(gachaId);
	if (!gachaRecord)
	{
		co_return HandleResult::error("Invalid gacha", "Unknown summon gate");
	}

	GachaInfoMst mst{};
	if (!GachaArchiver::instance().populatePacket(*gachaRecord, mst))
	{
		co_return HandleResult::error("Invalid gacha", "Missing in MST");
	}

	// A SELECTOR gate is never paid for.  Its SUMMON button is supposed to open
	// the unit picker and come back as UnitSelectorGachaTicket.  Several of these
	// gates carry the 999 "not for sale" cost, but 17650, 17790, 17800, 18270 and
	// 19000 carry a plain 5 gems — so if a pull ever arrives here for one (a
	// client that missed JukkSeNA, a replayed body), the only safe answer is to
	// refuse it rather than sell a guaranteed 7-star or Omni for gems.
	{
		const auto& selectors = theServer()->cache().unitSelectorGacha();
		const bool isSelectorGate = std::any_of(selectors.begin(), selectors.end(),
			[&](const UnitSelectorGachaMst& sel)
			{ return static_cast<uint32_t>(sel.gacha_id) == gachaId; });
		if (isSelectorGate)
		{
			LOG_WARN << "GachaAction: gate " << gachaId
				<< " is a selector gate and cannot be pulled for currency; refused";
			co_return HandleResult::error("Invalid gacha request",
				"This gate is redeemed with its selector ticket");
		}
	}

	// Only a gate the rail offers can be pulled.  The archive also holds gates
	// no tile names, and GachaList is display only, so without this a crafted
	// request could open any of them.  A Sandbox account's rail holds the whole
	// weekly rotation pool (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await gme::sandbox::isOn(theDb(), identity.userId);
	if (!gme::isOnSummonRail(gachaId, sandboxed))
	{
		LOG_WARN << "GachaAction: gate " << gachaId << " is not on the summon rail; refused";
		// A featured gate from a rail loaded before Monday 08:00 is stale client
		// state, not a crafted request: say so and send the player Home, which
		// reloads the rail on the next visit.
		if (gme::isRotatingGate(gachaId))
		{
			co_return HandleResult::refuseToHome(
				"This banner has rotated out. Open Summon again to see this week's featured banners.",
				"Gate not featured this week");
		}
		co_return HandleResult::error("Invalid gacha request", "This gate is not open");
	}

	// A banner the overlay rules close or lock (rules.json schedules and
	// unlocks; forge/Rules.hpp).  The rail no longer lists it, so this is a
	// rail loaded before the window shut: the reason, then Home.  Every unlock
	// is open for a Sandbox account (gme/common/Sandbox.hpp).
	if (const auto forgeRules = forge::rules(); forgeRules->hidesGates())
	{
		std::set<int32_t> cleared;
		if (sandboxed)
			cleared = gme::sandbox::everyMission();
		else if (forgeRules->gateUnlocks())
			for (const auto& done : co_await gme::getClearedMissions(theDb(), identity))
				cleared.insert(done.mission_id);
		if (const auto refused = forgeRules->gateRefusal(static_cast<int32_t>(gachaId), cleared, gme::unixNow()))
			co_return HandleResult::refuseToHome(refused->message,
				"forge rule " + refused->rule + ": banner " + std::to_string(gachaId) + " refused");
	}

	// The tutorial door is used once, from tuto15.  GachaList and UserInfo stop
	// listing it when the tutorial ends, and it is refused from then on too.
	if (gachaId == gme::kTutorialGachaId)
	{
		const bool inTutorial = co_await gme::tutorialInProgress(theDb(), identity);
		if (!inTutorial)
		{
			co_return HandleResult::error("Invalid gacha request", "This gate is not open");
		}
	}

	// The checkbox is only offered on gem gates; Honor gates are paid in Honor.
	if (usingTicket && !gme::summonTicketsAccepted(gachaId, mst.type))
	{
		co_return HandleResult::error("Invalid gacha request",
			"Summon tickets cannot be used on this gate");
	}

	// The client picks the kind of ticket by the gate, never by the player:
	// the gate's own V2 types when it has any, the plain counter otherwise.
	const bool gateHasTickets = !gme::summonTicketTypesForGate(gachaId).empty();
	if (payment == gme::SummonPayment::PlainTicket && gateHasTickets)
	{
		co_return HandleResult::error("Invalid gacha request",
			"This gate takes its own summon tickets");
	}
	if (payment == gme::SummonPayment::GateTicket && !gateHasTickets)
	{
		co_return HandleResult::error("Invalid gacha request",
			"This gate has no summon tickets of its own");
	}

	// 64-bit so no count/price pair can wrap into a small price.
	const auto gemCost = static_cast<int64_t>(mst.gems) * count;
	const auto friendPointCost = static_cast<int64_t>(mst.friend_points) * count;

	// Ensure the user has enough currency to perform the summon.  This is the
	// early, friendly refusal; the debit below re-checks inside the
	// transaction, so two requests racing on one balance cannot both pay.
	auto currency = co_await db::DatabaseInterface::read(
		theDb(),
		"user_info",
		{
			db::Data("gems"),
			db::Data("friend_points"),
			db::Data("summon_tickets"),
			db::Lookup("gumi_user_id", identity.gumiUserId),
			db::Lookup("id", identity.userId),
		});
	const auto currentGems = currency.front<int32_t>("gems");
	const auto currentFriendPoints = currency.front<int32_t>("friend_points");
	const auto currentTickets = currency.front<int32_t>("summon_tickets");

	// A gate's own tickets are counted per type by spendSummonTicketsV2 inside
	// the transaction.
	if (payment == gme::SummonPayment::PlainTicket && currentTickets < static_cast<int32_t>(count))
	{
		co_return HandleResult::error("Invalid gacha request", "Not enough summon tickets");
	}
	if (payment == gme::SummonPayment::Currency
		&& (gemCost > currentGems || friendPointCost > currentFriendPoints))
	{
		co_return HandleResult::error("Invalid gacha request", "Not enough currency");
	}

	const auto summonedUnits = GachaArchiver::instance().summonFrom(*gachaRecord, count);
	if (summonedUnits.size() != count)
	{
		co_return HandleResult::error("Invalid gacha", "Unable to summon from gate");
	}

	// We need to wrap these operations in a transaction to avoid an invalid
	// intermediate state.
	GachaActionResp resp{};
	resp.signal_key = req.signal_key;
	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			// Deduct what pays for the pull, and only that: a gate's own
			// tickets, the plain counter, or the currency.  Each debit is
			// relative and guarded by the balance it needs, so it is decided
			// against the balance inside this transaction rather than the one
			// read above.
			bool paid = false;
			if (payment == gme::SummonPayment::GateTicket)
			{
				paid = co_await gme::spendSummonTicketsV2(transaction, identity, gachaId, count);
			}
			else if (payment == gme::SummonPayment::PlainTicket)
			{
				const auto debited = co_await transaction->execSqlCoro(
					"UPDATE user_info SET summon_tickets = summon_tickets - $1"
					" WHERE id = $2 AND gumi_user_id = $3 AND summon_tickets >= $1;",
					static_cast<int64_t>(count), identity.userId, identity.gumiUserId);
				paid = debited.affectedRows() == 1;
			}
			else
			{
				const auto debited = co_await transaction->execSqlCoro(
					"UPDATE user_info SET gems = gems - $1, friend_points = friend_points - $2"
					" WHERE id = $3 AND gumi_user_id = $4 AND gems >= $1 AND friend_points >= $2;",
					gemCost, friendPointCost, identity.userId, identity.gumiUserId);
				paid = debited.affectedRows() == 1;
			}
			if (!paid)
			{
				transaction->rollback();
				co_return HandleResult::error("Invalid gacha request",
					usingTicket ? "Not enough summon tickets" : "Not enough currency");
			}
			if (payment == gme::SummonPayment::GateTicket)
			{
				resp.summon_ticket_v2_user = co_await gme::loadSummonTicketsV2(transaction, identity);
			}

			// Commit the summoned units and populate the response.
			for (const auto& unitId : summonedUnits)
			{
				// Build and inject the unit.
				const auto unitRecord = UnitArchiver::instance().lookup(unitId);
				if (!unitRecord)
				{
					throw std::runtime_error("Unable to find summoned unit archive record");
				}

				// The personality type follows F_UNIT_TYPE_MST's appearance
				// rates (Lord 23, Anima 23, Breaker 22, Guardian 22, Oracle 10,
				// Rex 0), the game's own table, not a flat 1-in-5.
				auto unit = gme::fromArchivedUnit(
					unitId,
					MissionArchiver::rollUnitType());
				if (!unit)
				{
					throw std::runtime_error("Unable to create summoned unit from archive");
				}

				auto added = std::move(
					(co_await gme::addUserUnit(transaction, identity, *unit)).nonEmpty());
				const auto gachaEffect = getGachaEffect(unitRecord->rarity);
				if (!gachaEffect)
				{
					throw std::runtime_error(
						"Unable to find gacha effect for summoned unit rarity");
				}

				resp.ope_user_unit.push_back({
					.user_unit_id = added.user_unit_id,
					.gacha_effect_id = *gachaEffect,
				});
				resp.unit_info.push_back(std::move(added));
			}

			// TROPHY 100100 (Honor spent).  Counted here because this is the
			// only place Honor leaves the balance; its counterpart 100090 is
			// bumped at MissionEnd, where it is earned.  Zero for a gem or
			// ticket pull, and bumpArchiveCounters drops zero deltas.
			co_await gme::bumpArchiveCounters(transaction, identity, {
				{ "friend_p_use", usingTicket ? 0 : static_cast<int64_t>(friendPointCost) },
			});

			// The COMPLETE dictionary, not just the summoned species: this
			// response rebuilds the reference list the summon lineup reads
			// (UserUnitDictionary in net/user.kdl).
			resp.unit_dictionary = co_await gme::loadUnitDictionary(transaction, identity);

			resp.team_info = std::move((co_await gme::getTeamInfo(transaction, identity)).nonEmpty());
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	std::string buffer;
	const auto& ec2 = glz::write_json(resp, buffer);
	if (ec2)
	{
		const auto& glze = glz::format_error(ec2, buffer);
		LOG_DEBUG << "Gme GachaAction Error during JSON writing: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}

	co_return HandleResult::success(buffer);
}

HANDLEF(GachaList)
{
	GachaListReq req = {};
	const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json);
	if (ec)
	{
		const auto& fmte = glz::format_error(ec, json);
		LOG_DEBUG << "Gme GachaList Error during JSON read: " << fmte;
		co_return HandleResult::error("Deserialization error", fmte);
	}

	// The Tutorial Gacha (gate 2000, type 20000 — the client's own label, see
	// GachaActionScene::initConnect) is a TUTORIAL-ONLY door.  tuto15 drives the
	// player into it by tapping a fixed screen position, so it has to be in the
	// banner rail while the tutorial runs — but leaving it there afterwards is
	// wrong twice over: it displaces the large event banner from the top of the
	// rail, and opening it outside the tutorial crashes the client.
	//
	// So it is filtered out once the player reports the tutorial finished
	// (gme::tutorialInProgress).  The rail then returns to its original order
	// with the event tile first.
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	const bool tutorialDone = !(co_await gme::tutorialInProgress(theDb(), identity));

	GachaListResp resp = theServer()->cache().gachaListRsp();
	resp.gacha_info = GachaArchiver::instance().populateAllPackets();
	resp.signal_key = req.signal_key;

	// This week's featured banners, before the per-player tiles below are
	// appended (gme/common/SummonRotation.hpp), the whole pool for a Sandbox
	// account.  UserInfo does the same.
	const auto now = gme::unixNow();
	const bool sandboxed = co_await gme::sandbox::isOn(theDb(), identity.userId);
	gme::applyWeeklyRail(resp.gacha_categories, now, sandboxed);
	gme::applyFeaturedNotes(resp.gacha_info, now);

	// The selector catalogue, which is what tells the detail scene that a gate
	// is ticket-only — see the JukkSeNA field doc in net/handlers.kdl.
	resp.unit_selector_gacha = theServer()->cache().unitSelectorGacha();
	// Same rotation UserInfo applies.  BOTH senders have to do it: this is the
	// reply the summon screen itself reads, so applying it in only one place
	// would leave the picker showing the placeholder pool on whichever screen
	// happened to refresh last.
	gme::applyWeeklySelector(resp.unit_selector_gacha);

	// Selector banners are per player: a selector gate is redeemable only with
	// its own ticket, so its rail tile is added here and only while one is held
	// (gme/common/SelectorBanners.hpp).  gacha_category_mst.json holds the
	// permanent tiles.
	{
		auto selectors = co_await gme::selectorGachaCategories(theDb(), identity);
		resp.gacha_categories.insert(resp.gacha_categories.end(),
			std::make_move_iterator(selectors.begin()),
			std::make_move_iterator(selectors.end()));
	}
	// The 50-gem unit pick: its 24 gates and pickers, and its four tiles
	// while the account has not used it (gme/common/UnitPick.hpp).
	co_await gme::addUnitPick(theDb(), identity,
		resp.gacha_categories, resp.gacha_info, resp.unit_selector_gacha);

	if (tutorialDone)
	{
		gme::hideTutorialGacha(resp.gacha_categories, resp.gacha_info);
	}

	// Banners the overlay rules close or lock leave the rail (their
	// GachaInfo rows stay: a tile is what the player taps).
	if (const auto forgeRules = forge::rules(); forgeRules->hidesGates())
	{
		std::set<int32_t> cleared;
		if (sandboxed)
			cleared = gme::sandbox::everyMission();   // every unlock open
		else if (forgeRules->gateUnlocks())
			for (const auto& done : co_await gme::getClearedMissions(theDb(), identity))
				cleared.insert(done.mission_id);
		forge::filterRail(resp.gacha_categories, forgeRules->hiddenGates(cleared, now));
	}

	std::string buffer;
	const auto& ec2 = glz::write_json(resp, buffer);
	if (ec2)
	{
		const auto& glze = glz::format_error(ec2, buffer);
		LOG_DEBUG << "Gme GachaList Error during JSON writing: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}

	co_return HandleResult::success(buffer);
}
