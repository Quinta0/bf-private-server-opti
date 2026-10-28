#include "App.hpp"

#include <gimuserver/gme/common/ExtraSkills.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>

namespace gme
{

namespace
{

int64_t nowSec()
{
	return static_cast<int64_t>(std::time(nullptr));
}

/*! Leading integer of a stored id ("10011", "10011_x", ""); 0 when none. */
int32_t leadingInt(const std::string& raw)
{
	try { return raw.empty() ? 0 : std::stoi(raw); }
	catch (const std::exception&) { return 0; }
}

/*! user_units.element is a word; FriendInfo wants the client's element id. */
int32_t elementId(const std::string& s)
{
	static const std::map<std::string, int32_t> kIds{
		{ "fire", 1 }, { "water", 2 }, { "earth", 3 },
		{ "thunder", 4 }, { "light", 5 }, { "dark", 6 } };
	const auto it = kIds.find(s);
	return it != kIds.end() ? it->second : 1;
}

/*!
* req_time (3InKeya4), which the client prints raw after FRIEND_REQUEST_TIME
* and sorts on as a string (sortFriendListByReqTime) -- so it must sort
* lexically.  AUTHORED format; the original is unknown.
*/
std::string reqTimeString(int64_t at)
{
	const auto t = static_cast<std::time_t>(at);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

/*!
* 0CAQ6wUe as the client reads it: SECONDS SINCE last login, not a timestamp.
* Observed on device 2026-10-16: a raw unix time drew "20741 days ago" in
* FriendRandomSearchScene (1792089852 / 86400), and "999 days" (the cap) in
* the Friend List.
*/
int32_t secondsSinceLogin(int64_t lastLoginAt)
{
	if (lastLoginAt <= 0)
		return 0;
	return static_cast<int32_t>(std::max<int64_t>(0, nowSec() - lastLoginAt));
}

/*!
* Both rows of the pair (me, other), in either direction.
*
* $1/$2 must be the FIRST placeholders in the statement: SQLite treats "$n" as
* a NAMED parameter and numbers names by first appearance, so a statement
* whose text mentions $3 before $1 would bind the arguments out of order.
*/
constexpr const char* kPairWhere =
	"((user_id = $1 AND other_id = $2) OR (user_id = $2 AND other_id = $1))";

drogon::Task<int32_t> myState(db::Database database, const std::string& me, const std::string& other)
{
	const auto row = co_await database->execSqlCoro(
		"SELECT state FROM user_relations WHERE user_id = $1 AND other_id = $2;", me, other);
	co_return row.empty() ? 0 : row[0]["state"].as<int32_t>();
}

drogon::Task<int32_t> countState(db::Database database, const std::string& userId, int32_t state)
{
	const auto row = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_relations WHERE user_id = $1 AND state = $2;", userId, state);
	co_return row[0]["n"].as<int32_t>();
}

drogon::Task<bool> hasRoom(db::Database database, const std::string& userId)
{
	co_return (co_await friendCount(database, userId)) < (co_await friendCapacity(database, userId));
}

drogon::Task<void> befriendBoth(db::Database database, const std::string& me, const std::string& other)
{
	co_await database->execSqlCoro(
		"UPDATE user_relations SET state = 1, created_at = $1"
		" WHERE (user_id = $2 AND other_id = $3) OR (user_id = $3 AND other_id = $2);",
		nowSec(), me, other);
}

PlayerCard cardFromRow(const drogon::orm::Row& r)
{
	PlayerCard c{};
	c.user_id = r["id"].as<std::string>();
	c.handle_name = r["username"].as<std::string>();
	c.friend_id = r["fid"].as<std::string>();
	c.want_gift = r["want_gift"].as<std::string>();
	c.guild_id = r["gid"].as<int32_t>();
	c.level = r["level"].as<int32_t>();
	c.last_login_at = r["lla"].as<int64_t>();
	c.unit_id = leadingInt(r["unit_id"].as<std::string>());
	c.unit_lv = std::max(r["unit_lvl"].as<int32_t>(), 1);
	c.unit_type_id = r["unit_type_id"].as<int32_t>();
	c.element = elementId(r["element"].as<std::string>());
	c.base_hp = r["base_hp"].as<int32_t>();
	c.base_atk = r["base_atk"].as<int32_t>();
	c.base_def = r["base_def"].as<int32_t>();
	c.base_rec = r["base_rec"].as<int32_t>();
	c.ext_hp = r["ext_hp"].as<int32_t>();
	c.ext_atk = r["ext_atk"].as<int32_t>();
	c.ext_def = r["ext_def"].as<int32_t>();
	c.ext_rec = r["ext_rec"].as<int32_t>();
	c.bb_id = leadingInt(r["bb_id"].as<std::string>());
	c.bb_lvl = r["bb_lvl"].as<int32_t>();
	c.sbb_id = leadingInt(r["sbb_id"].as<std::string>());
	c.sbb_lvl = r["sbb_lvl"].as<int32_t>();
	c.sphere_1 = std::max(r["s1"].as<int32_t>(), 0);
	c.sphere_2 = std::max(r["s2"].as<int32_t>(), 0);
	c.fe_skill_info = r["fe_skill_info"].as<std::string>();
	c.elgif_skill_id = std::max(leadingInt(r["es2"].as<std::string>()), 0);
	return c;
}

} // namespace

drogon::Task<std::vector<PlayerCard>> loadOwnHelperCards(db::Database database, const std::string& userId)
{
	// The units in Squad 10, in on-screen order (Relations.hpp kHelperSquad); favourites play no
	// part.  $N must first appear in argument order (SQLite numbers them).
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.id, i.username, i.level, i.want_gift, COALESCE(i.friend_id, '') AS fid, 0 AS gid,"
		" COALESCE(g.last_login_at, 0) AS lla, CAST(u.unit_id AS TEXT) AS unit_id, u.unit_lvl,"
		" u.unit_type_id, u.element, u.base_hp, u.base_atk, u.base_def, u.base_rec,"
		" u.ext_hp, u.ext_atk, u.ext_def, u.ext_rec, u.bb_id, u.bb_lvl, u.sbb_id, u.sbb_lvl,"
		" COALESCE(u.eqip_item_id, 0) AS s1, COALESCE(u.eqip_item_id2, 0) AS s2, u.fe_skill_info,"
		" COALESCE(u.extra_passive_skill_id2, 0) AS es2, u.user_unit_id AS uuid"
		" FROM user_decks d"
		" JOIN user_info i ON i.id = d.user_id"
		" LEFT JOIN gumi_live_users g ON g.id = i.gumi_user_id"
		" JOIN user_units u ON u.user_id = d.user_id AND u.user_unit_id = d.user_unit_id"
		" WHERE d.user_id = $1 AND d.deck_type = 1 AND d.deck_num = $2"
		" ORDER BY d.disp_order LIMIT $3;",
		userId, kHelperSquad, kOwnHelperCount);
	std::vector<PlayerCard> out;
	for (const auto& r : rows)
	{
		auto card = cardFromRow(r);
		if (card.unit_id == 0)
			continue;
		card.user_id = ownHelperId(r["uuid"].as<int64_t>());
		out.push_back(std::move(card));
	}
	co_return out;
}

drogon::Task<std::optional<PlayerCard>> loadUnitCard(db::Database database, const std::string& userId,
	const std::string& userUnitId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT i.id, i.username, i.level, i.want_gift, COALESCE(i.friend_id, '') AS fid,"
		" COALESCE((SELECT gm.guild_id FROM user_guild_members gm WHERE gm.member_id = i.id"
		"  ORDER BY gm.joined_day LIMIT 1), 0) AS gid,"
		" COALESCE(g.last_login_at, 0) AS lla, CAST(u.unit_id AS TEXT) AS unit_id, u.unit_lvl,"
		" u.unit_type_id, u.element, u.base_hp, u.base_atk, u.base_def, u.base_rec,"
		" u.ext_hp, u.ext_atk, u.ext_def, u.ext_rec, u.bb_id, u.bb_lvl, u.sbb_id, u.sbb_lvl,"
		" COALESCE(u.eqip_item_id, 0) AS s1, COALESCE(u.eqip_item_id2, 0) AS s2, u.fe_skill_info,"
		" COALESCE(u.extra_passive_skill_id2, 0) AS es2"
		" FROM user_info i"
		" LEFT JOIN gumi_live_users g ON g.id = i.gumi_user_id"
		" JOIN user_units u ON u.user_id = i.id"
		" WHERE i.id = $1 AND u.user_unit_id = $2;",
		userId, userUnitId);
	if (rows.empty())
		co_return std::nullopt;
	auto card = cardFromRow(rows[0]);
	if (card.unit_id == 0)
		co_return std::nullopt;
	co_return card;
}

drogon::Task<bool> isRealPlayer(db::Database database, const std::string& userId)
{
	if (userId.empty())
		co_return false;
	const auto row = co_await database->execSqlCoro(
		"SELECT 1 FROM user_info WHERE id = $1;", userId);
	co_return !row.empty();
}

drogon::Task<std::vector<Relation>> loadRelations(db::Database database, const std::string& userId)
{
	std::vector<Relation> out;
	for (const auto& r : co_await database->execSqlCoro(
		"SELECT other_id, state, favorite, created_at FROM user_relations"
		" WHERE user_id = $1 ORDER BY created_at DESC;", userId))
	{
		out.push_back(Relation{ r["other_id"].as<std::string>(), r["state"].as<int32_t>(),
			r["favorite"].as<int32_t>(), r["created_at"].as<int64_t>() });
	}
	co_return out;
}

drogon::Task<std::optional<PlayerCard>> loadPlayerCard(db::Database database, const std::string& userId)
{
	// The leader of the ACTIVE party (deck_type 1 is what addDefaultDecks
	// writes for quest parties; member_type 0 is the leader).  ORDER BY falls
	// back to the strongest unit when the deck row is missing, so a player
	// whose deck was never saved still shows up with something real.
	//
	// The leader is looked up straight from the deck first: the one query that
	// did both joined every unit the player owns to the decks and sorted them,
	// 0.19 ms a card, and FriendGet builds one card per friend plus ten
	// strangers.  Only a player with no leader row pays for the full walk.
	static constexpr std::string_view kCardColumns =
		"SELECT i.id, i.username, i.level, i.want_gift, COALESCE(i.friend_id, '') AS fid,"
		" COALESCE((SELECT gm.guild_id FROM user_guild_members gm WHERE gm.member_id = i.id"
		"  ORDER BY gm.joined_day LIMIT 1), 0) AS gid,"
		" COALESCE(g.last_login_at, 0) AS lla, CAST(u.unit_id AS TEXT) AS unit_id, u.unit_lvl,"
		" u.unit_type_id, u.element, u.base_hp, u.base_atk, u.base_def, u.base_rec,"
		" u.ext_hp, u.ext_atk, u.ext_def, u.ext_rec, u.bb_id, u.bb_lvl, u.sbb_id, u.sbb_lvl,"
		" COALESCE(u.eqip_item_id, 0) AS s1, COALESCE(u.eqip_item_id2, 0) AS s2, u.fe_skill_info,"
		" COALESCE(u.extra_passive_skill_id2, 0) AS es2"
		" FROM user_info i"
		" LEFT JOIN gumi_live_users g ON g.id = i.gumi_user_id";
	auto rows = co_await database->execSqlCoro(std::string(kCardColumns) +
		" JOIN user_decks d ON d.user_id = i.id AND d.deck_type = 1"
		"  AND d.deck_num = i.active_deck AND d.member_type = 0"
		" JOIN user_units u ON u.user_unit_id = d.user_unit_id AND u.user_id = i.id"
		" WHERE i.id = $1"
		" ORDER BY u.unit_lvl DESC LIMIT 1;",
		userId);
	if (rows.empty())
		rows = co_await database->execSqlCoro(std::string(kCardColumns) +
			" JOIN user_units u ON u.user_id = i.id"
			" LEFT JOIN user_decks d ON d.user_id = i.id AND d.user_unit_id = u.user_unit_id"
			"  AND d.deck_type = 1 AND d.deck_num = i.active_deck AND d.member_type = 0"
			" WHERE i.id = $1"
			" ORDER BY (d.user_unit_id IS NOT NULL) DESC, u.unit_lvl DESC LIMIT 1;",
			userId);
	if (rows.empty())
		co_return std::nullopt;
	auto card = cardFromRow(rows[0]);
	if (card.unit_id == 0)
		co_return std::nullopt;
	co_return card;
}

drogon::Task<std::optional<std::string>> userIdForPublicId(db::Database database, const std::string& digits)
{
	const auto row = co_await database->execSqlCoro(
		"SELECT id FROM user_info WHERE friend_id = $1;", digits);
	if (row.empty())
		co_return std::nullopt;
	co_return row[0]["id"].as<std::string>();
}

drogon::Task<std::vector<PlayerCard>> unrelatedPlayers(
	db::Database database, const std::string& me, int32_t limit, bool recentFirst)
{
	// Anyone with ANY relation to me (friend or pending either way) is left
	// out: they are already in my list, and offering them again would only
	// produce a duplicate request.
	// Only the player's own pool: Sandbox and normal accounts never meet
	// (gme/common/Sandbox.hpp).
	const bool sandboxed = co_await sandbox::isOn(database, me);
	const std::string kStranger =
		" WHERE i.id <> $1"
		" AND NOT EXISTS (SELECT 1 FROM user_relations r WHERE r.user_id = $1 AND r.other_id = i.id)"
		" AND EXISTS (SELECT 1 FROM user_units u WHERE u.user_id = i.id)"
		+ sandbox::poolSql("i.id", sandboxed);
	drogon::orm::Result ids{nullptr};
	if (recentFirst)
	{
		// Newest login first, read off gumi_live_users_last_login and stopped at
		// the limit (CROSS JOIN keeps gumi_live_users the outer loop): sorting
		// every account instead cost 9 ms per FriendGet.  The full query below
		// still answers when this finds too few, e.g. accounts with no login row.
		ids = co_await database->execSqlCoro(
			"SELECT i.id FROM gumi_live_users g CROSS JOIN user_info i ON i.gumi_user_id = g.id"
			+ std::string(kStranger) + " ORDER BY g.last_login_at DESC LIMIT $2;",
			me, limit);
	}
	if (!recentFirst || ids.size() < static_cast<size_t>(std::max(limit, 0)))
	{
		const std::string order = recentFirst
			? " ORDER BY COALESCE(g.last_login_at, 0) DESC"
			: " ORDER BY ABS(i.level - (SELECT level FROM user_info WHERE id = $1)),"
			  " COALESCE(g.last_login_at, 0) DESC";
		ids = co_await database->execSqlCoro(
			"SELECT i.id FROM user_info i LEFT JOIN gumi_live_users g ON g.id = i.gumi_user_id"
			+ std::string(kStranger) + order + " LIMIT $2;",
			me, limit);
	}

	std::vector<PlayerCard> out;
	for (const auto& r : ids)
	{
		if (auto card = co_await loadPlayerCard(database, r["id"].as<std::string>()))
			out.push_back(std::move(*card));
	}
	co_return out;
}

drogon::Task<int32_t> friendCount(db::Database database, const std::string& userId)
{
	const auto row = co_await database->execSqlCoro(
		"SELECT (SELECT COUNT(*) FROM user_friends WHERE user_id = $1)"
		" + (SELECT COUNT(*) FROM user_relations WHERE user_id = $1 AND state = 1) AS n;",
		userId);
	co_return row[0]["n"].as<int32_t>();
}

drogon::Task<int32_t> friendCapacity(db::Database database, const std::string& userId)
{
	// The client's "n / cap": the level's allowance plus slots bought through
	// ShopUse type 7, built exactly as gme::getTeamInfo and recruitFriend do.
	const auto info = co_await database->execSqlCoro(
		"SELECT level, max_friend_count FROM user_info WHERE id = $1;", userId);
	if (info.empty())
		co_return 0;
	const auto levelMst = getLevelMst(info[0]["level"].as<uint32_t>());
	if (!levelMst)
		co_return 9999;   // no table loaded: do not invent a cap
	co_return levelMst->friend_count
		+ std::min(info[0]["max_friend_count"].as<int32_t>(), levelMst->add_friend_count);
}

drogon::Task<ApplyOutcome> sendRequest(db::Database database, const std::string& me, const std::string& other)
{
	if (me == other || !(co_await isRealPlayer(database, other)))
		co_return ApplyOutcome::NotFound;
	// Across the Sandbox line there is nobody to ask (gme/common/Sandbox.hpp).
	if (!(co_await sandbox::samePool(database, me, other)))
		co_return ApplyOutcome::NotFound;

	const auto state = co_await myState(database, me, other);
	if (state == kRelFriend)
		co_return ApplyOutcome::AlreadyFriends;
	if (state == kRelSent)
		co_return ApplyOutcome::AlreadyPending;
	if (state == kRelReceived)
	{
		// They already asked me: asking back is a yes (FRIENDS_SPEC §6.3).
		// Without room on both sides it just stays pending for Agree, which
		// is the one screen that can refuse cleanly.
		if (!(co_await hasRoom(database, me)) || !(co_await hasRoom(database, other)))
			co_return ApplyOutcome::AlreadyPending;
		co_await befriendBoth(database, me, other);
		co_return ApplyOutcome::AutoAccepted;
	}

	if ((co_await countState(database, me, kRelSent)) >= kMaxPendingOut
		|| (co_await countState(database, other, kRelReceived)) >= kMaxPendingIn)
		co_return ApplyOutcome::Limited;

	// Both rows in one statement, so there is never a half-made request.
	co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO user_relations (user_id, other_id, state, favorite, created_at)"
		" VALUES ($1, $2, 2, 0, $3), ($2, $1, 3, 0, $3);",
		me, other, nowSec());
	co_return ApplyOutcome::Sent;
}

drogon::Task<AgreeOutcome> acceptRequest(db::Database database, const std::string& me, const std::string& other)
{
	if ((co_await myState(database, me, other)) != kRelReceived)
		co_return AgreeOutcome::NoRequest;
	if (!(co_await hasRoom(database, me)))
		co_return AgreeOutcome::SelfFull;
	if (!(co_await hasRoom(database, other)))
		co_return AgreeOutcome::TargetFull;
	co_await befriendBoth(database, me, other);
	co_return AgreeOutcome::Accepted;
}

drogon::Task<int32_t> refuseOrCancel(db::Database database, const std::string& me, const std::vector<std::string>& ids)
{
	// Only PENDING pairs: the client's reject-all list can be stale
	// (FRIENDS_SPEC §4.4), and a stale id must never unfriend anyone.
	int32_t dropped = 0;
	for (const auto& id : ids)
	{
		const auto res = co_await database->execSqlCoro(
			std::string("DELETE FROM user_relations WHERE state IN (2, 3) AND ") + kPairWhere + ";",
			me, id);
		dropped += res.affectedRows() > 0 ? 1 : 0;
	}
	co_return dropped;
}

drogon::Task<bool> removeRelation(db::Database database, const std::string& me, const std::string& other)
{
	const auto res = co_await database->execSqlCoro(
		std::string("DELETE FROM user_relations WHERE ") + kPairWhere + ";", me, other);
	co_return res.affectedRows() > 0;
}

drogon::Task<bool> setRelationFavorite(db::Database database, const std::string& me, const std::string& other, bool on)
{
	const auto res = co_await database->execSqlCoro(
		"UPDATE user_relations SET favorite = $1 WHERE user_id = $2 AND other_id = $3 AND state = 1;",
		on ? 1 : 0, me, other);
	co_return res.affectedRows() > 0;
}

drogon::Task<void> expireStaleRequests(db::Database database, const std::string& me)
{
	// Both rows of a pair name `me` on one side or the other, so this one
	// statement removes whole pairs and never strands half of one.
	co_await database->execSqlCoro(
		"DELETE FROM user_relations WHERE state IN (2, 3) AND created_at < $1"
		" AND (user_id = $2 OR other_id = $2);",
		nowSec() - kPendingExpirySec, me);
}

::FriendInfo friendInfoFromCard(const PlayerCard& card, int32_t friendType, const Relation* rel)
{
	::FriendInfo fi{};
	fi.user_id = card.user_id;
	fi.handle_name = card.handle_name;
	fi.team_lv = card.level;
	fi.friend_type = friendType;
	fi.last_login_date = secondsSinceLogin(card.last_login_at);
	fi.unit_id = card.unit_id;
	fi.unit_lv = card.unit_lv;
	fi.base_hp = card.base_hp;
	fi.base_atk = card.base_atk;
	fi.base_def = card.base_def;
	fi.base_heal = card.base_rec;
	fi.ext_hp = card.ext_hp;
	fi.ext_atk = card.ext_atk;
	fi.ext_def = card.ext_def;
	fi.ext_heal = card.ext_rec;
	fi.skill_id = std::to_string(card.bb_id);
	fi.skill_lv = card.bb_lvl;
	fi.extra_skill_id = std::to_string(card.sbb_id);
	fi.extra_skill_lv = card.sbb_lvl;
	fi.unit_type_id = card.unit_type_id;
	fi.element = card.element;
	fi.equipitem_id = std::to_string(card.sphere_1);
	fi.equipitem_id2 = std::to_string(card.sphere_2);
	fi.friend_id = card.friend_id;
	// sD73jd20: a profile shows View Guild (GuildMemberGuildInfo) when it is
	// not 0.  mgNdrCEe is a string member, sent as it always was.
	fi.guild_id = card.guild_id;
	fi.ability_info = "0";
	fi.want_gift = padWantGift(card.want_gift); // three slots or GiftSendScene2 crashes
	fi.friend_message = "";
	fi.favorite = rel ? rel->favorite : 0;
	if (rel && (friendType == kRelSent || friendType == kRelReceived))
		fi.req_time = reqTimeString(rel->created_at);
	// priority > 1 hides a row from every list and counter; jkldTrhL is the
	// Ep3 summoner flag and must be 0 for the classic game (FRIENDS_SPEC §1.2).
	fi.priority = 1;
	fi.unk_jkld_trh_l = 0;
	return fi;
}

::ReinforcementInfo reinforcementFromCard(const PlayerCard& card, int32_t friendType, int32_t userUnitId)
{
	::ReinforcementInfo ri{};
	ri.user_id = card.user_id;
	ri.handle_name = card.handle_name;
	ri.team_lv = card.level;
	ri.target_lv = card.unit_lv;
	ri.friend_type = friendType;
	ri.last_login_date = secondsSinceLogin(card.last_login_at);
	// Not 0 shows View Guild on the mission-result profile.
	ri.guild_id = card.guild_id;
	ri.unit_id = card.unit_id;
	ri.base_hp = card.base_hp;
	ri.base_atk = card.base_atk;
	ri.base_def = card.base_def;
	ri.base_heal = card.base_rec;
	ri.ext_hp = card.ext_hp;
	ri.ext_atk = card.ext_atk;
	ri.ext_def = card.ext_def;
	ri.ext_heal = card.ext_rec;
	// The row gates and 6e4b7sQt gives the amount (see FriendPointInfo), so
	// a friend's row carries the friend rate and a stranger's the normal one.
	ri.friend_point = friendType == kRelFriend ? kHonorPerFriendHelper : kHonorPerNormalHelper;
	ri.normal_friend_point = kHonorPerNormalHelper;
	ri.skill_id = card.bb_id;
	ri.skill_lv = card.bb_lvl;
	ri.extra_skill_id = card.sbb_id;
	ri.extra_skill_lv = card.sbb_lvl;
	ri.unit_type_id = card.unit_type_id;
	ri.element = std::to_string(card.element);
	ri.user_unit_id = userUnitId;
	ri.equipitem_id = card.sphere_1;
	ri.equipitem_id2 = card.sphere_2;
	// The battle reads SP picks and all three Extra Skill slots off the helper
	// (getFeEffectiveSkillInfo, getExtraPassiveSkillIDList), and the unit's own
	// Extra Skill is MST data the client never fills itself (ExtraSkills.hpp).
	// Without them a borrowed unit fought weaker than on its owner's account.
	ri.fe_skill_info = card.fe_skill_info;
	ri.extra_passive_skill_id = unitOwnExtraSkill(static_cast<uint32_t>(card.unit_id));
	ri.extra_passive_skill_id2 = card.elgif_skill_id;
	ri.friend_id = leadingInt(card.friend_id);
	ri.priority = 1;
	return ri;
}

drogon::Task<std::vector<::FriendInfo>> buildSocialList(
	db::Database database, const UserIdentity& identity, std::vector<::FriendInfo> extra)
{
	const auto peak = co_await playerPeak(database, identity);
	const auto roster = co_await loadFriendRoster(database, identity, false);
	auto out = socialList(roster, peak);

	for (const auto& rel : co_await loadRelations(database, identity.userId))
	{
		if (const auto card = co_await loadPlayerCard(database, rel.other_id))
			out.push_back(friendInfoFromCard(*card, rel.state, &rel));
	}
	for (auto& row : extra)
		out.push_back(std::move(row));

	if (out.empty())
	{
		::FriendInfo sentinel{};
		sentinel.user_id = "0";
		sentinel.friend_type = 0;
		sentinel.priority = 2;
		sentinel.ability_info = "0"; // mgNdrCEe, as it was always sent
		out.push_back(std::move(sentinel));
	}
	co_return out;
}

drogon::Task<std::string> socialReply(
	db::Database database, const UserIdentity& identity, std::vector<::FriendInfo> extra)
{
	::FriendSocialResp resp{};
	resp.friend_info = co_await buildSocialList(database, identity, std::move(extra));
	resp.friend_point_info = friendPointInfo();

	std::string buffer;
	if (const auto ec = glz::write_json(resp, buffer); ec)
	{
		LOG_ERROR << "Relations: serialization error: " << glz::format_error(ec, buffer);
		co_return std::string("{}");
	}
	co_return buffer;
}

bool allowRate(const std::string& bucket, const std::string& userId, int32_t maxCalls, int64_t windowSec)
{
	static std::mutex mutex;
	static std::map<std::string, std::deque<int64_t>> calls;

	const std::lock_guard lock(mutex);
	auto& q = calls[bucket + ":" + userId];
	const auto now = nowSec();
	while (!q.empty() && q.front() <= now - windowSec)
		q.pop_front();
	if (static_cast<int32_t>(q.size()) >= maxCalls)
		return false;
	q.push_back(now);
	return true;
}

} // namespace gme
