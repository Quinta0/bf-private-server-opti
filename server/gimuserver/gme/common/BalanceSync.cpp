#include "App.hpp"

#include <gimuserver/gme/common/BalanceSync.hpp>
#include <gimuserver/gme/common/OmniPlus.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace gme
{

namespace
{

/*!
* The part of a unit_mst row that user_units.base_* / ext_* depend on.
* Arrays are HP, ATK, DEF, REC -- the order of param_max and of the columns.
*/
struct StatBasis
{
	int32_t maxLv = 0;
	std::array<int32_t, 4> mins{};
	std::array<int32_t, 4> maxs{};
	std::string impCaps;

	bool operator==(const StatBasis&) const = default;
};

constexpr std::array<const char*, 4> kStatNames = { "hp", "atk", "def", "rec" };

template <typename List>
std::string joinImpCaps(const List& caps)
{
	std::string out;
	for (const auto cap : caps)
	{
		if (!out.empty())
			out += ':';
		out += std::to_string(cap);
	}
	return out;
}

/*!
* "HP:ATK:DEF:REC" back into numbers.  A short or unreadable list means no
* imps allowed, the same reading UnitMix gives param_max.
*/
std::array<int32_t, 4> parseImpCaps(const std::string& text)
{
	std::array<int32_t, 4> caps{};
	size_t start = 0;
	for (size_t i = 0; i < caps.size() && start <= text.size(); ++i)
	{
		const auto end = text.find(':', start);
		const auto piece = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		try { caps[i] = piece.empty() ? 0 : std::stoi(piece); }
		catch (const std::exception&) { caps[i] = 0; }
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return caps;
}

StatBasis basisOf(const UnitMst& unit)
{
	StatBasis basis{};
	basis.maxLv = unit.max_lv;
	basis.mins = { unit.min_hp, unit.min_atk, unit.min_def, unit.min_rec };
	basis.maxs = { unit.max_hp, unit.max_atk, unit.max_def, unit.max_rec };
	basis.impCaps = joinImpCaps(unit.param_max);
	return basis;
}

/*!
* The base-stat curve.  MUST stay identical to unitMix_statAtLevel
* (UnitMix.cpp) and scaleUnitBaseStats (Common.hpp): the migration subtracts
* the old curve to recover type growth, and a different rounding here would
* leak into every stored stat as phantom growth.
*/
int32_t statAt(int32_t minV, int32_t maxV, int32_t level, int32_t maxLv)
{
	if (maxLv <= 1 || level <= 1) return minV;
	if (level >= maxLv)           return maxV;
	return minV + (int)((double)(maxV - minV) * (double)(level - 1) / (double)(maxLv - 1));
}

void writeBasis(const drogon::orm::DbClientPtr& db, int32_t unitId, const StatBasis& basis)
{
	db->execSqlSync(
		"INSERT OR REPLACE INTO unit_stat_basis(unit_id, max_lv, min_hp, max_hp,"
		" min_atk, max_atk, min_def, max_def, min_rec, max_rec, imp_caps)"
		" VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11);",
		unitId, basis.maxLv,
		basis.mins[0], basis.maxs[0], basis.mins[1], basis.maxs[1],
		basis.mins[2], basis.maxs[2], basis.mins[3], basis.maxs[3],
		basis.impCaps);
}

struct MigrationCount
{
	size_t rows = 0;
	size_t clamped = 0;
};

/*!
* Re-curves every owned copy of one unit from `before` to `after`.
*/
MigrationCount migrateOwnedUnits(const drogon::orm::DbClientPtr& db, int32_t unitId,
	const StatBasis& before, const StatBasis& after)
{
	MigrationCount count{};
	const auto oldCaps = parseImpCaps(before.impCaps);
	const auto newCaps = parseImpCaps(after.impCaps);
	// An Omni+ copy's caps sit above the unit's by its level's bonus.
	int32_t cost = 0;
	for (const auto& unit : theServer()->cache().unitMst())
		if (unit.id == unitId) { cost = unit.cost; break; }

	const auto owned = db->execSqlSync(
		"SELECT user_unit_id, unit_lvl, base_hp, base_atk, base_def, base_rec,"
		" ext_hp, ext_atk, ext_def, ext_rec, omni_level FROM user_units WHERE unit_id = $1;",
		unitId);
	for (const auto& row : owned)
	{
		const auto level = row["unit_lvl"].as<int32_t>();
		const auto omni = gme::omniImpBonus(cost, row["omni_level"].as<int32_t>());
		std::array<int32_t, 4> base{};
		std::array<int32_t, 4> ext{};
		bool clamped = false;
		for (size_t s = 0; s < kStatNames.size(); ++s)
		{
			const auto stored = row[std::string("base_") + kStatNames[s]].as<int32_t>();
			const auto growth = stored - statAt(before.mins[s], before.maxs[s], level, before.maxLv);
			base[s] = statAt(after.mins[s], after.maxs[s], level, after.maxLv) + growth;

			ext[s] = row[std::string("ext_") + kStatNames[s]].as<int32_t>();
			const auto cap = std::max(newCaps[s] + omni[s], 0);
			if (newCaps[s] < oldCaps[s] && ext[s] > cap)
			{
				ext[s] = cap;
				clamped = true;
			}
		}

		db->execSqlSync(
			"UPDATE user_units SET base_hp = $1, base_atk = $2, base_def = $3, base_rec = $4,"
			" ext_hp = $5, ext_atk = $6, ext_def = $7, ext_rec = $8 WHERE user_unit_id = $9;",
			base[0], base[1], base[2], base[3], ext[0], ext[1], ext[2], ext[3],
			row["user_unit_id"].as<int64_t>());
		++count.rows;
		if (clamped)
			++count.clamped;
	}
	return count;
}

} // namespace

void syncUnitStatBasis(const drogon::orm::DbClientPtr& db)
{
	try
	{
		const auto& cache = theServer()->cache();

		std::map<int32_t, StatBasis> effective;
		for (const auto& unit : cache.unitMst())
			effective[unit.id] = basisOf(unit);
		if (effective.empty())
		{
			LOG_WARN << "balance: unit table is empty, stat basis left alone";
			return;
		}

		const auto trans = gme::newTransactionSync(db, [](bool committed) {
			if (!committed)
				LOG_ERROR << "balance: unit stat sync failed to commit; stored stats unchanged";
		});
		try
		{
			std::map<int32_t, StatBasis> basis;
			const auto stored = trans->execSqlSync(
				"SELECT unit_id, max_lv, min_hp, max_hp, min_atk, max_atk,"
				" min_def, max_def, min_rec, max_rec, imp_caps FROM unit_stat_basis;");
			const bool seeding = stored.empty();
			if (seeding)
			{
				// Owned units were computed from the table as it was before any
				// override, so that is the honest starting basis.
				basis = effective;
				for (const auto& [id, unit] : cache.unitMstBeforeBalance())
					basis[id] = basisOf(unit);
			}
			else
			{
				for (const auto& row : stored)
				{
					StatBasis b{};
					b.maxLv = row["max_lv"].as<int32_t>();
					for (size_t s = 0; s < kStatNames.size(); ++s)
					{
						b.mins[s] = row[std::string("min_") + kStatNames[s]].as<int32_t>();
						b.maxs[s] = row[std::string("max_") + kStatNames[s]].as<int32_t>();
					}
					b.impCaps = row["imp_caps"].as<std::string>();
					basis[row["unit_id"].as<int32_t>()] = std::move(b);
				}
			}

			size_t unitsChanged = 0, unitsAdded = 0, rowsMigrated = 0, rowsClamped = 0;
			for (const auto& [id, now] : effective)
			{
				const auto it = basis.find(id);
				if (it == basis.end())
				{
					// A unit new to the table (e.g. homebrew added since the last
					// boot).  Nothing owned was computed from an older curve of
					// it, so record it and move on.
					writeBasis(trans, id, now);
					++unitsAdded;
					continue;
				}
				if (it->second == now)
				{
					if (seeding)
						writeBasis(trans, id, now);
					continue;
				}

				const auto count = migrateOwnedUnits(trans, id, it->second, now);
				writeBasis(trans, id, now);
				++unitsChanged;
				rowsMigrated += count.rows;
				rowsClamped += count.clamped;
				LOG_INFO << "balance: unit " << id << " curve changed; "
					<< count.rows << " owned row(s) re-curved"
					<< (count.clamped ? ", " + std::to_string(count.clamped) + " imp clamp(s)" : "");
			}

			if (seeding)
				LOG_INFO << "balance: seeded unit_stat_basis with " << effective.size() << " unit(s)";
			LOG_INFO << "balance: unit stat sync -- " << unitsChanged << " unit(s) changed, "
				<< rowsMigrated << " owned row(s) migrated, " << rowsClamped << " imp clamp(s), "
				<< unitsAdded << " new unit(s) recorded";
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			trans->rollback();
			LOG_ERROR << "balance: unit stat sync rolled back: " << ex.base().what();
		}
		catch (const std::exception& ex)
		{
			trans->rollback();
			LOG_ERROR << "balance: unit stat sync rolled back: " << ex.what();
		}
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "balance: unit stat sync could not start: " << ex.what();
	}
}

} // namespace gme
