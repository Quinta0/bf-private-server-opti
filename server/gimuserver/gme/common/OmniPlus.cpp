#include "OmniPlus.hpp"

#include "App.hpp"

namespace gme
{
namespace
{

const UnitEvoOmniMst* omniRow(const int32_t cost, const int32_t omniLevel)
{
	if (omniLevel <= 0)
		return nullptr;
	for (const auto& row : theServer()->cache().unitEvoOmniMst())
	{
		if (row.cost == cost && row.omni_level == omniLevel)
			return &row;
	}
	return nullptr;
}

} // namespace

std::array<int32_t, 4> omniImpBonus(const int32_t cost, const int32_t omniLevel)
{
	std::array<int32_t, 4> bonus{ 0, 0, 0, 0 };
	if (const auto* row = omniRow(cost, omniLevel))
	{
		size_t i = 0;
		for (const auto value : row->stat_award)
		{
			if (i >= bonus.size())
				break;
			bonus[i++] = value;
		}
	}
	return bonus;
}

int32_t omniSpStep(const int32_t cost, const int32_t omniLevel)
{
	const auto* row = omniRow(cost, omniLevel);
	return row ? row->unk1 : 0;
}

} // namespace gme
