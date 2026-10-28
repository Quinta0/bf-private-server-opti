#include "App.hpp"

#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidRooms.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace gme::raid
{

namespace
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE.  Drogon's SQLite client hands the
// arguments to `?` slots positionally, so `$3 ... $1` or a repeated `$6`
// silently binds the wrong value (the first wire test caught login ids that
// never saved).  Every statement here numbers its placeholders left to right.

int64_t nowSec()
{
	return static_cast<int64_t>(std::time(nullptr));
}

/*!
* Regenerate aube from the stored count and the time it was last written.
* Stored, not ticked: `aube_ts` is when `aube` was last true, and every full
* kAubeRecoverSec since then is one more point, up to the cap.
*/
void deriveAube(RaidUser& user, int32_t stored, int64_t storedAt, int64_t now)
{
	if (stored >= kAubeMax || storedAt <= 0)
	{
		user.aube = std::min(stored, kAubeMax);
		user.rest_timer = 0;
		return;
	}
	const auto elapsed = std::max<int64_t>(0, now - storedAt);
	const auto gained = static_cast<int32_t>(elapsed / kAubeRecoverSec);
	user.aube = std::min(kAubeMax, stored + gained);
	user.rest_timer = user.aube >= kAubeMax
		? 0 : static_cast<int32_t>(kAubeRecoverSec - elapsed % kAubeRecoverSec);
}

::RaidUserInfo toWire(const RaidUser& user)
{
	::RaidUserInfo out{};
	out.rc = user.rc;
	out.agreement = user.agreement;
	out.login_world_id = user.login_world_id;
	out.login_room_id = user.login_room_id;
	out.open_flg = 1;
	out.active_deck = user.active_deck;
	out.aube = user.aube;
	out.aube_max = kAubeMax;
	out.rest_timer = user.rest_timer;
	return out;
}

drogon::Task<std::vector<::RaidClearMissionInfo>> clearedMissions(
	const db::Database database,
	const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT mission_id FROM raid_clear WHERE user_id = $1;", userId);
	std::vector<::RaidClearMissionInfo> out;
	for (const auto& row : rows)
	{
		::RaidClearMissionInfo clear{};
		clear.user_id = userId;
		clear.mission_id = row["mission_id"].as<std::string>();
		out.push_back(std::move(clear));
	}
	co_return out;
}

} // namespace

drogon::Task<RaidUser> loadRaidUser(const db::Database database, const std::string userId)
{
	co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO raid_user(user_id, aube, aube_ts) VALUES ($1, $2, 0);",
		userId, kAubeMax);
	const auto rows = co_await database->execSqlCoro(
		"SELECT r.rc, r.agreement, r.login_world_id, r.login_room_id, r.aube, r.aube_ts,"
		" COALESCE(u.active_deck, 0) AS active_deck"
		" FROM raid_user r LEFT JOIN user_info u ON u.id = r.user_id WHERE r.user_id = $1;",
		userId);

	RaidUser user{};
	user.user_id = userId;
	const auto& row = rows[0];
	user.rc = row["rc"].as<int32_t>();
	user.agreement = row["agreement"].as<int32_t>();
	user.login_world_id = row["login_world_id"].as<std::string>();
	user.login_room_id = row["login_room_id"].as<std::string>();
	user.active_deck = row["active_deck"].as<int32_t>();
	deriveAube(user, row["aube"].as<int32_t>(), row["aube_ts"].as<int64_t>(), nowSec());

	// Forget a room that died while the player was away (idle, dissolved by its
	// master): naming it would make RaidInitScene try to resume it.
	if (!user.login_room_id.empty() && !(co_await roomIsLive(database, user.login_room_id)))
	{
		LOG_INFO << "Raid: room " << user.login_room_id << " of " << userId
			<< " is no longer live; closing it";
		co_await closeRoom(database, user.login_room_id);
		user.login_world_id.clear();
		user.login_room_id.clear();
	}
	co_return user;
}

drogon::Task<bool> spendAube(const db::Database database, const std::string userId)
{
	co_await loadRaidUser(database, userId);   // the row exists first
	const auto rows = co_await database->execSqlCoro(
		"SELECT aube, aube_ts FROM raid_user WHERE user_id = $1;", userId);
	const auto stored = rows[0]["aube"].as<int32_t>();
	const auto storedAt = rows[0]["aube_ts"].as<int64_t>();
	RaidUser user{};
	const auto now = nowSec();
	deriveAube(user, stored, storedAt, now);
	if (user.aube <= 0)
		co_return false;
	// Keep the partial hour already earned: the clock moves on by the whole
	// orbs regenerated so far.  A full bar starts it now.
	const auto regenerated = user.aube >= kAubeMax || storedAt <= 0
		? now : storedAt + static_cast<int64_t>(user.aube - stored) * kAubeRecoverSec;
	const auto spent = co_await database->execSqlCoro(
		"UPDATE raid_user SET aube = $1, aube_ts = $2 WHERE user_id = $3 AND aube = $4 AND aube_ts = $5;",
		user.aube - 1, regenerated, userId, stored, storedAt);
	co_return spent.affectedRows() > 0;
}

drogon::Task<void> seedRaidDecks(const db::Database database, const std::string userId)
{
	const auto seeded = co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO user_decks(user_id, user_unit_id, deck_type, deck_num, member_type, disp_order)"
		" SELECT user_id, user_unit_id, $1, deck_num, member_type, disp_order FROM user_decks"
		" WHERE user_id = $2 AND deck_type = 1"
		" AND NOT EXISTS (SELECT 1 FROM user_decks WHERE user_id = $3 AND deck_type = $4);",
		kRaidDeckType, userId, userId, kRaidDeckType);
	if (seeded.affectedRows() > 0)
		LOG_INFO << "Raid: seeded " << seeded.affectedRows() << " raid squad row(s) for " << userId;
}

drogon::Task<void> acceptAgreement(const db::Database database, const std::string userId)
{
	co_await database->execSqlCoro(
		"UPDATE raid_user SET agreement = 1 WHERE user_id = $1;", userId);
}

std::vector<::RaidPermitMissionInfo> permittedMissions(int32_t rc)
{
	std::vector<::RaidPermitMissionInfo> out;
	for (const auto& mission : theServer()->cache().raidMissionMst())
	{
		if (mission.rc_id > rc)
			continue;
		::RaidPermitMissionInfo permit{};
		permit.mission_id = std::to_string(mission.id);
		permit.time_limit = mission.time_limit;
		out.push_back(std::move(permit));
	}
	return out;
}

std::optional<::RaidMissionMst> findMission(const std::string& missionId)
{
	for (const auto& mission : theServer()->cache().raidMissionMst())
	{
		if (std::to_string(mission.id) == missionId)
			return mission;
	}
	return std::nullopt;
}

drogon::Task<::RaidUserInfoResp> userInfoResp(const db::Database database, const std::string userId)
{
	const auto user = co_await loadRaidUser(database, userId);
	::RaidUserInfoResp resp{};
	resp.user = toWire(user);
	resp.permit = permittedMissions(user.rc);
	resp.clear = co_await clearedMissions(database, userId);
	co_return resp;
}

drogon::Task<::RaidWorldInfoResp> worldInfoResp(const db::Database database, const std::string userId)
{
	const auto user = co_await loadRaidUser(database, userId);
	::RaidWorldInfoResp resp{};
	for (const auto& world : theServer()->cache().raidWorldMst())
	{
		const auto worldId = std::to_string(world.id);
		const auto members = (co_await database->execSqlCoro(
			"SELECT COUNT(*) AS n FROM raid_room_member m JOIN raid_room r ON r.room_id = m.room_id"
			" WHERE r.world_id = $1 AND r.status != $2 AND m.last_seen >= $3;",
			worldId, kRoomClosed, nowSec() - kRoomIdleSec))[0]["n"].as<int32_t>();

		::RaidWorldInfo row{};
		row.world_id = worldId;
		// Display only: a few dozen raiders already reads as busy on a
		// community server.
		row.crowded_percent = std::min(100, members * 5);
		row.open_flg = 1;
		resp.worlds.push_back(std::move(row));
	}
	resp.permit = permittedMissions(user.rc);
	resp.clear = co_await clearedMissions(database, userId);
	co_return resp;
}

drogon::Task<RaidProfile> loadProfile(const db::Database database, const std::string userId)
{
	RaidProfile profile{};
	const auto info = co_await database->execSqlCoro(
		"SELECT username, level, active_deck FROM user_info WHERE id = $1;", userId);
	if (info.empty())
		co_return profile;
	profile.name = info[0]["username"].as<std::string>();
	profile.level = info[0]["level"].as<int32_t>();

	// The active party's leader (member_type 0), as the room list shows it.
	const auto leader = co_await database->execSqlCoro(
		"SELECT u.unit_id, u.unit_lvl FROM user_decks d JOIN user_units u"
		" ON u.user_unit_id = d.user_unit_id AND u.user_id = d.user_id"
		" WHERE d.user_id = $1 AND d.deck_type = 1 AND d.deck_num = $2 AND d.member_type = 0 LIMIT 1;",
		userId, info[0]["active_deck"].as<int32_t>());
	if (!leader.empty())
	{
		profile.leader_unit_id = std::to_string(leader[0]["unit_id"].as<int64_t>());
		profile.leader_unit_lv = leader[0]["unit_lvl"].as<int32_t>();
	}

	const auto guild = co_await database->execSqlCoro(
		"SELECT guild_id FROM user_guild_members WHERE member_id = $1 LIMIT 1;", userId);
	if (!guild.empty())
		profile.guild_id = guild[0]["guild_id"].as<int32_t>();
	co_return profile;
}

drogon::Task<std::string> createSoloRoom(
	const db::Database database,
	const std::string userId,
	const std::string missionId,
	const std::string helperJson)
{
	const auto user = co_await loadRaidUser(database, userId);
	if (!user.login_room_id.empty())
		co_await leaveRoom(database, userId, user.login_room_id);

	const auto now = nowSec();
	const auto worldId = theServer()->cache().raidWorldMst().empty()
		? std::string("1") : std::to_string(theServer()->cache().raidWorldMst().front().id);

	// RETURNING, not a follow-up last_insert_rowid(): other requests share the
	// connection, and their INSERTs can land between two separate awaits.
	const auto created = co_await database->execSqlCoro(
		"INSERT INTO raid_room(world_id, master_user_id, mission_id, quick_start, status,"
		" helper_json, created_at, updated_at) VALUES ($1, $2, $3, 1, $4, $5, $6, $7)"
		" RETURNING room_id;",
		worldId, userId, missionId, kRoomLobby, helperJson, now, now);
	const auto roomId = std::to_string(created[0]["room_id"].as<int64_t>());

	// Solo is always ready: there is nobody to wait for.
	co_await database->execSqlCoro(
		"INSERT INTO raid_room_member(room_id, user_id, ready, joined_at, last_seen)"
		" VALUES ($1, $2, 1, $3, $4);",
		roomId, userId, now, now);
	co_await database->execSqlCoro(
		"UPDATE raid_user SET login_world_id = $1, login_room_id = $2 WHERE user_id = $3;",
		worldId, roomId, userId);
	co_return roomId;
}

} // namespace gme::raid
