#include "App.hpp"

#include <gimuserver/gme/common/Sandbox.hpp>

namespace gme::sandbox
{
drogon::Task<bool> isOn(const db::Database database, const std::string userId)
{
	if (userId.empty())
		co_return false;
	const auto rows = co_await database->execSqlCoro(
		"SELECT sandbox_at FROM user_info WHERE id = $1;", userId);
	co_return !rows.empty() && rows[0]["sandbox_at"].as<int64_t>() > 0;
}

drogon::Task<bool> samePool(const db::Database database, const std::string userId, const std::string otherId)
{
	// Two statements: GCC 13 ICEs on co_await inside an expression like this.
	const bool mine = co_await isOn(database, userId);
	const bool theirs = co_await isOn(database, otherId);
	co_return mine == theirs;
}

std::string poolSql(const std::string_view userColumn, const bool sandbox)
{
	std::string sql = " AND ";
	sql += userColumn;
	sql += sandbox ? " IN" : " NOT IN";
	sql += " (SELECT id FROM user_info WHERE sandbox_at > 0)";
	return sql;
}

const std::set<int32_t>& everyMission()
{
	// ServerCache is immutable once the server is up.
	static const std::set<int32_t> missions = [] {
		std::set<int32_t> out;
		for (const auto& mission : theServer()->cache().missionMst())
			out.insert(mission.id);
		return out;
	}();
	return missions;
}
}
