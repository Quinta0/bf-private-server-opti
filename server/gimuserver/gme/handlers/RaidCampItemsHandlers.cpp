#include "RaidHandlerSupport.hpp"

#include <gimuserver/gme/common/ItemMixOrders.hpp>
#include <gimuserver/gme/common/RaidCamp.hpp>
#include <gimuserver/gme/common/RaidItems.hpp>
#include <gimuserver/gme/common/RaidStats.hpp>
#include <gimuserver/gme/common/Town.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <set>

// Classic Raid round 2 -- camp rest, raid items and the raid shop
// (RAIDS_ROUND2_CAMP_ITEMS.md 3 to 5).
//
// TWO KINDS OF REQUEST, TWO ERROR RULES.  The camp and the shop are
// interactive: a refusal is a refuseToRetry, which the camp/field scenes
// answer by re-entering the raid through 1201.  RaidItemEdit, RaidItemMix and
// RaidItemDelete are QUEUED (StepScene drains them on the next scene change),
// so an error would surface on a StepScene that knows nothing of raids: they
// never refuse, change nothing when they cannot apply, and always answer with
// the server's own lists (the ItemEdit.cpp precedent).

using namespace gme::raid;
using namespace gme::raid::handlers;
using gme::UserIdentity;

namespace
{

/*! RaidShopUse types (RaidSceneBase::shopConnect @0x19CE378). */
constexpr int32_t kShopAube = 100;
constexpr int32_t kShopLives = 101;
/*!
* Server prices in gems, authoritative whatever 03UGMHxF claims (the
* ShopUse.cpp pattern).  1 each: the client's own prompt says 1 Gem per life
* (seen on emulator-5558); the orb price is assumed the same.
*/
constexpr int32_t kRaidAubeRefillGems = 1;
constexpr int32_t kRaidLivesRefillGems = 1;

/*! The member's live state in the request's run, or why the camp is closed to them. */
struct CampCall
{
	FieldCall call;
	std::optional<RunMember> member;
	std::string refusal;
};

drogon::Task<CampCall> openCampCall(const char* name, const std::string json)
{
	CampCall out{ co_await openFieldCall(json), std::nullopt, {} };
	if (!out.call.run)
	{
		out.refusal = std::string(name) + ": no live run";
		co_return out;
	}
	out.member = co_await loadMember(theDb(), out.call.run->id, out.call.identity.userId);
	if (!out.member || out.member->ended)
		out.refusal = std::string(name) + ": " + out.call.identity.userId + " is not in run "
			+ std::to_string(out.call.run->id);
	co_return out;
}

/*! A camp point one route step from `point` that the member could walk to now; `point` when none. */
drogon::Task<int32_t> campOneStepAhead(const RaidRun run, const std::string userId, const int32_t mapId,
	const int32_t point)
{
	for (const auto& row : theServer()->cache().raidPointMst())
	{
		// A plain route only: moveTo would also spend an armed Teleport.
		if (row.map_id != mapId || row.point_id == point || !isCampPoint(mapId, row.point_id)
			|| !hasRoute(run.missionId, point, row.point_id))
			continue;
		if (co_await moveTo(theDb(), run, userId, row.point_id))
			co_return row.point_id;
	}
	co_return point;
}

HandleResult questEnded(const std::string& why)
{
	return HandleResult::refuseToRetry("This raid quest has ended.", why);
}

/*! The bag and set lists of a queued item request's reply (empty lists omitted: [] clears nothing). */
drogon::Task<::RaidItemsResp> itemsResp(
	const db::Database database, const std::string userId, const std::optional<RaidRun> run, const bool withSet)
{
	::RaidItemsResp resp{};
	if (withSet)
	{
		auto set = setRows(co_await loadSet(database, userId, kItemSetKind));
		if (!set.empty())
			resp.item_set = std::move(set);
	}
	if (run)
	{
		auto bag = bagRows(userId, co_await loadBag(database, run->id, userId));
		if (!bag.empty())
			resp.bag = std::move(bag);
	}
	co_return resp;
}

/*!
* Whether a run takes item edits: running, and the caller a member who has
* not left.  Read through the edit's own transaction, so the check and the
* bag write cannot be split by a MissionEnd in between.
*/
drogon::Task<bool> runTakesItems(const db::Database tx, const FieldCall& call)
{
	if (!call.run)
		co_return false;
	const auto result = co_await tx->execSqlCoro(
		"SELECT result FROM raid_room_mission WHERE room_mission_id = $1;", call.run->id);
	if (result.empty() || result[0]["result"].as<int32_t>() != kRunRunning)
		co_return false;
	const auto member = co_await loadMember(tx, call.run->id, call.identity.userId);
	co_return member && !member->ended;
}

/*! The caller's recipe permissions, as ItemMix derives them. */
drogon::Task<std::set<int32_t>> permittedRecipeIds(const db::Database database, const UserIdentity identity)
{
	std::set<int32_t> cleared;
	for (const auto& done : co_await gme::getClearedMissions(database, identity))
		cleared.insert(done.mission_id);
	std::set<int32_t> permitted;
	for (const auto& entry : co_await gme::Town::permittedRecipes(database, identity, cleared))
		permitted.insert(entry.recipe_id);
	co_return permitted;
}

/*! Charge `price` gems in one statement; false when the player has too few. */
drogon::Task<bool> spendGems(const db::Database database, const std::string userId, const int32_t price)
{
	const auto charged = co_await database->execSqlCoro(
		"UPDATE user_info SET gems = gems - $1 WHERE id = $2 AND gems >= $3;", price, userId, price);
	co_return charged.affectedRows() > 0;
}

/*! RaidShopUse 100: refill raid orbs (aube) to the cap.  Full already: no charge. */
drogon::Task<std::string> refillAube(const UserIdentity identity)
{
	// Read the orbs through the charge's own transaction: two taps in a row
	// must not both pass the "not full" check and pay twice.
	auto transaction = co_await gme::newTransaction(theDb());
	const auto user = co_await loadRaidUser(transaction, identity.userId);
	if (user.aube >= kAubeMax)
	{
		transaction->rollback();
		co_return std::string{};
	}
	if (!(co_await spendGems(transaction, identity.userId, kRaidAubeRefillGems)))
	{
		transaction->rollback();
		co_return "You do not have enough Gems.";
	}
	co_await transaction->execSqlCoro(
		"UPDATE raid_user SET aube = $1, aube_ts = 0 WHERE user_id = $2;", kAubeMax, identity.userId);
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return "The purchase could not be completed. Please try again.";
	LOG_INFO << "RaidShopUse: " << identity.userId << " refilled raid orbs for " << kRaidAubeRefillGems << " gem(s)";
	co_return std::string{};
}

/*! RaidShopUse 101: refill hearts in the live run.  Full already: no charge. */
drogon::Task<std::string> refillLives(const FieldCall& call)
{
	if (!call.run || call.run->result != kRunRunning)
		co_return "This raid quest has ended.";
	const auto member = co_await loadMember(theDb(), call.run->id, call.identity.userId);
	if (!member || member->ended)
		co_return "This raid quest has ended.";
	auto transaction = co_await gme::newTransaction(theDb());
	// Claim first, inside the charge's transaction: only a request that
	// actually raises the hearts pays, so a double tap cannot charge twice.
	// One life per purchase: the client's prompt reads "You can use 1 Gem to
	// recover 1 Life" (seen on emulator-5558; CAMP_ITEMS unknowns 2 and 3).
	const auto refilled = co_await transaction->execSqlCoro(
		"UPDATE raid_mission_user SET player_rest = player_rest + 1 WHERE room_mission_id = $1 AND user_id = $2"
		" AND player_rest < $3 AND ended = 0;",
		call.run->id, call.identity.userId, kHeartsMax);
	if (refilled.affectedRows() == 0)
	{
		transaction->rollback();
		co_return std::string{};
	}
	if (!(co_await spendGems(transaction, call.identity.userId, kRaidLivesRefillGems)))
	{
		transaction->rollback();
		co_return "You do not have enough Gems.";
	}
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return "The purchase could not be completed. Please try again.";
	LOG_INFO << "RaidShopUse: " << call.identity.userId << " refilled hearts in run " << call.run->id;
	co_return std::string{};
}

} // namespace

// RaidCampRestStart (I7nVUv0h) -- the Rest button on a camp point.  The
// reply's 81B4TvEK must carry the caller's row (connectEndCheck reads
// getCampFlg off the lookup without a null check).
HANDLEF(RaidCampRestStart)
{
	(void)session;
	auto camp = co_await openCampCall("RaidCampRestStart", json);
	if (!camp.refusal.empty() || camp.call.run->result != kRunRunning)
		co_return questEnded("RaidCampRestStart: " + camp.refusal);
	const auto& run = *camp.call.run;
	const auto& userId = camp.call.identity.userId;
	const auto mission = runMission(run);
	auto point = camp.member->nowPoint;
	// The Rest button is drawn the moment the avatar arrives, before the poll
	// that reports the walk (as with Harvest): take a camp one route step
	// ahead as the pending move.
	if (camp.member->battleSerial == 0 && mission && !isCampPoint(mission->map_id, point))
		point = co_await campOneStepAhead(run, userId, mission->map_id, point);
	if (camp.member->battleSerial != 0 || !mission || !isCampPoint(mission->map_id, point))
		co_return HandleResult::refuseToRetry("You can only rest at a camp.",
			"RaidCampRestStart: " + userId + " is at point " + std::to_string(point));

	if (co_await startCamp(theDb(), run.id, userId, nowSeconds()))
		co_await bumpStat(theDb(), userId, stat::kCampRecover);
	LOG_INFO << "RaidCampRestStart: " << userId << " rests at point " << camp.member->nowPoint << " in run " << run.id;
	co_return reply("RaidCampRestStart", co_await fieldResp(theDb(), userId, run));
}

// RaidCampRestGet (diIe8ux7) -- the camp's 4 s poll.  Heals lazily and
// answers with the whole field state, so the camp sees the run end or a boss
// arrive (fieldEffectCheck).  A finished run still answers normally: its
// Result plays the ending.
HANDLEF(RaidCampRestGet)
{
	(void)session;
	auto camp = co_await openCampCall("RaidCampRestGet", json);
	if (!camp.refusal.empty())
		co_return questEnded("RaidCampRestGet: " + camp.refusal);
	if (camp.call.run->result == kRunRunning)
		co_await restAtCamp(theDb(), camp.call.run->id, camp.call.identity.userId, nowSeconds());
	co_return reply("RaidCampRestGet", co_await fieldResp(theDb(), camp.call.identity.userId, *camp.call.run));
}

// RaidCampRestEnd (f9Dh4djx) -- leave the camp.  MUST answer 81B4TvEK with
// the caller's MFz8YRS6 = 0: the rebuilt field's initCheck reads the cached
// CampFlg and would send the player straight back to the camp.
HANDLEF(RaidCampRestEnd)
{
	(void)session;
	auto camp = co_await openCampCall("RaidCampRestEnd", json);
	if (!camp.refusal.empty())
		co_return questEnded("RaidCampRestEnd: " + camp.refusal);
	co_await endCamp(theDb(), camp.call.run->id, camp.call.identity.userId, nowSeconds());
	LOG_INFO << "RaidCampRestEnd: " << camp.call.identity.userId << " left the camp in run " << camp.call.run->id;
	co_return reply("RaidCampRestEnd", co_await fieldResp(theDb(), camp.call.identity.userId, *camp.call.run));
}

// RaidItemEdit (DZLI83hx, queued) -- the whole new raid item set; in the
// field the set only trades with the mission bag.
HANDLEF(RaidItemEdit)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	{
		auto transaction = co_await gme::newTransaction(theDb());
		const bool live = co_await runTakesItems(transaction, call);
		std::string why = "no live run";
		if (live)
		{
			const auto oldSet = co_await loadSet(transaction, userId, kItemSetKind);
			const auto bag = co_await loadBag(transaction, call.run->id, userId);
			const auto edit = applySetEdit(oldSet, readSetRows(call.req, "71U5wzhI"), bag);
			if (const auto* ok = std::get_if<SetEdit>(&edit))
			{
				co_await storeSet(transaction, userId, kItemSetKind, ok->set);
				co_await storeBag(transaction, call.run->id, userId, ok->bag);
				why.clear();
			}
			else
			{
				why = std::get<std::string>(edit);
			}
		}
		if (why.empty())
		{
			if (!(co_await gme::CommitTransaction(std::move(transaction))))
				LOG_ERROR << "RaidItemEdit: edit of " << userId << " did not commit";
		}
		else
		{
			transaction->rollback();
			LOG_WARN << "RaidItemEdit: " << userId << " edit not applied: " << why;
		}
	}
	co_return reply("RaidItemEdit", co_await itemsResp(theDb(), userId, call.run, true));
}

// RaidItemMix (K17JUYR2, queued) -- craft from bag materials: one product
// per craft, no karma, no warehouse.
HANDLEF(RaidItemMix)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	const auto orders = gme::parseMixOrders(call.req.text("JTf2jY5o", "4HqhTf3a"));
	{
		auto transaction = co_await gme::newTransaction(theDb());
		bool live = false;
		if (orders)
			live = co_await runTakesItems(transaction, call);
		if (live)
		{
			const auto permitted = co_await permittedRecipeIds(transaction, call.identity);
			const auto bag = co_await loadBag(transaction, call.run->id, userId);
			const auto outcome = mixInBag(*orders, permitted, bag);
			co_await storeBag(transaction, call.run->id, userId, outcome.bag);
			if (outcome.crafted > 0)
				co_await bumpStat(transaction, userId, stat::kItemMixCnt, outcome.crafted);
			if (!(co_await gme::CommitTransaction(std::move(transaction))))
				LOG_ERROR << "RaidItemMix: mix of " << userId << " did not commit";
			LOG_INFO << "RaidItemMix: " << userId << " crafted " << outcome.crafted << " in run " << call.run->id;
		}
		else
		{
			transaction->rollback();
			LOG_WARN << "RaidItemMix: nothing mixed for " << userId << (orders ? " (no live run)" : " (bad order list)");
		}
	}
	co_return reply("RaidItemMix", co_await itemsResp(theDb(), userId, call.run, false));
}

// RaidItemDelete (N43JmIf2) -- no caller in this client build (no
// constructor call anywhere); served anyway, never refused.
HANDLEF(RaidItemDelete)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	const auto* node = call.req.node("qa3iN58C");
	const auto frameId = static_cast<int32_t>(RaidRequest::numberOf(node, "n6E8iMf3", -1));
	const auto count = static_cast<int32_t>(RaidRequest::numberOf(node, "wgV86x1q"));
	{
		auto transaction = co_await gme::newTransaction(theDb());
		const bool live = co_await runTakesItems(transaction, call);
		if (live)
		{
			const auto bag = co_await loadBag(transaction, call.run->id, userId);
			co_await storeBag(transaction, call.run->id, userId, bagDrop(bag, frameId, count));
			if (!(co_await gme::CommitTransaction(std::move(transaction))))
				LOG_ERROR << "RaidItemDelete: delete of " << userId << " did not commit";
		}
		else
		{
			transaction->rollback();
		}
	}
	co_return reply("RaidItemDelete", co_await itemsResp(theDb(), userId, call.run, false));
}

// RaidShopUse (4zJ0WmTX) -- gems for raid orbs (100, outside a room) or
// hearts (101, the game-over prompt).  Gems are earned here, never sold.
HANDLEF(RaidShopUse)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	const auto* node = call.req.node("32ibWjFG");
	const auto type = static_cast<int32_t>(RaidRequest::numberOf(node, "60IsqxDt"));
	const auto claimed = RaidRequest::numberOf(node, "03UGMHxF");

	std::string refusal;
	if (type == kShopAube)
		refusal = co_await refillAube(call.identity);
	else if (type == kShopLives)
		refusal = co_await refillLives(call);
	else
		LOG_WARN << "RaidShopUse: " << userId << " sent unknown shop type " << type << "; nothing charged";
	if (!refusal.empty())
		co_return HandleResult::refuseToRetry(refusal, "RaidShopUse: type " + std::to_string(type) + " for " + userId);
	if ((type == kShopAube && claimed != kRaidAubeRefillGems) || (type == kShopLives && claimed != kRaidLivesRefillGems))
		LOG_INFO << "RaidShopUse: client showed " << claimed << " gem(s) for type " << type << "; server price applies";

	::RaidShopUseResp resp{};
	resp.team_info = std::move((co_await gme::getTeamInfo(theDb(), call.identity)).nonEmpty());
	if (type == kShopAube)
	{
		// The 1ry6BKoe reader resets every field first: send the whole node.
		const auto user = co_await userInfoResp(theDb(), userId);
		resp.user = std::vector<::RaidUserInfo>{ user.user };
		resp.permit = user.permit;
		resp.clear = user.clear;
	}
	else if (type == kShopLives && call.run)
	{
		// REQUIRED: the rebuilt field's initCheck re-reads PlayerRest from 81B4TvEK.
		const auto field = co_await fieldResp(theDb(), userId, *call.run);
		resp.mission_users = field.mission_users;
	}
	co_return reply("RaidShopUse", resp);
}
