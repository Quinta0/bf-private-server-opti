#include "App.hpp"

#include <gimuserver/gme/common/Guilds.hpp>
#include <gimuserver/gme/common/RaidBlackList.hpp>
#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/RaidRoomSearch.hpp>
#include <gimuserver/gme/common/Raids.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <tuple>

namespace gme::raid
{

namespace
{

// PLACEHOLDERS BIND IN ORDER OF APPEARANCE (Raids.cpp): every statement here
// numbers `$N` left to right and passes a value twice rather than repeat one.

/*!
* Candidate rooms read per search before the per-room checks.  Each candidate
* is pruned (3.3) before it is judged, and pruning writes, so this bounds the
* work one search can do on a busy server.  Far above what kRoomListMax rows
* need once the SQL filters have run.
*/
constexpr int32_t kSearchScanMax = 100;

/*! The SQL-side filters of one search, the ones its type ignores neutralised. */
struct SqlFilter
{
	std::string worldId;
	std::string playstyleId;
	std::string missionId;
	int32_t minRcAtLeast = 0;
	int32_t kick = -1;
	int32_t joinable = -1;
	int32_t minLvAtLeast = 0;
	int32_t quickMatching = 0;
};

/*! What the per-room checks need to know about the caller. */
struct Searcher
{
	UserIdentity identity;
	RoomSearch search;
	int32_t rc = 1;
	std::set<std::string> blocked;  // blackListLinks: either direction
	std::set<std::string> known;    // friends (type 2) or guild mates (type 4)
	int32_t guildId = 0;            // type 4: the caller's guild
	bool sandbox = false;           // only rooms whose master is in the caller's pool (Sandbox.hpp)
};

/*! One room the SQL filters let through. */
struct Candidate
{
	std::string roomId;
	int64_t roomNum = 0;
	std::string missionId;
	bool inField = false;
	int64_t createdAt = 0;
};

/*! A row that passed every check, with what the auto-join pick sorts on. */
struct Listed
{
	int64_t createdAt = 0;
	int64_t roomNum = 0;
	::RaidRoomInfo row;
};

int32_t toInt32(int64_t value)
{
	return static_cast<int32_t>(std::clamp<int64_t>(value,
		std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

bool needsKnownMember(int32_t type)
{
	return type == kSearchFriends || type == kSearchGuild;
}

SqlFilter filterFor(const RoomSearch& search)
{
	SqlFilter filter{};
	// Types 2/4 come without a world (RaidWorldScene::connectSearchCon passes
	// ""): a friend's room in any world counts, and each row names its own,
	// which RaidFriendListScene::strWorld draws.
	if (needsKnownMember(search.type))
		return filter;
	filter.worldId = search.worldId;
	filter.missionId = search.missionId;
	if (search.type == kSearchAuto)
	{
		// The menu's kick 0 / level "1" and 1224's RC -1 / level "0" are
		// constants of the call, not choices: auto-join keys on the mission
		// alone, among rooms that opted in with Auto-join On (KQA1oWj7).
		filter.quickMatching = 1;
		return filter;
	}
	// "1" is RaidPlaystyleMst "Not Specified": no filter.
	if (search.playstyleId != "1")
		filter.playstyleId = search.playstyleId;
	// INFERRED (MULTIPLAYER 4.4, 8.2): "RC n Over" (RAID_ROOMSEARCH_RC) lists
	// rooms asking at least n; the level filter reads the same way, and "0"/"1"
	// mean none.  Kick and joinable: -1 any, else exact.
	filter.minRcAtLeast = std::max(0, search.rc);
	filter.kick = std::max(-1, search.kick);
	filter.joinable = std::max(-1, search.joinable);
	filter.minLvAtLeast = search.level > 1 ? search.level : 0;
	return filter;
}

/*! 4.3 rule 7: a running quest takes newcomers only if they may play it. */
bool missionPermitted(const std::string& missionId, int32_t rc)
{
	const auto mission = findMission(missionId);
	return mission.has_value() && mission->rc_id <= rc;
}

/*! A RaidWorldMst id: the requested one when it exists, else the first world. */
std::string knownWorld(const std::string& requested)
{
	const auto& worlds = theServer()->cache().raidWorldMst();
	if (std::any_of(worlds.begin(), worlds.end(),
		[&requested](const auto& world) { return std::to_string(world.id) == requested; }))
		return requested;
	return worlds.empty() ? std::string("1") : std::to_string(worlds.front().id);
}

/*!
* Who counts as "someone I know" for types 2 and 4; empty for the others.
*
* REAL FRIENDS ONLY.  The NPC roster (user_friends, loadFriendRoster) holds ids
* that never own an account, so none of them can sit in a room; friendships
* between players are user_relations state 1 (Relations.hpp).  Guild mates are
* the caller's guild's member rows; NPC ids among them never match a member.
*/
drogon::Task<std::set<std::string>> knownPlayers(const db::Database database,
	const UserIdentity identity, const int32_t type)
{
	std::set<std::string> out;
	if (type == kSearchFriends)
	{
		for (const auto& relation : co_await gme::loadRelations(database, identity.userId))
			if (relation.state == gme::kRelFriend)
				out.insert(relation.other_id);
	}
	else if (type == kSearchGuild)
	{
		const auto guild = co_await gme::loadGuild(database, identity);
		if (!guild || guild->guild_id == 0)
			co_return out;
		for (const auto& row : co_await database->execSqlCoro(
			"SELECT member_id FROM user_guild_members WHERE guild_id = $1 AND member_id != $2;",
			guild->guild_id, identity.userId))
			out.insert(row["member_id"].as<std::string>());
	}
	co_return out;
}

/*!
* Rooms that pass every filter a single statement can decide.  Membership
* (full, blacklist, friends) is judged per room after its prune.
* `mission_started_at > now - window` is "now < started + window" (4.3 rule 7).
*/
drogon::Task<std::vector<Candidate>> candidateRooms(const db::Database database,
	const Searcher& searcher, const int32_t level)
{
	const auto filter = filterFor(searcher.search);
	const auto rows = co_await database->execSqlCoro(
		"SELECT r.room_id, r.mission_id, r.status, r.created_at FROM raid_room r"
		" WHERE r.quick_start = 0"
		" AND (r.status = $1 OR (r.status = $2 AND r.midway_joinable = 1 AND r.mission_started_at > $3))"
		" AND ($4 = '' OR r.world_id = $5)"
		" AND r.min_rc <= $6 AND r.min_lv <= $7"
		" AND NOT EXISTS (SELECT 1 FROM raid_room_member m WHERE m.room_id = r.room_id AND m.user_id = $8)"
		" AND ($9 = '' OR r.playstyle_id = $10)"
		" AND ($11 = '' OR r.mission_id = $12)"
		" AND r.min_rc >= $13 AND ($14 < 0 OR r.kick_flg = $15)"
		" AND ($16 < 0 OR r.midway_joinable = $17) AND r.min_lv >= $18"
		// Auto-join (type 3) never picks a room with a password: its RaidEnter
		// sends none and would always be refused.
		" AND ($19 = 0 OR (r.quick_matching = 1 AND r.password = ''))"
		// Types 2/4 look for a room with a friend / guild mate in it: decided
		// here, so the scan cap never hides an older friend's room behind
		// strangers' rooms (and strangers' rooms are not pruned for nothing).
		" AND ($20 = 0 OR EXISTS (SELECT 1 FROM raid_room_member m JOIN user_relations ur"
		" ON ur.other_id = m.user_id WHERE m.room_id = r.room_id AND m.kicked = 0"
		" AND ur.user_id = $21 AND ur.state = $22))"
		" AND ($23 = 0 OR EXISTS (SELECT 1 FROM raid_room_member m JOIN user_guild_members g"
		" ON g.member_id = m.user_id WHERE m.room_id = r.room_id AND m.kicked = 0 AND g.guild_id = $24))"
		+ gme::sandbox::poolSql("r.master_user_id", searcher.sandbox) +
		" ORDER BY r.created_at DESC, r.room_id DESC LIMIT $25;",
		kRoomLobby, kRoomInField, nowSeconds() - kMidwayJoinWindowSec,
		filter.worldId, filter.worldId, searcher.rc, level, searcher.identity.userId,
		filter.playstyleId, filter.playstyleId, filter.missionId, filter.missionId,
		filter.minRcAtLeast, filter.kick, filter.kick, filter.joinable, filter.joinable,
		filter.minLvAtLeast, filter.quickMatching,
		searcher.search.type == kSearchFriends ? 1 : 0, searcher.identity.userId, gme::kRelFriend,
		searcher.search.type == kSearchGuild ? 1 : 0, searcher.guildId, kSearchScanMax);

	std::vector<Candidate> out;
	for (const auto& row : rows)
	{
		Candidate candidate{};
		candidate.roomNum = row["room_id"].as<int64_t>();
		candidate.roomId = std::to_string(candidate.roomNum);
		candidate.missionId = row["mission_id"].as<std::string>();
		candidate.inField = row["status"].as<int32_t>() == kRoomInField;
		candidate.createdAt = row["created_at"].as<int64_t>();
		out.push_back(std::move(candidate));
	}
	co_return out;
}

/*! Non-kicked members, earliest joined first (the master is normally among the first). */
drogon::Task<std::vector<std::string>> liveMembers(const db::Database database, const std::string roomId)
{
	std::vector<std::string> out;
	for (const auto& row : co_await database->execSqlCoro(
		"SELECT user_id FROM raid_room_member WHERE room_id = $1 AND kicked = 0"
		" ORDER BY joined_at, user_id;", roomId))
		out.push_back(row["user_id"].as<std::string>());
	co_return out;
}

/*! The first member in `known`, if any. */
std::optional<std::string> firstKnown(const std::vector<std::string>& members, const std::set<std::string>& known)
{
	const auto it = std::find_if(members.begin(), members.end(),
		[&known](const std::string& id) { return known.contains(id); });
	return it == members.end() ? std::nullopt : std::optional<std::string>(*it);
}

/*!
* A room as a list shows it (4.4): no password whoever asks, the search type
* echoed, EnterableFlg 1, MissionStartFlg 1 for a running quest (the world
* screen's getListMissionFlg @0x12F8CF0 counts those apart).
*/
::RaidRoomInfo listedRow(::RaidRoomInfo row, const int32_t type)
{
	// mission_start_flg stays as roomRow set it, from the post-prune state.
	row.password.clear();
	row.search_type = type;
	row.enterable_flg = 1;
	return row;
}

/*!
* The friend columns name the member that made the room show up
* (RaidFriendListScene::listDraw reads getFriendUserName / LeaderUnitID /
* UnitImgType).  Image type 0 is the base art, as round 1 sends for the master.
*/
::RaidRoomInfo withFriend(::RaidRoomInfo row, const std::string& userId, const RaidProfile& profile)
{
	row.friend_user_id = userId;
	row.friend_user_name = profile.name;
	row.friend_user_lv = profile.level;
	row.friend_leader_unit_id = profile.leader_unit_id;
	row.friend_leader_unit_lv = profile.leader_unit_lv;
	row.friend_unit_img_type = 0;
	return row;
}

/*!
* The row for one candidate, or nullopt when it does not qualify for this
* caller.  Pruned first (3.3): an absent member must neither fill a slot nor
* count as the friend that lists the room, and a room whose last member left
* closes here.
*/
drogon::Task<std::optional<::RaidRoomInfo>> judgeRoom(const db::Database database,
	const Searcher& searcher, const Candidate& candidate)
{
	// RaidEnter refuses a quest above the caller's RC (4.3 rule 6), lobby or
	// field alike: never list (or auto-pick) such a room.
	if (!candidate.missionId.empty() && !missionPermitted(candidate.missionId, searcher.rc))
		co_return std::nullopt;
	co_await pruneRoom(database, candidate.roomId);

	const auto members = co_await liveMembers(database, candidate.roomId);
	const auto blocked = std::any_of(members.begin(), members.end(),
		[&searcher](const std::string& id) { return searcher.blocked.contains(id); });
	if (members.empty() || blocked)
		co_return std::nullopt;
	const auto known = firstKnown(members, searcher.known);
	if (needsKnownMember(searcher.search.type) && !known)
		co_return std::nullopt;

	// roomRow folds capacity (non-kicked members vs KR6JacN8), quick start and
	// the midway window into EnterableFlg; after the prune above that is the
	// final word on "full" and "closed".
	const auto row = co_await roomRow(database, candidate.roomId, searcher.identity.userId);
	if (!row || row->enterable_flg != 1)
		co_return std::nullopt;
	const auto listed = listedRow(*row, searcher.search.type);
	if (!known)
		co_return listed;
	const auto profile = co_await loadProfile(database, *known);
	co_return withFriend(listed, *known, profile);
}

/*!
* Auto-join sends only its best room (4.4): RaidRoomInfoList::getObjectTop(true)
* @0x12F8AFC takes row 0 and the menu / auto scenes RaidEnter it at once.  Most
* members first, so the room nearest to a full party starts sooner; then the
* oldest, which has waited longest.
*/
std::vector<::RaidRoomInfo> bestForAuto(const std::vector<Listed>& found)
{
	if (found.empty())
		return {};
	const auto best = std::min_element(found.begin(), found.end(),
		[](const Listed& a, const Listed& b)
		{
			return std::make_tuple(-a.row.member_num, a.createdAt, a.roomNum)
				< std::make_tuple(-b.row.member_num, b.createdAt, b.roomNum);
		});
	return { best->row };
}

/*!
* What to send when nothing qualifies (4.4, 1.3).
*  - 1: `[]`; the search scenes cleared the list -> RAID_ROOMSEARCH_LISTNULL.
*  - 3: KQA1oWj7 0, so getObjectTop(true) returns null -> RAID_ROOMMENU_AUTONULL
*    instead of auto-joining a stale room from an earlier list.
*  - 2/4: C1vG0iKh 0 and 3tWuc7de 1, which getListMissionFlg drops, so
*    RaidWorldScene::connectEndCheck @0x19EF540 counts 0 ->
*    RAID_WORLD_FRIEND_LISTNULL.
* The world must exist in RaidWorldMst or errorAll raises RAID_WORLDDATA_NULL.
*/
std::vector<::RaidRoomInfo> emptyReply(const RoomSearch& search)
{
	if (search.type == kSearchFilter)
		return {};
	::RaidRoomInfo row{};
	row.room_id = "0";
	row.world_id = knownWorld(search.worldId);
	row.search_type = search.type;
	row.member_num = 0;
	row.quick_matching_flg = 0;
	row.enterable_flg = 0;
	row.mission_start_flg = search.type == kSearchAuto ? 0 : 1;
	return { row };
}

/*! Judge candidates in order until the list is full (type 3 judges them all to pick one). */
drogon::Task<std::vector<Listed>> judgeAll(const db::Database database,
	const Searcher& searcher, const std::vector<Candidate>& candidates)
{
	std::vector<Listed> found;
	for (const auto& candidate : candidates)
	{
		auto row = co_await judgeRoom(database, searcher, candidate);
		if (!row)
			continue;
		found.push_back(Listed{ candidate.createdAt, candidate.roomNum, std::move(*row) });
		if (searcher.search.type != kSearchAuto && found.size() >= static_cast<size_t>(kRoomListMax))
			break;
	}
	co_return found;
}

} // namespace

RoomSearch readRoomSearch(const RaidRequest& req)
{
	// One node in 8iBuZa3n (RaidRoomListRequest::createBody @0x13C6884).
	// String params go out only when non-empty and the RC filter only when
	// non-zero, so a missing key is the "no filter" default, never an error.
	const auto* node = req.node("8iBuZa3n");
	RoomSearch search{};
	search.type = toInt32(RaidRequest::numberOf(node, "ouU5H7qm", search.type));
	search.worldId = RaidRequest::textOf(node, "43hMuY2I");
	search.playstyleId = RaidRequest::textOf(node, "NQR1Vd3t");
	search.rc = toInt32(RaidRequest::numberOf(node, "wYmg52xp", search.rc));
	search.missionId = RaidRequest::textOf(node, "j28VNcUW");
	search.kick = toInt32(RaidRequest::numberOf(node, "j3sGA1zt", search.kick));
	search.joinable = toInt32(RaidRequest::numberOf(node, "1Txpza2u", search.joinable));
	search.level = toInt32(RaidRequest::numberOf(node, "0TfXnP3m", search.level));
	return search;
}

drogon::Task<std::vector<::RaidRoomInfo>> searchRooms(const db::Database database,
	const UserIdentity identity, const RoomSearch search)
{
	if (search.type < kSearchFilter || search.type > kSearchGuild)
	{
		LOG_WARN << "RaidRoomList: unknown search type " << search.type << " from " << identity.userId;
		co_return std::vector<::RaidRoomInfo>{};
	}

	Searcher searcher{};
	searcher.identity = identity;
	searcher.search = search;
	searcher.rc = (co_await loadRaidUser(database, identity.userId)).rc;
	searcher.sandbox = co_await gme::sandbox::isOn(database, identity.userId);
	searcher.blocked = co_await blackListLinks(database, identity.userId);
	searcher.known = co_await knownPlayers(database, identity, search.type);
	if (search.type == kSearchGuild)
	{
		const auto guild = co_await gme::loadGuild(database, identity);
		searcher.guildId = guild ? guild->guild_id : 0;
	}
	const auto level = (co_await loadProfile(database, identity.userId)).level;

	// Nobody to look for: skip the scan (and its prunes) entirely.
	std::vector<Listed> found;
	if (!needsKnownMember(search.type) || !searcher.known.empty())
	{
		const auto candidates = co_await candidateRooms(database, searcher, level);
		found = co_await judgeAll(database, searcher, candidates);
	}

	std::vector<::RaidRoomInfo> rows;
	if (search.type == kSearchAuto)
	{
		rows = bestForAuto(found);
	}
	else
	{
		for (auto& listed : found)
			rows.push_back(std::move(listed.row));
	}

	LOG_INFO << "RaidRoomList: type " << search.type << " for " << identity.userId
		<< " -> " << rows.size() << " room(s)";
	co_return rows.empty() ? emptyReply(search) : rows;
}

} // namespace gme::raid
