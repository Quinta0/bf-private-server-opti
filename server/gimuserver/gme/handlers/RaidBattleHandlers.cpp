#include "RaidHandlerSupport.hpp"

#include <gimuserver/gme/common/RaidStats.hpp>

#include <gimuserver/gme/common/Achievements.hpp>
#include <gimuserver/gme/common/DailyChallenges.hpp>
#include <gimuserver/gme/common/MissionRuns.hpp>
#include <gimuserver/gme/common/RaidRewards.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <cstdlib>

// Classic Raid round 2 -- battles and the mission end (RAIDS_ROUND2_CORE.md
// 4.4 to 4.10): BattleStart / UpdateProgress (every turn) / BattleEnd /
// BattleRestart, and MissionEnd's payout.
//
// Boss HP is driven ONLY by the client's type-1 events, applied at most once
// each (applyEvents), so a turn re-sent after a retry or an app restart never
// counts twice.  Party HP and gauges carry over between battles because the
// server stores what the battle reports and serves it back in wc2DiMY1, which
// PlayerParty::entryRaid @0x10C7024 rebuilds the party from.

using namespace gme::raid;
using namespace gme::raid::handlers;
using gme::UserIdentity;

namespace
{

struct BattlePlan
{
	std::string bossEncId;   // mission boss id, empty for a route battle
	int32_t point = 0;
};

/*! The battle content for a plan, at the run's current boss HP. */
drogon::Task<std::optional<::RaidBattleContentResp>> buildContent(
	const RaidRun run, const ::RaidMissionMst mission, const BattlePlan plan, const uint32_t serial)
{
	if (plan.bossEncId.empty())
		co_return routeBattle(mission.id, mission.map_id, plan.point, mission.rc_id, serial);
	for (const auto& boss : co_await loadBosses(theDb(), run.id))
		if (boss.missionBossId == plan.bossEncId)
			co_return bossBattle(mission.id, boss.bossId, mission.rc_id, boss.parts);
	co_return std::nullopt;
}

/*! Field state + battle content + the battle status, as one reply. */
drogon::Task<HandleResult> battleReply(
	const char* name, const UserIdentity identity, const RaidRun run, ::RaidBattleContentResp content,
	const BattlePlan plan, const uint32_t serial, const std::string startInfoEcho)
{
	const auto& userId = identity.userId;
	const auto member = co_await loadMember(theDb(), run.id, userId);
	auto field = co_await fieldResp(theDb(), userId, run, true);
	field.battle_status = std::vector<::RaidBattleStatus>{ {
		std::to_string(serial), std::to_string((member ? member->lastEventId : 0) + 1) } };
	content.mission_num.serial_id = serial;
	content.battle_info = { std::to_string(run.id), plan.point, plan.bossEncId };
	content.battle_skills = battleSkillRows(content.monsters);

	auto extra = serialize(name, content);
	if (!extra)
		co_return HandleResult::error("Serialization error", name);
	// 9Q1Lq5FS goes back exactly as the client built it ({user id, mission}).
	if (!startInfoEcho.empty())
		extra = mergeJson(*extra, "{\"9Q1Lq5FS\":" + startInfoEcho + "}");
	// The battle builds the party from RaidUserUnitInfoList (raidUnitsGroup).
	extra = mergeJson(*extra, co_await raidUnitsGroup(identity, run.id));
	co_return reply(name, field, *extra);
}

/*! RaidMapPointMst AZFM14In = 1 on the run's map. */
bool isCampPoint(const RaidRun& run, int32_t point)
{
	const auto mission = runMission(run);
	if (!mission)
		return false;
	for (const auto& row : theServer()->cache().raidPointMst())
		if (row.map_id == mission->map_id && row.point_id == point)
			return row.point_type == 1;
	return false;
}

/*! Is `plan` a battle the member may start now?  Empty string = yes, else why not. */
drogon::Task<std::string> battleRefusal(const RaidRun run, const RunMember member, const BattlePlan plan)
{
	if (member.nowPoint != plan.point)
		co_return "not at point " + std::to_string(plan.point) + " (at " + std::to_string(member.nowPoint) + ")";
	// The battle button is only drawn on field points (fleldPointBtnSet
	// @0x198EEC0: a camp shows Rest instead), so a route battle at a camp is
	// a stale or forged request.
	if (plan.bossEncId.empty())
		co_return isCampPoint(run, plan.point) ? "point " + std::to_string(plan.point) + " is a camp" : std::string{};
	for (const auto& boss : co_await loadBosses(theDb(), run.id))
	{
		if (boss.missionBossId != plan.bossEncId)
			continue;
		if (boss.bodyHp() <= 0)
			co_return "boss already down";
		if (boss.point != plan.point)
			co_return "boss is at point " + std::to_string(boss.point);
		co_return std::string{};
	}
	co_return "unknown boss " + plan.bossEncId;
}

/*! "missionBossId:count,..." (RaidMissionUesrInfo 5soj5vGI) with one more battle against `bossId`. */
std::string bumpEncounter(const std::string& csv, const std::string& bossId)
{
	std::string out;
	bool found = false;
	size_t start = 0;
	while (start < csv.size())
	{
		const auto end = std::min(csv.find(',', start), csv.size());
		const auto entry = csv.substr(start, end - start);
		const auto colon = entry.find(':');
		start = end + 1;
		if (colon == std::string::npos)
			continue;
		auto count = std::atoll(entry.c_str() + colon + 1);
		if (entry.compare(0, colon, bossId) == 0)
		{
			++count;
			found = true;
		}
		out += (out.empty() ? "" : ",") + entry.substr(0, colon) + ":" + std::to_string(count);
	}
	if (!found)
		out += (out.empty() ? "" : ",") + bossId + ":1";
	return out;
}

/*! Banked drop text per member stops growing past this (payDrops pays at most 20 rows anyway). */
constexpr int64_t kMaxBankedDropsChars = 1024;

/*!
* Keep only well-formed "itemId:count" pairs of a client drop list, at most
* 20 of them: the text is client input and is banked on the member row.
*/
std::string sanitizeDrops(const std::string& raw)
{
	std::string out;
	size_t rows = 0;
	size_t start = 0;
	while (start <= raw.size() && rows < 20)
	{
		const auto end = std::min(raw.find(',', start), raw.size());
		const auto entry = raw.substr(start, end - start);
		const auto colon = entry.find(':');
		const auto digits = [](const std::string& text) {
			return !text.empty() && text.size() <= 9
				&& std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
		};
		if (colon != std::string::npos && digits(entry.substr(0, colon)) && digits(entry.substr(colon + 1)))
		{
			out += (out.empty() ? "" : ",") + entry;
			++rows;
		}
		start = end + 1;
	}
	return out;
}

/*! True when every own unit the battle reported is at 0 HP. */
bool partyWiped(const std::vector<PartyRow>& rows)
{
	bool anyOwn = false;
	for (const auto& row : rows)
	{
		if (!row.friendId.empty())
			continue;
		anyOwn = true;
		if (row.hp > 0)
			return false;
	}
	return anyOwn;
}

/*!
* Does this report speak for the party now?  A battle report (yL3Yrf1M sent)
* only while its serial is the member's open battle: a retried or stale
* BattleEnd / UpdateProgress must not roll the stored HP back.  The field
* form (after a field item effect) carries no serial.
*/
bool reportOwnsParty(const FieldCall& call, const std::optional<RunMember>& member)
{
	const auto serial = call.req.text("yL3Yrf1M", "1kisc6IF");
	if (call.req.node("yL3Yrf1M") == nullptr)
		return true;
	return member && member->battleSerial != 0 && serial == std::to_string(member->battleSerial);
}

/*! Shared by UpdateProgress and BattleEnd: events (always acked), and the party when the report owns it. */
drogon::Task<std::vector<::RaidEventAck>> applyBattleReport(const FieldCall& call, const std::optional<RunMember> member)
{
	const auto& userId = call.identity.userId;
	// No named copy of the transaction: CommitTransaction needs the only reference.
	auto transaction = co_await gme::newTransaction(theDb());
	const auto acks = co_await applyEvents(transaction, *call.run, userId, readEvents(call.req));
	// Re-read through the transaction: a retried BattleEnd overlapping the
	// original must not save its (wiped) party after the original closed the
	// battle and restored it.
	const auto current = co_await loadMember(transaction, call.run->id, userId);
	(void)member;
	if (reportOwnsParty(call, current))
		co_await saveParty(transaction, *call.run, userId, readPartyRows(call.req));
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
	{
		// Unacked events stay queued on the client and come back next turn.
		LOG_ERROR << "Raid battle: report of " << userId << " for run " << call.run->id << " did not commit";
		co_return std::vector<::RaidEventAck>{};
	}
	co_return acks;
}

/*! The MissionEnd reply, read through the payout's own transaction. */
drogon::Task<::RaidMissionEndResp> missionEndResp(const db::Database tx, const FieldCall& call, const RaidPayout payout)
{
	const auto& userId = call.identity.userId;
	::RaidMissionEndResp resp{};
	resp.result.first_clear_flg = payout.firstClear ? 1 : 0;
	resp.result.friend_cnt = 0;
	if (call.run->result == kRunWon)
		for (const auto& boss : co_await loadBosses(tx, call.run->id))
			for (const auto& row : theServer()->cache().raidBossMst())
				if (row.id == boss.bossId && boss.bodyHp() <= 0)
					resp.result.boss_finished = row.name;
	// A run that paid nothing still sends one blank row (reward type 0, which
	// no result scene lists): the client keeps the last list it was given, so
	// an empty reply would show the previous run's rewards again (the raid bag
	// has the same rule, RaidItems.cpp).
	resp.rewards = payout.rewards.empty() ? std::vector<::RaidMissionRewardInfo>(1) : payout.rewards;
	resp.rc_up.rc_up_flg = payout.rcUp ? 1 : 0;
	resp.rc_up.rc_before = payout.rcBefore;
	resp.room_mission.mission_id = call.run->missionId;
	resp.room_mission.result = call.run->result;
	const auto room = co_await roomResp(tx, userId, call.run->roomId);
	resp.rooms = room.rooms;
	resp.members = room.members;
	const auto raidUser = co_await userInfoResp(tx, userId);
	resp.user = raidUser.user;
	resp.permit = raidUser.permit;
	resp.clear = raidUser.clear;
	resp.team_info = std::move((co_await gme::getTeamInfo(tx, call.identity)).nonEmpty());
	co_return resp;
}

/*!
* The finished, uncollected run of the caller's room that MissionEnd names
* (ja39HMLq) when it is not the room's current run any more; nullopt when it
* is not theirs to collect.
*/
drogon::Task<std::optional<RaidRun>> olderRunToCollect(const FieldCall& call)
{
	const auto named = RaidRequest::numberOf(call.req.node("g97TWISb"), "ja39HMLq");
	if (named <= 0)
		co_return std::nullopt;
	const auto run = co_await loadRun(theDb(), named);
	if (!run || run->result == kRunRunning || !(co_await isRoomMember(theDb(), run->roomId, call.identity.userId)))
		co_return std::nullopt;
	const auto member = co_await loadMember(theDb(), run->id, call.identity.userId);
	if (!member || member->ended)
		co_return std::nullopt;
	LOG_INFO << "RaidMissionEnd: " << call.identity.userId << " collects earlier run " << run->id
		<< " of room " << run->roomId;
	co_return run;
}

/*!
* The battle records of a closed battle (zS9s5bEI, MULTIPLAYER 4.7).  Vanity
* numbers the client reports; clamped so a forged report cannot mint huge
* values, never trusted for anything else.
*/
drogon::Task<void> recordBattleStats(const std::string userId, const RaidRequest& req)
{
	constexpr int64_t kStatCap = 1'000'000'000'000LL;
	const auto value = [&](const char* key) {
		return std::clamp<int64_t>(RaidRequest::numberOf(req.node("zS9s5bEI"), key), 0, kStatCap);
	};
	co_await bumpStat(theDb(), userId, stat::kCrystalB, value("hoG2ieT5"));
	co_await bumpStat(theDb(), userId, stat::kCrystalH, value("6PLsn8xo"));
	co_await raiseStat(theDb(), userId, stat::kCrystalBMax, value("hoG2ieT5"));
	co_await raiseStat(theDb(), userId, stat::kCrystalHMax, value("6PLsn8xo"));
	co_await raiseStat(theDb(), userId, stat::kBattleTurnMaxDamage, value("5NRJQ1LU"));
	co_await raiseStat(theDb(), userId, stat::kBattleTurnMaxSpark, value("XP06YWdT"));
	co_await bumpStat(theDb(), userId, stat::kSparkCnt, value("U8uZLA34"));
	co_await bumpStat(theDb(), userId, stat::kSkillCnt, value("rZQJF5G9"));
	co_await bumpStat(theDb(), userId, stat::kBattleDamage, value("kqI45VU1"));
	co_await bumpStat(theDb(), userId, stat::kEscapeCnt, value("E5S7XgR6"));
	co_await bumpStat(theDb(), userId, stat::kOverkillCnt, value("Wnc6A31E"));
}

/*!
* The member's raid records for a collected run (MULTIPLAYER 4.7): wins by
* RC, solo vs co-op, play time and the fastest clear.  Challenge is counted
* at MissionStart, kill credits where the boss falls.
*/
drogon::Task<void> recordRunStats(const db::Database tx, const std::string userId, const RaidRun run)
{
	if (run.result != kRunWon)
		co_return;
	const auto times = co_await tx->execSqlCoro(
		"SELECT started_at, ended_at, (SELECT COUNT(*) FROM raid_mission_user u WHERE u.room_mission_id = $1)"
		" AS members FROM raid_room_mission WHERE room_mission_id = $2;",
		run.id, run.id);
	if (times.empty())
		co_return;
	const auto playtime = std::max<int64_t>(0,
		times[0]["ended_at"].as<int64_t>() - times[0]["started_at"].as<int64_t>());
	co_await bumpStat(tx, userId, stat::kWin);
	co_await bumpStat(tx, userId, times[0]["members"].as<int32_t>() > 1 ? stat::kCooperationWin : stat::kSingleWin);
	if (const auto mission = runMission(run); mission)
		if (const auto* rcWin = stat::rcWin(mission->rc_id); rcWin != nullptr)
			co_await bumpStat(tx, userId, rcWin);
	co_await bumpStat(tx, userId, stat::kPlaytime, playtime);
	if (playtime > 0)
		co_await lowerStat(tx, userId, stat::kFastestPlaytime, playtime);
}

} // namespace

// RaidMissionBattleStart (8UiQj0uF) -- the battle button (route battle at the
// current point) or a boss encounter (SP29fLtH names the boss standing there).
HANDLEF(RaidMissionBattleStart)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run)
		co_return noRun("RaidMissionBattleStart", userId);
	const auto mission = runMission(*call.run);
	// Over already: the client skips the battle and goes to BattleEnd
	// (RaidBattleStartScene::nextScene @0x19474C0 on Result 2..4).
	if (call.run->result != kRunRunning)
		co_return reply("RaidMissionBattleStart", co_await fieldResp(theDb(), userId, *call.run));

	const BattlePlan plan{ call.req.text("2cBk5WH8", "SP29fLtH"),
		std::atoi(call.req.text("2cBk5WH8", "qbJcuC60").c_str()) };
	// A boss met on arrival starts its battle before any poll has reported
	// the walk (walkingSet sends nothing; initCheck runs bossCheck at once),
	// so the point named here may be one step ahead: take it as that move.
	const auto member = co_await catchUpMove(theDb(), *call.run, userId, plan.point);
	if (!member || member->ended || !mission)
		co_return noRun("RaidMissionBattleStart", userId);
	if (const auto why = co_await battleRefusal(*call.run, *member, plan); !why.empty())
		co_return HandleResult::refuseToRetry("That battle is no longer available.",
			"RaidMissionBattleStart: " + userId + ": " + why);

	const auto serial = co_await gme::issueMissionSerial(theDb(), call.identity, static_cast<uint32_t>(mission->id));
	auto content = co_await buildContent(*call.run, *mission, plan, serial);
	if (!content)
		co_return HandleResult::refuseToRetry("This battle could not be prepared.",
			"RaidMissionBattleStart: no battle content for run " + std::to_string(call.run->id));

	co_await theDb()->execSqlCoro(
		"UPDATE raid_mission_user SET battle_serial = $1, battle_boss = $2, battle_boss_id = $3, battle_point = $4"
		" WHERE room_mission_id = $5 AND user_id = $6;",
		static_cast<int64_t>(serial), plan.bossEncId.empty() ? 0 : 1, plan.bossEncId, plan.point,
		call.run->id, userId);
	if (!plan.bossEncId.empty())
	{
		const auto seen = co_await theDb()->execSqlCoro(
			"SELECT boss_encounters FROM raid_mission_user WHERE room_mission_id = $1 AND user_id = $2;",
			call.run->id, userId);
		const auto counts = bumpEncounter(seen.empty() ? std::string{} : seen[0]["boss_encounters"].as<std::string>(),
			plan.bossEncId);
		co_await theDb()->execSqlCoro(
			"UPDATE raid_mission_user SET boss_encounters = $1 WHERE room_mission_id = $2 AND user_id = $3;",
			counts, call.run->id, userId);
	}
	LOG_INFO << "RaidMissionBattleStart: " << userId << " opened battle " << serial << " at point " << plan.point
		<< (plan.bossEncId.empty() ? " (route)" : " against boss " + plan.bossEncId);
	co_return co_await battleReply("RaidMissionBattleStart", call.identity, *call.run, std::move(*content), plan,
		serial, call.req.raw("9Q1Lq5FS"));
}

// RaidMissionBattleRestart (La24KGAp) -- RaidBattleRestartScene after an app
// kill mid battle; the battle itself resumes from the client's suspend files.
HANDLEF(RaidMissionBattleRestart)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run)
		co_return noRun("RaidMissionBattleRestart", userId);
	const auto member = co_await loadMember(theDb(), call.run->id, userId);
	const auto mission = runMission(*call.run);
	const auto serial = call.req.text("yL3Yrf1M", "1kisc6IF");
	if (!member || !mission || member->battleSerial == 0 || serial != std::to_string(member->battleSerial))
		co_return HandleResult::refuseToRetry("That battle has already ended.",
			"RaidMissionBattleRestart: serial '" + serial + "' is not the open battle of " + userId);
	if (call.run->result != kRunRunning)
		co_return reply("RaidMissionBattleRestart", co_await fieldResp(theDb(), userId, *call.run));

	const BattlePlan plan{ member->battleBossId, member->battlePoint };
	const auto open = static_cast<uint32_t>(member->battleSerial);
	auto content = co_await buildContent(*call.run, *mission, plan, open);
	if (!content)
		co_return HandleResult::refuseToRetry("This battle could not be prepared.",
			"RaidMissionBattleRestart: no battle content for run " + std::to_string(call.run->id));
	LOG_INFO << "RaidMissionBattleRestart: " << userId << " resumed battle " << open;
	co_return co_await battleReply("RaidMissionBattleRestart", call.identity, *call.run, std::move(*content), plan, open, {});
}

// RaidUpdateProgress (7geDL5Vb) -- every battle turn, and after a field item
// effect.  Acks the events so the client stops re-sending them; Result 2 in
// the reply makes the battle drop every enemy at the end of the turn
// (EnemyParty::exitTurnChild @0x10BF560).
HANDLEF(RaidUpdateProgress)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	if (!call.run)
		co_return noRun("RaidUpdateProgress", call.identity.userId);
	const auto member = co_await loadMember(theDb(), call.run->id, call.identity.userId);
	const auto acks = co_await applyBattleReport(call, member);
	auto field = co_await fieldResp(theDb(), call.identity.userId, *call.run);
	if (!acks.empty())
		field.acks = acks;
	co_return reply("RaidUpdateProgress", field);
}

// RaidMissionBattleEnd (5Ko2BjPA) -- back to the field.  A wipe costs a heart
// and the party is restored (RAIDS_ROUND2_CORE 4.6: a design choice; the
// original's camp return is not decoded).  Win/loss is NOT read from
// zS9s5bEI.M4NAYyc6, whose 2/3 meaning is unknown, but from the party HP.
HANDLEF(RaidMissionBattleEnd)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run)
		co_return noRun("RaidMissionBattleEnd", userId);
	const auto member = co_await loadMember(theDb(), call.run->id, userId);
	const auto serial = call.req.text("yL3Yrf1M", "1kisc6IF");
	const auto acks = co_await applyBattleReport(call, member);

	// A duplicate BattleEnd (a retried request) finds no open battle: it was
	// acked above and changes nothing else.
	if (member && member->battleSerial != 0 && serial == std::to_string(member->battleSerial))
	{
		const auto rows = readPartyRows(call.req);
		const bool wiped = partyWiped(rows);
		// Drops are "itemId:count,..." (BattleRewardList::getItemCsv @0x10DFA24),
		// banked comma-joined and paid only by MissionEnd (RaidRewards).
		const auto drops = sanitizeDrops(call.req.text("zS9s5bEI", "4T0Q2Bh5"));
		// The serial guard makes the close happen once even when a retried
		// BattleEnd overlaps the original: only one of them finds it open.
		const auto closed = co_await theDb()->execSqlCoro(
			"UPDATE raid_mission_user SET battle_serial = 0, battle_boss = 0, battle_boss_id = '',"
			" player_rest = MAX(player_rest - $1, 0),"
			" drops = CASE WHEN $2 = '' OR LENGTH(drops) > $3 THEN drops WHEN drops = '' THEN $4"
			" ELSE drops || ',' || $5 END"
			" WHERE room_mission_id = $6 AND user_id = $7 AND battle_serial = $8;",
			wiped ? 1 : 0, drops, kMaxBankedDropsChars, drops, drops, call.run->id, userId, member->battleSerial);
		if (closed.affectedRows() > 0 && member->battleBoss)
			co_await theDb()->execSqlCoro(
				"UPDATE raid_mission_user SET boss_escape = 1 WHERE room_mission_id = $1 AND user_id = $2;",
				call.run->id, userId);
		if (wiped && closed.affectedRows() > 0)
			co_await theDb()->execSqlCoro(
				"UPDATE raid_mission_party SET now_hp = max_hp, bb_gauge = 0, sb_gauge = 0"
				" WHERE room_mission_id = $1 AND user_id = $2;",
				call.run->id, userId);
		if (closed.affectedRows() > 0)
		{
			// Once per battle (the serial guard above): the room's kill tally
			// for type-2 clear conditions, and the member's battle records.
			co_await recordKills(theDb(), *call.run, userId, call.req.text("zS9s5bEI", "6RxkwBv7"));
			co_await recordBattleStats(userId, call.req);
		}
		LOG_INFO << "RaidMissionBattleEnd: " << userId << " closed battle " << serial
			<< (wiped ? ": party wiped, one heart spent" : "");
	}

	auto field = co_await fieldResp(theDb(), userId, *call.run);
	if (!acks.empty())
		field.acks = acks;
	co_return reply("RaidMissionBattleEnd", field);
}

// RaidMissionEnd (vgin9Mf1) -- the result scene collects the run once per
// member.  Idempotent: a second MissionEnd returns the stored reply and pays
// nothing.  A quick room is retired here (quickStartExit sends no request).
HANDLEF(RaidMissionEnd)
{
	(void)session;
	auto call = co_await openFieldCall(json);
	const auto& userId = call.identity.userId;
	if (!call.run)
	{
		// Collected already and the quick room retired: answer from the
		// stored reply of the run the request names.
		const auto stored = co_await theDb()->execSqlCoro(
			"SELECT result_json FROM raid_mission_user WHERE room_mission_id = $1 AND user_id = $2 AND ended = 1;",
			call.req.text("g97TWISb", "ja39HMLq"), userId);
		if (!stored.empty() && !stored[0]["result_json"].as<std::string>().empty())
			co_return HandleResult::success(co_await gme::withCurrentBadge(
				theDb(), call.identity, stored[0]["result_json"].as<std::string>()));
		// A member who was away when the room moved on still holds that
		// run's result (memberRow names it): collect it now.
		call.run = co_await olderRunToCollect(call);
		if (!call.run)
			co_return noRun("RaidMissionEnd", userId);
	}
	const auto member = co_await loadMember(theDb(), call.run->id, userId);
	if (!member)
		co_return noRun("RaidMissionEnd", userId);
	if (member->ended)
	{
		// Collected (or exited) before: never pay twice.
		if (!member->resultJson.empty())
			co_return HandleResult::success(
				co_await gme::withCurrentBadge(theDb(), call.identity, member->resultJson));
		co_return noRun("RaidMissionEnd", userId);
	}
	if (call.run->result == kRunRunning)
		co_return HandleResult::refuseToRetry("This raid is still under way.",
			"RaidMissionEnd: run " + std::to_string(call.run->id) + " has not finished");

	// One transaction for the payout AND the stored reply: a retried
	// MissionEnd must find either nothing paid or the reply that says what was.
	// No named copy of the transaction: CommitTransaction needs the only reference.
	auto transaction = co_await gme::newTransaction(theDb());
	// Claim first: only the request that flips `ended` pays.  An overlapping
	// retry finds it taken and answers with the stored reply instead.
	const auto claim = co_await transaction->execSqlCoro(
		"UPDATE raid_mission_user SET ended = 1, battle_serial = 0 WHERE room_mission_id = $1 AND user_id = $2"
		" AND ended = 0;",
		call.run->id, userId);
	if (claim.affectedRows() == 0)
	{
		// Read through the transaction: it holds the only connection until it
		// is destroyed, so a theDb() query here would never run.
		const auto again = co_await loadMember(transaction, call.run->id, userId);
		const auto stored = (again && !again->resultJson.empty())
			? co_await gme::withCurrentBadge(transaction, call.identity, again->resultJson)
			: std::string{};
		transaction->rollback();
		if (!stored.empty())
			co_return HandleResult::success(stored);
		co_return noRun("RaidMissionEnd", userId);
	}
	const auto payout = co_await payRun(transaction, call.identity, *call.run);
	co_await transaction->execSqlCoro(
		"UPDATE raid_room_member SET mission_status = 0 WHERE room_id = $1 AND user_id = $2;",
		call.run->roomId, userId);
	co_await recordRunStats(transaction, userId, *call.run);
	// "Clear N Raid Quests": a won run, once per member (the claim above).
	if (call.run->result == kRunWon)
	{
		gme::challenge::Progress cleared{};
		cleared.raidClears = 1;
		co_await gme::advanceDailyChallenges(transaction, call.identity, cleared);
	}
	if (call.run->quick)
		co_await retireQuickRoom(transaction, call.run->roomId);
	else
		co_await settleRoomAfterRun(transaction, call.run->roomId, call.run->id);

	auto resp = co_await missionEndResp(transaction, call, payout);
	const auto buffer = serialize("RaidMissionEnd", resp);
	if (!buffer)
	{
		transaction->rollback();
		co_return HandleResult::error("Serialization error", "RaidMissionEnd");
	}
	co_await transaction->execSqlCoro(
		"UPDATE raid_mission_user SET result_json = $1 WHERE room_mission_id = $2 AND user_id = $3;",
		*buffer, call.run->id, userId);
	// THE BADGES, on the live reply only: a "Clear N Raid Quests" Challenge
	// this run finished lights Missions > Challenges at once.  The stored copy
	// above leaves them out, because a retry replaying this count would
	// re-light a challenge claimed since; retries get a fresh one instead
	// (gme::withCurrentBadge).
	resp.badge = co_await gme::achievementBadge(transaction, call.identity);
	const auto live = resp.badge ? serialize("RaidMissionEnd", resp) : std::nullopt;
	if (!(co_await gme::CommitTransaction(std::move(transaction))))
		co_return HandleResult::refuseToRetry("The raid result could not be saved. Please try again.",
			"RaidMissionEnd: payout of run " + std::to_string(call.run->id) + " did not commit");
	LOG_INFO << "RaidMissionEnd: " << userId << " collected run " << call.run->id << " (result "
		<< call.run->result << ", " << payout.rewards.size() << " reward row(s)"
		<< (payout.firstClear ? ", first clear" : "") << (payout.rcUp ? ", RC up" : "") << ")";
	co_return HandleResult::success(live ? *live : *buffer);
}
