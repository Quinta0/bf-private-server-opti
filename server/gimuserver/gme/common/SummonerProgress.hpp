#pragma once

#include <gimuserver/db/Types.h>

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <string>

// SUMMONER PROGRESSION — what a cleared quest pays the Summoner Avatar.
//
// The rates are the game's own, from F_MISSION_EP3_MST (mission_ep3_mst.json),
// one packed row per Chapter 3 quest: "EXP:500,SP:1,ELEM_EXP_RATE:3,
// ARM_EXP_RATE:10".  The help text says the rest: "In Chapter 3 of Brave
// Frontier, the Summoner himself or herself will level up ... as a reward for
// completing Quests", a level-up pays Summoner SP, and Summoning Arts EXP goes
// to "the Summoner's current element" only.
//
// ⚠ BOTH LEVEL TABLES ARE CUMULATIVE THRESHOLDS, unlike UserLevelMst's
// per-level chunks.  SummonerUnitUtils::getRestSummonerUnitExp @0x11D3118
// shows the bar as SummonerLevelMst[level + 1].exp - UserSummonerInfo.exp, and
// getRestElementExp @0x11D3608 does the same for SummonerElementLevelMst, so
// user_summoner.exp / expN are lifetime totals and the level is the highest
// row the total has reached.  SummonerElementLevelMst drops back at 41/61/81
// (3501 -> 2500); levels are walked in order, so a total past a tier's end
// simply carries on into the next tier and the bar never reads negative.

namespace gme
{
struct UserIdentity;

/*! What one cleared quest pays, before level-up SP. */
struct SummonerGain
{
	int64_t exp = 0;            //!< Summoner EXP, first-clear bonus included
	int64_t expClearBonus = 0;  //!< the FIRST_CLEAR_EXP part of `exp`
	int64_t elemExp = 0;        //!< Summoning Arts EXP for the current element
	int64_t armExp = 0;         //!< EXP for the equipped Summoner Arm
	int64_t sp = 0;             //!< SP the quest pays itself
	int32_t summonSlot = 0;     //!< Summoning Pedestals this clear unlocks (0 = none)
	int64_t stPoints = 0;       //!< Summoner Training Points (a Chapter 3 helper's payout)

	bool empty() const
	{
		return exp <= 0 && elemExp <= 0 && armExp <= 0 && sp <= 0 && summonSlot <= 0 && stPoints <= 0;
	}
};

/*! Before/after of one credit, for the reply. */
struct SummonerCredit
{
	SummonerGain gain;
	int64_t levelUpSp = 0;
	int32_t beforeLevel = 1, afterLevel = 1;
	int64_t beforeExp = 0;
	int32_t element = 1;
	int32_t beforeElemLevel = 1, afterElemLevel = 1;
	int64_t beforeElemExp = 0;
	int32_t armLevel = 1;
	int64_t beforeArmExp = 0;
	bool armCredited = false;
};

/*!
* The Chapter 3 row for a quest, or nothing for a quest outside Chapter 3.
*
* ELEM_EXP / ARM_EXP are flat; ELEM_EXP_RATE / ARM_EXP_RATE are read as a
* percentage of the quest's base EXP (the flat rows sit at 12% / 100% of
* theirs, the story rows at 2-5% / 5-20%).  FIRST_CLEAR_EXP is added on the
* first clear only and does not feed the rates.
*/
std::optional<SummonerGain> ep3QuestGain(int32_t missionId, bool firstClear);

/*!
* What a quest OUTSIDE Chapter 3 pays.  Not the original's rule: there, only
* Chapter 3 quests paid the Summoner, and Chapter 3 is not playable here yet.
* The owner's call (2026-10-23): normal quests pay "quite a bit less" so the
* grind is not wasted but Chapter 3 stays the way to max out.  So a quest pays
* 1 Summoner EXP per 3,200 lifetime player EXP, which takes a player who only
* plays normal quests to about Summoner Lv 112 of 500 at player Lv 999, and
* 1% / 3% of that as Summoning Arts / arm EXP (Arts Lv ~23 of 100).  The
* level-ups then pay SummonerLevelMst's own SP.
*
* Paid as the step in floor(lifetime / rate) across the quest, not as
* floor(earned / rate): most quests earn under 3,200 EXP and would round to
* nothing, and this way live play lands exactly where the one-time backfill
* (scripts/gen_summoner_backfill.py, same rates) puts an account.
*
* @param lifetimeBefore lifetimePlayerExp before the quest's EXP.
* @param lifetimeAfter  lifetimePlayerExp after it.
*/
SummonerGain regularQuestGain(int64_t lifetimeBefore, int64_t lifetimeAfter);

/*! Lifetime player EXP: UserLevelMst chunks up to `level` plus the residual. */
int64_t lifetimePlayerExp(uint32_t level, uint32_t exp);

/*! Lifetime player EXP per Summoner EXP for regularQuestGain. */
inline constexpr int64_t kPlayerExpPerSummonerExp = 3'200;

/*!
* Whether a quest is Chapter 3 (has a mission_ep3_mst row).  Its Summoner Helper
* pays Summoner Training Points, not Honor: "You can gain Summoner Training (ST)
* Points by taking your Friends as Summoner Helpers in battle ... In Brave
* Frontier's Chapter 3, you will not gain Honor Points" (help text).
*/
bool isChapter3Quest(int32_t missionId);

/*! Highest Summoner level a lifetime EXP total reaches (SummonerLevelMst). */
int32_t summonerLevelFor(int64_t exp);

/*! Highest Summoning Arts level an element's EXP total reaches. */
int32_t elementLevelFor(int32_t element, int64_t exp);

/*! SP SummonerLevelMst pays for levels (from, to]. */
int64_t summonerLevelUpSp(int32_t from, int32_t to);

/*!
* Credits a gain to user_summoner (and the equipped arm), in the caller's
* transaction.
*
* Levels follow the totals, a level-up pays SummonerLevelMst.get_sp, SP is
* capped at 99,999 like every other SP grant.  Arm EXP lands only on an arm
* the player already holds a user_summoner_arms row for, and arm levels do not
* move: F_SU_ARM_LEVEL_MST was never ported, so there is no curve to read.
*/
drogon::Task<SummonerCredit> creditSummoner(
	db::Database database, UserIdentity identity, SummonerGain gain);

/*!
* Seeds the Chapter 3 squads (deck_type 4) for an account that has none.
*
* UserPartyDeckList::addObject @0x129ACAC files deck_type 4 into the list
* isEp3 / getTypeList read in Summoner's Home, and falls back to the normal
* squads while that list is empty, so without these rows Summoner's Road
* fought with the normal leader and no Summoner.  The leader slot is the
* Summoner Avatar: user_unit_id "-1" (UserPartyInfo::isSummoner @0x129DC84,
* "You cannot alter your Summoner Unit"), member_type 0 at the centre
* (disp_order 2), in each of the ten squads.  Units are added by the player.
*/
drogon::Task<void> seedSummonerDecks(db::Database database, std::string userId);

} // namespace gme
