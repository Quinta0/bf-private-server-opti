#pragma once

#include <drogon/orm/DbClient.h>

// Keeps owned units on the curve the server's unit_mst says they are on.
//
// user_units.base_* is not derived on read: it is FROZEN at grant, fusion and
// evolution time from the server's unit_mst curve (plus rolled type growth),
// and the client draws it and fights with it verbatim.  So when the balance
// overlay (<balance_root>/server/unit_mst.json) changes a unit's min/max stats
// or max level, every copy of that unit already owned keeps the old numbers --
// and the next fusion would absorb the difference as fake type growth
// (UnitMix carries growth as `stored - statAt(newCurve, oldLevel)`).
//
// unit_stat_basis records the curve each unit's stored stats were last
// computed from.  At boot, after migrations and with the effective unit_mst
// (shipped + homebrew + balance overrides) loaded, this re-curves every owned
// row whose unit's curve moved:
//
//     g      = base_X - statAt(old curve, unit_lvl)   -- type growth, kept
//     base_X = statAt(new curve, unit_lvl) + g
//
// and clamps ext_X (the imp bucket) where its cap in param_max went down.
// add_* and limit_over_* are untouched.  Exactly invertible: publishing the old
// curve again restores the old numbers, unless a clamp or an evolution
// happened in between.
//
// NOT migrated, deliberately: arena_battle enemy_units/my_units and
// raid_room.helper_json.  Those are per-battle/per-room snapshots that are
// resent byte for byte on Restart; rewriting them mid-battle would be worse
// than letting the open ones finish on the old numbers.  Guild cards, the
// campaign helper and player arena snapshots read user_units live, and arena
// bots / simulated friends compute from unit_mst live, so all of those follow
// without help.

namespace gme
{

/*!
* Seeds or applies unit_stat_basis against the effective unit_mst, in one
* transaction.  Call once at boot, after MigrationManager::RunMigrations and
* after the GimuServer plugin has loaded its cache.
*
* An empty basis (first boot with this code) is seeded from the curve owned
* units were actually computed from -- the shipped/homebrew rows, i.e. the
* rows as they were BEFORE any balance override -- and then synced like any
* other boot, so a publish already staged on that first boot still migrates.
* With no overrides present that seeding changes nothing.
*
* Never throws: a failure is logged and rolled back, and the server carries
* on with the stored stats unchanged.
*
* @param db Database client.
*/
void syncUnitStatBasis(const drogon::orm::DbClientPtr& db);

} // namespace gme
