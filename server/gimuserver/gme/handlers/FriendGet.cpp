#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>
#include <gimuserver/gme/common/Gifts.hpp>   // giftToday(), the shared day key

#include <ctime>

// FriendGet (2o4axPIC) — fires when the player opens the Reinforcement /
// "Choose a Helper" screen during squad selection (and also after
// MissionEnd to refresh the friend list for the next mission).
//
// Wire shape: the captured production response (BF-WorkingDir/server/deploy/
// log_res/2o4axPIC_*.json) emits {"xZH6EIQ7":[...]} — so the canonical
// response key is xZH6EIQ7 (populates ReinforcementInfoList).  Per the IDA
// dispatch table (tools/ida/audits/tojMy68W_audit.txt around line 114-117),
// the binary's master GameResponseParser::getResponseObject maps xZH6EIQ7 →
// ReinforcementInfoResponse → its readParam populates ReinforcementInfoList,
// which the picker is the only confirmed consumer of for the reinforce/
// helper UI.
//
// We also emit tojMy68W (FriendInfoResponse → FriendInfoList) with the same
// source data so any other UI that reads from FriendInfoList (general
// friend roster, friend-detail dialog) sees a populated list too.  Cheap to
// duplicate since both classes have nearly identical schemas; only the
// destination singleton differs.
//
// Request body carries a "StQIyohe":[{"jkldTrhL":"N"}] mode flag — observed
// values 0 and 2.  Probably toggles between full friend list and
// reinforce-eligible-only.  We currently ignore the mode and always return
// the same single fake friend.
//
// Offline-server stand-in: one entry sourced from the player's active-deck
// leader (fall back to highest-level unit).  When a curated pre-made friend
// set lands later, replace the single-row SQL with a loop emitting multiple
// entries to both arrays.

// Helper: convert user_units.element string ("fire"/"water"/...) to the
// integer element id FriendInfo / ReinforcementInfo expect.
static int32_t friendGet_elementToInt(const std::string& s)
{
    if (s == "fire")    return 1;
    if (s == "water")   return 2;
    if (s == "earth")   return 3;
    if (s == "thunder") return 4;
    if (s == "light")   return 5;
    if (s == "dark")    return 6;
    return 1;
}

namespace gme
{
// Both helper lists exactly as FriendGet sends them: the picker (xZH6EIQ7) and the
// Social rows (tojMy68W).  Also attached to the replies that change the player's
// own units (UnitMix, UnitEvo, UnitFavorite, MissionEnd with a favourite), because
// the client only asks for FriendGet at login and before a repeat quest: without
// that, an own-unit card kept the level it had at login (owner, 2026-10-21).
// `database` may be the caller's open transaction (one SQLite connection).
drogon::Task<::FriendGetResp> buildHelperLists(db::Database database, const UserIdentity identity)
{
    const std::string kUserId = identity.userId;
    ::FriendGetResp resp{};

    // THE ROSTER, not a single mirror of the player's own leader.
    //
    // Every friend goes into BOTH lists: `xZH6EIQ7` (ReinforcementInfo) is the
    // pre-battle helper picker and `tojMy68W` (FriendInfo) is the Social list.
    // They are different client caches and populating one does not update the
    // other -- the handbook calls this out by name.
    //
    // THE TWO LISTS DO NOT CARRY THE SAME SET, and that is the whole mechanic:
    // a friend whose evolution chain can no longer reach the player's rarity is
    // hidden from the PICKER (they cannot field a comparable helper) but stays
    // on the SOCIAL list, because that is where the player sees who has fallen
    // behind and unfriends them.  Filtering both would hide the very rows the
    // player is supposed to act on.
    try
    {
        const auto peak = co_await gme::playerPeak(database, identity);
        // The FULL roster; staleness is decided per friend below.
        const auto roster = co_await gme::loadFriendRoster(database, identity, false);
        const auto loginTimestamp = static_cast<int32_t>(std::time(nullptr));

        for (const auto& mate : roster)
        {
            const auto unit = gme::friendUnitFor(mate, peak);
            if (unit.unit_id == 0)
            {
                LOG_WARN << "FriendGet: " << mate.handle_name << " has unknown chain base "
                         << mate.base_unit_id << "; skipped rather than sent half-built";
                continue;
            }

            // Can this friend still keep up?  If their chain tops out below
            // the player, they are dropped from the picker only.
            const bool stale = gme::chainTopRarity(mate.base_unit_id) < peak.rarity;

            // === ReinforcementInfo (xZH6EIQ7) -- the helper picker.
            if (!stale)
            {
            ReinforcementInfo ri{};
            ri.user_id             = mate.friend_id;
            ri.handle_name         = mate.handle_name;
            ri.team_lv             = 999;
            ri.target_lv           = unit.level;
            // MUST be 1: existTypeOK @0x12607CC accepts nothing else, and it is
            // what pays the friend Honor rate instead of the stranger rate.
            ri.friend_type         = 1;
            ri.last_login_date     = loginTimestamp;
            ri.unit_id             = unit.unit_id;
            ri.base_hp             = unit.base_hp;
            ri.base_atk            = unit.base_atk;
            ri.base_def            = unit.base_def;
            ri.base_heal           = unit.base_rec;
            ri.friend_point        = gme::kHonorPerFriendHelper;
            ri.normal_friend_point = gme::kHonorPerNormalHelper;
            ri.skill_id            = unit.bb_id;
            ri.skill_lv            = unit.bb_lvl;
            ri.extra_skill_id      = unit.sbb_id;
            ri.extra_skill_lv      = unit.sbb_lvl;
            ri.unit_type_id        = unit.unit_type_id;
            ri.element             = std::to_string(unit.element);
            ri.user_unit_id        = gme::kFriendUnitIdBase + static_cast<int32_t>(resp.reinforce_info.size());
            // The helper's SPHERES.  Ge8Yo32T is setEquipItemID -- it was
            // documented as setMissionID for a long time, which is why helpers
            // used to show an empty sphere slot.
            //
            // STORED, not re-rolled.  A friend's kit was fixed when they were
            // added and stays that way however far they evolve; only the second
            // SLOT opens later, at 7-star.
            ri.equipitem_id        = mate.sphere_1;
            ri.equipitem_id2       = unit.rarity >= gme::kSecondSphereRarity
                                     ? mate.sphere_2 : 0;
            resp.reinforce_info.emplace_back(std::move(ri));
            }

        }

        // The Social list, built by the one shared function every friend
        // reply uses -- tojMy68W is a full replace, so they must all agree.
        // It carries pending requests (types 2 and 3) as well, which is what
        // lights the footer badge at login (getCountAgreeRec counts type 3).
        co_await gme::expireStaleRequests(database, identity.userId);
        resp.friend_info = co_await gme::buildSocialList(database, identity);

        // REAL PLAYERS IN THE PICKER.  Friends go to the very top (they pay
        // the friend Honor rate); a few recently active strangers follow the
        // NPC roster so borrowing one raises the post-mission "add as a
        // friend?" prompt for a real player.  The NPC strangers below still
        // fill the board on a small server.
        int32_t realSlot = 0;
        std::vector<ReinforcementInfo> realFriends;
        for (const auto& rel : co_await gme::loadRelations(database, identity.userId))
        {
            if (rel.state != gme::kRelFriend)
                continue;
            // A friend across the Sandbox line stays on the Social list but
            // lends no helper (gme/common/Sandbox.hpp).
            if (!(co_await gme::sandbox::samePool(database, identity.userId, rel.other_id)))
                continue;
            if (const auto card = co_await gme::loadPlayerCard(database, rel.other_id))
                realFriends.push_back(gme::reinforcementFromCard(
                    *card, gme::kRelFriend, gme::kRealHelperUnitIdBase + realSlot++));
        }
        resp.reinforce_info.insert(resp.reinforce_info.begin(), realFriends.begin(), realFriends.end());
        // THE PLAYER'S OWN UNITS (Squad 10, Relations.hpp kOwnHelperCount): a
        // sixth unit of their own.  No Honor either way, so the cards say 0.
        //
        // The picker ignores this order: FriendListUtils::getReinforcementList
        // @0x11689dc lists helpers whose user id is a favourite friend first,
        // then friends (FriendInfoList::existTypeOK), then the rest, each group
        // in the player's Sort order.  So each own card also gets a friend row
        // under its id, with priority 2: existTypeOK and getObjectWithUserID
        // ignore priority, while the Social list, the friend counts and the
        // gift list skip rows above 1, so the row is never seen.  It puts the
        // cards with the friends and means the post-battle "add as a friend?"
        // prompt no longer comes up for them.
        //
        // NOT a favourite (ticket #142): the picker draws the
        // favourite star from the same flag (FriendListUtils::setUserHandleName
        // @0x116ad84), so favourite rows put a star on units the player never
        // starred.  The star and the favourites-first group cannot be split
        // from the server, so the cards sit with the friends, below any
        // favourited friend.
        std::vector<ReinforcementInfo> ownUnits;
        int32_t ownSlot = 0;
        for (const auto& card : co_await gme::loadOwnHelperCards(database, identity.userId))
        {
            auto ri = gme::reinforcementFromCard(card, 0, gme::kOwnHelperUnitIdBase + ownSlot++);
            ri.friend_point = 0;
            ri.normal_friend_point = 0;
            ownUnits.push_back(std::move(ri));

            auto row = gme::friendInfoFromCard(card, gme::kRelFriend, nullptr);
            row.favorite = 0;
            row.priority = 2;
            resp.friend_info.push_back(std::move(row));
        }
        resp.reinforce_info.insert(resp.reinforce_info.begin(), ownUnits.begin(), ownUnits.end());
        for (const auto& card : co_await gme::unrelatedPlayers(
                 database, identity.userId, gme::kRealStrangerCount, true))
        {
            resp.reinforce_info.push_back(gme::reinforcementFromCard(
                card, 0, gme::kRealHelperUnitIdBase + realSlot++));
        }

        // Emitting a stranger card.  Shared by the developer encounter and the
        // ordinary suggestions: the only thing that differs is which reserved
        // user_unit_id block they draw from.
        //
        // STRANGERS GO IN THE PICKER ONLY, and that is load-bearing.
        // MissionResultFriendRequestScene::initialize @0x18C16BC offers to add
        // the helper you just borrowed only when FriendInfoList::exist() says
        // they are NOT already a friend -- so putting any of these in the Social
        // list would answer that question "yes" and suppress the very prompt
        // that recruits them.
        //
        // friend_type is NOT 1 here: they are strangers until recruited, which
        // is honest and means borrowing one pays the stranger Honor rate.
        // Befriending them is the upgrade.
        const auto addStranger = [&](const gme::FriendRow& who, int32_t userUnitId) -> bool {
            const auto unit = gme::friendUnitFor(who, peak);
            if (unit.unit_id == 0)
                return false;

            ReinforcementInfo ri{};
            ri.user_id             = who.friend_id;
            ri.handle_name         = who.handle_name;
            ri.team_lv             = 999;
            ri.target_lv           = unit.level;
            ri.friend_type         = 0;   // a stranger, for now
            ri.last_login_date     = loginTimestamp;
            ri.unit_id             = unit.unit_id;
            ri.base_hp             = unit.base_hp;
            ri.base_atk            = unit.base_atk;
            ri.base_def            = unit.base_def;
            ri.base_heal           = unit.base_rec;
            // Both at the stranger rate: getFriendPoint gates on the row being
            // >= 1 and then takes the amount from the 6e4b7sQt singleton
            // according to existTypeOK, which says "not a friend" for these.
            ri.friend_point        = gme::kHonorPerNormalHelper;
            ri.normal_friend_point = gme::kHonorPerNormalHelper;
            ri.skill_id            = unit.bb_id;
            ri.skill_lv            = unit.bb_lvl;
            ri.extra_skill_id      = unit.sbb_id;
            ri.extra_skill_lv      = unit.sbb_lvl;
            ri.unit_type_id        = unit.unit_type_id;
            ri.element             = std::to_string(unit.element);
            ri.user_unit_id        = userUnitId;
            // A STRANGER'S kit is rolled on the spot, seeded on who they are
            // and the day, so what the picker shows is stable while the player
            // is looking at it and is exactly what gets locked in if they
            // befriend them -- rollFriendSpheres is seeded on the friend id
            // alone, and that is the same id recruitFriend stores under.
            const auto spheres     = gme::rollFriendSpheres(
                who.friend_id, gme::chainTopRarity(who.base_unit_id));
            ri.equipitem_id        = spheres.first;
            ri.equipitem_id2       = unit.rarity >= gme::kSecondSphereRarity
                                     ? spheres.second : 0;
            resp.reinforce_info.emplace_back(std::move(ri));
            return true;
        };

        // One day key for the whole reply, so a picker built on the stroke of
        // 08:00 UTC cannot mix two days' strangers.
        const auto day = gme::giftToday();

        // TODAY'S DEVELOPER ENCOUNTER -- the easter egg, at the head of the
        // strangers so it is findable rather than buried mid-list.
        if (const auto met = gme::devEncounterFor(roster, identity.userId, day))
        {
            const gme::FriendRow asRow{ gme::devFriendId(*met), met->name, met->base_unit_id, 1, 0 };
            if (addStranger(asRow, gme::kEncounterUnitId))
            {
                LOG_INFO << "FriendGet: " << met->name << " is out there today, fielding "
                         << met->unit;
            }
        }

        // THE OTHER SUMMONERS.  Without these the picker only ever shows people
        // the player already knows, and the post-mission "add as a friend?"
        // prompt -- which only fires for a helper FriendInfoList::exist() says
        // is NOT a friend -- can never come up at all.
        const auto strangers =
            gme::strangerSuggestions(roster, identity.userId, day, peak);
        int32_t offered = 0;
        for (const auto& who : strangers)
        {
            if (addStranger(who, gme::kStrangerUnitIdBase + offered))
                ++offered;
        }

        LOG_INFO << "FriendGet: " << resp.reinforce_info.size() << " pickable ("
                 << offered << " stranger(s)) of " << resp.friend_info.size()
                 << " friend(s) for " << identity.userId
                 << " at peak r" << peak.rarity << " lv " << peak.level;
    }
    catch (const drogon::orm::DrogonDbException& ex)
    {
        LOG_WARN << "FriendGet: roster query failed: " << ex.base().what();
    }

    // What each of those cards is worth (6e4b7sQt).  A SINGLETON the client
    // defaults to "0" in its ctor, and the picker this reply draws is its only
    // reader — so it goes out with the list rather than only at login.
    resp.friend_point_info = gme::friendPointInfo();
    co_return resp;
}
} // namespace gme

HANDLEF(FriendGet)
{
    (void)session;
    LOG_INFO << "FriendGet: " << json;

    // Identity-only request (no body beyond the login-info tag).
    FriendGetReq req = {};
    {
        glz::context ctx{};
        if (const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx); ec)
            LOG_WARN << "FriendGet: parse error: " << glz::format_error(ec, json);
    }

    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
    auto resp = co_await gme::buildHelperLists(theDb(), identity);

    std::string buffer{};
    if (const auto& ec = glz::write_json(resp, buffer); ec)
    {
        const auto& glze = glz::format_error(ec, buffer);
        LOG_ERROR << "FriendGet: serialization error: " << glze;
        co_return HandleResult::error("Serialization error", glze);
    }

    co_return HandleResult::success(buffer);
}
