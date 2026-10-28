#pragma once

#include <gimuserver/packets/all.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Weekly summon banner rotation.
//
// The rail is a fixed core of tiles that are always open
// (gacha_category_mst.json) plus a few FEATURED slots whose gates change every
// game week, Monday 08:00 UTC (GameWeek.hpp), the same for every player.  The
// pool, its order, the slots and the art live in
// deploy/archive/summon_rotation.json.
//
// NO STORED STATE, like the 6-star selector (SelectorRotation.hpp).  The week's
// gates are a pure function of the week number: the pool sits in ONE fixed
// order and each week takes the next slots' worth, wrapping, so no gate comes
// back before the whole pool has had its turn.
//
// THE POOL OWNS ITS GATES.  A pool gate is open only in its featured weeks,
// whatever static tile also names it: applyWeeklyRail strips pool gates from
// the static tiles (the collab tile, the Halloween tile, the new banners' tiles
// stay in gacha_category_mst.json for the generators and gate_info.py --check,
// and simply never ship), and isOnSummonRail asks isFeaturedNow.  So the rail a
// player sees and the gates GachaAction sells cannot disagree.
//
// THE SERVER, NOT THE PHONE, DECIDES.  Featured tiles go out with the same
// 0..2147483647 window as every other tile.  The client does honour a tile's
// start/end, but against the phone's own clock (getGachaCategoryList
// @0x1ea6978 compares with time(NULL), no server offset), so a wrong phone
// clock would hide or show a slot at the wrong time; and touchEnded @0x1ea70dc
// maps a tap by index into a re-filtered list, so a tile expiring while the
// rail is open would shift every later tap onto its neighbour.  The client
// sends GachaList on every visit to the rail (a new SummonsCategorizationScene
// each time), so swapping the gates server-side is enough.
//
// STABLE TILE IDS.  Each slot keeps its tile id every week; only the gate behind
// it and its art change.  The gate pager looks its tile up again by id
// (0x16dd510), so a tile id that vanished between two visits is a risk.
namespace gme
{

/*! Load deploy/archive/summon_rotation.json.  Needs GachaArchiver set up. */
void loadSummonRotation(const std::string& archiveRoot);

/*! Whether the rotation is loaded and has gates to feature. */
bool summonRotationActive();

/*!
* The gates featured in a game week, one per slot, in slot order.
*
* @param week Game week (gameWeekAt).
* @return Empty when the rotation is not loaded.
*/
std::vector<uint32_t> featuredGates(int64_t week);

/*! Whether a gate is in the rotation pool (open only in its featured weeks). */
bool isRotatingGate(uint32_t gachaId);

/*!
* Whether a pool gate may be pulled now: featured this week, or featured last
* week and the week turned over less than the grace period ago.  The grace is
* for a player who sat on the gate pager across Monday 08:00; GachaList always
* shows the current week only.
*
* @param gachaId Summon gate id.
* @param now Unix seconds.
*/
bool isFeaturedNow(uint32_t gachaId, int64_t now);

/*!
* Turn the static rail into this week's rail, in place.
*
* Strips pool gates from the static tiles (dropping tiles left empty), inserts
* one tile per featured slot right after the configured core tile, and numbers
* the display order 1..n so no two of these tiles tie (the client sorts ties by
* heap pointer, i.e. at random).  Per-player tiles are appended by the caller
* afterwards and keep their own orders.
*
* @param categories The cache's static tiles, copied for this reply.
* @param now Unix seconds.
* @param wholePool A Sandbox account's rail (gme/common/Sandbox.hpp): each
*        featured tile pages through its week's gate and then a share of the
*        rest of the pool, so every pool gate is on it.
*/
void applyWeeklyRail(std::vector<::GachaCategory>& categories, int64_t now, bool wholePool = false);

/*!
* Fill the featured line of the pool gates' caption text for this reply.
*
* gate_info.py puts kFeaturedToken on its own line under the title of every
* pool gate (that line is budgeted out of the caption box); this replaces it
* with "Featured until Mon 27 Oct, 08:00 UTC", or with a plain label for a gate
* not featured this week (its tile is not on the rail then).
*
* @param gates The gate rows of this reply (1IR86sAv).
* @param now Unix seconds.
*/
void applyFeaturedNotes(std::vector<::GachaInfoMst>& gates, int64_t now);

/*! The placeholder gate_info.py writes into pool gates' captions. */
inline constexpr std::string_view kFeaturedToken = "{featured_until}";

/*!
* This week's rotation as JSON for the Discord bot and the site:
* {"week","starts","ends","featured":[{"gate","title","art"}],"next":[...]}.
*
* @param now Unix seconds.
*/
std::string summonRotationJson(int64_t now);

/*!
* Every unit a player can summon, and where, as JSON for the Discord bot's
* /unit command:
* {"week","gates":[{"gate","title","gems","honor","rotating","featured","until","next"}],
*  "units":[{"id","name","rarity","in":[[gate, percent], ...]}]}.
* Gates are the static rail's (without the tutorial gate) plus the rotation
* pool; "until" is the end of a featured week, "next" the start of the next one.
*
* @param now Unix seconds.
*/
std::string summonUnitsJson(int64_t now);

} // namespace gme
