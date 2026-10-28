#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <optional>
#include <string>

// Classic Raid rooms: membership, the lobby and the ROOM reply
// (docs/features/RAIDS_ROUND2_MULTIPLAYER.md 1.3, 1.5, 3.3, 4.1).
//
// THE SERVER'S LOGIN IDS ARE THE SOURCE OF TRUTH.  Every raid scene answers a
// refuseToRetry by re-entering through RaidUserInfo (scene 1201), which
// resumes raid_user.login_room_id when that room is live and drops to the
// world list otherwise.  So whoever removes a player from a room blanks their
// login ids in the same breath, and a refusal never leaves them naming a room
// that will not take them back.
//
// Nothing ticks here either: absent members, master promotion and the return
// to the lobby after a run are applied lazily by pruneRoom on every read.
namespace gme::raid
{

/*! raid_room.status: the room's quest is running (lobby 0 and closed 3 are in Raids.hpp). */
inline constexpr int32_t kRoomInField = 2;
/*!
* A lobby member with no raid request naming the room for this long is
* dropped.  Party and item edit send nothing while open; three minutes covers
* them without leaving ghosts in other players' lobbies for long.
*/
inline constexpr int32_t kLobbyAbsentSec = 180;
/*!
* How long a member holding an uncollected run result (mission_status 2..4)
* keeps their place when silent: a run's whole time limit, so stepping away
* during a fight does not forfeit the reward.
*/
inline constexpr int32_t kUncollectedHoldSec = 1800;
/*! Mid-quest join window after the master sets out (infoMsgSet compares RoomMissionJoinableRestTime with 300). */
inline constexpr int32_t kMidwayJoinWindowSec = 300;
/*! Rows per RaidRoomList reply (the client has no cap and sorts by member count itself). */
inline constexpr int32_t kRoomListMax = 30;
/*! Members when the room has no quest selected yet. */
inline constexpr int32_t kDefaultRoomCapacity = 4;

/*! Member cap of a room: RaidMissionMst KR6JacN8 of its mission. */
int32_t roomCapacity(const std::string& missionId);

/*! Blank a player's login ids if they still name `roomId` (a later room keeps them). */
drogon::Task<void> forgetRoom(const db::Database database, const std::string userId, const std::string roomId);

/*!
* MULTIPLAYER 3.3 rule 1 for requests that name no room (the queued
* RoonItemEdit / UpDateCash): refresh the caller's presence in their login room.
*/
drogon::Task<void> touchLoginRoom(const db::Database database, const std::string userId);

/*! Open, and either on a run or polled recently. */
drogon::Task<bool> roomIsLive(const db::Database database, const std::string roomId);

/*! A quick room MissionEnd closed in the last kClosedRoomGraceSec, still answering this member. */
drogon::Task<bool> roomInGrace(const db::Database database, const std::string roomId, const std::string userId);

/*! Close a room: any run on it lost, members out, every login id that named it blanked. */
drogon::Task<void> closeRoom(const db::Database database, const std::string roomId);

/*! A member who has not been kicked. */
drogon::Task<bool> isActiveMember(const db::Database database, const std::string roomId, const std::string userId);

/*!
* The lazy membership rules (3.3): drop lobby members absent for
* kLobbyAbsentSec, promote a new master or close the room when the master is
* gone, and put a room whose finished run every present member has collected
* back in the lobby.  Quick rooms are left alone (their only member sits on
* scenes that never poll).
*/
drogon::Task<void> pruneRoom(const db::Database database, const std::string roomId);

/*!
* Take the player out of `roomId`.  A kicked member keeps the kicked row so
* rejoining stays blocked; a leaving master hands the room to the earliest
* joined member; the last member out closes it.
*/
drogon::Task<void> leaveRoom(const db::Database database, const std::string userId, const std::string roomId);

/*!
* One 8iBuZa3n row for `roomId` as `viewerId` sees it: the password only for
* the master, member_num counting non-kicked members, EnterableFlg and the
* mid-quest window computed.  nullopt when the room row does not exist.
*/
drogon::Task<std::optional<::RaidRoomInfo>> roomRow(
	const db::Database database, const std::string roomId, const std::string viewerId);

/*!
* ROOM: the room and its members (master first, then join order) for a live
* room, refreshing the viewer's last_seen.  Kicked rows go only to the kicked
* player themself.  Empty lists when the room is gone for this viewer.
*/
drogon::Task<::RaidRoomResp> roomResp(const db::Database database, const std::string userId, const std::string roomId);

/*!
* The member card's party list (9PS8BLiC, RaidRoomPartyInfo) as a JSON object
* to splice into a room reply ("" when nobody has a squad).  One row per unit
* of every non-kicked member: their run party while on the room's run, else
* their active raid squad (deck type 3, user_info.active_deck).
* RaidUnitScene draws a member's units from it; without it the card shows
* empty frames (MULTIPLAYER 4.1 item 5).
*/
drogon::Task<std::string> roomPartyGroup(const db::Database database, const std::string roomId);

/*!
* End a quick room after its run was collected or abandoned: closed, login ids
* blanked, member rows kept so the room answers them for kClosedRoomGraceSec.
*/
drogon::Task<void> retireQuickRoom(const db::Database database, const std::string roomId);

/*! A member's raid_room_member.mission_status, -1 when they are not a (non-kicked) member. */
drogon::Task<int32_t> memberMissionStatus(const db::Database database, const std::string roomId, const std::string userId);

/*!
* The master set out (MULTIPLAYER 5.2): every other member who is ready gets
* mission_status 1, which sends them to RaidFieldReady on their next poll.
* Members not ready stay in the lobby and may join midway (ReadyUp 2).
* @return How many members were sent.
*/
drogon::Task<int32_t> sendReadyMembers(const db::Database database, const std::string roomId, const std::string masterId);

/*!
* After a run in a shared room: back to the lobby once every present member
* of `runId` has collected it (MissionEnd) or left.  No-op otherwise.
*/
drogon::Task<void> settleRoomAfterRun(const db::Database database, const std::string roomId, const int64_t runId);

} // namespace gme::raid
