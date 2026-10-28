#pragma once

#include <array>
#include <cstdint>
#include <optional>

/*!
* Balance Forge tuning: <balance_root>/server/tuning.json, one flat JSON object
* of numbers the Forge can change without a rebuild.  A key the file leaves out
* keeps its default, and a key this server does not know is ignored (a newer
* Forge may write more).  The file is re-read when it changes (checked on every
* call by inode, size and times); a file that does not read, or a value out of
* range, keeps the last good values and is logged.
*
*   fusion_sp_chance_pct           20   chance (0-100) that one "SPUP!?" unit
*                                       fused into an SP-eligible unit gives 1 SP
*   fusion_sp_chance_per_star_pct   1   added per star of that unit above 1
*   dup_evo_enabled                 1   1: a unit can evolve by consuming one copy of
*                                       itself instead of its recipe (#157); 0: off
*   dup_evo_zel_pct                 0   that evolution's Zel, as a percentage (0-100)
*                                       of the recipe's
*   dup_evo_karma_pct               0   the same for Karma
*   capture_floor_pct              40   every capturable enemy without authored_capture
*                                       can be captured at least this often (0-100,
*                                       whole); applied at MissionStart (ticket #164)
*   boss_drop_pct_r1 .. _r8         -   a boss's base-form drop chance (whole 0-100) when
*                                       its line tops out at that rarity (8 = Omni);
*                                       left out: archive/boss_drops.json's value
*   boss_drop_merit_pct             -   what a caught copy trades in for, in percent of a
*                                       summoned one; left out: boss_drops.json's value
*/
namespace forge
{

struct Tuning
{
	double fusionSpChancePct = 20.0;
	double fusionSpChancePerStarPct = 1.0;
	bool dupEvoEnabled = true;
	double dupEvoZelPct = 0.0;
	double dupEvoKarmaPct = 0.0;
	uint32_t captureFloorPct = 40;
	std::array<std::optional<uint32_t>, 9> bossDropPct{};   // [rarity]; index 0 unused
	std::optional<uint32_t> bossDropMeritPct;
};

/*! The live values (defaults without a file). */
Tuning tuning();

} // namespace forge
