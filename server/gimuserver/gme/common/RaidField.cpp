#include "App.hpp"

#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidItems.hpp>
#include <gimuserver/gme/common/RaidStats.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace gme::raid
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (see Raids.cpp): every statement
// below numbers its $N left to right and never repeats one.

namespace
{

std::vector<std::string> split(const std::string& text, char separator)
{
	std::vector<std::string> out;
	std::stringstream stream(text);
	std::string item;
	while (std::getline(stream, item, separator))
		if (!item.empty())
			out.push_back(item);
	return out;
}

int64_t toInt(const std::string& text, int64_t fallback = 0)
{
	if (text.empty())
		return fallback;
	char* end = nullptr;
	const auto value = std::strtoll(text.c_str(), &end, 10);
	return end == text.c_str() ? fallback : value;
}

/*! The first camp of the mission's map: where the party sets out from (RAIDS_ROUND2_CORE 5.1). */
int32_t startPoint(int32_t mapId)
{
	int32_t camp = -1;
	int32_t lowest = -1;
	for (const auto& point : theServer()->cache().raidPointMst())
	{
		if (point.map_id != mapId)
			continue;
		if (lowest < 0 || point.point_id < lowest)
			lowest = point.point_id;
		if (point.point_type == 1 && (camp < 0 || point.point_id < camp))
			camp = point.point_id;
	}
	return camp >= 0 ? camp : std::max(lowest, 0);
}

bool isMapPoint(int32_t mapId, int32_t point)
{
	for (const auto& row : theServer()->cache().raidPointMst())
		if (row.map_id == mapId && row.point_id == point)
			return true;
	return false;
}

bool routeExists(int32_t missionId, int32_t from, int32_t to)
{
	for (const auto& route : theServer()->cache().raidUserRouteMst())
		if (route.mission_id == missionId && route.unk1 == from && route.unk2 == to)
			return true;
	return false;
}

/*!
* Hops along the mission's walkable routes from `from` to `to`, or -1 when no
* path exists.  The client reports a walk only with its next 4 s poll
* (RaidMissionInfo carries the latest SendPointNum, and nothing goes out while
* the avatar walks), so a player who taps the next Move before that poll
* reports a point two or more hops from the stored one.  Seen on prod
* ("1 -> 5" on 3011, "7 -> 3" on 5008); on 4011 the player reached the boss at
* point 11 while the server still had them at 2, so the boss battle was
* refused as "not at point" (ticket #125).
*/
int32_t routeHops(int32_t missionId, int32_t from, int32_t to)
{
	std::map<int32_t, std::set<int32_t>> next;
	for (const auto& route : theServer()->cache().raidUserRouteMst())
		if (route.mission_id == missionId)
			next[route.unk1].insert(route.unk2);
	std::map<int32_t, int32_t> hops{ { from, 0 } };
	std::vector<int32_t> queue{ from };
	for (size_t i = 0; i < queue.size(); ++i)
	{
		const auto at = queue[i];
		if (at == to)
			return hops[at];
		for (const auto step : next[at])
			if (hops.emplace(step, hops[at] + 1).second)
				queue.push_back(step);
	}
	return -1;
}

/*! One RaidMisCondClearMst target: "type:id:count". */
struct ClearTarget
{
	std::string type;
	std::string id;
	int64_t count = 0;
};

std::vector<ClearTarget> clearTargets(const std::string& missionId)
{
	std::vector<ClearTarget> out;
	for (const auto& cond : theServer()->cache().raidMissionClearCndMst())
	{
		if (std::to_string(cond.mission_id) != missionId)
			continue;
		for (const auto& entry : cond.clear_id)
		{
			const auto fields = split(entry, ':');
			if (fields.size() >= 2 && (fields[0] == "1" || fields[0] == "2"))
				out.push_back({ fields[0], fields[1], fields.size() >= 3 ? toInt(fields[2]) : 0 });
		}
	}
	return out;
}

/*!
* RaidMisCondClearMst: type 1 "1:<missionBossId>:0" (that boss down) and
* type 2 "2:<monsterId>:<count>" (that many of a monster defeated by the
* room, e.g. mission 2003 "2:213101:5").  Every listed target must be met; a
* mission with no targets is won when any main body is down.
*/
/*! Tally key that counts every defeated enemy of the run. */
constexpr const char* kAnyKill = "*";

/*!
* Whether a type-2 target names a monster this server can put in a battle.
* The shipped targets (213101.., 87001000) name raid field monsters for which
* no MonsterMst row survives, so no battle ever reports them; RaidBossPartsMst
* monsters are the only raid monsters the server fields.  A target it cannot
* field counts every defeated enemy instead, so "defeat 5 Granomos" plays as
* "defeat 5 enemies" rather than being unwinnable.
*/
bool canField(const std::string& monsterId)
{
	static const auto ids = [] {
		std::set<std::string> out;
		for (const auto& part : theServer()->cache().raidBossPartsMst())
			out.insert(std::to_string(part.monster_id));
		return out;
	}();
	return ids.contains(monsterId);
}

bool runIsWon(const std::string& missionId, const std::vector<RunBoss>& bosses,
	const std::map<std::string, int64_t>& kills)
{
	const auto targets = clearTargets(missionId);
	bool anyDown = false;
	for (const auto& boss : bosses)
		anyDown = anyDown || boss.bodyHp() <= 0;
	if (targets.empty())
		return anyDown;
	for (const auto& target : targets)
	{
		if (target.type == "2")
		{
			const auto killed = kills.find(canField(target.id) ? target.id : std::string(kAnyKill));
			if (killed == kills.end() || killed->second < std::max<int64_t>(1, target.count))
				return false;
			continue;
		}
		const auto boss = std::find_if(bosses.begin(), bosses.end(),
			[&](const RunBoss& b) { return b.missionBossId == target.id; });
		if (boss != bosses.end() && boss->bodyHp() > 0)
			return false;
	}
	return true;
}

drogon::Task<std::map<std::string, int64_t>> loadKills(const db::Database database, const int64_t runId)
{
	std::map<std::string, int64_t> out;
	const auto rows = co_await database->execSqlCoro(
		"SELECT monster_id, num FROM raid_mission_kill WHERE room_mission_id = $1;", runId);
	for (const auto& row : rows)
		out[row["monster_id"].as<std::string>()] = row["num"].as<int64_t>();
	co_return out;
}

RaidRun runFromRow(const drogon::orm::Row& row)
{
	RaidRun run{};
	run.roomId = std::to_string(row["room_id"].as<int64_t>());
	run.id = row["room_mission_id"].as<int64_t>();
	run.missionId = row["mission_id"].as<std::string>();
	run.result = row["result"].as<int32_t>();
	run.startedAt = row["started_at"].as<int64_t>();
	run.timeLimit = row["time_limit"].as<int32_t>();
	run.quick = row["quick_start"].as<int32_t>() == 1;
	return run;
}

/*! Apply one boss-damage event: "bossEncId:monsterId:damage,..." (RAIDS_ROUND2_CORE 3.1). */
void applyDamage(
	std::vector<RunBoss>& bosses, const std::string& fighting, const std::string& params, std::set<size_t>& touched)
{
	for (const auto& entry : split(params, ','))
	{
		const auto fields = split(entry, ':');
		if (fields.size() < 3)
			continue;
		const auto damage = std::max<int64_t>(0, toInt(fields[2]));
		if (fields[0] != fighting)
			continue;
		for (size_t i = 0; i < bosses.size(); ++i)
		{
			if (bosses[i].missionBossId != fields[0])
				continue;
			for (auto& part : bosses[i].parts)
			{
				if (std::to_string(part.monsterId) != fields[1])
					continue;
				part.nowHp = std::max<int64_t>(0, part.nowHp - damage);
				touched.insert(i);
			}
		}
	}
}

drogon::Task<::RaidMissionUserInfo> missionUserRow(
	const db::Database database, const drogon::orm::Row& row)
{
	const auto userId = row["user_id"].as<std::string>();
	const auto profile = co_await loadProfile(database, userId);
	::RaidMissionUserInfo out{};
	out.user_id = userId;
	out.user_name = profile.name;
	out.leader_unit_id = profile.leader_unit_id;
	out.now_point = row["now_point"].as<int32_t>();
	out.dst_point = row["dst_point"].as<int32_t>();
	out.player_rest = row["player_rest"].as<int32_t>();
	out.camp_flg = row["camp_flg"].as<int32_t>();
	out.search_info = row["search_info"].as<std::string>();
	out.battle_num = static_cast<int32_t>(row["battle_serial"].as<int64_t>());
	out.battle_boss_flg = row["battle_boss"].as<int32_t>();
	// Non-zero after a boss battle the boss survived, until the player walks
	// on: bossCheck @0x19CAFA0 skips the encounter only while it is set, so
	// without it the next poll drags the player straight back into the fight.
	out.boss_escape_limit = row["boss_escape"].as<int32_t>();
	// "missionBossId:count,..." of this member's boss battles, read by boss AI
	// conditions (BattleUnit::getRaidBossEncountNum).
	out.boss_encount_info = row["boss_encounters"].as<std::string>();
	co_return out;
}

::RaidUserPartyDeckInfo partyRowOut(const drogon::orm::Row& row)
{
	::RaidUserPartyDeckInfo out{};
	out.user_id = row["user_id"].as<std::string>();
	out.user_unit_id = row["user_unit_id"].as<std::string>();
	out.unit_id = row["unit_id"].as<std::string>();
	out.unit_lv = row["unit_lv"].as<int32_t>();
	out.member_type = row["member_type"].as<int32_t>();
	out.disp_order = row["disp_order"].as<int32_t>();
	out.max_hp = row["max_hp"].as<int64_t>();
	out.now_hp = row["now_hp"].as<int64_t>();
	out.bb_gauge = row["bb_gauge"].as<int32_t>();
	out.sb_gauge = row["sb_gauge"].as<int32_t>();
	out.hel = static_cast<int32_t>(row["hel"].as<int64_t>());
	out.friend_id = row["friend_id"].as<std::string>();
	out.unit_type_id = row["unit_type_id"].as<std::string>();
	out.img_type = row["img_type"].as<int32_t>();
	return out;
}

} // namespace

int64_t RunBoss::bodyHp() const
{
	for (const auto& part : parts)
		if (part.body)
			return part.nowHp;
	return parts.empty() ? 0 : parts.front().nowHp;
}

int64_t nowSeconds()
{
	return static_cast<int64_t>(std::time(nullptr));
}

std::vector<PartyRow> readPartyRows(const RaidRequest& req)
{
	std::vector<PartyRow> out;
	for (const auto* row : req.rows("wc2DiMY1"))
	{
		PartyRow party{};
		party.userUnitId = RaidRequest::textOf(row, "edy7fq3L");
		party.unitId = RaidRequest::textOf(row, "pn16CNah");
		party.friendId = RaidRequest::textOf(row, "98WfKiyA");
		party.unitTypeId = RaidRequest::textOf(row, "nBTx56W9");
		party.unitLv = static_cast<int32_t>(RaidRequest::numberOf(row, "4A6LzBxr", 1));
		party.memberType = static_cast<int32_t>(RaidRequest::numberOf(row, "gr48vsdJ", 1));
		party.dispOrder = static_cast<int32_t>(RaidRequest::numberOf(row, "XuJL4pc5"));
		party.imgType = static_cast<int32_t>(RaidRequest::numberOf(row, "2pAyFjmZ"));
		party.hp = RaidRequest::numberOf(row, "e7DK0FQT");
		party.hel = RaidRequest::numberOf(row, "PWXu25cg");
		party.bbGauge = static_cast<int32_t>(RaidRequest::numberOf(row, "gU2xtQ0V"));
		party.sbGauge = static_cast<int32_t>(RaidRequest::numberOf(row, "nIGZ1X9C"));
		// "0" is how an own unit names no helper (RaidUserPartyDeckInfo::isFriend @0x1301B28).
		if (party.friendId == "0")
			party.friendId.clear();
		out.push_back(std::move(party));
	}
	return out;
}

std::vector<ClientEvent> readEvents(const RaidRequest& req)
{
	std::vector<ClientEvent> out;
	for (const auto* row : req.rows("QP4cwt7n"))
	{
		ClientEvent event{};
		event.id = RaidRequest::numberOf(row, "o96QPNZn", -1);
		event.type = static_cast<int32_t>(RaidRequest::numberOf(row, "NH65Wj0f"));
		event.params = RaidRequest::textOf(row, "xIB0C8PN");
		if (event.id >= 0)
			out.push_back(std::move(event));
	}
	return out;
}

std::optional<::RaidMissionMst> runMission(const RaidRun& run)
{
	return findMission(run.missionId);
}

drogon::Task<std::optional<RaidRun>> roomRun(const db::Database database, const std::string roomId)
{
	if (roomId.empty())
		co_return std::nullopt;
	const auto rows = co_await database->execSqlCoro(
		"SELECT m.room_mission_id, m.room_id, m.mission_id, m.result, m.started_at, m.time_limit,"
		" r.quick_start FROM raid_room r JOIN raid_room_mission m"
		" ON m.room_mission_id = r.current_room_mission_id WHERE r.room_id = $1;",
		roomId);
	if (rows.empty())
		co_return std::nullopt;
	auto run = runFromRow(rows[0]);
	// THE CLOCK IS DERIVED, NOT TICKED: the first read past the limit ends the
	// run as time up for everyone in it.
	if (run.result == kRunRunning && nowSeconds() >= run.startedAt + run.timeLimit)
	{
		LOG_INFO << "Raid: run " << run.id << " of room " << roomId << " timed out";
		co_await finishRun(database, run, kRunTimeUp);
		run.result = kRunTimeUp;
	}
	co_return run;
}

drogon::Task<bool> isRoomMember(const db::Database database, const std::string roomId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_room_member WHERE room_id = $1 AND user_id = $2;", roomId, userId);
	co_return !rows.empty();
}

drogon::Task<std::optional<RaidRun>> requestRun(
	const db::Database database, const RaidRequest& req, const std::string userId)
{
	auto roomId = req.text("8iBuZa3n", "8VYd6xSX");
	if (roomId.empty())
		roomId = (co_await loadRaidUser(database, userId)).login_room_id;
	if (roomId.empty() || !(co_await isRoomMember(database, roomId, userId)))
		co_return std::nullopt;

	auto run = co_await roomRun(database, roomId);
	// "0" is what a client sends before any CurrentRoomMissionID reached it:
	// it names no run, so it means the room's current one.
	const auto named = req.text("g97TWISb", "ja39HMLq");
	if (run && !named.empty() && named != "0" && named != std::to_string(run->id))
	{
		LOG_WARN << "Raid: " << userId << " named run " << named << " but room " << roomId
			<< " is on run " << run->id;
		co_return std::nullopt;
	}
	co_return run;
}

drogon::Task<RaidRun> startRun(const db::Database database, const std::string roomId, const ::RaidMissionMst mission)
{
	const auto now = nowSeconds();
	const auto created = co_await database->execSqlCoro(
		"INSERT INTO raid_room_mission(room_id, mission_id, result, started_at, time_limit)"
		" VALUES ($1, $2, $3, $4, $5) RETURNING room_mission_id;",
		roomId, std::to_string(mission.id), kRunRunning, now, mission.time_limit);
	const auto runId = created[0]["room_mission_id"].as<int64_t>();

	for (const auto& placement : theServer()->cache().raidMissionBossMst())
	{
		if (placement.mission_id != mission.id)
			continue;
		const auto parts = initialBossParts(placement.boss_id, mission.rc_id);
		co_await database->execSqlCoro(
			"INSERT INTO raid_mission_boss(room_mission_id, mission_boss_id, boss_id, current_point,"
			" visible, parts_json) VALUES ($1, $2, $3, $4, $5, $6);",
			runId, std::to_string(placement.mission_boss_id), std::to_string(placement.boss_id),
			placement.init_point_num, placement.boss_visible_flg, encodeParts(parts));
	}

	co_await database->execSqlCoro(
		"UPDATE raid_room SET mission_start_flg = 1, current_room_mission_id = $1, status = $2,"
		" mission_started_at = $3, updated_at = $4 WHERE room_id = $5;",
		runId, kRoomInField, now, now, roomId);
	LOG_INFO << "Raid: room " << roomId << " started run " << runId << " of mission " << mission.id;

	RaidRun run{};
	run.roomId = roomId;
	run.id = runId;
	run.missionId = std::to_string(mission.id);
	run.startedAt = now;
	run.timeLimit = mission.time_limit;
	co_return run;
}

drogon::Task<void> joinRun(
	const db::Database database, const RaidRun run, const std::string userId,
	const std::vector<PartyRow> party, const ::RaidMissionMst mission)
{
	const auto start = startPoint(mission.map_id);
	co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO raid_mission_user(room_mission_id, user_id, now_point, dst_point,"
		" player_rest) VALUES ($1, $2, $3, $4, $5);",
		run.id, userId, start, start, kHeartsMax);
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET mission_status = $1, last_seen = $2 WHERE room_id = $3 AND user_id = $4;",
		kRunRunning, nowSeconds(), run.roomId, userId);

	for (const auto& row : party)
	{
		// The HP MissionStart sends is the unit's full HP after spheres and
		// passives, computed client side: it is the run's max HP.
		co_await database->execSqlCoro(
			"INSERT OR IGNORE INTO raid_mission_party(room_mission_id, user_id, disp_order, user_unit_id,"
			" unit_id, unit_lv, member_type, friend_id, unit_type_id, img_type, max_hp, now_hp, hel)"
			" VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13);",
			run.id, userId, row.dispOrder, row.userUnitId, row.unitId, row.unitLv, row.memberType,
			row.friendId, row.unitTypeId, row.imgType, row.hp, row.hp, row.hel);
	}
}

drogon::Task<std::optional<RunMember>> loadMember(const db::Database database, const int64_t runId, const std::string userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT now_point, dst_point, player_rest, search_info, battle_serial, battle_boss, battle_boss_id,"
		" battle_point, last_event_id, ended, drops, result_json FROM raid_mission_user"
		" WHERE room_mission_id = $1 AND user_id = $2;",
		runId, userId);
	if (rows.empty())
		co_return std::nullopt;
	const auto& row = rows[0];
	RunMember member{};
	member.nowPoint = row["now_point"].as<int32_t>();
	member.dstPoint = row["dst_point"].as<int32_t>();
	member.hearts = row["player_rest"].as<int32_t>();
	member.searchInfo = row["search_info"].as<std::string>();
	member.battleSerial = row["battle_serial"].as<int64_t>();
	member.battleBoss = row["battle_boss"].as<int32_t>() == 1;
	member.battleBossId = row["battle_boss_id"].as<std::string>();
	member.battlePoint = row["battle_point"].as<int32_t>();
	member.lastEventId = row["last_event_id"].as<int64_t>();
	member.ended = row["ended"].as<int32_t>() == 1;
	member.drops = row["drops"].as<std::string>();
	member.resultJson = row["result_json"].as<std::string>();
	co_return member;
}

drogon::Task<void> finishRun(const db::Database database, const RaidRun run, const int32_t result)
{
	co_await database->execSqlCoro(
		"UPDATE raid_room_mission SET result = $1, ended_at = $2 WHERE room_mission_id = $3 AND result = $4;",
		result, nowSeconds(), run.id, kRunRunning);
	// MissionStatus 2..4 is "over, result not collected": RaidInitScene sends a
	// resuming player straight to the result scene (1261) with it.
	// Only members who joined the run (have a raid_mission_user row) get its
	// result; one who was sent (ready, or ReadyUp 2) but never sent their own
	// MissionStart goes back to the lobby instead of a result it cannot collect.
	co_await database->execSqlCoro(
		"UPDATE raid_room_member SET mission_status = CASE WHEN EXISTS (SELECT 1 FROM raid_mission_user u"
		" WHERE u.room_mission_id = $1 AND u.user_id = raid_room_member.user_id) THEN $2 ELSE 0 END"
		" WHERE room_id = $3 AND mission_status = $4;",
		run.id, result, run.roomId, kRunRunning);
}

drogon::Task<std::optional<RaidRun>> loadRun(const db::Database database, const int64_t runId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT m.room_mission_id, m.room_id, m.mission_id, m.result, m.started_at, m.time_limit,"
		" r.quick_start FROM raid_room_mission m JOIN raid_room r ON r.room_id = m.room_id"
		" WHERE m.room_mission_id = $1;",
		runId);
	if (rows.empty())
		co_return std::nullopt;
	co_return runFromRow(rows[0]);
}

bool hasRoute(const std::string& missionId, int32_t from, int32_t to)
{
	return routeExists(static_cast<int32_t>(toInt(missionId)), from, to);
}

drogon::Task<bool> moveTo(const db::Database database, const RaidRun run, const std::string userId, const int32_t target)
{
	const auto member = co_await loadMember(database, run.id, userId);
	if (!member || run.result != kRunRunning)
		co_return false;
	if (member->nowPoint == target)
		co_return true;
	const auto hops = routeHops(static_cast<int32_t>(toInt(run.missionId)), member->nowPoint, target);
	if (hops < 0)
	{
		// A Teleport armed by RaidLimitedUseItem goes anywhere on the map, once.
		const auto mission = runMission(run);
		if (mission && isMapPoint(mission->map_id, target))
		{
			const auto warped = co_await database->execSqlCoro(
				"UPDATE raid_mission_user SET now_point = $1, dst_point = $2, warp_pending = 0, boss_escape = 0"
				" WHERE room_mission_id = $3 AND user_id = $4 AND warp_pending = 1;",
				target, target, run.id, userId);
			if (warped.affectedRows() > 0)
			{
				LOG_INFO << "Raid: " << userId << " teleported " << member->nowPoint << " -> " << target;
				co_return true;
			}
		}
		// Not refused: the next reply carries the stored point and snaps the
		// avatar back (RAIDS_ROUND2_CORE 2.4).
		LOG_WARN << "Raid: " << userId << " tried to walk " << member->nowPoint << " -> " << target
			<< " on mission " << run.missionId << " without a route";
		co_return false;
	}
	if (hops > 1)
		LOG_INFO << "Raid: " << userId << " walked " << member->nowPoint << " -> " << target << " (" << hops
			<< " hops) on mission " << run.missionId;
	co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET now_point = $1, dst_point = $2, boss_escape = 0"
		" WHERE room_mission_id = $3 AND user_id = $4;",
		target, target, run.id, userId);
	co_return true;
}

drogon::Task<std::optional<RunMember>> catchUpMove(
	const db::Database database, const RaidRun run, const std::string userId, const int32_t point)
{
	auto member = co_await loadMember(database, run.id, userId);
	if (!member || member->nowPoint == point)
		co_return member;
	// No co_await inside a short-circuit condition: GCC 13 ICEs on it.
	const bool moved = co_await moveTo(database, run, userId, point);
	if (moved)
		member = co_await loadMember(database, run.id, userId);
	co_return member;
}

drogon::Task<void> saveParty(
	const db::Database database, const RaidRun run, const std::string userId, const std::vector<PartyRow> rows)
{
	for (const auto& row : rows)
	{
		// Matched by the unit, not the display order: the battle reports
		// getSendDisporder, which need not number slots the way MissionStart's
		// getDispOrder did.  The helper has no user unit id of ours, so it is
		// matched by its owner instead.
		const bool helper = !row.friendId.empty();
		co_await database->execSqlCoro(
			"UPDATE raid_mission_party SET now_hp = MIN(MAX($1, 0), max_hp), bb_gauge = $2, sb_gauge = $3"
			" WHERE room_mission_id = $4 AND user_id = $5 AND"
			" ((friend_id = '' AND user_unit_id = $6 AND $7 = 0) OR (friend_id = $8 AND $9 = 1));",
			row.hp, row.bbGauge, row.sbGauge, run.id, userId, row.userUnitId, helper ? 1 : 0,
			row.friendId, helper ? 1 : 0);
	}
}

drogon::Task<std::vector<RunBoss>> loadBosses(const db::Database database, const int64_t runId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT mission_boss_id, boss_id, current_point, visible, parts_json FROM raid_mission_boss"
		" WHERE room_mission_id = $1 ORDER BY mission_boss_id;",
		runId);
	std::vector<RunBoss> out;
	for (const auto& row : rows)
	{
		RunBoss boss{};
		boss.missionBossId = row["mission_boss_id"].as<std::string>();
		boss.bossId = static_cast<int32_t>(toInt(row["boss_id"].as<std::string>()));
		boss.point = row["current_point"].as<int32_t>();
		boss.visible = row["visible"].as<int32_t>() == 1;
		boss.parts = decodeParts(row["parts_json"].as<std::string>());
		out.push_back(std::move(boss));
	}
	co_return out;
}

drogon::Task<std::vector<::RaidEventAck>> applyEvents(
	const db::Database database, const RaidRun run, const std::string userId,
	const std::vector<ClientEvent> events)
{
	std::vector<::RaidEventAck> acks;
	if (events.empty())
		co_return acks;

	auto bosses = co_await loadBosses(database, run.id);
	// Damage counts only against the boss of the member's OPEN battle: the
	// client never reports boss damage from anywhere else.
	const auto member = co_await loadMember(database, run.id, userId);
	const auto fighting = member && member->battleSerial != 0 && member->battleBoss
		? member->battleBossId : std::string{};
	std::set<size_t> touched;
	std::vector<int64_t> hpBefore;
	for (const auto& boss : bosses)
		hpBefore.push_back(boss.bodyHp());
	int64_t highest = 0;
	for (const auto& event : events)
	{
		acks.push_back({ std::to_string(event.id) });
		highest = std::max(highest, event.id);
		// AT MOST ONCE: the client re-sends an event with every turn until it
		// sees the ack, and again after an app restart.
		const auto inserted = co_await database->execSqlCoro(
			"INSERT OR IGNORE INTO raid_mission_event(room_mission_id, user_id, event_id, type, applied_at)"
			" VALUES ($1, $2, $3, $4, $5);",
			run.id, userId, event.id, event.type, nowSeconds());
		if (inserted.affectedRows() == 0)
			continue;
		// Type 1 is boss damage; the BattleEnd total (g5L3bKPu) repeats the
		// same damage and is deliberately not applied.
		if (event.type == 1 && run.result == kRunRunning && !fighting.empty())
			applyDamage(bosses, fighting, event.params, touched);
		// Type 2 is a battle item use ("0:itemId:useNum,..."): battle items
		// come from the raid item set (GameUtils::getBattleEquipItemInfo).
		if (event.type == 2)
		{
			const auto used = co_await spendSetItems(database, userId, event.params);
			if (used > 0)
				co_await bumpStat(database, userId, stat::kRaidItemUse, used);
		}
	}

	// The caller runs this inside a transaction, which holds the only SQLite
	// connection: the read above and these writes cannot interleave with
	// another member's report, so shared boss HP never loses damage and the
	// kill is credited to exactly the report that took the body to 0
	// (MULTIPLAYER 5.1, BossFinishCnt).
	for (const auto index : touched)
	{
		co_await database->execSqlCoro(
			"UPDATE raid_mission_boss SET parts_json = $1 WHERE room_mission_id = $2 AND mission_boss_id = $3;",
			encodeParts(bosses[index].parts), run.id, bosses[index].missionBossId);
		if (hpBefore[index] > 0 && bosses[index].bodyHp() <= 0)
			co_await bumpStat(database, userId, stat::kBossFinishCnt);
	}
	co_await database->execSqlCoro(
		"UPDATE raid_mission_user SET last_event_id = MAX(last_event_id, $1)"
		" WHERE room_mission_id = $2 AND user_id = $3;",
		highest, run.id, userId);

	if (!touched.empty() && run.result == kRunRunning)
	{
		const auto kills = co_await loadKills(database, run.id);
		if (runIsWon(run.missionId, bosses, kills))
		{
			LOG_INFO << "Raid: run " << run.id << " won: clear conditions met";
			co_await finishRun(database, run, kRunWon);
		}
	}
	co_return acks;
}

drogon::Task<bool> recordKills(const db::Database database, const RaidRun run, const std::string userId,
	const std::string defeated)
{
	if (run.result != kRunRunning)
		co_return false;
	int32_t rows = 0;
	int64_t total = 0;
	for (const auto& entry : split(defeated, ','))
	{
		const auto fields = split(entry, ':');
		// One battle is a few waves of a handful of monsters: a larger
		// count is a forged report and is clamped, never trusted.
		const auto count = fields.size() == 2 ? std::clamp<int64_t>(toInt(fields[1]), 0, kMaxKillsPerBattle) : 0;
		if (count <= 0 || toInt(fields[0]) <= 0 || ++rows > kMaxKillsPerBattle)
			continue;
		co_await database->execSqlCoro(
			"INSERT INTO raid_mission_kill(room_mission_id, monster_id, num) VALUES ($1, $2, $3)"
			" ON CONFLICT(room_mission_id, monster_id) DO UPDATE SET num = num + excluded.num;",
			run.id, std::to_string(toInt(fields[0])), count);
		total += count;
	}
	if (total > 0)
		co_await database->execSqlCoro(
			"INSERT INTO raid_mission_kill(room_mission_id, monster_id, num) VALUES ($1, $2, $3)"
			" ON CONFLICT(room_mission_id, monster_id) DO UPDATE SET num = num + excluded.num;",
			run.id, std::string(kAnyKill), std::min(total, kMaxKillsPerBattle));
	const auto bosses = co_await loadBosses(database, run.id);
	const auto kills = co_await loadKills(database, run.id);
	if (!runIsWon(run.missionId, bosses, kills))
		co_return false;
	LOG_INFO << "Raid: run " << run.id << " won by " << userId << "'s battle: clear conditions met";
	co_await finishRun(database, run, kRunWon);
	co_return true;
}

drogon::Task<::RaidFieldResp> fieldResp(const db::Database database, const std::string userId, const RaidRun run,
	const bool withSet)
{
	::RaidFieldResp resp{};
	const auto room = co_await roomResp(database, userId, run.roomId);
	resp.rooms = room.rooms;
	resp.members = room.members;

	// Re-read: the request may just have finished the run.
	const auto fresh = co_await database->execSqlCoro(
		"SELECT result FROM raid_room_mission WHERE room_mission_id = $1;", run.id);
	const auto result = fresh.empty() ? run.result : fresh[0]["result"].as<int32_t>();
	resp.room_mission.mission_id = run.missionId;
	resp.room_mission.result = result;
	resp.room_mission.rest_time = static_cast<int32_t>(
		std::max<int64_t>(0, run.startedAt + run.timeLimit - nowSeconds()));

	const auto users = co_await database->execSqlCoro(
		"SELECT user_id, now_point, dst_point, player_rest, camp_flg, search_info, battle_serial, battle_boss, boss_escape,"
		" boss_encounters"
		" FROM raid_mission_user WHERE room_mission_id = $1 AND ended = 0 ORDER BY user_id;",
		run.id);
	for (const auto& row : users)
		resp.mission_users.push_back(co_await missionUserRow(database, row));

	for (const auto& boss : co_await loadBosses(database, run.id))
	{
		::RaidMissionBossInfo out{};
		out.mission_boss_id = boss.missionBossId;
		out.now_hp = boss.bodyHp();
		out.current_point = boss.point;
		out.visible_flg = boss.visible && boss.bodyHp() > 0 ? 1 : 0;
		resp.bosses.push_back(std::move(out));
	}

	const auto party = co_await database->execSqlCoro(
		"SELECT * FROM raid_mission_party WHERE room_mission_id = $1 ORDER BY user_id, disp_order;", run.id);
	for (const auto& row : party)
		resp.party.push_back(partyRowOut(row));

	// Emptied stacks go out at 0 and an empty bag as one blank row (bagRows):
	// the list is replaced only by a non-empty reply.
	auto bag = bagRows(userId, co_await loadBag(database, run.id, userId));
	if (!bag.empty())
		resp.bag = std::move(bag);
	// The raid item set the battles read (GameUtils::getBattleEquipItemInfo),
	// so a resumed run shows what the server holds.
	if (withSet)
	{
		auto set = setRows(co_await loadSet(database, userId, kItemSetKind));
		if (!set.empty())
			resp.item_set = std::move(set);
	}
	co_return resp;
}

std::string mergeJson(std::string base, const std::string& extra)
{
	// Both sides are serialized objects; an empty object adds nothing.
	if (extra.size() <= 2 || base.size() < 2 || base.back() != '}' || extra.front() != '{')
		return base;
	base.pop_back();
	if (base.size() > 1)
		base += ',';
	base.append(extra, 1, std::string::npos);
	return base;
}

} // namespace gme::raid
