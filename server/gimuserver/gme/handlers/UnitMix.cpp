#include "App.hpp"
#include "Handlers.hpp"
#include <gimuserver/gme/common/OmniPlus.hpp>
#include <gimuserver/forge/Tuning.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <gimuserver/gme/common/SummonerJournal.hpp>

#include <gimuserver/db/PacketInterface.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/DailyTask.hpp>
#include <gimuserver/gme/common/Dbb.hpp>
#include <gimuserver/gme/common/Transactions.hpp>
#include <algorithm>
#include <charconv>
#include <array>
#include <random>
#include <cmath>
#include <deque>

// UnitMix (Power Fusion) — fuse material units into a base unit, gaining exp.
//
// Request  (group Mw08CIg2, key JnegC7RrN3FoW8dQ):
//   "60subGk3": [{"81GjwoWy":"1","2vnqRIr3":"2"}]  — mix/ingredient type (log only)
//   "mCE3rUu5": [{"Rs7bCE3t":"<zelCost>"}]          — the CLIENT's zel price (string):
//                                                    logged, never charged (unitMix_mixGold)
//   "Km35HAXv": [{"edy7fq3L":"<id>","mnZ5K4Ii":"1"}, // base unit  (role 1)
//                {"edy7fq3L":"<id>","mnZ5K4Ii":"2"}, // material   (role 2) x N]
//
// Response:
//   "1ZbHB6Im": [UnitOpeResult]      — the result screen (xp sweep, BB level)
//   "4ceMWH6k": [UserUnitInfo]       — full-replace unit cache
//   "fEi17cnx": [UserTeamInfo]       — updated zel
//   never "xZH6EIQ7" — that is the helper picker list (see "Build response")

// The request struct (UnitMixReq + UnitMixUnitEntry/UnitMixZelEntry) and the
// response struct (UnitMixResp) are generated from
// packet-generator/assets/net/{handlers,unit}.kdl.

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string unitMix_stripSuffix(const std::string& raw)
{
    auto pos = raw.find('_');
    return (pos != std::string::npos) ? raw.substr(0, pos) : raw;
}

// ---------------------------------------------------------------------------
// Special fusion materials
//
// Not every material is exp fodder.  Two families do something else entirely,
// and until now this handler ignored both — fusing a Burst Frog left the BB
// gauge at 0/10 and fusing an imp changed no stat at all.
//
//   Burst frogs  raise the recipient's BB/SBB level.  How far is DATA: the
//                fodder's own UnitMst::burst_level_boost (PXD4v2KY), which is
//                non-zero on exactly the four frogs.
//
//   Imps         permanently raise one stat, up to a cap that is per-RECIPIENT
//                (UnitMst::imp_caps, "HP:ATK:DEF:REC" — 147 distinct values,
//                and 0:0:0:0 for fodder that cannot be imped at all).
//
// The imp AMOUNTS are the one part with no column behind them; they come from
// the wiki's Units page, which states each imp's effect outright.
// ---------------------------------------------------------------------------
// The Sphere Frog (unit 20302, MST_UNIT_20302_NAME) is the third special
// material.  It grants the base unit a second sphere slot, and the game's own
// help text (MST_HELP_SUBTOPIC_100_8_DESCRIPTION, "Sphere Capacity Increase")
// states the rules outright:
//
//   "the sphere capacity of the base unit will increase.  You can only add 1
//    extra sphere per unit, so you cannot fuse 2 or more Sphere Frogs into 1
//    unit.  Those units who have increased their sphere capacity will inherit
//    this trait even when they evolve.  However, even if you fuse them with
//    other materials afterwards, their sphere capacity will not increase any
//    further."
//
// So: one bit, set once, idempotent, inherited by evolution (which updates the
// row in place), and granted by nothing else.  Extra frogs are still consumed
// as ordinary exp fodder -- the cap is on the slot, not on the fusion.
static constexpr int32_t kSphereFrogUnitId = 20302;

// ---------------------------------------------------------------------------
// Omni enhancement SP (#25)
//
// A max-level Omni unit whose SBB is at 10 collects SP from fusion.  Which
// materials give how much is the CLIENT's own table --
// UserState::getUserUnitListEfPoint @0x1247888, the number the fusion screen
// uses to decide whether an SP-only material (a Burst or Omni Frog fed to a
// maxed unit) is selectable at all -- and it matches the Global wiki's list
// figure for figure (Unit Skills rev 656738, "Enhancements"):
//
//   burst_level_boost 1..4   Burst Frog 1, Lawson Frog 2 (the value itself
//                            when it is 2, else 1)
//   burst_level_boost > 4    Burst Emperor 5; 20 on Burst Queen (750004) only
//   Sphere Frog              10, but only once the base ALREADY has its second
//                            sphere slot (UnitExtMst.double_sphere_trigger)
//   same category_no         5 -- a duplicate, in any evolutionary form
//   Omni Frog (750003)       30
//   Omni Emperor (750005)    5, and +5 to the limit, only at Omni+3 and
//                            only while the limit is below 130 --
//                            UnitMixElemSelectScene::checkElemUnit @0x1C09B98:
//                            feUnitCheck, omni level >= 3, then
//                            getFeMaxUsableBP() + the limit the Emperors
//                            already picked add < 0x82.  A +3 unit sits at
//                            120, so two Emperors take it to the 130 ceiling.
//                            Until 2026-10-23 they were spent as exp here.
//
//   any other unit           a CHANCE of 1 (the wiki: "Chance to raise by 1";
//                            the fusion screen's "SPUP!?", FEUPMaybe).  Which
//                            units: GameUtils::bbUpTypeFeChange @0x11A86D8 gives
//                            11 (Maybe) for an ordinary unit -- UnitMst kind 0
//                            with a BB -- of another category, 12 (Certainty,
//                            the +5 above) for the same category; frogs, imps
//                            and evolution materials are not kind 0.
//                            THE RATE IS AN ESTIMATE: no source states it.  Alim
//                            said only that higher-star fodder raises it
//                            "slightly" (BF Live vol. 30 Q&A, 2016-11-28);
//                            players quoted 20% a unit and ~30 SP from 99.  So
//                            20% plus 1% a star above 1, both Forge-tunable
//                            (forge/Tuning.hpp: fusion_sp_chance_pct,
//                            fusion_sp_chance_per_star_pct), rolled per unit.
//
// Eligibility is the client's GameUtils::feUnitCheck @0x11A8504 on the unit
// as the material screen saw it: rarity >= 8, SBB level exactly 10, level ==
// UnitMst max_lv.  The limit is fe_sp + fe_used_sp <= fe_max_sp
// (GameUtils::mixFeBpCheck @0x1EC4C80).
// ---------------------------------------------------------------------------
static constexpr int32_t kOmniFrogUnitId    = 750003;
static constexpr int32_t kBurstQueenUnitId  = 750004;
static constexpr int32_t kOmniEmperorUnitId = 750005;
static constexpr int kOmniEmperorSp         = 5;    // SP and limit, each
static constexpr int kOmniEmperorMinLevel   = 3;    // Omni+3
static constexpr int kOmniEmperorLimitCap   = 130;  // checkElemUnit's 0x82

// The "SPUP!?" units: an ordinary unit with a BB from another category.
static bool unitMix_isSpChance(const UnitMst& base, const UnitMst& mat)
{
    return mat.kind == 0 && mat.skill_id != 0 && mat.category != base.category;
}

static bool unitMix_isDoubleSphereTrigger(int32_t unitId)
{
    for (const auto& ext : theServer()->cache().unitExtMst())
        if (ext.unit_id == unitId) return ext.double_sphere_trigger != 0;
    return false;
}

static int unitMix_materialSp(const UnitMst& base, const UnitMst& mat, bool baseHasSecondSlot)
{
    int sp = 0;
    const int adjust = mat.burst_level_boost;
    if (adjust >= 1)
    {
        if (adjust <= 4)
            sp += adjust == 2 ? 2 : 1;
        else if (adjust == 20 && mat.id == kBurstQueenUnitId)
            sp += 20;
        else
            sp += 5;
    }
    if (baseHasSecondSlot && unitMix_isDoubleSphereTrigger(mat.id))
        sp += 10;
    if (base.category == mat.category)
        sp += 5;
    if (mat.id == kOmniFrogUnitId)
        sp += 30;
    return sp;
}

// ---------------------------------------------------------------------------
// Brave Burst level from ordinary materials (ticket #119)
//
// Burst frogs carry their gain as data (burst_level_boost, above).  Everything
// else follows the original game's fusion rules, which no MST or client table
// encodes:
//
//   a copy of the base unit   +1, always.  "Copy" is the evolution line
//                             (UnitMst::category, Vargas 10011..10017 = 10010),
//                             the same test the client uses for duplicate SP.
//   Omni Frog (750003)        +1, always (its burst_level_boost is 0).
//   any other unit with a     +1 on a roll, likelier when it shares the base's
//   Brave Burst of its own    element.  BB-less fodder (metals, ghosts, frogs,
//                             emperors) never rolls.
//
// // TUNABLE: the roll's odds are ours; the original server owned them.
// ---------------------------------------------------------------------------
static constexpr int kBurstChanceSameElement  = 20;   // percent per material
static constexpr int kBurstChanceOtherElement = 10;
// The level the UI tops out at, and the client's SBB test for enhancement SP
// (feUnitCheck: SBB level exactly 10).  Every BB and SBB in SkillLevelMst has
// ten levels; this is also the fallback for a skill the table lacks.
static constexpr int kMaxBurstLevel = 10;

// A skill's top level is the last rung of its SkillLevelMst ladder,
// "1:<cost>:<params>|2:...|...|10:...".
static int unitMix_skillMaxLevel(const std::string& skillId)
{
    int32_t id = 0;
    std::from_chars(skillId.data(), skillId.data() + skillId.size(), id);
    if (id <= 0) return kMaxBurstLevel;
    for (const auto& row : theServer()->cache().skillLevelMst())
    {
        if (row.skill_id != id) continue;
        const std::string& info = row.level_info;
        int top = 0;
        for (size_t pos = 0; pos < info.size();)
        {
            int lv = 0;
            std::from_chars(info.data() + pos, info.data() + info.size(), lv);
            top = std::max(top, lv);
            const auto bar = info.find('|', pos);
            if (bar == std::string::npos) break;
            pos = bar + 1;
        }
        return top > 0 ? top : kMaxBurstLevel;
    }
    return kMaxBurstLevel;
}

struct ImpBonus { int hp, atk, def, rec; };

static const ImpBonus* unitMix_impBonus(int32_t unitId)
{
    // id -> what one of them adds.  Five units, stable since launch.
    static const std::pair<int32_t, ImpBonus> kImps[] = {
        { 40432, {  50,  0,  0,  0 } },   // Vigor Imp Molin      +50 HP
        { 10452, {   0, 20,  0,  0 } },   // Power Imp Pakpak     +20 ATK
        { 20442, {   0,  0, 20,  0 } },   // Guard Imp Ganju      +20 DEF
        { 30432, {   0,  0,  0, 20 } },   // Healing Imp Fwahl    +20 REC
        { 50612, { 150, 60, 60, 60 } },   // Almighty Imp Arton   all four
    };
    for (const auto& [id, bonus] : kImps)
        if (id == unitId) return &bonus;
    return nullptr;
}

// UnitMst::param_max is already decoded as a colon-separated int list
// (HP:ATK:DEF:REC).  A short or absent list means "no imps allowed", which is
// what the 0:0:0:0 rows say anyway.
static ImpBonus unitMix_impCaps(const std::deque<int32_t>& paramMax)
{
    ImpBonus caps{ 0, 0, 0, 0 };
    int* out[] = { &caps.hp, &caps.atk, &caps.def, &caps.rec };
    for (size_t i = 0; i < 4 && i < paramMax.size(); ++i)
        *out[i] = paramMax[i];
    return caps;
}

// Cumulative exp needed to REACH `level` from level 1.
// UnitExpPatternMst::need_exp is the incremental cost per level transition.
static int unitMix_expForLevel(const std::vector<UnitExpPatternMst>& pat,
                                int patternId, int level)
{
    int acc = 0;
    for (const auto& e : pat)
    {
        if (e.id != patternId) continue;
        if (e.lv <= 1)         continue;
        if (e.lv > level)      break;
        acc += e.need_exp;
    }
    return acc;
}

// A unit's base stat at a given level.  BF scales linearly from the MST's
// min (level 1) to its max (max_lv); user_units stores only the min, so the
// level scaling has to be recomputed here.
//
// // UNVERIFIED: the rounding.  Linear interpolation is the established BF
// formula, but whether the client floors or rounds at each step has not been
// checked against a capture — an off-by-one in a displayed stat is cosmetic,
// and the structure of the payload is what matters (see buildLvupStatus).
static int unitMix_statAtLevel(int minV, int maxV, int level, int maxLv)
{
    if (maxLv <= 1 || level <= 1) return minV;
    if (level >= maxLv)           return maxV;
    return minV + (int)((double)(maxV - minV) * (double)(level - 1) / (double)(maxLv - 1));
}

// Highest level reachable with totalExp under maxLevel cap.
static int unitMix_levelFromExp(const std::vector<UnitExpPatternMst>& pat,
                                 int patternId, int maxLevel, int totalExp)
{
    int acc = 0, level = 1;
    for (const auto& e : pat)
    {
        if (e.id != patternId)   continue;
        if (e.lv <= 1)           continue;
        if (e.lv > maxLevel)     break;
        if (acc + e.need_exp <= totalExp) { acc += e.need_exp; level = e.lv; }
        else break;
    }
    return level;
}

// ---------------------------------------------------------------------------
// The fusion price (ticket #166 audit)
//
// mCE3rUu5[0].Rs7bCE3t is only the client's own figure, and this handler used
// to charge it as sent -- a client could fuse for 0 Zel.  The figure is
// GameUtils::getMixGold @0x1EAF2C0, which UnitMixRequest::createBody @0x13AF178
// calls with the base (UserState::getSelectUserUnitID) and the selected
// materials (UserState::getUserUnitList); UnitMixMainScene::getMixPrice
// @0x1C10EB8 shows the same number and UnitMixMainScene::isMix @0x1C13048 holds
// it against UserTeamInfo::getZel.  No MST table or DefineMst field is read:
//
//   gold  = sum over materials of (base level + material level) * 100  (float)
//   gold *= 0.25 when the base's level >= its UnitMst max_lv
//   price = gold rounded half away from zero (fcvtas)
//
// "Level" is UserUnitInfoBase::getUnitLv @0xE681AC (UserUnitInfo vtable slot
// 0xb0), the unit cache's D9wXQI2V -- user_units.unit_lvl, NOT the level that
// total_exp maps to.  A base with no UnitMst row gets no discount, as in the
// client (getMixGold skips the 0.25 when UnitMstList::getObject is null).
// ---------------------------------------------------------------------------
namespace
{
int32_t unitMix_mixGold(int baseLv, const std::vector<int>& matLevels, const UnitMst* baseMst)
{
    float gold = 0.0f;
    for (const int matLv : matLevels)
        gold += static_cast<float>((baseLv + matLv) * 100);
    if (baseMst && baseLv >= baseMst->max_lv)
        gold *= 0.25f;
    return static_cast<int32_t>(std::lround(gold));
}
} // namespace

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------
HANDLEF(UnitMix)
{
    (void)session;
    LOG_INFO << "UnitMix: " << json;

    // Parse request.  Use error_on_unknown_keys=false so the extra "60subGk3"
    // operation-type group sent by the client doesn't abort parsing.
    UnitMixReq req = {};
    {
        glz::context ctx{};
        if (const auto& ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(req, json, ctx); ec)
        {
            LOG_WARN << "UnitMix: bad request JSON: " << glz::format_error(ec, json);
            co_return HandleResult::error("Deserialization error");
        }
    }

    // Resolve the current user from the request's login info.
    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
    const std::string kUserId = identity.userId;

    // Split base vs material units.
    int32_t baseId = 0;
    std::vector<int32_t> matIds;
    for (const auto& u : req.units)
    {
        if (u.role == "1")       baseId = u.user_unit_id;
        else if (u.user_unit_id) matIds.push_back(u.user_unit_id);
    }

    if (baseId == 0)
    {
        LOG_WARN << "UnitMix: no base unit in request";
        co_return HandleResult::error("UnitMix: no base unit");
    }

    std::sort(matIds.begin(), matIds.end());
    if (matIds.empty() || std::adjacent_find(matIds.begin(), matIds.end()) != matIds.end()
        || std::binary_search(matIds.begin(), matIds.end(), baseId))
        co_return HandleResult::error("Invalid fusion materials");

    // Only compared with the server's price below; never charged.
    const int32_t clientZel = req.zel_cost_list.empty() ? 0 : req.zel_cost_list[0].cost;

    // Build material IN-clause.
    std::string matList;
    for (size_t i = 0; i < matIds.size(); ++i)
    {
        if (i) matList += ',';
        matList += std::to_string(matIds[i]);
    }

    // Keep material consumption, the type reset and compensation in one
    // transaction, including the reads that prevent duplicate consumption.
    auto transaction = co_await gme::newTransaction(theDb());
    try
    {
        // Step 1: SELECT base unit full stats.
        const auto baseRows = co_await transaction->execSqlCoro(
            "SELECT user_unit_id, unit_id, unit_lvl, total_exp, bb_id, bb_lvl, sbb_id, sbb_lvl,"
            " base_hp, base_atk, base_def, base_rec,"
            " add_hp, add_atk, add_def, add_rec,"
            " ext_hp, ext_atk, ext_def, ext_rec,"
            " limit_over_hp, limit_over_atk, limit_over_def, limit_over_rec,"
            " skill_id, skill_lv, extra_skill_id, extra_skill_lv,"
            " element, unit_type_id,"
            " eqip_item_id, eqip_item_frame_id, eqip_item_id2, eqip_item_frame_id2,"
            " sphere_ext, dbb_unlocked, fe_sp, fe_used_sp, fe_max_sp, omni_level,"
            " extra_passive_skill_id2"
            " FROM user_units WHERE user_id=$1 AND user_unit_id=$2 LIMIT 1;",
            std::string(kUserId), baseId
        );

        if (baseRows.empty())
        {
            LOG_WARN << "UnitMix: base unit " << baseId << " not found";
            co_return HandleResult::error("UnitMix: base unit not found");
        }
        const auto& br = baseRows[0];

        const std::string rawBaseUnitId = br["unit_id"].as<std::string>();
        const std::string baseMstId     = unitMix_stripSuffix(rawBaseUnitId);
        const int32_t     baseMstIdInt  = std::stoi(baseMstId);
        const int         baseTotalExp  = br["total_exp"].as<int32_t>();

        // Lookup base unit MST data.
        const auto& unitMst = theServer()->cache().unitMst();
        const UnitMst* baseMstData = nullptr;
        for (const auto& u : unitMst) { if (u.id == baseMstIdInt) { baseMstData = &u; break; } }

        const int baseElement   = baseMstData ? baseMstData->element       : 0;
        const int baseRare      = baseMstData ? baseMstData->rarity        : 0;
        const int expPatternId  = baseMstData ? baseMstData->exp_pattern_id: 10;
        const int maxLevel      = baseMstData ? baseMstData->max_lv        : 100;

        // Step 2: SELECT material stats to compute exp gain -- and the two special
        // effects that are not exp at all (see unitMix_impBonus above).
        float    gainedExpF     = 0.0f;
        int      burstLevelGain = 0;
        int      sureBurstMats  = 0;   // copies of the base and Omni Frogs
        int      luckyBurstMats = 0;   // other fodder whose roll came up
        int      golemMats = 0;
        int      matchingGolems = 0;
        int      mismatchedGolemElement = 0;
        int      sphereFrogs    = 0;
        int32_t  elgifSkill     = 0;   // the Extra Skill an Elgif material hands over
        int      voidTablets    = 0;   // Void Tablets take the Elgif skill off
        int      materialSp     = 0;   // enhancement SP, if the base is eligible
        int      omniEmperors   = 0;
        std::vector<int> spChanceStars;  // rarity of each "SPUP!?" unit
        int      mysteryKind = 0;
        int      newUnitType = br["unit_type_id"].as<int32_t>();
        // Counted for the great/super success roll below: matching elements are the
        // one condition the game has always said improves a fusion.
        int      matCount       = 0;
        int      sameElementMats = 0;
        ImpBonus impGain{ 0, 0, 0, 0 };
        std::vector<int> matLevels;      // unit_lvl of each material, for the price
        if (!matIds.empty())
        {
            static const int kRarityBonus[] = { 0, 100, 200, 500, 1000, 1500, 3000, 5000, 10000 };

            const auto matRows = co_await transaction->execSqlCoro(
                "SELECT unit_id, unit_lvl, total_exp, add_extra_passive_skill_id FROM user_units"
                " WHERE user_id=$1 AND user_unit_id IN (" + matList + ");",
                std::string(kUserId)
            );

            if (matRows.size() != matIds.size())
                co_return HandleResult::error("Fusion material not owned");
            for (const auto& row : matRows)
            {
                const std::string matMstId    = unitMix_stripSuffix(row["unit_id"].as<std::string>());
                const int32_t     matMstIdInt = std::stoi(matMstId);
                const int         matTotalExp = row["total_exp"].as<int32_t>();
                matLevels.push_back(row["unit_lvl"].as<int32_t>());

                const UnitMst* matData = nullptr;
                for (const auto& u : unitMst) { if (u.id == matMstIdInt) { matData = &u; break; } }

                const int matAdjust = matData ? matData->adjust_exp : 0;
                const int matCost   = matData ? matData->cost       : 1;
                const int matRare   = matData ? matData->rarity     : 1;
                const int matElem   = matData ? matData->element    : 0;

                float matExp = (float)matTotalExp / 2.5f;
                matExp += (float)(matCost * 2);
                matExp += (float)matAdjust;
                if (matRare >= 1 && matRare <= 8)
                    matExp += (float)kRarityBonus[matRare];
                ++matCount;
                if (baseElement != 0 && matElem == baseElement)
                {
                    matExp *= 1.5f;
                    ++sameElementMats;
                }

                if (matData && matData->kind >= 10010 && matData->kind <= 10016)
                {
                    // The client restricts type changing to a single material.
                    if (matIds.size() != 1 || !baseMstData || baseMstData->max_lv <= 1)
                        co_return HandleResult::error("Use one Mystery Frog on a levelable unit");
                    mysteryKind = matData->kind;
                }
                gainedExpF += matExp;

                // A frog and an imp still hand over their exp; what they ALSO do
                // is the part that was missing.
                if (matData && matData->burst_level_boost > 0)
                    burstLevelGain += matData->burst_level_boost;
                // The ordinary-material BB rules (#119, see kBurstChanceSameElement).
                else if (matMstIdInt == kOmniFrogUnitId
                         || (matData && baseMstData && matData->category == baseMstData->category))
                {
                    ++burstLevelGain;
                    ++sureBurstMats;
                }
                else if (matData && matData->skill_id > 0
                         && RandomUInt(1, 100) <= static_cast<uint32_t>(
                                baseElement != 0 && matElem == baseElement
                                    ? kBurstChanceSameElement : kBurstChanceOtherElement))
                {
                    ++burstLevelGain;
                    ++luckyBurstMats;
                }
                if (matMstIdInt == kSphereFrogUnitId)
                    ++sphereFrogs;
                // ELGIF (#38): its skill becomes the base's Elgif Extra Skill,
                // replacing any it had -- the client warns about exactly that
                // (UnitMixMainScene::mixOverRideExSkill).  With two, the later one wins.
                if (const auto carried = row["add_extra_passive_skill_id"].as<int32_t>(); carried > 0)
                    elgifSkill = carried;
                // VOID TABLET: the reverse, it strips the Elgif skill the base
                // carries (ticket #78: "Void tablet emgif
                // doesn't work": the skill stayed).  An Elgif in the same fusion
                // still wins, since its skill replaces whatever was there.
                if (matMstIdInt == gme::kVoidTabletUnitId)
                    ++voidTablets;
                if (matMstIdInt == kOmniEmperorUnitId)
                    ++omniEmperors;
                else if (matData && baseMstData)
                {
                    materialSp += unitMix_materialSp(*baseMstData, *matData,
                        br["eqip_item_frame_id2"].as<int32_t>() != gme::kNoSecondSphereSlot);
                    if (unitMix_isSpChance(*baseMstData, *matData))
                        spChanceStars.push_back(matData->rarity);
                }
                // ELEMENTAL GOLEM -> the DBB slot.  Same element only; the wiki is
                // explicit ("e.g. a Fire Golem can only be fused to a Fire Element
                // unit") and a mismatch is spent as ordinary fodder, which is what
                // the real game's own screen would have prevented.
                if (const auto golem = gme::dbbGolemElement(matMstId); golem != 0)
                {
                    ++golemMats;
                    if (golem == baseElement)
                        ++matchingGolems;
                    else
                        mismatchedGolemElement = golem;
                }
                if (const ImpBonus* imp = unitMix_impBonus(matMstIdInt))
                {
                    impGain.hp  += imp->hp;
                    impGain.atk += imp->atk;
                    impGain.def += imp->def;
                    impGain.rec += imp->rec;
                }
            }
        }

        // THE PRICE IS THE SERVER'S (unitMix_mixGold), whatever the request
        // says.  isMix never sends a fusion the player cannot pay for, so a
        // short wallet is stale state or a modified client: refuse it here,
        // before anything is written, and softly (ReturnToGame -> Home).  The
        // old MAX(0, zel - cost) below let an empty wallet fuse anyway.
        const int baseUnitLv = br["unit_lvl"].as<int32_t>();
        const int32_t zelCost = unitMix_mixGold(baseUnitLv, matLevels, baseMstData);
        if (clientZel != zelCost)
            LOG_WARN << "UnitMix: client priced " << clientZel << " zel, getMixGold gives "
                     << zelCost << " (base lv " << baseUnitLv << ", " << matLevels.size()
                     << " material(s)); charging " << zelCost;
        {
            const auto wallet = co_await transaction->execSqlCoro(
                "SELECT zel FROM user_info WHERE id=$1;", std::string(kUserId));
            const int64_t zelHeld = wallet.empty() ? 0 : wallet[0]["zel"].as<int64_t>();
            if (zelHeld < zelCost)
            {
                LOG_WARN << "UnitMix: user " << kUserId << " holds " << zelHeld
                         << " zel, the fusion costs " << zelCost << "; refused";
                transaction->rollback();
                co_return HandleResult::refuseToHome("You do not have enough Zel for this fusion.",
                                                     "UnitMix: not enough zel");
            }
        }

        if (mysteryKind)
        {
            if (mysteryKind == 10010)
            {
                // Offline distribution: equal ordinary types, rarer Rex. The live
                // server's exact random-frog probabilities are not documented.
                std::array<double, 6> weights{1, 1, 1, 1, 1, 0.25};
                if (newUnitType >= 1 && newUnitType <= 6) weights[newUnitType - 1] = 0;
                newUnitType = 1 + std::discrete_distribution<int>(weights.begin(), weights.end())(RandomEngine());
            }
            else newUnitType = mysteryKind - 10010;
            gainedExpF = 0;
        }

        // GREAT / SUPER SUCCESS.  CONFIRMED from the client:
        // UnitMixPlayScene::ConvSuccessSet @0x1C200A4 branches on the value straight
        // out of UnitOpeResult::getSuccessType —
        //     2 -> ConvSuccessSuper.sam   (Super Success)
        //     1 -> ConvSuccessBig.sam     (Great Success)
        //     0 -> ConvSuccess.sam        (normal)
        // and all three animations ship in game_content.  This settles the KDL's
        // "UNVERIFIED: the value set" note on 6hMI5xeF; the field had been pinned to
        // 0, so the tier could never fire however good the displayed rate looked.
        //
        // The MULTIPLIERS are MST data, not invented: DefineMst.unit_mix_great_exp_rate
        // (2inP0tCg) = 1.5 and unit_mix_super_exp_rate (zn65EXYF) = 2.0.
        //
        // The CHANCE is not in any MST — defines_mst carries the two rates and no
        // probability — so the original server owned it and these numbers are ours.
        // Same-element fusion improving the odds is the rule the game has always
        // stated; baseExp already applies its own 1.5x for matching elements, and
        // this is the second half of that bonus.
        // // TUNABLE: 10% great / 2% super, doubled when every material matched the
        // base unit's element.  No capture backs these figures.
        int successType = 0;
        {
            const bool allSameElement = matCount > 0 && sameElementMats == matCount;
            const int greatChance = allSameElement ? 20 : 10;
            const int superChance = allSameElement ? 4 : 2;
            const auto roll = RandomUInt(1, 100);
            if (roll <= static_cast<uint32_t>(superChance))
                successType = 2;
            else if (roll <= static_cast<uint32_t>(superChance + greatChance))
                successType = 1;
        }

        const auto& defines = theServer()->cache().initializeResp().defines;
        const float successRate =
            successType == 2 ? static_cast<float>(defines.unit_mix_super_exp_rate)
          : successType == 1 ? static_cast<float>(defines.unit_mix_great_exp_rate)
          : 1.0f;
        if (successType != 0)
        {
            gainedExpF *= successRate;
            LOG_INFO << "UnitMix: " << (successType == 2 ? "SUPER" : "GREAT")
                     << " SUCCESS — exp x" << successRate;
        }

        if (mysteryKind) successType = 0;
        const int gainedExp = (int)llroundf(gainedExpF);

        // Compute new level / exp.
        const auto& expPat      = theServer()->cache().initializeResp().exp_pattern;
        const int   maxTotalExp = unitMix_expForLevel(expPat, expPatternId, maxLevel);
        int         newTotalExp = mysteryKind ? 0 : baseTotalExp + gainedExp;
        if (maxTotalExp > 0 && newTotalExp > maxTotalExp) newTotalExp = maxTotalExp;

        const int newLevel    = unitMix_levelFromExp(expPat, expPatternId, maxLevel, newTotalExp);
        const int levelExpFlr = unitMix_expForLevel(expPat, expPatternId, newLevel);
        const int newExp      = newTotalExp - levelExpFlr;

        // The BEFORE side of the result screen.  Derived the same way as the after
        // side so the two are guaranteed consistent: the row stores total_exp, not
        // a level, so the pre-fusion level is whatever that total maps to.
        const int oldLevel    = unitMix_levelFromExp(expPat, expPatternId, maxLevel, baseTotalExp);
        const int oldExp      = baseTotalExp - unitMix_expForLevel(expPat, expPatternId, oldLevel);

        LOG_INFO << "UnitMix: unit=" << baseId << " mst=" << baseMstId
                 << " totalExp " << baseTotalExp << "+" << gainedExp << "=" << newTotalExp
                 << " lv->" << newLevel << "/" << maxLevel;

        // Imps raise the stat up to a cap that belongs to the RECIPIENT, so clamp
        // against what this unit is allowed rather than a global constant.  Fodder
        // carries 0:0:0:0 and therefore cannot be imped at all — which is correct,
        // and also why the clamp has to happen after the base MST lookup.
        // ⚠ IMPS LIVE IN ext_*, NOT add_*.  CONFIRMED IN-CLIENT 2026-09-05 by
        // probing both buckets with distinct values on one unit: the card drew
        //     HP 5365  [111]   from base 5243 + add 11 + ext 111
        // i.e. the big stat is base+add+ext, and the SMALL ORANGE NUMBER beside it
        // -- the imp total the player actually looks at -- is ext_* alone.
        //
        // Writing imps to add_* (what this did before) still moved the big number,
        // which is why it looked like it worked, but the orange imp figure stayed
        // absent and the cap it represents was invisible.  net/user.kdl called
        // TokWs1B3 the "Imp/stat-up HP bucket" all along.
        //
        // add_* is a separate bonus that folds silently into the total; nothing in
        // this handler writes it, and nothing should until we know what it is.
        // An Omni+ unit's caps are raised by its level (gme/common/OmniPlus.hpp).
        ImpBonus impCaps = baseMstData ? unitMix_impCaps(baseMstData->param_max)
                                       : ImpBonus{ 0, 0, 0, 0 };
        if (baseMstData)
        {
            const auto omni = gme::omniImpBonus(baseMstData->cost, br["omni_level"].as<int32_t>());
            impCaps.hp += omni[0]; impCaps.atk += omni[1];
            impCaps.def += omni[2]; impCaps.rec += omni[3];
        }
        const int oldImpHp  = br["ext_hp"].as<int32_t>();
        const int oldImpAtk = br["ext_atk"].as<int32_t>();
        const int oldImpDef = br["ext_def"].as<int32_t>();
        const int oldImpRec = br["ext_rec"].as<int32_t>();
        const auto clampAdd = [](int have, int gain, int cap) {
            return cap <= 0 ? have : std::min(have + gain, cap);
        };
        const int newImpHp  = clampAdd(oldImpHp,  impGain.hp,  impCaps.hp);
        const int newImpAtk = clampAdd(oldImpAtk, impGain.atk, impCaps.atk);
        const int newImpDef = clampAdd(oldImpDef, impGain.def, impCaps.def);
        const int newImpRec = clampAdd(oldImpRec, impGain.rec, impCaps.rec);

        // BB and SBB level share the frog: one Burst Frog raises whichever the
        // unit has, each up to its own skill's top level (unitMix_skillMaxLevel).
        // UBB has no per-unit level on the wire (UserUnitInfo carries bb_lvl and
        // sbb_lvl only), so there is nothing of it for fusion to raise.
        //
        // ⚠ GATE ON THE SKILL ID, NOT THE LEVEL.  A unit with no Brave Burst at all
        // must not gain a BB level -- 134 of unit_mst's 2291 rows carry skill_id 0
        // (every frog, emperor, queen, ghost and metal, plus a few oddities like
        // Thunder Mecha God 40334), and the archive correctly leaves their bb_id
        // empty.  The previous test was `oldBbLvl > 0 || burstLevelGain > 0` with a
        // `max(oldBbLvl, 1)` floor, which fed a frog into a BB-less unit and moved
        // it from 0 straight to 2 -- inventing a Brave Burst the unit does not have
        // and cannot use, and skipping level 1 on the way.
        //
        // A BB-less unit still eats the frog for exp, which is what the real game
        // does; it simply gains no burst level.
        const bool hasBb    = !br["bb_id"].isNull()  && !br["bb_id"].as<std::string>().empty()
                              && br["bb_id"].as<std::string>() != "0";
        const bool hasSbb   = !br["sbb_id"].isNull() && !br["sbb_id"].as<std::string>().empty()
                              && br["sbb_id"].as<std::string>() != "0";
        const int oldBbLvl  = br["bb_lvl"].as<int32_t>();
        const int oldSbbLvl = br["sbb_lvl"].as<int32_t>();
        const int bbMax     = hasBb  ? unitMix_skillMaxLevel(br["bb_id"].as<std::string>())  : 0;
        const int sbbMax    = hasSbb ? unitMix_skillMaxLevel(br["sbb_id"].as<std::string>()) : 0;
        // Spend a single level budget in order: BB, then SBB. Unlocking SBB at
        // level 1 is free, and the remainder of this fusion can immediately raise it.
        int remainingBurstLevels = burstLevelGain;
        int newBbLvl = oldBbLvl;
        if (hasBb)
        {
            newBbLvl = std::clamp(oldBbLvl, 1, bbMax);
            const int gain = std::min(remainingBurstLevels, bbMax - newBbLvl);
            newBbLvl += gain;
            remainingBurstLevels -= gain;
        }
        int newSbbLvl = oldSbbLvl;
        if (hasBb && hasSbb && newBbLvl == bbMax)
            newSbbLvl = std::min(std::max(oldSbbLvl, 1) + remainingBurstLevels, sbbMax);

        // THE DBB SLOT.  Global wiki, Bonding: fusing an Elemental Golem of the
        // unit's OWN element "will now permanently unlock the unit's DBB Slot",
        // after which it can be bonded to its partner.
        //
        // ⚠ THE CLIENT CANNOT ENFORCE THIS.  UserUnitInfo::isDbbEligible @0x12B5EA4
        // tests only "has a DbbMst partner AND rarity >= 8 AND SBB level 10" -- it
        // has no notion of a golem ever having been fused -- so the slot appears on
        // the detail screen regardless and the gate has to live here and in
        // DbbBond.  gme::dbbEligible mirrors that same test so the two agree about
        // WHO may do it; the golem is the extra step only this server knows about.
        const int32_t oldDbbUnlocked = br["dbb_unlocked"].as<int32_t>();
        int32_t newDbbUnlocked = oldDbbUnlocked;
        if (matchingGolems > 0)
        {
            if (oldDbbUnlocked != 0)
            {
                LOG_INFO << "UnitMix: unit " << baseId << " already has its DBB slot — "
                         << matchingGolems << " golem(s) spent as exp fodder only";
            }
            else if (!gme::dbbEligible(baseMstId, baseRare, newSbbLvl, newLevel, maxLevel))
            {
                LOG_INFO << "UnitMix: unit " << baseId << " (" << baseMstId
                         << ") is not DBB-eligible yet — needs a catalogued partner, "
                         << "rarity 8, Super Brave Burst 10 and level " << maxLevel
                         << " (it is " << newLevel << "); golem spent as exp fodder";
            }
            else
            {
                newDbbUnlocked = 1;
                LOG_INFO << "UnitMix: unit " << baseId << " (" << baseMstId
                         << ") DBB SLOT UNLOCKED by an element-" << baseElement << " golem";
            }
        }
        else if (golemMats > 0)
        {
            LOG_WARN << "UnitMix: unit " << baseId << " is element " << baseElement
                     << " but was fed an element-" << mismatchedGolemElement
                     << " golem — no DBB slot, spent as exp fodder";
        }

        if (burstLevelGain > 0 && !hasBb)
        {
            LOG_INFO << "UnitMix: unit " << baseId << " has no Brave Burst (bb_id empty) — "
                     << "burst material spent as exp only";
        }

        // One slot, ever.  A second frog on a unit that already has the slot is not
        // an error -- it is the case the help text says the client should have
        // stopped -- so it is logged and the frog is spent as plain fodder.
        const int oldSphereExt = br["sphere_ext"].as<int32_t>();
        const int newSphereExt = (sphereFrogs > 0) ? 1 : oldSphereExt;

        // THE SLOT ITSELF lives in eqip_item_frame_id2, not in sphere_ext: -1 hides
        // it, 0 shows it empty, 1..14 shows it holding that sphere type.  See
        // gme::kNoSecondSphereSlot.  max(0, old) is what opens the slot -- it lifts
        // -1 to "empty" while leaving an already-equipped frame untouched, so a
        // second frog on a unit that has the slot cannot wipe its sphere.
        const int oldEqpFrame2 = br["eqip_item_frame_id2"].as<int32_t>();
        const int newEqpFrame2 = newSphereExt ? std::max(0, oldEqpFrame2)
                                              : gme::kNoSecondSphereSlot;

        if (sphereFrogs > 0)
        {
            if (oldSphereExt)
                LOG_WARN << "UnitMix: unit " << baseId << " already has the extra sphere slot — "
                         << sphereFrogs << " Sphere Frog(s) spent as exp fodder only";
            else
                LOG_INFO << "UnitMix: unit " << baseId << " gains the second sphere slot"
                         << (sphereFrogs > 1 ? " (only one of the frogs could grant it)" : "");
        }

        // ENHANCEMENT SP (see unitMix_materialSp).  Judged on the unit as the
        // material screen saw it, before this fusion: that is the state
        // checkElemUnit validated when it let an SP-only material be picked.
        const int oldFeSp   = br["fe_sp"].as<int32_t>();
        const int feUsedSp  = br["fe_used_sp"].as<int32_t>();
        const int feMaxSp   = br["fe_max_sp"].as<int32_t>();
        const bool feEligible = baseMstData && baseRare >= 8 && oldSbbLvl == kMaxBurstLevel
                             && oldLevel == maxLevel;
        // Each Omni Emperor raises the limit and the SP by 5 together, so it
        // never needs room under the limit; the client's mixFeBpCheck likewise
        // lets ordinary SP materials use the limit the Emperors just added.
        int newFeMaxSp = feMaxSp;
        int emperorSp  = 0;
        if (feEligible && br["omni_level"].as<int32_t>() >= kOmniEmperorMinLevel)
            for (int i = 0; i < omniEmperors && newFeMaxSp < kOmniEmperorLimitCap; ++i)
            {
                newFeMaxSp += kOmniEmperorSp;
                emperorSp  += kOmniEmperorSp;
            }
        // One roll per "SPUP!?" unit, after the sure SP, and only while there
        // is room under the limit.
        int luckySp = 0;
        if (feEligible && !spChanceStars.empty())
        {
            const auto tune = forge::tuning();
            for (const int stars : spChanceStars)
            {
                const double pct = tune.fusionSpChancePct
                                 + tune.fusionSpChancePerStarPct * std::max(0, stars - 1);
                if (std::bernoulli_distribution(std::clamp(pct, 0.0, 100.0) / 100.0)(RandomEngine()))
                    ++luckySp;
            }
        }
        const int rolledSp = materialSp + luckySp;
        int newFeSp = oldFeSp + emperorSp;
        if (feEligible && rolledSp > 0)
            newFeSp += std::clamp(newFeMaxSp - feUsedSp - newFeSp, 0, rolledSp);
        if (materialSp > 0 || omniEmperors > 0 || (feEligible && !spChanceStars.empty()))
        {
            LOG_INFO << "UnitMix: enhancement SP " << (feEligible ? "" : "(base not eligible) ")
                     << oldFeSp << "+" << materialSp << "+" << emperorSp
                     << "+" << luckySp << "/" << spChanceStars.size() << " lucky -> " << newFeSp
                     << " (spent " << feUsedSp << ", limit " << feMaxSp << " -> " << newFeMaxSp << ")";
            if (emperorSp < omniEmperors * kOmniEmperorSp)
                LOG_WARN << "UnitMix: " << omniEmperors - emperorSp / kOmniEmperorSp
                         << " Omni Emperor(s) spent as exp (needs Omni+3, limit < "
                         << kOmniEmperorLimitCap << "; omni_level "
                         << br["omni_level"].as<int32_t>() << ")";
        }

        if (burstLevelGain || impGain.hp || impGain.atk || impGain.def || impGain.rec)
        {
            LOG_INFO << "UnitMix: special materials — burst +" << burstLevelGain
                     << " (" << sureBurstMats << " sure, " << luckyBurstMats << " rolled)"
                     << " (bb " << oldBbLvl << "->" << newBbLvl << "/" << bbMax
                     << ", sbb " << oldSbbLvl << "->" << newSbbLvl << ")"
                     << ", imps +" << impGain.hp << "/" << impGain.atk << "/"
                     << impGain.def << "/" << impGain.rec
                     << " capped to " << newImpHp << "/" << newImpAtk << "/"
                     << newImpDef << "/" << newImpRec
                     << " (caps " << impCaps.hp << ":" << impCaps.atk << ":"
                     << impCaps.def << ":" << impCaps.rec << ")";
        }

        // ⚠ BASE STATS ARE LEVEL-SCALED AND MUST BE REWRITTEN HERE.
        //
        // user_units.base_* holds the stats AT THE UNIT'S CURRENT LEVEL -- the
        // client displays them verbatim and does no scaling of its own (the unit
        // card showed 3039 HP on a Lv80/80 Nemia whose max_hp is 4854, which is
        // exactly the stored level-1 value).  Levelling a unit without rewriting
        // them left every levelled unit fighting with its level-1 statline.
        //
        // Scale from the MST's min/max, NOT from the stored base_*: the stored
        // value is already scaled to the OLD level, so feeding it back as the
        // minimum compounds the error on every fusion.
        const auto scaleStat = [&](int minV, int maxV) {
            return unitMix_statAtLevel(minV, maxV, newLevel, maxLevel);
        };
        // Carry the previously rolled type growth and roll each newly gained
        // level from UnitTypeMst. A Mystery Frog starts that growth over at level 1.
        std::array<std::vector<int>, 4> typeGrowth;
        const UnitTypeMst* typeData = nullptr;
        for (const auto& t : theServer()->cache().unitTypeMst())
            if (t.unit_type_id == newUnitType) typeData = &t;
        const int mins[] = {baseMstData ? baseMstData->min_hp : 0,
                            baseMstData ? baseMstData->min_atk : 0,
                            baseMstData ? baseMstData->min_def : 0,
                            baseMstData ? baseMstData->min_rec : 0};
        const int maxs[] = {baseMstData ? baseMstData->max_hp : 0,
                            baseMstData ? baseMstData->max_atk : 0,
                            baseMstData ? baseMstData->max_def : 0,
                            baseMstData ? baseMstData->max_rec : 0};
        const char* columns[] = {"base_hp", "base_atk", "base_def", "base_rec"};
        for (int stat = 0; stat < 4; ++stat)
        {
            const int previous = mysteryKind || !baseMstData ? 0 :
                br[columns[stat]].as<int32_t>() - unitMix_statAtLevel(mins[stat], maxs[stat], oldLevel, maxLevel);
            typeGrowth[stat].resize(newLevel + 1, previous);
            if (!typeData || mysteryKind) continue;
            const auto& range = stat == 0 ? typeData->unk1 : stat == 1 ? typeData->unk2 :
                                stat == 2 ? typeData->unk3 : typeData->unk4;
            for (int lv = oldLevel + 1; lv <= newLevel; ++lv)
                typeGrowth[stat][lv] = typeGrowth[stat][lv - 1] +
                    (range.size() == 2 ? std::uniform_int_distribution<int>(range[0], range[1])(RandomEngine()) : 0);
        }
        const int newBaseHp = baseMstData ? scaleStat(baseMstData->min_hp, baseMstData->max_hp) + typeGrowth[0][newLevel] : br["base_hp"].as<int32_t>();
        const int newBaseAtk = baseMstData ? scaleStat(baseMstData->min_atk, baseMstData->max_atk) + typeGrowth[1][newLevel] : br["base_atk"].as<int32_t>();
        const int newBaseDef = baseMstData ? scaleStat(baseMstData->min_def, baseMstData->max_def) + typeGrowth[2][newLevel] : br["base_def"].as<int32_t>();
        const int newBaseRec = baseMstData ? scaleStat(baseMstData->min_rec, baseMstData->max_rec) + typeGrowth[3][newLevel] : br["base_rec"].as<int32_t>();

        if (newLevel != oldLevel)
        {
            LOG_INFO << "UnitMix: base stats rescaled for lv " << oldLevel << "->" << newLevel
                     << " — hp " << br["base_hp"].as<int32_t>() << "->" << newBaseHp
                     << ", atk " << br["base_atk"].as<int32_t>() << "->" << newBaseAtk
                     << ", def " << br["base_def"].as<int32_t>() << "->" << newBaseDef
                     << ", rec " << br["base_rec"].as<int32_t>() << "->" << newBaseRec;
        }

        // Step 3: UPDATE base unit level/exp, the rescaled base stats, plus the imp
        // and burst-level columns the special materials just changed.
        co_await transaction->execSqlCoro(
            "UPDATE user_units SET unit_lvl=$1, exp=$2, total_exp=$3,"
            " ext_hp=$4, ext_atk=$5, ext_def=$6, ext_rec=$7, bb_lvl=$8, sbb_lvl=$9,"
            " sphere_ext=$10, eqip_item_frame_id2=$11,"
            " base_hp=$12, base_atk=$13, base_def=$14, base_rec=$15,"
            " dbb_unlocked=$16, unit_type_id=$17, fe_sp=$18, fe_max_sp=$19"
            " WHERE user_unit_id=$20 AND user_id=$21;",
            newLevel, newExp, newTotalExp,
            newImpHp, newImpAtk, newImpDef, newImpRec, newBbLvl, newSbbLvl,
            newSphereExt, newEqpFrame2,
            newBaseHp, newBaseAtk, newBaseDef, newBaseRec,
            newDbbUnlocked,
            newUnitType, newFeSp, newFeMaxSp, baseId, std::string(kUserId)
        );

        // VOID TABLET: the Emgif's empty tablet takes the technique and turns
        // into an Elgif carrying it (unit_comment_mst 60812), which the game sent
        // to the present box.  Without that return, one-of-a-kind skills such as
        // Omen of Nograth were destroyed (ticket #78 follow-up).  It
        // takes the skill before an Elgif in the same fusion writes its own; a
        // tablet with nothing to take goes back to the present box unchanged.
        if (voidTablets > 0)
        {
            const int32_t heldSkill = br["extra_passive_skill_id2"].as<int32_t>();
            int32_t returnedTablets = voidTablets;
            if (heldSkill > 0)
            {
                co_await transaction->execSqlCoro(
                    "UPDATE user_units SET extra_passive_skill_id2=0 WHERE user_unit_id=$1 AND user_id=$2;",
                    baseId, std::string(kUserId));
                co_await gme::addUserPresent(transaction, identity, 6, std::to_string(gme::kElgifUnitId), 1, 0,
                                             "Elgif taken off by a Void Tablet", heldSkill);
                --returnedTablets;
                LOG_INFO << "UnitMix: Void Tablet took Elgif skill " << heldSkill << " off unit " << baseId
                         << " and sent it to the present box";
            }
            if (returnedTablets > 0)
            {
                co_await gme::addUserPresent(transaction, identity, 6, std::to_string(gme::kVoidTabletUnitId),
                                             returnedTablets, 0, "Void Tablet with no Elgif to take");
                LOG_INFO << "UnitMix: " << returnedTablets << " Void Tablet(s) had no Elgif skill on unit "
                         << baseId << " to take; returned";
            }
        }
        if (elgifSkill > 0)
        {
            co_await transaction->execSqlCoro(
                "UPDATE user_units SET extra_passive_skill_id2=$1 WHERE user_unit_id=$2 AND user_id=$3;",
                elgifSkill, baseId, std::string(kUserId));
            LOG_INFO << "UnitMix: Elgif skill " << elgifSkill << " given to unit " << baseId;
        }

        // Step 4: return spheres equipped on the fodder, then DELETE the material
        // units — deleting without the return would destroy the equipped items.
        uint32_t spheresReturned = 0;
        if (!matIds.empty())
        {
            spheresReturned = co_await gme::returnEquippedSpheres(transaction, identity, matList);
            co_await transaction->execSqlCoro(
                "DELETE FROM user_units WHERE user_id=$1 AND user_unit_id IN (" + matList + ");",
                std::string(kUserId)
            );
        }

        if (mysteryKind)
            co_await gme::addUserPresent(transaction, identity, 6, "750006", 3, 0,
                                        "Mystery Frog fusion: Rainbow Crystals");

        // Step 5: deduct zel -- the server's price, already checked against the
        // wallet in this transaction; the MAX(0, ...) is only a backstop.
        if (zelCost > 0)
        {
            co_await transaction->execSqlCoro(
                "UPDATE user_info SET zel = MAX(0, zel - $1) WHERE id=$2;",
                zelCost, std::string(kUserId)
            );
        }


        // Build response.
        //
        // NOTHING GOES UNDER xZH6EIQ7.  That key is the helper picker list
        // (ReinforcementInfoResponse): row 0 clears ReinforcementInfoList, and a
        // row with no user id is then thrown away instead of added.  This reply
        // used to carry one id-less "DecompDev" row there, so every fusion
        // emptied the friend picker and the next quest said "Reinforcements could
        // not be found" until a FriendGet refilled it.  The result screen reads
        // 1ZbHB6Im below and the unit cards read the 4ceMWH6k roster -- the row
        // never reached any list, so it never drove the animation it was named
        // for.  The field is optional and stays unset.
        UnitMixResp resp = {};

        // Incremental unit cache update.
        //
        // Read back through PacketInterfaceFor<UserUnitInfo> rather than
        // hand-assembling the entry.  This is the SAME read UserInfo uses at login,
        // and login is the one path the client provably accepts — so building the
        // entry any other way is guessing at a shape we already have.
        //
        // Hand-assembly is how the level stopped showing after a fusion: the entry
        // was 47 fields and complete-looking, but `received_order` (Bvkx8s6M) was
        // never assigned and so went out as 0, where login sends 1000.  It is not
        // a column — the schema maps it onto `user_unit_id` — so there was nothing
        // in the SELECT to notice was missing.  Any field added to UserUnitInfo in
        // future would have silently defaulted the same way.
        //
        // The DB writes above have already landed, so this read reflects the new
        // level, exp and total_exp.
        //
        // FULL-REPLACE THE UNIT CACHE (4ceMWH6k).  Confirmed working in-client
        // 2026-08-10: the fusion menu shows the new level and the fodder are gone,
        // with no trip to Home.
        //
        // We deliberately do NOT echo the base unit under qC2tJs4E.  That key is
        // INSERT-IF-ABSENT: readParam's tail asks UserUnitInfoList::exist() and
        // returns without committing when the client already owns the unit, so for
        // a fusion base it is a guaranteed no-op.
        //
        // 4ceMWH6k is the only key that can change an owned unit, because its
        // readParam calls removeAllObjects() first — which is also why it must
        // carry the WHOLE roster, not just the fused unit.
        //
        // It is needed because the client has no local apply path of its own: it
        // never writes the fused unit (no UserUnitInfo mutator is reachable from a
        // fusion scene) and never drops the fodder (removeObject is called by the
        // sale scenes and the FrontierGate/FGPlus friend lists, never by a fusion
        // scene; removeObjectWithUserUnitID has none), and it issues no request
        // after the fusion.
        //
        // The historical objection was that removeAllObjects release()s every
        // CCObject in the list, dangling any raw UserUnitInfo* a live scene holds —
        // it soft-locked the result screen once.  That build also sent
        // lvup_status "1", which crashes parseMixResult by itself; with the ritual
        // payload correct, this key is fine here.  If a future scene does start
        // soft-locking on it, that is the mechanism to suspect.
        resp.unit_refresh = std::move((co_await db::PacketInterfaceFor<::UserUnitInfo>::read(
            transaction,
            "user_units",
            { db::Lookup("user_id", std::string(kUserId)) })).data);

        LOG_INFO << "UnitMix: full-replace unit cache — " << resp.unit_refresh->size()
                 << " unit(s) under 4ceMWH6k";

        // THE RESULT SCREEN (1ZbHB6Im).  This is what actually drives the xp-bar
        // sweep and the level-up flourish, and the server had never sent it — the
        // screen rendered the unit and then sat there with nothing to animate.
        // Every field here is a before/after pair for exactly that reason.
        {
            UnitOpeResult r = {};
            r.unit_id       = baseMstId;
            r.user_unit_id  = std::to_string(baseId);
            r.zel           = zelCost;
            r.exp           = gainedExp;
            r.success_type  = successType;          // 0 normal / 1 great / 2 super
            r.before_lv     = oldLevel;
            r.after_lv      = newLevel;
            r.before_exp    = oldExp;
            r.after_exp     = newExp;

            // ⚠ before/after_skill_lv IS THE BRAVE BURST LEVEL, and fusion DOES
            // change it.  This block (UnitOpeResult, 1ZbHB6Im) is what the fusion
            // RESULT screen draws its "BB Lv. x/10" from -- not the reinforce entry
            // and not the unit cache, both of which carry the right value and are
            // ignored for this field.  Confirmed 2026-09-05 against a live capture:
            // Eliza fused 1->2, the response carried bb_lvl 2 in BOTH xZH6EIQ7 and
            // 4ceMWH6k, and the screen still read 0/10 because these two were 0.
            //
            // They were being read from user_units.skill_lv / extra_skill_lv, an
            // unused pair of columns that is 0 on every row -- the same wrong-column
            // mistake as the reinforce entry, in a second place.
            //
            // Slot 1's frame genuinely is unchanged by fusion, so it keeps the
            // before == after treatment (a 0/0 pair would read as "dropped to
            // nothing" on any stat the UI chooses to flourish).
            //
            // SLOT 2's FRAME IS NOT INERT: a Sphere Frog moves it from -1 to 0,
            // which is the whole visible effect of that fusion.  Sending the old
            // value on both sides is what made the frog look like it did nothing
            // on the result screen.
            const auto eqpFrame     = br["eqip_item_frame_id"].as<int32_t>();

            r.before_skill_lv         = oldBbLvl;
            r.after_skill_lv          = newBbLvl;
            r.before_extra_skill_lv   = oldSbbLvl;
            r.after_extra_skill_lv    = newSbbLvl;
            r.before_eqp_frame_id     = eqpFrame;
            r.after_eqp_frame_id      = eqpFrame;
            r.before_eqp_frame_id2    = oldEqpFrame2;
            r.after_eqp_frame_id2     = newEqpFrame2;

            // Unspent SP before/after -- parseMixResult @0x1C18D6C adds the
            // unit's fe_used_bp to both for display -- and the limit, which
            // only an Omni+3 Omni Emperor moves.
            r.before_fe_bp = oldFeSp;
            r.after_fe_bp = newFeSp;
            r.before_max_fe_bp = feMaxSp;
            r.after_max_fe_bp  = newFeMaxSp;
            // DBB is not implemented; equal zeroes are the honest "no change"
            // for a feature that does not exist yet.
            r.before_dbb_skill_level = r.after_dbb_skill_level = 0;

            // THE STAT TABLE.  lvup_status and param_up_state are a matched pair
            // and must be built together — decoded from parseMixResult:
            //
            //   lvup_status    strtok(s, ",")  -> one MixResultStatus per chunk,
            //                  each chunk "lv:hp:atk:def:heal"
            //   param_up_state parseList(':')  -> a single "lv:hp:atk:def:heal";
            //                  its lv is looked up AGAINST that table
            //
            // Sent empty, the client falls back to building ONE row from the unit
            // it is displaying — at the OLD level — which is why the bar moved but
            // no stats or level-up ever appeared.  And param_up_state alone would
            // look up a level absent from that one-row table, leaving `v119 = 0`
            // and dereferencing it (`MixResultStatus::getHp(v119)`) — the crash.
            //
            // So: emit a row for every level from before to after, then point
            // param_up_state at the final one.
            {
                // Scaled from the MST's MIN, not from the stored base_*: the
                // stored value is already scaled to the unit's level, so using it
                // as the floor made every row of the animation drift upward.
                // ⚠ THE TWO STRINGS MUST DIFFER OR NOTHING IS SHOWN.
                //
                // parseMixResult builds a MixResultStatus table from lvup_status
                // (one "lv:hp:atk:def:heal" chunk per level) and then looks
                // param_up_state's level up IN that table -- the stat-gain flourish
                // is the DIFFERENCE between the two.  Building both from the same
                // post-fusion numbers makes that difference zero, which is why an
                // imp fusion moved the totals and animated nothing.
                //
                // So: the table carries the statline WITHOUT this fusion's imps
                // (levelling alone), and param_up_state carries it WITH them.  For a
                // pure level-up the imp terms are equal and the flourish is the
                // level gain, exactly as before.
                const auto statRow = [&](int lv, int impHp, int impAtk, int impDef, int impRec) {
                    const int hp = unitMix_statAtLevel(
                                         baseMstData ? baseMstData->min_hp : br["base_hp"].as<int32_t>(),
                                         baseMstData ? baseMstData->max_hp : br["base_hp"].as<int32_t>(),
                                         lv, maxLevel)
                                     + typeGrowth[0][lv] + br["add_hp"].as<int32_t>() + impHp
                                     + br["limit_over_hp"].as<int32_t>();
                    const int atk = unitMix_statAtLevel(
                                         baseMstData ? baseMstData->min_atk : br["base_atk"].as<int32_t>(),
                                         baseMstData ? baseMstData->max_atk : br["base_atk"].as<int32_t>(),
                                         lv, maxLevel)
                                     + typeGrowth[1][lv] + br["add_atk"].as<int32_t>() + impAtk
                                     + br["limit_over_atk"].as<int32_t>();
                    const int def = unitMix_statAtLevel(
                                         baseMstData ? baseMstData->min_def : br["base_def"].as<int32_t>(),
                                         baseMstData ? baseMstData->max_def : br["base_def"].as<int32_t>(),
                                         lv, maxLevel)
                                     + typeGrowth[2][lv] + br["add_def"].as<int32_t>() + impDef
                                     + br["limit_over_def"].as<int32_t>();
                    const int rec = unitMix_statAtLevel(
                                         baseMstData ? baseMstData->min_rec : br["base_rec"].as<int32_t>(),
                                         baseMstData ? baseMstData->max_rec : br["base_rec"].as<int32_t>(),
                                         lv, maxLevel)
                                     + typeGrowth[3][lv] + br["add_rec"].as<int32_t>() + impRec
                                     + br["limit_over_rec"].as<int32_t>();
                    return std::to_string(lv) + ':' + std::to_string(hp) + ':'
                         + std::to_string(atk) + ':' + std::to_string(def) + ':'
                         + std::to_string(rec);
                };

                std::string table;
                for (int lv = std::min(oldLevel, newLevel); lv <= newLevel; ++lv)
                {
                    if (!table.empty()) table += ',';
                    table += statRow(lv, oldImpHp, oldImpAtk, oldImpDef, oldImpRec);
                }
                r.lvup_status    = table;
                // Same level as the table's last row, so the lookup resolves -- a
                // level absent from the table null-derefs in parseMixResult.
                r.param_up_state = statRow(newLevel, newImpHp, newImpAtk, newImpDef, newImpRec);
            }

            // FE is not implemented; empty is the honest "no allocation".  These
            // are plain string setters with no list parsing behind them.
            r.before_fe_info = "";
            r.after_fe_info  = "";

            resp.ope_result.push_back(std::move(r));

            LOG_INFO << "UnitMix: result lv " << oldLevel << "->" << newLevel
                     << " exp " << oldExp << "->" << newExp
                     << " (+" << gainedExp << "), lvup=" << r.lvup_status;
        }

        // DAILY TASK `PU`.  ⚠ IT COUNTS UNITS FUSED, NOT FUSIONS PERFORMED — the
        // wiki and the client agree the task is "Fuse 5 units", and feeding five
        // fodder in one go is five.  Counting the operation instead made a full
        // five-material fusion read [1/5], which is what Evan hit on 2026-09-20.
        co_await gme::advanceDailyTask(transaction, identity, "PU",
            static_cast<int32_t>(matIds.size()));

        resp.team_info = std::move(
            (co_await gme::getTeamInfo(transaction, identity)).nonEmpty());

        // Spheres handed back from the fodder: no fusion scene touches the client
        // warehouse, so they need the full snapshot (UnitMixResp in handlers.kdl).
        if (spheresReturned > 0)
        {
            auto warehouse = co_await gme::loadWarehouseSnapshot(transaction, identity);
            resp.warehouse_info = std::move(warehouse.warehouse);
            resp.item_favorite = std::move(warehouse.favorites);
            resp.item_dictionary_info = std::move(warehouse.dictionary);
        }

        // THE DBB LIST, whenever this fusion could have changed it.
        //
        // A golem fusion opens the unit's DBB slot, and the ONLY way the client
        // learns a slot is open is by finding the unit in this list
        // (GameUtils::setDbbFusedIcon @0x1EC4CF8 -> objectForKey).  Nothing in the
        // fusion scene refreshes it, so without this the column was set correctly
        // in the database and the screen showed no slot, no icon and no animation
        // -- handbook §6: a successful SQL update is not a UI refresh.
        //
        // Sent when the unlock changed, and also when the unit already had a slot,
        // because the fusion reply is the only refresh this scene gets.
        if (newDbbUnlocked != 0)
            co_await gme::fillDbb(transaction, identity, resp);

        // The fused unit may be a favourited own helper card: send the COMPLETE
        // helper list (never a partial row, see "Build response") so its card shows
        // the new level instead of the one from login.
        {
            auto lists = co_await gme::buildHelperLists(transaction, identity);
            resp.helper_list = std::move(lists.reinforce_info);
            resp.helper_friends = std::move(lists.friend_info);
        }

        std::string buffer{};
        if (const auto& ec2 = glz::write_json(resp, buffer); ec2)
        {
            LOG_ERROR << "UnitMix: serialization error: " << glz::format_error(ec2, buffer);
            transaction->rollback();
            co_return HandleResult::error("Serialization error");
        }

        // Journal: "10 Fusions performed".  Counts the units consumed, which is
        // what a player means by a fusion - feeding five fodder in one action is
        // five fusions, not one.
        co_await gme::addJournalProgress(
            transaction, identity, gme::kJournalTaskUnitFusion,
            static_cast<int32_t>(std::max<size_t>(matIds.size(), 1)));

        // Trophies 100220 合成回数 / 100230 合成ユニット素材使用数 / 100040 総合ゼル使用額.
        // matIds is the material set the handler actually consumed, and zelCost is
        // what it charged -- both taken after validation, not from the raw request.
        co_await gme::bumpArchiveCounters(transaction, identity, {
            { "unit_mix_cnt",      1 },
            { "unit_mix_elem_cnt", static_cast<int64_t>(matIds.size()) },
            { "zel_use",           static_cast<int64_t>(zelCost) },
        });

        co_return HandleResult::success(buffer);
    }
    catch (...)
    {
        transaction->rollback();
        throw;
    }

}
