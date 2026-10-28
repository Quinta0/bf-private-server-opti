#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Dbb.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <string>
#include <vector>

// DbbBond (0EtanubR / Tr7dR4dR) — pair two units for a Dual Brave Burst, or
// release the pair.  One request serves both buttons; `deKa4iva` is 1 to bond
// and 2 to unbond (DbbBondRequest::setDbbTypeBond / setDbbTypeUnbond).
//
// Reachable from any unit's detail page once DbbMst is on the wire, which is
// new — see gme/common/Dbb.hpp for why the whole feature was dark.  That also
// means this handler had to exist before the catalogue was sent: an
// unregistered GroupId closes the session, and the Bond button is one tap from
// the unit list.
//
// A bond is free.  Raising one is UnitBondBoost (tr5rOwro), a separate request
// and a separate handler — it is named Unit*, not Dbb*, which is why the first
// pass through this subsystem missed it.

HANDLEF(DbbBond)
{
	(void)session;

	DbbBondReq req{};
	{
		glz::context ctx{};
		if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
			LOG_WARN << "DbbBond: parse error: " << glz::format_error(ec, json);
	}
	if (req.nodes.empty())
		co_return HandleResult::error("Invalid bond request", "no units named");

	const auto& node = req.nodes.front();
	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	const auto unbond = node.bond_type == 2;

	DbbBondResp resp{};
	resp.signal_key.key = "5EdKHavF";
	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			if (unbond)
			{
				// Released from either side: the client only ever names the
				// pair it is looking at, and which of the two is "main" is a
				// detail of how it was forged.
				const auto dropped = co_await transaction->execSqlCoro(
					"DELETE FROM user_unit_dbb WHERE user_id = $1"
					" AND (user_unit_id = $2 OR bonded_user_unit_id = $2"
					"      OR user_unit_id = $3 OR bonded_user_unit_id = $3)"
					" RETURNING user_unit_id;",
					identity.userId, node.main_unit_id, node.sub_unit_id);
				if (dropped.empty())
				{
					transaction->rollback();
					co_return HandleResult::error("Invalid bond request", "those units are not bonded");
				}
				LOG_INFO << "DbbBond: " << identity.userId << " released "
					<< node.main_unit_id << " / " << node.sub_unit_id;
			}
			else
			{
				// Both units have to be this player's, and the species pair has
				// to be one the catalogue names — the client only offers valid
				// partners, so anything else is a replayed or forged body.
				const auto owned = co_await transaction->execSqlCoro(
					"SELECT user_unit_id, unit_id, dbb_unlocked FROM user_units"
					" WHERE user_id = $1 AND user_unit_id IN ($2, $3);",
					identity.userId, node.main_unit_id, node.sub_unit_id);
				if (owned.size() != 2 || node.main_unit_id == node.sub_unit_id)
				{
					transaction->rollback();
					co_return HandleResult::error("Invalid bond request", "unit not owned");
				}
				std::string mainSpecies, subSpecies;
				for (const auto& row : owned)
				{
					if (row["user_unit_id"].as<int32_t>() == node.main_unit_id)
						mainSpecies = row["unit_id"].as<std::string>();
					else
						subSpecies = row["unit_id"].as<std::string>();
				}

				// BOTH SLOTS HAVE TO BE OPEN.  Global wiki, Bonding: a unit is
				// bondable only after being fused with an Elemental Golem of its
				// own element, "allowing it to become Bonded with its partner
				// (Unit Pair) THAT HAS COMPLETED THE SAME STEPS".
				//
				// Enforced here because the client cannot: isDbbEligible
				// @0x12B5EA4 knows nothing about golems, so it offers the Bond
				// button to any omni with SBB 10 and a catalogued partner.  The
				// refusal names which side is missing rather than saying no.
				std::vector<int32_t> locked;
				for (const auto& row : owned)
				{
					if (row["dbb_unlocked"].as<int32_t>() == 0)
						locked.push_back(row["user_unit_id"].as<int32_t>());
				}
				if (!locked.empty())
				{
					transaction->rollback();
					std::string which;
					for (const auto id : locked)
					{
						if (!which.empty()) which += ", ";
						which += std::to_string(id);
					}
					LOG_INFO << "DbbBond: " << identity.userId
						<< " tried to bond with an unopened DBB slot on unit(s) " << which;
					co_return HandleResult::error(
						"Invalid bond request",
						"fuse an Elemental Golem of the unit's own element first (unit "
							+ which + ")");
				}

				const auto dbbId = gme::dbbIdForPair(mainSpecies, subSpecies);
				if (dbbId.empty())
				{
					transaction->rollback();
					LOG_WARN << "DbbBond: " << mainSpecies << " + " << subSpecies
						<< " is not a pairing DbbMst names";
					co_return HandleResult::error("Invalid bond request", "those units have no shared DBB");
				}

				// THE RANK IS THE SERVER'S, NOT THE CLIENT'S (ticket #166
				// audit).  M81OjLwr is only an echo of UserUnitDbbInfo::
				// getBondLevel() (doBondRequest @0x1E9B7F0), and it used to be
				// stored as sent: a forged body could open a brand-new bond at
				// rank 6 and skip every DbbBondRecipeMst payment UnitBondBoost
				// charges.  The rank a pairing already earned is whatever is
				// stored for its dbb_id -- the level list is keyed by it, and
				// UnitBondBoost raises every row of it -- so it is read here,
				// BEFORE the clearing DELETE below can drop the very row a
				// re-bond of the same pair replaces.  Nothing stored means a
				// first bond, rank 1; the clamp keeps an out-of-range stored row
				// inside what the recipes price.
				const auto stored = co_await transaction->execSqlCoro(
					"SELECT COALESCE(MAX(bond_level), 1) AS bond_level FROM user_unit_dbb"
					" WHERE user_id = $1 AND dbb_id = $2;",
					identity.userId, dbbId);
				const auto level = std::clamp(
					stored.empty() ? 1 : stored[0]["bond_level"].as<int32_t>(),
					1, gme::kDbbMaxBondLevel);
				if (node.bond_level != level && node.bond_level >= 1)
				{
					LOG_INFO << "DbbBond: " << identity.userId << " sent rank " << node.bond_level
						<< " for DBB " << dbbId << ", stored rank is " << level << "; kept the stored one";
				}

				// A unit can hold one bond, on either side of it.  Clearing
				// first is what makes re-bonding to a different partner work,
				// which is exactly what the scene offers.
				co_await transaction->execSqlCoro(
					"DELETE FROM user_unit_dbb WHERE user_id = $1"
					" AND (user_unit_id = $2 OR bonded_user_unit_id = $2"
					"      OR user_unit_id = $3 OR bonded_user_unit_id = $3);",
					identity.userId, node.main_unit_id, node.sub_unit_id);

				co_await transaction->execSqlCoro(
					"INSERT INTO user_unit_dbb"
					" (user_id, user_unit_id, bonded_user_unit_id, dbb_id, bond_level)"
					" VALUES ($1, $2, $3, $4, $5);",
					identity.userId, node.main_unit_id, node.sub_unit_id, dbbId, level);

				LOG_INFO << "DbbBond: " << identity.userId << " bonded unit "
					<< node.main_unit_id << " (" << mainSpecies << ") to "
					<< node.sub_unit_id << " (" << subSpecies << ") as DBB " << dbbId
					<< " at rank " << level;
			}

			co_await gme::fillDbb(transaction, identity, resp,
				unbond ? std::vector<int32_t>{ node.main_unit_id, node.sub_unit_id }
				       : std::vector<int32_t>{});
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
