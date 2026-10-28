#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Friends.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Friends between REAL players: requests, the social graph, and the cards
// other players see.  docs/features/FRIENDS_SPEC.md is the design; the NPC
// roster in Friends.hpp is untouched and keeps the helper picker full on a
// small server.
//
// ONE RELATION, TWO ROWS.  user_relations holds a row per side, and its state
// IS the client's FriendInfo.friend_type for that side: 1 friend, 2 request
// sent, 3 request received.  Every write touches both rows in ONE statement so
// a crash can never leave a half-made relation.
//
// THE SOCIAL LIST IS A FULL REPLACE.  FriendInfoResponse::readParam
// @0x13D93C0 clears FriendInfoList on row 0, so every reply goes through
// buildSocialList and carries everything: the NPC roster, every relation, and
// any type-4 search rows.  A handler that sent only what it changed would
// silently delete the rest on the client.
namespace gme
{

/*! FriendInfo.friend_type values this module emits (FRIENDS_SPEC §1.2). */
inline constexpr int32_t kRelFriend = 1;
inline constexpr int32_t kRelSent = 2;
inline constexpr int32_t kRelReceived = 3;
inline constexpr int32_t kRelSearchRow = 4;

/*! AUTHORED limits (FRIENDS_SPEC §6.5). */
inline constexpr int32_t kMaxPendingOut = 30;
inline constexpr int32_t kMaxPendingIn = 50;
inline constexpr int64_t kPendingExpirySec = 14 * 24 * 3600;
inline constexpr int32_t kRandomSearchCount = 20;
inline constexpr int32_t kRealStrangerCount = 10;

/*!
* First reserved picker user_unit_id for a REAL player's helper card.
*
* Never the lender's real user_unit_id: two lenders could be in one picker and
* the client keys cards on it.  Clear of the roster (990000), encounter
* (990900) and NPC stranger (991000) blocks.
*/
inline constexpr int32_t kRealHelperUnitIdBase = 992000;

/*!
* The player's OWN units in their helper picker (owner's go, 2026-10-20, on the
* "Use own unit instead of friend helper" suggestion), at the top of the list.
*
* HELPER SQUAD ONLY (ticket #98, 2026-10-22; first asked for on Discord,
* "select specific units as friend helper"): the own helper cards are exactly the
* units in Squad 10 (kHelperSquad), in the squad's on-screen order, up to
* kOwnHelperCount.  Favourites no longer matter, and there is no fallback to
* them: a squad is never empty (addDefaultDecks seeds the starter as leader of
* all ten), so a player who never edits Squad 10 offers that starter.  MissionStart
* refuses a card whose unit is already in the squad that fights (ownHelperUnitId).
* The list reaches the
* client with FriendGet and the UnitFavorite/UnitMix/UnitEvo replies, not with
* DeckEdit: a DeckEdit can come from Manage Squad inside mission check, which
* keeps a raw pointer to the picked card that a new list would free.
*
* The client keeps the pick as a user id (UserState::setReinforcementUserID)
* and finds the card again with ReinforcementInfoList::getObjectWithUserID, so
* each card needs an id of its own: ownHelperId(user_unit_id), which no player,
* NPC band or stranger encoding uses.  MissionStart records such a helper as a
* solo run (no Honor, nobody to credit), and a friend request the post-battle
* prompt sends to one adds nobody (gme::recruitFriend cannot decode it).
*/
inline constexpr int32_t kOwnHelperCount = 10;
inline constexpr int32_t kHelperSquad = 9;  // user_decks.deck_num of "Squad 10"
inline constexpr int32_t kOwnHelperUnitIdBase = 993000;
inline constexpr std::string_view kOwnHelperPrefix = "MYUNIT";

inline std::string ownHelperId(int64_t userUnitId)
{
	return std::string(kOwnHelperPrefix) + std::to_string(userUnitId);
}

inline bool isOwnHelperId(const std::string& id)
{
	return id.size() > kOwnHelperPrefix.size() && id.compare(0, kOwnHelperPrefix.size(), kOwnHelperPrefix) == 0;
}

/*! The user_unit_id an ownHelperId names, or nullopt for anything that is not exactly MYUNIT<digits>. */
inline std::optional<int64_t> ownHelperUnitId(const std::string& id)
{
	if (!isOwnHelperId(id))
		return std::nullopt;
	const char* first = id.data() + kOwnHelperPrefix.size();
	const char* last = id.data() + id.size();
	int64_t value = 0;
	const auto parsed = std::from_chars(first, last, value);
	if (parsed.ec != std::errc{} || parsed.ptr != last)
		return std::nullopt;
	return value;
}

/*! What other players see of someone: name, level and their party leader. */
struct PlayerCard
{
	std::string user_id;
	std::string handle_name;
	std::string friend_id;
	std::string want_gift;
	int32_t guild_id = 0;   // their guild, 0 for none
	int32_t level = 1;
	int64_t last_login_at = 0;
	int32_t unit_id = 0;
	int32_t unit_lv = 1;
	int32_t unit_type_id = 1;
	int32_t element = 1;
	int32_t base_hp = 0, base_atk = 0, base_def = 0, base_rec = 0;
	int32_t ext_hp = 0, ext_atk = 0, ext_def = 0, ext_rec = 0;
	int32_t bb_id = 0, bb_lvl = 1, sbb_id = 0, sbb_lvl = 1;
	int32_t sphere_1 = 0, sphere_2 = 0;
	std::string fe_skill_info;   // the leader's SP enhancements (user_units.fe_skill_info)
	int32_t elgif_skill_id = 0;  // the Extra Skill an Elgif gave it (user_units.extra_passive_skill_id2)
};

/*! This side's row of one relation. */
struct Relation
{
	std::string other_id;
	int32_t state = 0;
	int32_t favorite = 0;
	int64_t created_at = 0;
};

enum class ApplyOutcome { Sent, AlreadyPending, AlreadyFriends, AutoAccepted, Limited, NotFound };
enum class AgreeOutcome { Accepted, NoRequest, SelfFull, TargetFull };

/*! Whether this id names a real account (user_info), as opposed to an NPC id. */
drogon::Task<bool> isRealPlayer(db::Database database, const std::string& userId);

/*! This side's relations, newest first. */
drogon::Task<std::vector<Relation>> loadRelations(db::Database database, const std::string& userId);

/*! Cards for the units in the player's Squad 10, in squad order (see kOwnHelperCount),
* user_id = ownHelperId. */
drogon::Task<std::vector<PlayerCard>> loadOwnHelperCards(db::Database database, const std::string& userId);

/*! Both helper lists as FriendGet sends them (FriendGet.cpp).  `database` may be an open
* transaction.  Also attached to UnitMix/UnitEvo/UnitFavorite and MissionEnd (with a
* favourite) so an own-unit card never shows a level from before the change. */
drogon::Task<::FriendGetResp> buildHelperLists(db::Database database, const UserIdentity identity);

/*! A player's card, or nullopt when they own no unit to lead with. */
drogon::Task<std::optional<PlayerCard>> loadPlayerCard(db::Database database, const std::string& userId);

/*!
* A card for one particular unit of the player (the raid's own-unit helper,
* RaidUnitSelectionScene), built from user_units like loadPlayerCard; nullopt
* when the player does not own that unit.
*/
drogon::Task<std::optional<PlayerCard>> loadUnitCard(db::Database database, const std::string& userId,
	const std::string& userUnitId);

/*! Internal user id for a typed public friend ID; nullopt when nobody has it. */
drogon::Task<std::optional<std::string>> userIdForPublicId(db::Database database, const std::string& digits);

/*!
* Real players with no relation to `me` at all, closest level first.
*
* @param recentFirst true orders by last login instead (the helper picker,
*                    which wants players who are around).
*/
drogon::Task<std::vector<PlayerCard>> unrelatedPlayers(
	db::Database database, const std::string& me, int32_t limit, bool recentFirst);

/*! Friends held (NPC roster + real), and the cap the client shows as "n / cap". */
drogon::Task<int32_t> friendCount(db::Database database, const std::string& userId);
drogon::Task<int32_t> friendCapacity(db::Database database, const std::string& userId);

/*! FriendApply against a real player.  Never fails the request (FRIENDS_SPEC §4.2). */
drogon::Task<ApplyOutcome> sendRequest(db::Database database, const std::string& me, const std::string& other);

/*! FriendAgree: turn their request to me into a friendship on both sides. */
drogon::Task<AgreeOutcome> acceptRequest(db::Database database, const std::string& me, const std::string& other);

/*! FriendRefuse: drop pending rows (either direction) for each id; returns how many. */
drogon::Task<int32_t> refuseOrCancel(db::Database database, const std::string& me, const std::vector<std::string>& ids);

/*! FriendDelete for a real id: both sides, whatever the state. */
drogon::Task<bool> removeRelation(db::Database database, const std::string& me, const std::string& other);

/*! FriendFavorite for a real friend; this side only. */
drogon::Task<bool> setRelationFavorite(db::Database database, const std::string& me, const std::string& other, bool on);

/*! Lazily drop pending requests older than kPendingExpirySec that involve `me`. */
drogon::Task<void> expireStaleRequests(db::Database database, const std::string& me);

/*! A card as a FriendInfo row of the given type; `rel` supplies favorite and req_time. */
::FriendInfo friendInfoFromCard(const PlayerCard& card, int32_t friendType, const Relation* rel);

/*! A card as a helper-picker row; friendType 1 for a friend, 0 otherwise. */
::ReinforcementInfo reinforcementFromCard(const PlayerCard& card, int32_t friendType, int32_t userUnitId);

/*!
* THE builder for tojMy68W: NPC roster (type 1) + every real relation + extras.
*
* An empty list cannot clear the client (no row reaches readParam), so an
* otherwise-empty list gets one sentinel row every reader skips (type 0,
* priority 2).  UNVERIFIED on device; see FRIENDS_SPEC §4.6.
*/
drogon::Task<std::vector<::FriendInfo>> buildSocialList(
	db::Database database, const UserIdentity& identity, std::vector<::FriendInfo> extra = {});

/*! A complete FriendSocialResp body, ready to return. */
drogon::Task<std::string> socialReply(
	db::Database database, const UserIdentity& identity, std::vector<::FriendInfo> extra = {});

/*!
* In-process sliding-window limiter (resets on restart, which is fine).
*
* @return true when this call is allowed and has been counted.
*/
bool allowRate(const std::string& bucket, const std::string& userId, int32_t maxCalls, int64_t windowSec);

} // namespace gme
