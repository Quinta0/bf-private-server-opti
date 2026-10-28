#include "App.hpp"
#include "Handlers.hpp"
#include <gimuserver/forge/Tuning.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <gimuserver/gme/common/SummonerJournal.hpp>

#include <gimuserver/db/PacketInterface.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/DailyTask.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

// UnitEvo — evolve a unit into its next form.
//
// Request  (group 0gUSE84e, key biHf01DxcrPou5Qt), UnitEvoRequest::createBody
// @0x13AE818:
//   "8Z2NQrx1": base  {mnZ5K4Ii 1, 29MgiJIQ 1, inU8Q4gL user_unit_id},
//               each material unit {2, 1, user_unit_id},
//               each material ITEM {2, 2, UserWarehouseInfo::getItemIndex}
//   "Km35HAXv": the base and the material units again, by edy7fq3L
//   "I82p0wCL": [{"pn16CNah":"<targetMstId>"}]   UnitEvoMst::getEvoUnitID
//   "mCE3rUu5": [{"Rs7bCE3t":"<zel>"}]           UnitEvoMst::getAmount
//
// Response:
//   "I82p0wCL": [EvoResultEntry]   — which MST id was evolved into
//   "4ceMWH6k": [UserUnitInfo]     — full-replace unit cache
//   "fEi17cnx": [UserTeamInfo]     — zel / karma
//   "9wjrh74P"/"bd5Rj6pN"          — warehouse, when items were spent or spheres returned
//
// THE RECIPE IS THE SERVER'S, NOT THE REQUEST'S.  The evolution is looked up in
// UnitEvoMst by (current form, target) and must match it exactly: the
// material units the player picked are those species and no others, the
// recipe's items are in storage, and Zel/Karma cover the recipe's price (the
// request's zel is only what the client computed from its own copy).
// Everything -- the evolved row, returned spheres, deleted materials, spent
// items and currency -- commits in one transaction, so a refusal changes
// nothing.  An item node's inU8Q4gL is a warehouse index, never a unit id: the
// old handler added it to the material list and would have deleted whichever
// unit happened to have that id.
//
// NOTE: this handler must NOT populate ope_result (1ZbHB6Im).  UnitOpeResult is
// a singleton whose only scene consumer is UnitMixPlayScene::parseMixResult —
// the evolution result screen reads UnitEvoInfo (I82p0wCL) instead.
namespace
{

std::string elementName(const int e)
{
    switch (e) {
        case 1: return "fire";
        case 2: return "water";
        case 3: return "earth";
        case 4: return "thunder";
        case 5: return "light";
        case 6: return "dark";
        default: return "fire";
    }
}

/// "10011" or the legacy evolved form "10011_100" -> 10011.
int32_t mstIdOf(const std::string& raw)
{
    const auto numPart = raw.substr(0, raw.find('_'));
    try { return numPart.empty() ? 0 : std::stoi(numPart); } catch (...) { return 0; }
}

struct Recipe
{
    const UnitEvoMst* row = nullptr;
    std::multiset<int32_t> units;            // material unit MST ids
    std::map<int32_t, int32_t> items;        // item id -> count
};

/// UnitEvoMst for (from -> to), with its nine slots split by kind: slot kind 2
/// is an ItemMst id, anything else a unit id (see mst/unit_evo.kdl).
std::optional<Recipe> findRecipe(const int32_t from, const int32_t to)
{
    for (const auto& row : theServer()->cache().unitEvoMst())
    {
        if (row.unit_id != from || row.evo_unit_id != to)
            continue;
        Recipe r;
        r.row = &row;
        const std::pair<int32_t, int32_t> slots[] = {
            { row.material_1, row.material_1_kind }, { row.material_2, row.material_2_kind },
            { row.material_3, row.material_3_kind }, { row.material_4, row.material_4_kind },
            { row.material_5, row.material_5_kind }, { row.material_6, row.material_6_kind },
            { row.material_7, row.material_7_kind }, { row.material_8, row.material_8_kind },
            { row.material_9, row.material_9_kind },
        };
        for (const auto& [id, kind] : slots)
        {
            if (id == 0)
                continue;
            if (kind == 2)
                ++r.items[id];
            else
                r.units.insert(id);
        }
        return r;
    }
    return std::nullopt;
}

/// Everything an evolution reply carries once the unit is in its new form:
/// the result row, the full unit cache, team info, the helper lists and, when
/// items or spheres moved, the warehouse snapshot.
drogon::Task<void> fillEvoReply(
    const db::Database database,
    const gme::UserIdentity identity,
    UnitEvoResp& resp,
    const int32_t fromMstId,
    const int32_t toMstId,
    const int32_t userUnitId,
    const bool warehouseChanged)
{
    {
        // UnitOpeEvoResponse::readParam builds a UnitEvoInfo from five keys
        // and commits it with UnitEvoInfoList::addObject.
        EvoResultEntry er = {};
        er.evolved_unit_id     = toMstId;     // pn16CNah — setUnitID, the form evolved INTO
        er.user_unit_id        = userUnitId;  // edy7fq3L — setUserUnitID
        er.orig_mst_id         = fromMstId;   // t9FEW2KC — setUnitIDBefore, drives the evo animation
        er.user_unit_id_before = userUnitId;  // u1ECvfg8 — setUserUnitIDBefore (evolved in place)
        resp.evo_result.emplace_back(er);
    }

    // FULL-REPLACE THE UNIT CACHE (4ceMWH6k), exactly as UnitMix does:
    // qC2tJs4E is insert-if-absent and the evolved id is always owned.
    resp.unit_refresh = std::move((co_await db::PacketInterfaceFor<::UserUnitInfo>::read(
        database,
        "user_units",
        { db::Lookup("user_id", identity.userId) })).data);

    resp.team_info = std::move(
        (co_await gme::getTeamInfo(database, identity)).nonEmpty());

    // No evolution scene touches the client warehouse, so spent items and
    // returned spheres need the full snapshot.
    if (warehouseChanged)
    {
        auto warehouse = co_await gme::loadWarehouseSnapshot(database, identity);
        resp.warehouse_info = std::move(warehouse.warehouse);
        resp.item_favorite = std::move(warehouse.favorites);
        resp.item_dictionary_info = std::move(warehouse.dictionary);
    }

    // xZH6EIQ7 is the helper picker list: an id-less row there empties it (see
    // UnitMix.cpp), so only the COMPLETE list goes there, refreshed because the
    // evolved unit may be a favourited own helper card.  The evolution screen
    // itself reads I82p0wCL and the 4ceMWH6k roster.
    auto lists = co_await gme::buildHelperLists(database, identity);
    resp.helper_list = std::move(lists.reinforce_info);
    resp.helper_friends = std::move(lists.friend_info);
}

} // namespace

HANDLEF(UnitEvo)
{
    (void)session;
    LOG_INFO << "UnitEvo: " << json;

    UnitEvoReq req = {};
    {
        glz::context ctx{};
        if (const auto& ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(req, json, ctx); ec)
        {
            LOG_WARN << "UnitEvo: bad request JSON: " << glz::format_error(ec, json);
            co_return HandleResult::error("Deserialization error");
        }
    }

    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
    const std::string kUserId = identity.userId;

    // The base and the material UNITS, from both groups (they list the same
    // units).  Item nodes (29MgiJIQ 2) are skipped: their id is an index.
    int32_t baseId = 0;
    std::set<int32_t> matIds;
    for (const auto& u : req.units)
    {
        if (u.role == "1")       baseId = u.user_unit_id;
        else if (u.user_unit_id) matIds.insert(u.user_unit_id);
    }
    for (const auto& e : req.elem_units)
    {
        if (e.material_kind.value_or(1) == 2)
            continue;
        if (e.role == "1")        baseId = e.user_unit_id;
        else if (e.user_unit_id)  matIds.insert(e.user_unit_id);
    }

    if (baseId == 0 || req.target.empty() || matIds.contains(baseId))
    {
        LOG_WARN << "UnitEvo: missing base unit or target, or the base named as a material";
        co_return HandleResult::error("UnitEvo: incomplete request");
    }

    const int32_t targetMstId = req.target[0].target_mst_id;
    const auto& unitMst = theServer()->cache().unitMst();
    const UnitMst* targetMst = nullptr;
    for (const auto& u : unitMst) { if (u.id == targetMstId) { targetMst = &u; break; } }
    if (!targetMst)
    {
        LOG_WARN << "UnitEvo: target MST id " << targetMstId << " not found";
        co_return HandleResult::error("UnitEvo: unknown target unit");
    }

    UnitEvoResp resp = {};
    std::string buffer{};

    auto transaction = co_await gme::newTransaction(theDb());
    try
    {
        const auto baseRows = co_await transaction->execSqlCoro(
            "SELECT user_unit_id, unit_id, unit_lvl, add_hp, add_atk, add_def, add_rec,"
            " ext_hp, ext_atk, ext_def, ext_rec, unit_type_id, bb_lvl, sbb_lvl,"
            " eqip_item_id FROM user_units WHERE user_id=$1 AND user_unit_id=$2 LIMIT 1;",
            kUserId, baseId);
        if (baseRows.empty())
            throw std::runtime_error("UnitEvo: base unit not found");
        const auto& br = baseRows[0];
        const int32_t origMstId = mstIdOf(br["unit_id"].as<std::string>());

        // A RETRIED EVOLUTION.  When the client never applies the first reply,
        // its screen still shows the old form at max level and it sends the
        // identical request again; the base is already the target, so every
        // check below refused it and the player saw Karma and materials gone
        // but no new form (ticket #138).  Answer with the result of
        // the evolution that already happened; nothing is charged or counted.
        if (origMstId == targetMstId)
        {
            const auto& evoRows = theServer()->cache().unitEvoMst();
            const auto before = std::find_if(evoRows.begin(), evoRows.end(),
                [targetMstId](const auto& row) { return row.evo_unit_id == targetMstId; });
            if (before == evoRows.end())
                throw std::runtime_error("UnitEvo: nothing evolves into " + std::to_string(targetMstId));
            LOG_INFO << "UnitEvo: unit " << baseId << " is already " << targetMstId
                     << "; replaying the result, nothing charged";
            co_await fillEvoReply(transaction, identity, resp, before->unit_id, targetMstId, baseId, true);
            if (const auto& ec = glz::write_json(resp, buffer); ec)
                throw std::runtime_error("UnitEvo: serialization error: " + glz::format_error(ec, buffer));
            co_return HandleResult::success(buffer);
        }

        const auto sourceMst = std::find_if(unitMst.begin(), unitMst.end(),
            [origMstId](const auto& row) { return row.id == origMstId; });
        if (sourceMst == unitMst.end() || br["unit_lvl"].as<int32_t>() < sourceMst->max_lv)
            throw std::runtime_error("UnitEvo: base unit must be at maximum level");

        const auto recipe = findRecipe(origMstId, targetMstId);
        if (!recipe)
            throw std::runtime_error("UnitEvo: " + std::to_string(origMstId)
                + " does not evolve into " + std::to_string(targetMstId));

        // The chosen material units must be exactly the recipe's species.
        std::multiset<int32_t> chosen;
        std::string matList;
        if (!matIds.empty())
        {
            db::Values ids;
            for (const auto id : matIds)
            {
                ids.emplace_back(static_cast<int64_t>(id));
                if (!matList.empty()) matList += ',';
                matList += std::to_string(id);
            }
            const auto matRows = (co_await db::DatabaseInterface::read(transaction, "user_units", {
                db::Data("unit_id"),
                db::Data("favorite_flg"),
                db::Lookup("user_id", kUserId),
                db::LookupIn("user_unit_id", ids),
            })).data;
            if (matRows.size() != matIds.size())
                throw std::runtime_error("UnitEvo: a material unit is not owned");
            for (const auto& row : matRows)
            {
                if (row["favorite_flg"].as<int32_t>() != 0)
                    throw std::runtime_error("UnitEvo: favorite material cannot be consumed");
                chosen.insert(mstIdOf(row["unit_id"].as<std::string>()));
            }
        }
        // DUPLICATE EVOLUTION (ticket #157): instead of the recipe, one
        // copy of the base itself (same unit id, so the same evolution stage, any
        // level).  The patched client (tools/patch_apk.py --dup-evo) sends it as
        // the only material.  The recipe's own shape is tested first, so a recipe
        // that really is one self copy keeps its price.  The copy is consumed, so
        // besides the favourite check above it must be in no squad of any kind
        // and in no Dual Brave Burst bond.  Its cost is the recipe's Zel and Karma
        // scaled by forge/Tuning.hpp dup_evo_*_pct (0 by default); no items.
        // The copy consumed is the player's LOWEST-LEVEL eligible one, whichever
        // the client named: the client picks the first it finds, and this reply
        // rebuilds its whole unit list (fillEvoReply), so a levelled copy is
        // never eaten while a fresh one is there.
        const auto tune = forge::tuning();
        const bool duplicate = chosen != recipe->units && tune.dupEvoEnabled
            && matIds.size() == 1 && chosen == std::multiset<int32_t>{origMstId};
        if (chosen != recipe->units && !duplicate)
            throw std::runtime_error("UnitEvo: materials do not match the recipe for "
                + std::to_string(origMstId));
        if (duplicate)
        {
            const auto copies = co_await transaction->execSqlCoro(
                "SELECT u.user_unit_id, u.unit_id FROM user_units u"
                " WHERE u.user_id = $1 AND u.user_unit_id <> $2 AND u.favorite_flg = 0"
                " AND NOT EXISTS (SELECT 1 FROM user_decks d WHERE d.user_id = $1 AND d.user_unit_id = u.user_unit_id)"
                " AND NOT EXISTS (SELECT 1 FROM user_trial_decks d WHERE d.user_id = $1 AND d.user_unit_id = u.user_unit_id)"
                " AND NOT EXISTS (SELECT 1 FROM user_campaign_decks d WHERE d.user_id = $1 AND d.user_unit_id = u.user_unit_id)"
                " AND NOT EXISTS (SELECT 1 FROM user_unit_dbb b WHERE b.user_id = $1"
                "  AND (b.user_unit_id = u.user_unit_id OR b.bonded_user_unit_id = u.user_unit_id))"
                " ORDER BY u.unit_lvl, u.user_unit_id;",
                kUserId, baseId);
            int64_t copy = 0;
            for (const auto& row : copies)
            {
                if (mstIdOf(row["unit_id"].as<std::string>()) == origMstId)
                {
                    copy = row["user_unit_id"].as<int64_t>();
                    break;
                }
            }
            if (copy == 0)
                throw std::runtime_error("UnitEvo: no free duplicate (favourite, in a squad or in a bond)");
            matList = std::to_string(copy);
            LOG_INFO << "UnitEvo: duplicate evolution of " << baseId << " with " << copy
                     << " (client named " << *matIds.begin() << ")";
        }

        // Items and currency, all checked before anything is written.
        const std::map<int32_t, int32_t> noItems;
        const auto& items = duplicate ? noItems : recipe->items;
        for (const auto& [itemId, count] : items)
        {
            const auto stock = co_await transaction->execSqlCoro(
                "SELECT item_num FROM user_items WHERE user_id=$1 AND item_id=$2;", kUserId, itemId);
            if (stock.empty() || stock[0]["item_num"].as<int64_t>() < count)
                throw std::runtime_error("UnitEvo: not enough of item " + std::to_string(itemId));
        }
        const auto wallet = co_await transaction->execSqlCoro(
            "SELECT zel, karma FROM user_info WHERE id=$1;", kUserId);
        const auto scaled = [duplicate](int64_t cost, double pct) {
            return duplicate ? static_cast<int64_t>(std::llround(static_cast<double>(cost) * pct / 100.0)) : cost;
        };
        const int64_t zelCost = scaled(recipe->row->zel, tune.dupEvoZelPct);
        const int64_t karmaCost = scaled(recipe->row->karma, tune.dupEvoKarmaPct);
        if (wallet.empty() || wallet[0]["zel"].as<int64_t>() < zelCost
            || wallet[0]["karma"].as<int64_t>() < karmaCost)
            throw std::runtime_error("UnitEvo: not enough zel/karma");
        if (!duplicate && !req.zel_cost_list.empty() && req.zel_cost_list[0].cost != zelCost)
            LOG_WARN << "UnitEvo: client priced " << req.zel_cost_list[0].cost
                     << " zel, the recipe costs " << zelCost << "; charging the recipe";

        // BURST SKILLS COME FROM THE NEW FORM.  The unit packet reads bb_id /
        // sbb_id / bb_lvl / sbb_lvl (PacketInterfaceSchemas.hpp); this used to
        // write the legacy skill_id / extra_skill_id columns that nothing sends,
        // so every evolved unit kept its old form's Brave Burst and a form that
        // gains a Super Brave Burst never showed it.  The legacy columns are
        // kept in step only so nothing reading them disagrees.
        //
        // Levels are HALVED, rounded down (Global wiki, Unit Skills: "Upon
        // evolving, the unit's BB Level will be halved, rounded down"; the
        // page's own example contradicts the rule and is not followed).  A unit
        // with a BB never drops below 1.  An unlocked SBB halves but stays
        // unlocked (>= 1); a locked one (0) stays locked -- including a form
        // that gains an SBB, which unlocks at BB 10 as usual ("The normal Brave
        // Burst must first be levelled to 10 (MAX)").  UBB needs no state: the
        // client derives it from the SBB level and its own UnitMst.
        const int32_t oldBbLvl  = br["bb_lvl"].as<int32_t>();
        const int32_t oldSbbLvl = br["sbb_lvl"].as<int32_t>();
        const int32_t newBbLvl  = targetMst->skill_id == 0
            ? 0 : std::max(1, oldBbLvl / 2);
        const int32_t newSbbLvl = (oldSbbLvl <= 0 || targetMst->extra_skill_id == 0)
            ? 0 : std::max(1, oldSbbLvl / 2);
        const auto bbId  = targetMst->skill_id == 0 ? std::string("0") : std::to_string(targetMst->skill_id);
        const auto sbbId = targetMst->extra_skill_id == 0 ? std::string("0") : std::to_string(targetMst->extra_skill_id);

        // The evolved row: level/exp reset, the new form's base stats, element,
        // bursts and leader skill.  Kept: imps (add_*), type growth (ext_*),
        // limit-over, spheres, sphere capacity and FE -- none of them named here.
        co_await transaction->execSqlCoro(
            "UPDATE user_units SET"
            " unit_id=$1, unit_lvl=1, exp=0, total_exp=0,"
            " base_hp=$2, base_atk=$3, base_def=$4, base_rec=$5,"
            " bb_id=$6, bb_lvl=$7, sbb_id=$8, sbb_lvl=$9,"
            " skill_id=$10, skill_lv=$7, extra_skill_id=$11, extra_skill_lv=$9,"
            " leader_skill_id=$12, element=$13"
            " WHERE user_unit_id=$14 AND user_id=$15;",
            std::to_string(targetMstId),
            targetMst->min_hp, targetMst->min_atk, targetMst->min_def, targetMst->min_rec,
            bbId, newBbLvl, sbbId, newSbbLvl,
            targetMst->skill_id, targetMst->extra_skill_id,
            targetMst->leader_skill_id, elementName(targetMst->element),
            baseId, kUserId);

        // The art the unit shows (user_units.unit_img_type, see UnitFavorite)
        // belongs to the OLD form's unlock.  The new form keeps the alternate
        // art only if the player owns that form's too; otherwise it would come
        // back on art it has no unlock for, with no swap arrow to undo it
        // (860257 -> 860258 Zenia: only the first has an alt-art offer).
        co_await transaction->execSqlCoro(
            "UPDATE user_units SET unit_img_type = 0"
            " WHERE user_id = $1 AND user_unit_id = $2 AND unit_img_type <> 0"
            " AND NOT EXISTS (SELECT 1 FROM user_unit_alt_art a"
            "  WHERE a.user_id = $1 AND a.unit_id = CAST(user_units.unit_id AS INTEGER));",
            kUserId, baseId);

        // The new form goes into the Unit Guide.  The row above changes the
        // unit's id in place, so addUserUnit -- which is what records every
        // other way a unit arrives -- never runs, and an evolved form stayed
        // hidden from the guide unless it was also summoned or dropped
        // (ticket #118).  Its lower forms come with it.
        co_await gme::recordUnitDictionary(transaction, kUserId, static_cast<uint32_t>(targetMstId));

        LOG_INFO << "UnitEvo: " << origMstId << " -> " << targetMstId
            << ", bb " << oldBbLvl << "->" << newBbLvl << " (" << bbId << ")"
            << ", sbb " << oldSbbLvl << "->" << newSbbLvl << " (" << sbbId << ")";

        // Spheres on the materials go back to storage before the rows go.
        uint32_t spheresReturned = 0;
        if (!matList.empty())
        {
            spheresReturned = co_await gme::returnEquippedSpheres(transaction, identity, matList);
            co_await transaction->execSqlCoro(
                "DELETE FROM user_units WHERE user_id=$1 AND user_unit_id IN (" + matList + ");",
                kUserId);
        }

        for (const auto& [itemId, count] : items)
        {
            // Rows stay at zero, like every other spend (instance ids survive).
            co_await transaction->execSqlCoro(
                "UPDATE user_items SET item_num = item_num - $1 WHERE user_id=$2 AND item_id=$3;",
                count, kUserId, itemId);
        }

        co_await transaction->execSqlCoro(
            "UPDATE user_info SET zel = zel - $1, karma = karma - $2 WHERE id=$3;",
            zelCost, karmaCost, kUserId);

        // DAILY TASK `UU` ("Evolve any Units 3 Times").
        co_await gme::advanceDailyTask(transaction, identity, "UU");

        // No GV81ctzR on this reply (ticket #138, net/handlers.kdl
        // UnitEvoResp): the guide row written above arrives with the next UserInfo.
        co_await fillEvoReply(transaction, identity, resp, origMstId, targetMstId, baseId,
            spheresReturned > 0 || !items.empty());

        if (const auto& ec2 = glz::write_json(resp, buffer); ec2)
            throw std::runtime_error("UnitEvo: serialization error: " + glz::format_error(ec2, buffer));

        // Journal: "Evolve 1 unit".  Trophy 100240 進化回数.
        co_await gme::addJournalProgress(transaction, identity, gme::kJournalTaskEvolution, 1);
        co_await gme::bumpArchiveCounters(transaction, identity, {
            { "unit_evo_cnt", 1 },
        });
    }
    catch (...)
    {
        transaction->rollback();
        throw;
    }

    co_return HandleResult::success(buffer);
}
