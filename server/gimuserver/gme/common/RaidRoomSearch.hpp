#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Classic Raid round 2, band "multiplayer": RaidRoomList (`YF9UQpn6`), the one
// request behind browse, search, auto-join and the world screen's friend and
// guild shortcuts (RAIDS_ROUND2_MULTIPLAYER.md 4.4).
//
// EVERY LISTED ROOM IS ONE THE CALLER COULD ENTER NOW.  The same checks
// RaidEnter repeats (4.3) run here first, so a row the player taps only fails
// when somebody else got there in the 4 s since: open, not quick start, the
// caller not in it (or kicked from it), no blacklist link either way, the
// caller meets its RC and level, not full, and either in the lobby or in a
// running quest whose midway window is open.  Each candidate is pruned (3.3)
// before it is judged, so an absent member neither fills a slot nor makes a
// room a "friend's room".
//
// THE REPLY IS NEVER "NOTHING" UNLESS THE CLIENT CLEARED ITS LIST.  An empty
// `8iBuZa3n` never reaches RaidRoomInfoResponse::readParam, so the previous
// rows stay (1.3).  Only the type-1 search scenes clear the list before they
// send; every other caller gets one placeholder row that the client's own
// reading turns into "no rooms".
namespace gme::raid
{

/*! `ouU5H7qm` search types (RaidRoomListRequest::setParam @0x13C6768; callers in MULTIPLAYER 4.4). */
inline constexpr int32_t kSearchFilter = 1;   // RaidRoomSearchScene2, RaidRoomSearchListScene
inline constexpr int32_t kSearchFriends = 2;  // RaidWorldScene shortcut, RaidFriendListScene
inline constexpr int32_t kSearchAuto = 3;     // RaidRoomMenuScene, RaidRoomAutoEnterScene
inline constexpr int32_t kSearchGuild = 4;    // RaidWorldScene guild shortcut

/*! One `8iBuZa3n` RaidRoomList node; a missing key keeps its default ("no filter"). */
struct RoomSearch
{
	int32_t type = kSearchFilter;  // ouU5H7qm: 1 search, 2 friends, 3 auto, 4 guild
	std::string worldId;           // 43hMuY2I (absent for 2/4)
	std::string playstyleId;       // NQR1Vd3t
	int32_t rc = 0;                // wYmg52xp (auto-join sends -1)
	std::string missionId;         // j28VNcUW
	int32_t kick = -1;             // j3sGA1zt
	int32_t joinable = -1;         // 1Txpza2u
	int32_t level = 0;             // 0TfXnP3m (string on the wire)
};

/*! Read the RaidRoomList node with the loose raid reader; never fails. */
RoomSearch readRoomSearch(const RaidRequest& req);

/*!
* The reply rows for a search, including the empty-result placeholders of 4.4.
*
* Type 1 lists up to kRoomListMax rooms newest first, or nothing.  Types 2/4
* list rooms of the caller's friends / guild mates in any world, each row's
* friend columns naming that member.  Type 3 sends only the best auto-join
* room (most members, then oldest).  Prunes every candidate room it judges,
* so the caller runs it inside the request's transaction.
*
* @param database Database client or transaction (the only handle used).
* @param identity Resolved caller.
* @param search   The request node (readRoomSearch).
*/
drogon::Task<std::vector<::RaidRoomInfo>> searchRooms(const db::Database database,
	const UserIdentity identity, const RoomSearch search);

} // namespace gme::raid
