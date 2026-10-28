#include "RaidHandlerSupport.hpp"

#include <gimuserver/gme/common/RaidItems.hpp>
#include <gimuserver/gme/common/RaidStats.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <map>
#include <random>
#include <sstream>

// Classic Raid round 2 -- the field of a running raid (RAIDS_ROUND2_CORE.md
// 2 and 4): MissionStart (new run or resume), the 4 s MissionInfo heartbeat
// with walking, MissionInfoLight, Harvest, the two item-use requests,
// MissionExit, and the room-path MissionSelect (RaidEnter is in
// RaidRoomHandlers.cpp).  Battles and the
// mission end are in RaidBattleHandlers.cpp.

using namespace gme::raid;
using namespace gme::raid::handlers;

namespace
{

/*! "point:count,..." as a map. */
std::map<int32_t, int32_t> parseSearchInfo(const std::string& text)
{
	std::map<int32_t, int32_t> out;
	std::stringstream rows(text);
	std::string row;
	while (std::getline(rows, row, ','))
	{
		const auto colon = row.find(':');
		if (colon != std::string::npos)
			out[std::atoi(row.substr(0, colon).c_str())] = std::atoi(row.substr(colon + 1).c_str());
	}
	return out;
}

std::string formatSearchInfo(const std::map<int32_t, int32_t>& counts)
{
	std::string out;
	for (const auto& [point, count] : counts)
	{
		if (!out.empty())
			out += ',';
		out += std::to_string(point) + ":" + std::to_string(count);
	}
	return out;
}

const ::RaidMissionPointMst* missionPoint(const std::string& missionId, int32_t point)
{
	for (const auto& row : theServer()->cache().raidMissionPointMst())
		if (std::to_string(row.mission_id) == missionId && row.point_id == point)
			return &row;
	return nullptr;
}

/*!
* Roll one harvest from "weight:itemId:num,..." (weights out of 10000,
* INFERRED).  Weights short of 10000 leave room for finding nothing.
*/
std::optional<std::pair<std::string, int32_t>> rollHarvest(const ::RaidMissionPointMst& point)
{
	static thread_local std::mt19937 rng{ std::random_device{}() };
	const auto roll = std::uniform_int_distribution<int32_t>(0, 9999)(rng);
	int32_t cumulative = 0;
	for (const auto& entry : point.item_info)
	{
		std::stringstream fields(entry);
		std::string weight, item, num;
		if (!std::getline(fields, weight, ':') || !std::getline(fields, item, ':') || !std::getline(fields, num, ':'))
			continue;
		cumulative += std::atoi(weight.c_str());
		if (roll < cumulative)
			return std::make_pair(item, std::max(1, std::atoi(num.c_str())));
	}
	return std::nullopt;
}

/*!
* Put `num` of `itemId` in the bag the way RaidUtils::incBagItem does (top up
* stacks to max_stack, then new stacks), so both sides agree on the stacks.
* False, and nothing stored, when that would need more than kBagCapacity
* stacks.
*/
drogon::Task<bool> addToBag(const db::Database tx, const RaidRun run, const std::string userId,
	const std::string itemId, const int32_t num)
{
	const auto before = co_await loadBag(tx, run.id, userId);
	const auto after = bagAdd(before, std::atoi(itemId.c_str()), num);
	// A set edit may legitimately leave more than kBagCapacity stacks (it has
	// no cap): only a find that opens stacks past the cap is turned away.
	if (bagActiveStacks(after) > std::max(kBagCapacity, bagActiveStacks(before)))
		co_return false;
	co_await storeBag(tx, run.id, userId, after);
	co_return true;
}

/*! Take one `itemId` out of the bag (its smallest stack, as RaidUtils::decBagItem does); false when there is none. */
drogon::Task<bool> takeFromBag(const db::Database tx, const RaidRun run, const std::string userId,
	const std::string itemId)
{
	const auto after = bagTake(co_await loadBag(tx, run.id, userId), std::atoi(itemId.c_str()), 1);
	if (!after)
		co_return false;
	co_await storeBag(tx, run.id, userId, *after);
	co_return true;
}

/*!
* Take one `itemId` off the raid item set (the equipped list, Y5k8LJs9 = 0).
* The set left the warehouse when it was filled (RoonItemEdit), so a use
* only shrinks it; false when no slot holds the item.
*/
drogon::Task<bool> takeFromSet(const db::Database tx, const std::string userId, const std::string itemId)
{
	auto set = co_await loadSet(tx, userId, kItemSetKind);
	const auto id = std::atoi(itemId.c_str());
	for (auto& slot : set)
	{
		if (slot.itemId != id || slot.num <= 0)
			continue;
		slot.num -= 1;
		co_await storeSet(tx, userId, kItemSetKind, set);
		co_return true;
	}
	co_return false;
}

/*! ItemMst skill type 905: the raid Teleport (item 70200). */
bool isTeleportItem(const std::string& itemId)
{
	for (const auto& item : theServer()->cache().itemMst())
		if (std::to_string(item.id) == itemId)
			return item.skill_type == "905";
	return false;
}

drogon::Task<HandleResult> fieldReply(const char* name, const std::string userId, const RaidRun run,
	const bool withSet = false)
{
	co_return reply(name, co_await fieldResp(theDb(), userId, run, withSet));
}

/*! Shared body of RaidMissionUseItem / RaidLimitedUseItem (RAIDS_ROUND2_CORE 4.9). */
drogon::Task<HandleResult> useItem(const char* name, const std::string json)
{
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run || call.run->result != kRunRunning)
		co_return noRun(name, userId);

	const auto itemId = call.req.text("igAhC0R5", "kixHbe54");
	// Y5k8LJs9 = 1: the item came from the mission bag (the server's to
	// decrement); 0: from the equipped list, which the item band stores.
	// Hoisted out of the condition: GCC 13 ICEs (gimple-low lower_stmt) on a
	// co_await short-circuited behind a temporary.
	const bool fromBag = call.req.text("igAhC0R5", "Y5k8LJs9") == "1";
	{
		auto transaction = co_await gme::newTransaction(theDb());
		bool taken = false;
		if (fromBag)
			taken = co_await takeFromBag(transaction, *call.run, userId, itemId);
		else
			taken = co_await takeFromSet(transaction, userId, itemId);
		if (taken)
		{
			co_await bumpStat(transaction, userId, stat::kRaidItemUse);
			// No co_await inside a condition: GCC 13 ICEs on it (see below).
			taken = co_await gme::CommitTransaction(std::move(transaction));
		}
		else
		{
			transaction->rollback();
		}
		if (!taken)
		{
			// An item from the set the server does not hold (a set filled
			// before RoonItemEdit stored anything) is still let through: the
			// client already applied it, and refusing would only re-init.
			if (fromBag)
				co_return HandleResult::refuseToRetry("You have none of that item left.",
					std::string(name) + ": " + itemId + " not in the bag of " + userId);
			LOG_WARN << name << ": " << userId << " used set item " << itemId << " the server does not hold";
		}
	}

	// The effect (heal, revive) is applied client side; the party rows it
	// sends are the result.
	co_await saveParty(theDb(), *call.run, userId, readPartyRows(call.req));
	// A Teleport (ItemMst skill 905) moves the avatar through SendPointNum on
	// the next poll, to any point of the map (RaidFieldScene::itemUseEff):
	// arm that one move so the route check lets it through.
	if (isTeleportItem(itemId))
		co_await theDb()->execSqlCoro(
			"UPDATE raid_mission_user SET warp_pending = 1 WHERE room_mission_id = $1 AND user_id = $2;",
			call.run->id, userId);
	LOG_INFO << name << ": " << userId << " used item " << itemId << " in run " << call.run->id;
	co_return co_await fieldReply(name, userId, *call.run, true);
}

/*!
* The helper picked for this room, as a {"ZX7gzQ6s": ...} object ("" when
* none).  PlayerParty::entryRaid @0x10C7024 adds the helper from
* RaidInfo::getSelFriendInfo, which lives only in client memory: after an app
* restart a resumed run fought without it.  The FriendInfo reader fills that
* selection wherever the group arrives (QuickStart's echo does the same), so
* MissionStart hands the stored pick back.
*/
drogon::Task<std::string> storedHelper(const std::string roomId, const std::string userId)
{
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT CASE WHEN r.quick_start = 1 THEN r.helper_json ELSE COALESCE(m.helper_json, '') END AS helper"
		" FROM raid_room r LEFT JOIN raid_room_member m ON m.room_id = r.room_id AND m.user_id = $1"
		" WHERE r.room_id = $2;",
		userId, roomId);
	if (rows.empty())
		co_return std::string{};
	const auto json = rows[0]["helper"].as<std::string>();
	if (json.empty() || json == "[]" || json == "[{}]" || (json.front() != '[' && json.front() != '{'))
		co_return std::string{};
	co_return "{\"ZX7gzQ6s\":" + json + "}";
}

/*!
* Why `userId` may not start or join the shared room's quest ("" = may).
* Resuming the member's own live run is always fine.
*/
drogon::Task<std::string> sharedStartRefusal(
	const std::string roomId, const std::string userId, const std::string masterId,
	const bool resume, const std::optional<RunMember> member)
{
	if (member && !member->ended)
		co_return std::string{};
	if (!resume)
	{
		if (userId != masterId)
			co_return userId + " is not the master of room " + roomId;
		// A new run replaces current_room_mission_id: members still on the
		// result screens of the last one would lose their MissionEnd.  The
		// room returns to the lobby (status 0) only once they collected it.
		const auto run = co_await roomRun(theDb(), roomId);
		if (run)
			co_await settleRoomAfterRun(theDb(), roomId, run->id);
		const auto status = co_await theDb()->execSqlCoro(
			"SELECT status FROM raid_room WHERE room_id = $1;", roomId);
		if (!status.empty() && status[0]["status"].as<int32_t>() == kRoomInField)
			co_return "room " + roomId + " still has members on the last quest's results";
		co_return std::string{};
	}
	if ((co_await memberMissionStatus(theDb(), roomId, userId)) != kRunRunning)
		co_return userId + " was not sent into room " + roomId + "'s quest";
	co_return std::string{};
}

} // namespace

// RaidMissionStart (2qDW9NBU) -- RaidFieldReady opens the run, and resends it
// after an app kill while the run is on: that must RESUME (RaidInitScene::
// caseGetRoom @0x19A06C0), never start over or charge anything.
HANDLEF(RaidMissionStart)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;

	auto roomId = call.req.text("8iBuZa3n", "8VYd6xSX");
	if (roomId.empty())
		roomId = (co_await loadRaidUser(theDb(), userId)).login_room_id;
	const auto room = co_await roomResp(theDb(), userId, roomId);
	// roomResp still answers a quick room for a short grace after its
	// MissionEnd closed it; that room must never start another run.
	const auto open = co_await theDb()->execSqlCoro(
		"SELECT 1 FROM raid_room WHERE room_id = $1 AND status != $2;", roomId, kRoomClosed);
	if (room.rooms.empty() || open.empty() || !(co_await isRoomMember(theDb(), roomId, userId)))
		co_return HandleResult::refuseToRetry("This raid room has closed.",
			"RaidMissionStart: room '" + roomId + "' is not live for " + userId);

	const auto mission = findMission(room.rooms.front().mission_id);
	if (!mission)
		co_return HandleResult::refuseToRetry("That raid quest does not exist.",
			"RaidMissionStart: room " + roomId + " has unknown mission " + room.rooms.front().mission_id);

	auto run = co_await roomRun(theDb(), roomId);
	std::optional<RunMember> member;
	if (run)
		member = co_await loadMember(theDb(), run->id, userId);
	if (run && member && member->ended && run->result == kRunRunning)
		co_return HandleResult::refuseToRetry("You have left this raid.",
			"RaidMissionStart: " + userId + " already exited run " + std::to_string(run->id));

	const bool resume = run && (run->result == kRunRunning || (member && !member->ended));
	const auto& roomRow = room.rooms.front();
	const bool shared = roomRow.quick_start_flg != 1;
	if (shared)
	{
		// MULTIPLAYER 5.2: only the master sets a shared room out; anyone else
		// joins a running quest only once their member row says 1 (the
		// master took them along ready, or ReadyUp 2 let them in midway).
		const auto why = co_await sharedStartRefusal(roomId, userId, roomRow.master_user_id, resume, member);
		if (!why.empty())
			co_return HandleResult::refuseToRetry("You cannot join this Quest.", "RaidMissionStart: " + why);
	}
	if (!member || member->ended || !resume)
	{
		// One transaction (it holds the only connection): a retried start
		// that overlaps the first finds the run the first one opened and
		// joins it instead of opening a second.
		auto transaction = co_await gme::newTransaction(theDb());
		if (!resume)
		{
			const auto opened = co_await roomRun(transaction, roomId);
			if (opened && opened->result == kRunRunning)
				run = opened;
			else
				run = co_await startRun(transaction, roomId, *mission);
			if (shared && !(opened && opened->result == kRunRunning))
			{
				const auto sent = co_await sendReadyMembers(transaction, roomId, userId);
				LOG_INFO << "RaidMissionStart: master " << userId << " set out with " << sent << " ready member(s)";
			}
		}
		const auto joined = co_await loadMember(transaction, run->id, userId);
		if (!joined || joined->ended)
		{
			// Setting out costs one raid orb (RaidUserInfo C45mRxnZ); a resume
			// never does.  The client checks first (questStartCheck offers the
			// orb shop at 0), so this only stops a stale or forged start.
			if (!(co_await spendAube(transaction, userId)))
			{
				// A member sent into a running quest goes back to the lobby, or
				// every poll would send them to RaidFieldReady and this refusal
				// again.  A master's start just rolls back (no run was opened).
				if (resume)
				{
					co_await transaction->execSqlCoro(
						"UPDATE raid_room_member SET ready = 0, mission_status = 0 WHERE room_id = $1 AND user_id = $2;",
						roomId, userId);
					co_await gme::CommitTransaction(std::move(transaction));
				}
				else
				{
					transaction->rollback();
				}
				co_return HandleResult::refuseToRetry("You have no Raid Orbs left.",
					"RaidMissionStart: " + userId + " has no aube for room " + roomId);
			}
			co_await joinRun(transaction, *run, userId, readPartyRows(call.req), *mission);
			co_await bumpStat(transaction, userId, stat::kChallenge);
		}
		if (!(co_await gme::CommitTransaction(std::move(transaction))))
			co_return HandleResult::refuseToRetry("The raid could not be started. Please try again.",
				"RaidMissionStart: start of room " + roomId + " did not commit for " + userId);
	}

	LOG_INFO << "RaidMissionStart: " << userId << (resume ? " resumed" : " started") << " run " << run->id
		<< " of mission " << run->missionId << " in room " << roomId;
	// The raid's own unit list (raidUnitsGroup): battles look the party up there.
	const auto units = co_await raidUnitsGroup(call.identity, run->id);
	const auto helper = co_await storedHelper(roomId, userId);
	co_return reply("RaidMissionStart", co_await fieldResp(theDb(), userId, *run, true), mergeJson(units.empty() ? "{}" : units, helper));
}

// RaidMissionInfo (BqF2x3EC) -- the field heartbeat, every RaidWorldMst
// interval (4 s).  A pending walk rides along as 81B4TvEK.pjX27bKB; an
// illegal one is not refused, the reply just snaps the avatar back.
HANDLEF(RaidMissionInfo)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	if (!call.run)
		co_return noRun("RaidMissionInfo", call.identity.userId);

	const auto target = call.req.text("81B4TvEK", "pjX27bKB");
	if (!target.empty())
		co_await moveTo(theDb(), *call.run, call.identity.userId, std::atoi(target.c_str()));
	co_return co_await fieldReply("RaidMissionInfo", call.identity.userId, *call.run);
}

// RaidMissionInfoLight (Argz1U6B) -- the same cadence from overlays opened
// over the field (items, team, quest info).
HANDLEF(RaidMissionInfoLight)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	if (!call.run)
		co_return noRun("RaidMissionInfoLight", call.identity.userId);
	co_return co_await fieldReply("RaidMissionInfoLight", call.identity.userId, *call.run);
}

// RaidMissionHarvest (9QdRVwB1) -- the pick button.  The scene diffs the
// reply's bag against the one it saved, so the find is simply what the bag
// gained; nothing found is a valid answer (RAID_FIELDPICK_MIS).
HANDLEF(RaidMissionHarvest)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run || call.run->result != kRunRunning)
		co_return noRun("RaidMissionHarvest", userId);

	const auto asked = call.req.text("81B4TvEK", "zHs17TE2");
	const auto point = std::atoi(asked.substr(0, asked.find(':')).c_str());
	// The pick button is drawn the moment the avatar arrives, before the
	// poll that reports the walk: the point may be one step ahead.
	const auto member = co_await catchUpMove(theDb(), *call.run, userId, point);
	const auto* table = missionPoint(call.run->missionId, point);
	auto counts = parseSearchInfo(member ? member->searchInfo : "");
	if (!member || member->nowPoint != point || table == nullptr || counts[point] >= table->count)
		co_return HandleResult::refuseToRetry("There is nothing more to find here.",
			"RaidMissionHarvest: " + userId + " cannot harvest point " + std::to_string(point));

	counts[point] += 1;
	// The check above read a snapshot: the write only lands if search_info is
	// still what was read, so a burst of parallel harvests cannot each pass
	// the limit (the losers are refused like an exhausted point).
	const auto claimed = co_await theDb()->execSqlCoro(
		"UPDATE raid_mission_user SET search_info = $1 WHERE room_mission_id = $2 AND user_id = $3"
		" AND search_info = $4;",
		formatSearchInfo(counts), call.run->id, userId, member->searchInfo);
	if (claimed.affectedRows() == 0)
		co_return HandleResult::refuseToRetry("There is nothing more to find here.",
			"RaidMissionHarvest: " + userId + " raced another harvest at point " + std::to_string(point));
	const auto found = rollHarvest(*table);
	if (found)
	{
		auto transaction = co_await gme::newTransaction(theDb());
		const bool stored = co_await addToBag(transaction, *call.run, userId, found->first, found->second);
		if (stored)
			co_await bumpStat(transaction, userId, stat::kItemHarvest, found->second);
		if (!stored)
			LOG_INFO << "RaidMissionHarvest: bag of " << userId << " is full";
		if (!(co_await gme::CommitTransaction(std::move(transaction))))
			LOG_ERROR << "RaidMissionHarvest: find of " << userId << " did not commit";
	}
	LOG_INFO << "RaidMissionHarvest: " << userId << " searched point " << point << " ("
		<< counts[point] << "/" << table->count << "): "
		<< (found ? found->first + " x" + std::to_string(found->second) : std::string("nothing"));
	co_return co_await fieldReply("RaidMissionHarvest", userId, *call.run);
}

// RaidMissionUseItem (o03kCtb1) -- the unit-target item screen.
HANDLEF(RaidMissionUseItem)
{
	(void)session;
	co_return co_await useItem("RaidMissionUseItem", json);
}

// RaidLimitedUseItem (SaN7v13E) -- field-effect items (ItemMst process 902..907).
HANDLEF(RaidLimitedUseItem)
{
	(void)session;
	co_return co_await useItem("RaidLimitedUseItem", json);
}

// RaidMissionExit (a12v0RcA) -- retire.  Forfeits everything: rewards are only
// paid by MissionEnd.  A quick room closes at once (quickStartExit sends no
// request); a shared room's run ends when its last member has left.
HANDLEF(RaidMissionExit)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (call.run)
	{
		// Guarded: a retried exit changes nothing and counts no second escape.
		const auto exited = co_await theDb()->execSqlCoro(
			"UPDATE raid_mission_user SET ended = 1, battle_serial = 0 WHERE room_mission_id = $1 AND user_id = $2"
			" AND ended = 0;",
			call.run->id, userId);
		// Only players still in the room hold the run open.
		const auto left = co_await theDb()->execSqlCoro(
			"SELECT COUNT(*) AS n FROM raid_mission_user u JOIN raid_room_member m ON m.room_id = $1"
			" AND m.user_id = u.user_id AND m.kicked = 0 WHERE u.room_mission_id = $2 AND u.ended = 0;",
			call.run->roomId, call.run->id);
		if (call.run->quick || left[0]["n"].as<int32_t>() == 0)
			co_await finishRun(theDb(), *call.run, kRunLost);
		LOG_INFO << "RaidMissionExit: " << userId << " retired from run " << call.run->id;
		if (!call.run->quick)
		{
			co_await theDb()->execSqlCoro(
				"UPDATE raid_room_member SET mission_status = 0, ready = 0 WHERE room_id = $1 AND user_id = $2;",
				call.run->roomId, userId);
			if (exited.affectedRows() > 0)
				co_await bumpStat(theDb(), userId, stat::kEscapeCnt);
			co_await settleRoomAfterRun(theDb(), call.run->roomId, call.run->id);
			co_return reply("RaidMissionExit", co_await roomResp(theDb(), userId, call.run->roomId));
		}
		co_await leaveRoom(theDb(), userId, call.run->roomId);
	}
	co_return reply("RaidMissionExit", co_await userInfoResp(theDb(), userId));
}

// RaidMissionSelect (8iQL3IKA) -- room path: the master picks the room's
// mission.  Every member's RC must allow it.
HANDLEF(RaidMissionSelect)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	const auto roomId = call.req.text("8iBuZa3n", "8VYd6xSX");
	const auto missionId = call.req.text("8iBuZa3n", "j28VNcUW");
	const auto mission = findMission(missionId);
	const auto room = co_await theDb()->execSqlCoro(
		"SELECT master_user_id, mission_start_flg FROM raid_room WHERE room_id = $1 AND status != $2;",
		roomId, kRoomClosed);
	if (!mission || room.empty() || room[0]["master_user_id"].as<std::string>() != userId
		|| room[0]["mission_start_flg"].as<int32_t>() == 1)
		co_return HandleResult::refuseToRetry("Only the room master can choose the quest.",
			"RaidMissionSelect: " + userId + " may not set mission '" + missionId + "' in room " + roomId);

	const auto members = co_await theDb()->execSqlCoro(
		"SELECT COUNT(*) AS n, COALESCE(SUM(CASE WHEN COALESCE(u.rc, 1) < $1 THEN 1 ELSE 0 END), 0) AS low"
		" FROM raid_room_member m LEFT JOIN raid_user u ON u.user_id = m.user_id"
		" WHERE m.room_id = $2 AND m.kicked = 0;",
		mission->rc_id, roomId);
	if (members[0]["low"].as<int32_t>() > 0)
		co_return HandleResult::refuseToRetry("A member's Raid Class is too low for that quest.",
			"RaidMissionSelect: mission " + missionId + " needs rc " + std::to_string(mission->rc_id));
	if (members[0]["n"].as<int32_t>() > roomCapacity(missionId))
		co_return HandleResult::refuseToRetry("That quest has room for fewer players than are in this Room.",
			"RaidMissionSelect: mission " + missionId + " is too small for room " + roomId);

	co_await theDb()->execSqlCoro(
		"UPDATE raid_room SET mission_id = $1, updated_at = $2 WHERE room_id = $3;", missionId, nowSeconds(), roomId);
	co_await theDb()->execSqlCoro(
		"UPDATE raid_room_member SET ready = 0 WHERE room_id = $1 AND user_id != $2;", roomId, userId);
	LOG_INFO << "RaidMissionSelect: room " << roomId << " now plays mission " << missionId;
	co_return reply("RaidMissionSelect", co_await roomResp(theDb(), userId, roomId));
}
