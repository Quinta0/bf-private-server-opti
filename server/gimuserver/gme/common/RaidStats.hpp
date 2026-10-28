#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <cstdint>
#include <string>

// Classic Raid: the player's own raid records, shown by
// RaidPlayerInfoResultScene 1209 through RaidGetPlayerInfo `81zkMjfN`
// (RAIDS_ROUND2_MULTIPLAYER.md 4.7).
//
// ONE ROW PER COUNTER, KEYED BY ITS WIRE KEY.  raid_user_stat(user_id, stat,
// value) stores each RaidUserTeamArchiveInfo (`6nqGvdY1`) counter under the
// key the client reads it with, so adding a counter later is a new constant
// and nothing else: no migration, no column.  A missing row reads as 0, which
// is what a player who never raided must see anyway.
//
// This band only reads.  The mission band writes (Challenge at MissionStart,
// Win / RcNWin / SingleWin / CooperationWin at a clear, BossFinishCnt for the
// kill credit of 5.1, ...) through the three writers below, each one a single
// upsert so it is safe inside the caller's transaction.
namespace gme::raid
{

/*!
* RaidUserTeamArchiveInfo keys (RaidUserTeamArchiveInfoResponse::readParam
* kmap, OBSERVED).  The comment after each is the client's setter name.
*/
namespace stat
{
inline constexpr const char* kChallenge = "T7BvL1kh";            // Challenge: runs started
inline constexpr const char* kWin = "8CEu9Kcm";                  // Win: runs won
inline constexpr const char* kRc1Win = "58rGsYvL";               // Rc1win: wins on an RC 1 mission
inline constexpr const char* kRc2Win = "2dVBPNQ5";               // Rc2win
inline constexpr const char* kRc3Win = "ai0gh9Vo";               // Rc3win
inline constexpr const char* kRc4Win = "UNycbmV0";               // Rc4win
inline constexpr const char* kRc5Win = "x0s78E4G";               // Rc5win
inline constexpr const char* kRc6Win = "y6fVdTch";               // Rc6win
inline constexpr const char* kItemHarvest = "Js8Amd2N";          // ItemHarvest
inline constexpr const char* kCrystalBMax = "0LwvAF3H";          // CrystalBMax (keep max)
inline constexpr const char* kCrystalHMax = "rQ3TAy6I";          // CrystalHMax (keep max)
inline constexpr const char* kCrystalB = "hoG2ieT5";             // CrystalB
inline constexpr const char* kCrystalH = "6PLsn8xo";             // CrystalH
inline constexpr const char* kBattleTurnMaxDamage = "5NRJQ1LU";  // BattleTurnMaxDamage (keep max)
inline constexpr const char* kBattleTurnMaxSpark = "XP06YWdT";   // BattleTurnMaxSpark (keep max)
inline constexpr const char* kOverkillCnt = "Wnc6A31E";          // OverkillCnt
inline constexpr const char* kBattleDamage = "kqI45VU1";         // BattleDamage
inline constexpr const char* kSparkCnt = "kHbYZ9w1";             // SparkCnt
inline constexpr const char* kSkillCnt = "4vx39EIB";             // SkillCnt
inline constexpr const char* kItemMixCnt = "c3Bo97kI";           // ItemMixCnt
inline constexpr const char* kRaidItemUse = "1VbdgX76";          // RaidItemUse
inline constexpr const char* kEscapeCnt = "E5S7XgR6";            // EscapeCnt
inline constexpr const char* kBossFinishCnt = "bprW7k3f";        // BossFinishCnt
inline constexpr const char* kSingleWin = "D8EZfi4p";            // SingleWin
inline constexpr const char* kCooperationWin = "nLb68MSq";       // CooperationWin
inline constexpr const char* kFastestPlaytime = "54imIPa2";      // FastestPlaytime (keep min non-zero)
inline constexpr const char* kPlaytime = "nbu1dyP5";             // Playtime
inline constexpr const char* kCampRecover = "b28wpmvq";          // CampRecover
/*! In the reader's kmap but its setter is unresolved (MULTIPLAYER 8.8); nothing writes it, so it reads "0". */
inline constexpr const char* kUnresolvedB5 = "b5LZj7fy";

/*! The Rc<N>win key for a mission of Raid Class `rc`, nullptr outside 1..6. */
inline const char* rcWin(int32_t rc)
{
	constexpr const char* keys[] = { kRc1Win, kRc2Win, kRc3Win, kRc4Win, kRc5Win, kRc6Win };
	return rc >= 1 && rc <= 6 ? keys[rc - 1] : nullptr;
}
} // namespace stat

/*!
* Add `delta` to a counter.  Unknown keys and non-positive deltas are ignored
* (counters only grow; a typo must not mint a junk row).
*/
drogon::Task<void> bumpStat(const db::Database database, const std::string userId, const char* stat, const int64_t delta = 1);

/*! Keep the larger of the stored value and `value` (the *Max records). */
drogon::Task<void> raiseStat(const db::Database database, const std::string userId, const char* stat, const int64_t value);

/*!
* Keep the smaller NON-ZERO of the stored value and `value` (FastestPlaytime):
* 0 means "no record yet", so it must never win the comparison.  A
* non-positive `value` is ignored.
*/
drogon::Task<void> lowerStat(const db::Database database, const std::string userId, const char* stat, const int64_t value);

/*! The `6nqGvdY1` node for `userId`: every counter, missing rows as 0. Never fails. */
drogon::Task<::RaidUserTeamArchiveInfo> archiveInfo(const db::Database database, const std::string userId);

} // namespace gme::raid
