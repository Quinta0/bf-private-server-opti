#include "App.hpp"

#include <gimuserver/gme/common/RaidStats.hpp>

#include <algorithm>
#include <limits>
#include <string_view>

namespace gme::raid
{

namespace
{

/*! Where each stored counter lands in the reply; every counter is an int64. */
using StatField = int64_t (::RaidUserTeamArchiveInfo::*);

struct StatSlot
{
	const char* key;
	StatField field;
};

// The single list of known counters: archiveInfo fills from it and the writers
// accept only its keys.  Order follows the client's kmap (4.7).
constexpr StatSlot kSlots[] = {
	{ stat::kChallenge, &::RaidUserTeamArchiveInfo::challenge },
	{ stat::kWin, &::RaidUserTeamArchiveInfo::win },
	{ stat::kRc1Win, &::RaidUserTeamArchiveInfo::rc1_win },
	{ stat::kRc2Win, &::RaidUserTeamArchiveInfo::rc2_win },
	{ stat::kRc3Win, &::RaidUserTeamArchiveInfo::rc3_win },
	{ stat::kRc4Win, &::RaidUserTeamArchiveInfo::rc4_win },
	{ stat::kRc5Win, &::RaidUserTeamArchiveInfo::rc5_win },
	{ stat::kRc6Win, &::RaidUserTeamArchiveInfo::rc6_win },
	{ stat::kItemHarvest, &::RaidUserTeamArchiveInfo::item_harvest },
	{ stat::kCrystalBMax, &::RaidUserTeamArchiveInfo::crystal_b_max },
	{ stat::kCrystalHMax, &::RaidUserTeamArchiveInfo::crystal_h_max },
	{ stat::kCrystalB, &::RaidUserTeamArchiveInfo::crystal_b },
	{ stat::kCrystalH, &::RaidUserTeamArchiveInfo::crystal_h },
	{ stat::kBattleTurnMaxDamage, &::RaidUserTeamArchiveInfo::battle_turn_max_damage },
	{ stat::kBattleTurnMaxSpark, &::RaidUserTeamArchiveInfo::battle_turn_max_spark },
	{ stat::kOverkillCnt, &::RaidUserTeamArchiveInfo::overkill_cnt },
	{ stat::kBattleDamage, &::RaidUserTeamArchiveInfo::battle_damage },
	{ stat::kSparkCnt, &::RaidUserTeamArchiveInfo::spark_cnt },
	{ stat::kSkillCnt, &::RaidUserTeamArchiveInfo::skill_cnt },
	{ stat::kItemMixCnt, &::RaidUserTeamArchiveInfo::item_mix_cnt },
	{ stat::kRaidItemUse, &::RaidUserTeamArchiveInfo::raid_item_use },
	{ stat::kEscapeCnt, &::RaidUserTeamArchiveInfo::escape_cnt },
	{ stat::kBossFinishCnt, &::RaidUserTeamArchiveInfo::boss_finish_cnt },
	{ stat::kSingleWin, &::RaidUserTeamArchiveInfo::single_win },
	{ stat::kCooperationWin, &::RaidUserTeamArchiveInfo::cooperation_win },
	{ stat::kFastestPlaytime, &::RaidUserTeamArchiveInfo::fastest_playtime },
	{ stat::kPlaytime, &::RaidUserTeamArchiveInfo::playtime },
	{ stat::kCampRecover, &::RaidUserTeamArchiveInfo::camp_recover },
	{ stat::kUnresolvedB5, &::RaidUserTeamArchiveInfo::unk_b5_lzj7_fy },
};

const StatSlot* findSlot(std::string_view key)
{
	for (const auto& slot : kSlots)
	{
		if (key == slot.key)
			return &slot;
	}
	return nullptr;
}

/*!
* A writer's guard: a known key for a real caller.  An unknown key is a
* programming error in the caller, so it is logged rather than stored.
*/
bool writable(const std::string& userId, const char* stat, const char* writer)
{
	if (stat == nullptr || findSlot(stat) == nullptr)
	{
		LOG_WARN << "Raid stats: " << writer << " ignored unknown counter '"
			<< (stat ? stat : "(null)") << "' for " << userId;
		return false;
	}
	return !userId.empty();
}

/*!
* Every counter is read by an int setter on the client (INFERRED from the kmap
* types), so a 64-bit total such as BattleDamage after years of play would wrap
* there.  Clamp on the way out; the stored value stays exact.
*/
int64_t wireValue(int64_t stored)
{
	return std::clamp<int64_t>(stored, 0, std::numeric_limits<int32_t>::max());
}

} // namespace

// Each writer is ONE upsert: no read-modify-write that a concurrent request on
// the shared connection could interleave with.  `excluded.value` is the row the
// INSERT tried to add, so no placeholder repeats (binding is by order).

drogon::Task<void> bumpStat(const db::Database database, const std::string userId, const char* stat, const int64_t delta)
{
	if (delta <= 0 || !writable(userId, stat, "bumpStat"))
		co_return;
	co_await database->execSqlCoro(
		"INSERT INTO raid_user_stat(user_id, stat, value) VALUES ($1, $2, $3)"
		" ON CONFLICT(user_id, stat) DO UPDATE SET value = value + excluded.value;",
		userId, std::string(stat), delta);
}

drogon::Task<void> raiseStat(const db::Database database, const std::string userId, const char* stat, const int64_t value)
{
	// A missing row already reads 0, so a non-positive record changes nothing.
	if (value <= 0 || !writable(userId, stat, "raiseStat"))
		co_return;
	co_await database->execSqlCoro(
		"INSERT INTO raid_user_stat(user_id, stat, value) VALUES ($1, $2, $3)"
		" ON CONFLICT(user_id, stat) DO UPDATE SET value = MAX(value, excluded.value);",
		userId, std::string(stat), value);
}

drogon::Task<void> lowerStat(const db::Database database, const std::string userId, const char* stat, const int64_t value)
{
	// 0 is "no record yet": storing it would make every later time lose.
	if (value <= 0 || !writable(userId, stat, "lowerStat"))
		co_return;
	co_await database->execSqlCoro(
		"INSERT INTO raid_user_stat(user_id, stat, value) VALUES ($1, $2, $3)"
		" ON CONFLICT(user_id, stat) DO UPDATE SET value ="
		" CASE WHEN value <= 0 THEN excluded.value ELSE MIN(value, excluded.value) END;",
		userId, std::string(stat), value);
}

drogon::Task<::RaidUserTeamArchiveInfo> archiveInfo(const db::Database database, const std::string userId)
{
	// Value-initialised: the generated struct has no member defaults, and every
	// counter without a row must go out as "0".
	::RaidUserTeamArchiveInfo info{};
	info.user_id = userId;
	const auto rows = co_await database->execSqlCoro(
		"SELECT stat, value FROM raid_user_stat WHERE user_id = $1;", userId);
	for (const auto& row : rows)
	{
		const auto* slot = findSlot(row["stat"].as<std::string>());
		if (slot == nullptr)
			continue;   // a retired key left in the table: not on the wire
		info.*(slot->field) = wireValue(row["value"].as<int64_t>());
	}
	co_return info;
}

} // namespace gme::raid
