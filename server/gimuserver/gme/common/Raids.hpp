#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <optional>
#include <string>
#include <vector>

// Classic Raid Battle -- slice 1: the entry screens and a solo quick-start room.
//
// SQLite is the only state.  Nothing ticks: aube regeneration and stale-room
// cleanup are derived from timestamps whenever a request reads them, so a
// server restart loses nothing and there is no timer to drift (the HunterOrbs
// pattern).  Wire shapes and the scene flow they drive are in
// docs/features/RAIDS_SPEC.md.
//
// From RaidMissionStart on, the run itself lives in RaidField.hpp (round 2).
namespace gme::raid
{

/*! DefineMst raid_max_p: the aube cap. */
inline constexpr int32_t kAubeMax = 3;
/*! DefineMst recover_time_raid: seconds per aube point. */
inline constexpr int32_t kAubeRecoverSec = 3600;
/*!
* A room nobody has polled for this long is closed on the next touch.  Generous
* on purpose: the solo path sits on RaidQuickCheckScene / RaidFieldReady, which
* do not heartbeat, and closing under a player who is still choosing items would
* fail their MissionStart for no reason.
*/
inline constexpr int32_t kRoomIdleSec = 600;
/*! Seconds a quick room closed by MissionEnd still answers its members' RaidRoomInfo. */
inline constexpr int32_t kClosedRoomGraceSec = 20;
/*! Room status column: lobby, and closed (rows kept for the log). */
inline constexpr int32_t kRoomLobby = 0;
inline constexpr int32_t kRoomClosed = 3;

/*! One raid_user row with aube already regenerated to `now`. */
struct RaidUser
{
	std::string user_id;
	int32_t rc = 1;
	int32_t agreement = 0;
	std::string login_world_id;
	std::string login_room_id;
	int32_t aube = kAubeMax;
	int32_t rest_timer = 0;
	int32_t active_deck = 0;
};

/*! What a room row shows about a player (handle, level, leader). */
struct RaidProfile
{
	std::string name;
	int32_t level = 1;
	std::string leader_unit_id;
	int32_t leader_unit_lv = 1;
	int32_t guild_id = 0;
};

/*!
* Load (creating on first touch) the player's raid row.  A login room that is no
* longer live is forgotten here, so RaidInitScene never resumes a dead room.
*/
drogon::Task<RaidUser> loadRaidUser(const db::Database database, const std::string userId);

/*! UserPartyDeck deck type of a raid squad (UserPartyDeckList::deckCopyRaid @0x129B91C sets 3). */
inline constexpr int32_t kRaidDeckType = 3;

/*!
* Give the player raid squads (deck type 3) copied from their quest decks
* (type 1) when they have none.  The client keeps raid squads apart from quest
* decks (isRaidDeckUnit) and its own copy routine, deckCopyRaid, is never
* called in this build, so without these rows the raid squad is empty and
* RaidMissionStart's wc2DiMY1 carries only the helper.  Called from UserInfo,
* which is what delivers decks to the client.
*/
drogon::Task<void> seedRaidDecks(const db::Database database, const std::string userId);

/*!
* Spend one raid orb (aube) to set out on a raid quest: the derived count
* drops by one and the regeneration clock keeps its progress (a full bar
* starts the clock now).  False, nothing written, when no orb is left; a
* compare-and-swap on the stored row so two starts cannot both spend the same
* orb.
*/
drogon::Task<bool> spendAube(const db::Database database, const std::string userId);

/*! Mark the raid terms as accepted. */
drogon::Task<void> acceptAgreement(const db::Database database, const std::string userId);

/*! Missions the player may start: every RaidMissionMst row at or below their RC. */
std::vector<::RaidPermitMissionInfo> permittedMissions(int32_t rc);

/*! The RaidMissionMst row for `missionId`, if it exists. */
std::optional<::RaidMissionMst> findMission(const std::string& missionId);

/*! The full RaidUserInfo reply: state, permits and clears. */
drogon::Task<::RaidUserInfoResp> userInfoResp(const db::Database database, const std::string userId);

/*! The world list, one row per RaidWorldMst, crowding from live rooms. */
drogon::Task<::RaidWorldInfoResp> worldInfoResp(const db::Database database, const std::string userId);

/*! Handle, level, leader and guild for a room row. */
drogon::Task<RaidProfile> loadProfile(const db::Database database, const std::string userId);

/*!
* Create a one-member quick-start room and point the player's login ids at it.
* Any room the player is still in is left first.
* @return The new room id.
*/
drogon::Task<std::string> createSoloRoom(
	const db::Database database,
	const std::string userId,
	const std::string missionId,
	const std::string helperJson);

} // namespace gme::raid

// Rooms, the lobby and the ROOM reply.
#include <gimuserver/gme/common/RaidRooms.hpp>
