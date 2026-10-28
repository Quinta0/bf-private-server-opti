#include "App.hpp"

#include <gimuserver/gme/common/RaidBlackList.hpp>
#include <gimuserver/gme/common/RaidField.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <limits>

namespace gme::raid
{

namespace
{

// RaidUpdateBlackListRequest::createBody @0x13C86D0.
constexpr const char* kEditGroup = "kKpAdqAu";
constexpr const char* kTargetKey = "KpTZlmkh";
constexpr const char* kOpKey = "mnZ5K4Ii";

/*!
* FriendInfo.friend_type is effectively binary on the client (only 1 means
* "friend", FriendInfoList::existTypeOK @0x12607CC); 0 keeps a blocked player
* from ever reading as a friend wherever the row ends up.
*/
constexpr int32_t kBlockedType = 0;

/*!
* A target with no unit to lead with (loadPlayerCard has nothing to show):
* name and level only, so the row still lists and can still be removed.
*/
::FriendInfo bareRow(const std::string& userId, const std::string& name, int32_t level)
{
	::FriendInfo row{};
	row.user_id = userId;
	row.handle_name = name;
	row.team_lv = level;
	row.friend_type = kBlockedType;
	row.ability_info = "0"; // mgNdrCEe, as it was always sent
	// priority > 1 hides a row from every FriendInfo list (Relations.cpp).
	row.priority = 1;
	return row;
}

drogon::Task<BlackListResult> addTarget(const db::Database database, const std::string& userId, const std::string& targetId)
{
	if (!(co_await gme::isRealPlayer(database, targetId)))
		co_return BlackListResult::Invalid;

	// The cap is checked by the INSERT itself, so two adds racing on the
	// shared connection cannot both squeeze past it.  Only targets that still
	// exist count, matching what blackListRows shows: a deleted account must
	// not hold an invisible slot.
	const auto inserted = co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO raid_blacklist(user_id, target_user_id, created_at)"
		" SELECT $1, $2, $3 WHERE (SELECT COUNT(*) FROM raid_blacklist b"
		" JOIN user_info u ON u.id = b.target_user_id WHERE b.user_id = $4) < $5;",
		userId, targetId, nowSeconds(), userId, kBlacklistMax);
	if (inserted.affectedRows() > 0)
		co_return BlackListResult::Ok;

	// Nothing written: either already listed (Ok, idempotent even at the cap)
	// or the list is full.
	const auto listed = co_await database->execSqlCoro(
		"SELECT 1 FROM raid_blacklist WHERE user_id = $1 AND target_user_id = $2;",
		userId, targetId);
	co_return listed.empty() ? BlackListResult::Full : BlackListResult::Ok;
}

} // namespace

BlackListEdit readBlackListEdit(const RaidRequest& req)
{
	const auto* node = req.node(kEditGroup);
	const auto op = RaidRequest::numberOf(node, kOpKey);
	BlackListEdit edit{};
	edit.targetUserId = RaidRequest::textOf(node, kTargetKey);
	// Out of range is an unknown op, never a value that wraps onto 1 or 3.
	const bool fits = op >= std::numeric_limits<int32_t>::min() && op <= std::numeric_limits<int32_t>::max();
	edit.op = fits ? static_cast<int32_t>(op) : 0;
	return edit;
}

drogon::Task<BlackListResult> updateBlackList(const db::Database database, const UserIdentity identity,
	const std::string targetUserId, const int32_t op)
{
	// 2 is never sent; anything but add/remove is a no-op success (4.9).
	if (op != kBlackListOpAdd && op != kBlackListOpRemove)
		co_return BlackListResult::Ok;
	if (identity.userId.empty() || targetUserId.empty() || targetUserId == identity.userId)
	{
		LOG_WARN << "Raid blacklist: " << identity.userId << " sent op " << op
			<< " for invalid target '" << targetUserId << "'";
		co_return BlackListResult::Invalid;
	}

	if (op == kBlackListOpRemove)
	{
		// No existence check: removing a row whose account is gone is
		// harmless, and an absent row makes this a no-op (idempotent).
		co_await database->execSqlCoro(
			"DELETE FROM raid_blacklist WHERE user_id = $1 AND target_user_id = $2;",
			identity.userId, targetUserId);
		LOG_INFO << "Raid blacklist: " << identity.userId << " unblocked " << targetUserId;
		co_return BlackListResult::Ok;
	}

	const auto result = co_await addTarget(database, identity.userId, targetUserId);
	// Braces on purpose: the LOG_* macros may expand to an `if`, which would
	// capture a bare `else`.
	if (result == BlackListResult::Ok)
	{
		LOG_INFO << "Raid blacklist: " << identity.userId << " blocked " << targetUserId;
	}
	else
	{
		LOG_WARN << "Raid blacklist: " << identity.userId << " could not block " << targetUserId
			<< (result == BlackListResult::Full ? " (list full)" : " (no such player)");
	}
	co_return result;
}

drogon::Task<std::vector<::FriendInfo>> blackListRows(const db::Database database, const UserIdentity identity)
{
	// The JOIN drops targets whose account is gone; LIMIT only guards, since
	// the add path already keeps the list at kBlacklistMax.
	const auto rows = co_await database->execSqlCoro(
		"SELECT b.target_user_id, u.username, u.level FROM raid_blacklist b"
		" JOIN user_info u ON u.id = b.target_user_id"
		" WHERE b.user_id = $1 ORDER BY b.created_at DESC, b.target_user_id LIMIT $2;",
		identity.userId, kBlacklistMax);

	std::vector<::FriendInfo> out;
	out.reserve(rows.size());
	for (const auto& row : rows)
	{
		const auto targetId = row["target_user_id"].as<std::string>();
		// The same public card the Social menu draws for a real player (name,
		// level, active leader), so the detail scene shows who it is.
		if (const auto card = co_await gme::loadPlayerCard(database, targetId))
			out.push_back(gme::friendInfoFromCard(*card, kBlockedType, nullptr));
		else
			out.push_back(bareRow(targetId, row["username"].as<std::string>(), row["level"].as<int32_t>()));
	}
	co_return out;
}

drogon::Task<std::set<std::string>> blackListLinks(const db::Database database, const std::string userId)
{
	std::set<std::string> out;
	if (userId.empty())
		co_return out;
	// Either direction: the player hides rooms of people they blocked AND of
	// people who blocked them.
	const auto rows = co_await database->execSqlCoro(
		"SELECT target_user_id AS other FROM raid_blacklist WHERE user_id = $1"
		" UNION SELECT user_id AS other FROM raid_blacklist WHERE target_user_id = $2;",
		userId, userId);
	for (const auto& row : rows)
		out.insert(row["other"].as<std::string>());
	co_return out;
}

} // namespace gme::raid
