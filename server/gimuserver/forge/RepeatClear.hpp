#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*!
* Mission reward lists: first-clear (mission_mst SiYs27Cj) and the Balance
* Forge's repeat-clear rewards (S2ChOKo6), both paid as presents.
*
* One grammar for both (the Forge server design, section 3.2):
*
*     list   = [ "daily:" N "," ] reward { "," reward }      (daily: repeat list only)
*     reward = type ":" id ":" amount ":0:0" [ ":" elgifSkill ]
*
* The client's F_MISSION_MST keeps S2ChOKo6 empty (it parses the column with
* setRepeatClearBonus, format unverified), so a repeat reward only ever comes
* from the server's own mission_mst copy, written through the mission_mst
* overlay, whose check (MstChecks rewardListError) already proved it parses.
*/
namespace forge
{

/*! One reward.  `id` is empty for a currency; `extraSkill` is an Elgif unit's skill (#38). */
struct ClearReward
{
	int32_t type = 0;
	std::string id;
	int32_t amount = 0;
	int32_t extraSkill = 0;
};

/*! A reward list; a malformed entry (or the daily: prefix) is skipped. */
std::vector<ClearReward> parseClearRewards(std::string_view raw);

/*! A mission's repeat-clear rewards. */
struct RepeatRewards
{
	int32_t dailyLimit = 0;                //!< repeat clears per game day that pay; 0 = every one
	std::vector<ClearReward> rewards;
};

/*! S2ChOKo6 read: nullopt when it names no reward. */
std::optional<RepeatRewards> parseRepeatRewards(std::string_view raw);

/*! Boot (ServerCache's mission_mst pass): one mission's S2ChOKo6, empty ones ignored. */
void noteRepeatRewards(int32_t missionId, std::string_view raw);

/*! What a repeat clear of the mission pays, or null.  Boot-immutable. */
const RepeatRewards* repeatRewards(int32_t missionId);

/*! The shop name the daily cap counts in, in user_exchange_purchases. */
inline constexpr const char* kRepeatClearShop = "repeat-clear";

} // namespace forge
