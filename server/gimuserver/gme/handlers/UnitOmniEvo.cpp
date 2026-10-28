#include "App.hpp"
#include "Handlers.hpp"
#include <gimuserver/gme/common/Relations.hpp>

#include <map>

#include <gimuserver/db/PacketInterface.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/OmniPlus.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

// UnitOmniEvo (4Dk4spf9 / 4s3lsODp) — Omni+ Boost: raise an Omni unit from
// +N to +N+1 (up to +3).
//
// Request, UnitOmniEvoRequest::createBody @0x1C6F68C, decoded from five prod
// taps (2026-10-22):
//   "34d0Slkf": [{"s049sdck": "1"}]          recipe type: 1 Elemental, 2 Duplicate
//   "8Z2NQrx1": one node per selection, {inU8Q4gL id, mnZ5K4Ii role,
//               29MgiJIQ kind, H6k1LIxC count}:
//       role 1, kind 1   the unit being boosted (user_unit_id)
//       role -1, kind 3  the duplicate copy it eats (user_unit_id)
//       role -1, kind 2  tomes (count 10 Elementum, or 5 Geminus)
//       role -1, kind 4  elemental shards (count 100 for a cost-60 unit)
//   An item node's id is a warehouse index, never an item id, so items are
//   spent by the recipe's item id, as UnitEvo does.
//
// THE RECIPE IS THE SERVER'S.  The base's (cost, omni_level + 1) selects the
// rows of unit_evo_omni_type_mst (Zel/Karma) and unit_evo_omni_recipe_mst
// (materials) for the chosen recipe type; the request only says which recipe
// and which duplicate.  An ELE_SHARD row lists all six shards (800901 Ignis ..
// 800906 Umbra, element order) and costs the one matching the base's element.
// Everything is checked before anything is written, and commits in one
// transaction.
//
// Effects (gme/common/OmniPlus.hpp): omni_level + 1, and the level's SP step
// added to both fe_sp and fe_max_sp.  The raised imp caps need no write: they
// are applied where imps are capped (UnitMix, BalanceSync).
//
// Reply: the UnitEvo response shape without its evolution result — the full
// unit cache (4ceMWH6k, now carrying 49sa3sld), team info (Zel/Karma), the
// warehouse (spent items) and the helper lists.
namespace
{

struct OmniRecipeTypeNode
{
	std::string recipe_type;
};

struct OmniEvoBody
{
	std::vector<OmniRecipeTypeNode> recipe;
};

/// "10011" or the legacy evolved form "10011_100" -> 10011.
int32_t mstIdOf(const std::string& raw)
{
	const auto numPart = raw.substr(0, raw.find('_'));
	try { return numPart.empty() ? 0 : std::stoi(numPart); } catch (...) { return 0; }
}


struct OmniPrice
{
	int64_t zel = 0;
	int64_t karma = 0;
	std::map<int32_t, int32_t> items;  // item id -> count
	bool duplicate = false;
	bool found = false;
};

/// The (cost, level, recipe type) price, or found=false when the tables have
/// no such step.  The shard row resolves to the base's own element.
OmniPrice priceFor(const int32_t cost, const int32_t level, const int32_t type, const int32_t element)
{
	OmniPrice price;
	const auto& cache = theServer()->cache();
	for (const auto& row : cache.unitEvoOmniTypeMst())
	{
		if (row.cost == cost && row.omni_level == level && row.evo_type == type)
		{
			price.zel = row.zel;
			price.karma = row.karma;
			price.found = true;
		}
	}
	for (const auto& row : cache.unitEvoOmniRecipeMst())
	{
		if (row.cost != cost || row.omni_level != level || row.evo_type != type)
			continue;
		if (row.material_type == "DUPLICATE")
			price.duplicate = true;
		else if (row.material_type == "ELE_SHARD")
		{
			if (element < 0 || element >= static_cast<int32_t>(row.material_id.size()))
				return OmniPrice{};
			price.items[row.material_id[element]] += row.material_count;
		}
		else if (!row.material_id.empty())
			price.items[row.material_id[0]] += row.material_count;
	}
	return price;
}

} // namespace

template <>
struct glz::meta<OmniRecipeTypeNode>
{
	static constexpr auto value = glz::object("s049sdck", &OmniRecipeTypeNode::recipe_type);
};

template <>
struct glz::meta<OmniEvoBody>
{
	static constexpr auto value = glz::object("34d0Slkf", &OmniEvoBody::recipe);
};

HANDLEF(UnitOmniEvo)
{
	(void)session;
	LOG_INFO << "UnitOmniEvo: " << json;

	UnitEvoReq req = {};
	OmniEvoBody body = {};
	{
		constexpr glz::opts kLoose{ .error_on_unknown_keys = false };
		glz::context ctx{};
		if (const auto& ec = glz::read<kLoose>(req, json, ctx); ec)
		{
			LOG_WARN << "UnitOmniEvo: bad request JSON: " << glz::format_error(ec, json);
			co_return HandleResult::error("Deserialization error");
		}
		glz::context ctx2{};
		if (const auto& ec = glz::read<kLoose>(body, json, ctx2); ec || body.recipe.empty())
		{
			LOG_WARN << "UnitOmniEvo: no recipe type in the request";
			co_return HandleResult::refuseToHome("Omni+ Boost could not be completed. Nothing was spent.",
			"UnitOmniEvo: incomplete request");
		}
	}

	int32_t recipeType = 0;
	try { recipeType = std::stoi(body.recipe[0].recipe_type); } catch (...) {}

	int32_t baseId = 0;
	int32_t dupId = 0;
	for (const auto& e : req.elem_units)
	{
		const auto kind = e.material_kind.value_or(0);
		if (e.role == "1" && kind == 1)
			baseId = e.user_unit_id;
		else if (kind == 3)
			dupId = e.user_unit_id;
	}
	if (baseId == 0 || (recipeType != 1 && recipeType != 2) || (dupId != 0 && dupId == baseId))
	{
		LOG_WARN << "UnitOmniEvo: missing base unit, bad recipe type " << recipeType
			<< " or the base named as its own duplicate";
		co_return HandleResult::refuseToHome("Omni+ Boost could not be completed. Nothing was spent.",
			"UnitOmniEvo: incomplete request");
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
	const std::string kUserId = identity.userId;

	UnitEvoResp resp = {};
	std::string buffer{};

	auto transaction = co_await gme::newTransaction(theDb());
	try
	{
		const auto baseRows = co_await transaction->execSqlCoro(
			"SELECT unit_id, omni_level, sbb_lvl FROM user_units"
			" WHERE user_id=$1 AND user_unit_id=$2 LIMIT 1;",
			kUserId, baseId);
		if (baseRows.empty())
			throw std::runtime_error("UnitOmniEvo: base unit not found");
		const int32_t mstId = mstIdOf(baseRows[0]["unit_id"].as<std::string>());
		const int32_t fromLevel = baseRows[0]["omni_level"].as<int32_t>();
		const int32_t toLevel = fromLevel + 1;
		if (toLevel > gme::kOmniPlusMaxLevel)
			throw std::runtime_error("UnitOmniEvo: unit is already +" + std::to_string(fromLevel));
		// The client offers Omni+ only to an Omni unit at SBB 10
		// (UnitEvoSelectScene::isOmniEvo @0x1BFBC18: rarity 8, getExtraSkillLv
		// 10, omni level 0..2, a UnitOmniEvoMst row for (cost, level + 1)).
		if (baseRows[0]["sbb_lvl"].as<int32_t>() != 10)
			throw std::runtime_error("UnitOmniEvo: the unit's SBB is not at level 10");

		const UnitMst* mst = nullptr;
		for (const auto& u : theServer()->cache().unitMst())
			if (u.id == mstId) { mst = &u; break; }
		if (!mst || mst->rarity != 8)
			throw std::runtime_error("UnitOmniEvo: unit " + std::to_string(mstId) + " is not an Omni unit");

		// The element comes from UnitMst (1 Fire .. 6 Dark): user_units.element
		// is empty on almost every row (only UnitEvo writes it), which failed
		// every boost on prod with "no recipe" (2026-10-22).
		const auto price = priceFor(mst->cost, toLevel, recipeType, mst->element - 1);
		if (!price.found)
			throw std::runtime_error("UnitOmniEvo: no +" + std::to_string(toLevel)
				+ " recipe for cost " + std::to_string(mst->cost));
		if (price.duplicate != (dupId != 0))
			throw std::runtime_error("UnitOmniEvo: duplicate missing or not wanted");

		if (dupId != 0)
		{
			const auto dupRows = co_await transaction->execSqlCoro(
				"SELECT unit_id, favorite_flg FROM user_units"
				" WHERE user_id=$1 AND user_unit_id=$2 LIMIT 1;",
				kUserId, dupId);
			if (dupRows.empty())
				throw std::runtime_error("UnitOmniEvo: duplicate not owned");
			if (mstIdOf(dupRows[0]["unit_id"].as<std::string>()) != mstId)
				throw std::runtime_error("UnitOmniEvo: duplicate is a different unit");
			if (dupRows[0]["favorite_flg"].as<int32_t>() != 0)
				throw std::runtime_error("UnitOmniEvo: favorite duplicate cannot be consumed");
		}

		for (const auto& [itemId, count] : price.items)
		{
			const auto stock = co_await transaction->execSqlCoro(
				"SELECT item_num FROM user_items WHERE user_id=$1 AND item_id=$2;", kUserId, itemId);
			if (stock.empty() || stock[0]["item_num"].as<int64_t>() < count)
				throw std::runtime_error("UnitOmniEvo: not enough of item " + std::to_string(itemId));
		}
		const auto wallet = co_await transaction->execSqlCoro(
			"SELECT zel, karma FROM user_info WHERE id=$1;", kUserId);
		if (wallet.empty() || wallet[0]["zel"].as<int64_t>() < price.zel
			|| wallet[0]["karma"].as<int64_t>() < price.karma)
			throw std::runtime_error("UnitOmniEvo: not enough zel/karma");

		const int32_t sp = gme::omniSpStep(mst->cost, toLevel);
		co_await transaction->execSqlCoro(
			"UPDATE user_units SET omni_level=$1, fe_sp = fe_sp + $2, fe_max_sp = fe_max_sp + $2"
			" WHERE user_id=$3 AND user_unit_id=$4;",
			toLevel, sp, kUserId, baseId);

		uint32_t spheresReturned = 0;
		if (dupId != 0)
		{
			const auto dupList = std::to_string(dupId);
			spheresReturned = co_await gme::returnEquippedSpheres(transaction, identity, dupList);
			co_await transaction->execSqlCoro(
				"DELETE FROM user_units WHERE user_id=$1 AND user_unit_id=$2;", kUserId, dupId);
		}
		for (const auto& [itemId, count] : price.items)
		{
			co_await transaction->execSqlCoro(
				"UPDATE user_items SET item_num = item_num - $1 WHERE user_id=$2 AND item_id=$3;",
				count, kUserId, itemId);
		}
		co_await transaction->execSqlCoro(
			"UPDATE user_info SET zel = zel - $1, karma = karma - $2 WHERE id=$3;",
			price.zel, price.karma, kUserId);

		LOG_INFO << "UnitOmniEvo: unit " << baseId << " (" << mstId << ", cost " << mst->cost
			<< ") +" << fromLevel << " -> +" << toLevel << ", recipe " << recipeType
			<< ", " << price.zel << " zel, " << price.karma << " karma, +" << sp << " SP";

		resp.unit_refresh = std::move((co_await db::PacketInterfaceFor<::UserUnitInfo>::read(
			transaction,
			"user_units",
			{ db::Lookup("user_id", kUserId) })).data);
		resp.team_info = std::move(
			(co_await gme::getTeamInfo(transaction, identity)).nonEmpty());
		if (spheresReturned > 0 || !price.items.empty())
		{
			auto warehouse = co_await gme::loadWarehouseSnapshot(transaction, identity);
			resp.warehouse_info = std::move(warehouse.warehouse);
			resp.item_favorite = std::move(warehouse.favorites);
			resp.item_dictionary_info = std::move(warehouse.dictionary);
		}
		{
			// The boosted unit may be a favourited own helper card (UnitEvo).
			auto lists = co_await gme::buildHelperLists(transaction, identity);
			resp.helper_list = std::move(lists.reinforce_info);
			resp.helper_friends = std::move(lists.friend_info);
		}

		if (const auto& ec2 = glz::write_json(resp, buffer); ec2)
			throw std::runtime_error("UnitOmniEvo: serialization error: " + glz::format_error(ec2, buffer));
	}
	catch (const std::exception& e)
	{
		transaction->rollback();
		LOG_WARN << e.what();
		co_return HandleResult::refuseToHome("Omni+ Boost could not be completed. Nothing was spent.", e.what());
	}

	co_return HandleResult::success(buffer);
}
