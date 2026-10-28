#pragma once

#include <array>
#include <cstdint>

// ---------------------------------------------------------------------------
// Omni+ Boost — what each +level gives a unit (unit_evo_omni_mst).
//
// The table is keyed by (unit cost, omni level), 18 rows for the six Omni cost
// bands, and each row carries:
//   js95kdsL  stat_award  "HP:ATK:DEF:REC" — the raised imp caps.  Read as the
//             TOTAL at that level (150 / 450 / 900 HP), not a step to add to
//             the previous level's.  The client agrees: its Omni+ preview for
//             a 1250-HP-cap unit reads 1400 at +1 and 1700 at +2.
//   ddD4lsc1  unk1        Enhancement SP for reaching that level (5 / 5 / 10),
//             a step: UnitOmniEvo adds it to fe_sp and fe_max_sp.
// user_units.omni_level is 0 for a unit that was never boosted.
// ---------------------------------------------------------------------------

namespace gme
{

/// The highest Omni+ level (+3).
constexpr int32_t kOmniPlusMaxLevel = 3;

/// Extra imp cap (HP, ATK, DEF, REC) an Omni+ level grants a unit of this
/// cost; all zero at level 0 or for a cost the table does not list.
std::array<int32_t, 4> omniImpBonus(int32_t cost, int32_t omniLevel);

/// Enhancement SP granted on REACHING this level; 0 if the table has no row.
int32_t omniSpStep(int32_t cost, int32_t omniLevel);

} // namespace gme
