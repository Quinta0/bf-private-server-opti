#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/db/DatabaseInterface.h>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/HunterOrbs.hpp>

#include <algorithm>

// FrontierGateStart (l3lkDBSc) — opens a Frontier Gate run.
//
// Request  : group Mg8K8Y1a carries the gate id + chosen support id.  The same
//            body also carries the deck (MxmCpDRC) and unit (52xDBRGr)
//            snapshots, which belong to tier 3 and are ignored for now — the
//            lenient read drops them.
// Response : key Mg8K8Y1a (FrontierGateNumResponse), a single OBJECT.
//
// Every setter in FrontierGateNumResponse::readParam @0x142FF48 is a real named
// call, so this struct carries no guessed field names — unlike dPM7oJDl.
//
// Evidence: tools/ida/audits/dPM7oJDl_audit.txt (§6c/§6d),
// tools/FRONTIER_GATE_STATE_MODEL.md.

namespace
{
// The client parses the three power-up rates with CommonUtils::StrToFloat and
// then divides by 100 before storing, so the wire unit is a PERCENTAGE and 100
// is the identity multiplier.  Sending 0 would scale the party's HP/ATK/DEF by
// zero, so the neutral value is 100 — not the zero-initialised default.
//
// This is not a guessed value in the §3.4 sense: the semantic is fully decoded,
// only the *source* of a non-neutral bonus is not.  When that source is found
// (gate MST battle params or the support effect), replace this constant.
constexpr float kNeutralPowerUpRate = 100.0f;
}

HANDLEF(FrontierGateStart)
{
    LOG_INFO << "FrontierGateStart: " << json;

    // `::` qualification throughout — the handler is GmeHandlers::FrontierGateStart
    // and the generated types sit in the global namespace (cf. BadgeInfo.cpp).
    ::FrontierGateStartReq req{};
    {
        glz::context ctx{};
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(req, json, ctx); ec)
            LOG_WARN << "FrontierGateStart: parse error: " << glz::format_error(ec, json);
    }

    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

    const int32_t gateId = req.frogate.frogate_id;

    // SANITISE THE SUPPORT ID BEFORE IT IS STORED OR ECHOED.
    //
    // Picking "None" on the support-select screen makes the client send
    // UNINITIALISED MEMORY here: a live capture (2026-08-07) carried
    // NungTq5g = -858993460, which is 0xCCCCCCCC, MSVC's debug fill pattern.
    //
    // Storing that was bad; echoing it back in the Mg8K8Y1a response was worse.
    // The client takes SelSupportID and looks it up in FrontierGateSupportMst
    // (61 rows, ids 100..202003), gets nothing back, and dereferences the miss
    // — the mission loaded its assets and then crashed.  Same class of failure
    // as the null UnitCgsMstList deref in handbook §3.2.
    //
    // Anything that is not a real support id becomes 0, which is the "no
    // support chosen" value the rest of this subsystem already understands
    // (FrontierGateInfo omits 0 from its comma list rather than emitting it).
    const auto& supportMst = theServer()->cache().frontierGateSupportMst();
    const bool supportKnown = std::any_of(
        supportMst.begin(), supportMst.end(),
        [&](const ::FrontierGateSupportMst& s) { return s.id == req.frogate.support_id; });

    const int32_t supportId = supportKnown ? req.frogate.support_id : 0;
    if (!supportKnown && req.frogate.support_id != 0)
    {
        LOG_WARN << "FrontierGateStart: support id " << req.frogate.support_id
                 << " is not in FrontierGateSupportMst"
                 << (req.frogate.support_id == -858993460
                         ? " (0xCCCCCCCC — client sent uninitialised memory, likely \"None\" selected)"
                         : "")
                 << "; storing 0 instead";
    }

    // Reject a gate the catalog does not know: the client would then hold a run
    // handle for a gate it cannot render.  Empty OK rather than a closed
    // session (handbook §5) so the UI simply does not enter.
    const auto& gateMst = theServer()->cache().frontierGateMst();
    const bool gateKnown = std::any_of(
        gateMst.begin(), gateMst.end(),
        [gateId](const ::FrontierGateMst& gate) { return gate.id == gateId; });
    if (!gateKnown)
    {
        LOG_WARN << "FrontierGateStart: unknown frogate_id " << gateId << ", refusing to open a run";
        co_return HandleResult::success("{}");
    }

    // A RUN IS RESUMED, NOT REOPENED.  The client sends this request before
    // EVERY floor, not only on entry: ~6 s after each FrontierGateContinue, and
    // again when a paused run (FrontierGateSave) is re-entered.  The request
    // looks the same each time (gate id and party, no run handle), so the only
    // way to tell a resume from a new run is the run row: one open on THIS gate
    // means the player is still in it.  It ends only through Retire
    // (FrontierGateEnd), a lost battle (MissionEnd) or Restart, all of which
    // delete the row.  Treating every Start as a new run charged a Hunter Orb
    // per floor and reset the run to floor 0 each time, and once the orbs ran
    // out the refusal left the player in a run the server no longer had
    // (ticket #37: 34 players, 69 resets on 10-18).
    ::FrontierGateStartResp resp{};

    try
    {
        const auto existing = co_await db::DatabaseInterface::read(
            theDb(),
            "user_frontier_gate_run",
            {
                db::Data("frogate_id"),
                db::Data("frogate_num"),
                db::Data("sel_support_id"),
                db::Lookup("user_id", identity.userId),
            });

        // The party is always the client's latest: it may pick a new helper
        // when it re-enters, and otherwise hands back what the last reply gave.
        const auto deckJson = glz::write_json(req.party_deck).value_or("[]");
        const auto unitsJson = glz::write_json(req.units).value_or("[]");

        if (existing.affected > 0 && existing.front<int32_t>("frogate_id") == gateId)
        {
            const int32_t runNum = existing.front<int32_t>("frogate_num");
            // The support is chosen once, on entry.  A resume that names none
            // (or one we do not know) keeps the run's.
            const int32_t runSupport =
                supportId != 0 ? supportId : existing.front<int32_t>("sel_support_id");

            co_await db::DatabaseInterface::update(
                theDb(),
                "user_frontier_gate_run",
                {
                    db::Data("mission_status", 0),
                    db::Data("sel_support_id", runSupport),
                    db::Data("party_deck_json", deckJson),
                    db::Data("party_units_json", unitsJson),
                    db::Lookup("user_id", identity.userId),
                });

            resp.num.frogate_num = runNum;
            resp.num.frogate_id = gateId;
            resp.num.sel_support_id = runSupport;
            resp.num.mission_status = 0;
            LOG_INFO << "FrontierGateStart: " << identity.userId << " resumes run " << runNum
                     << " on gate " << gateId << " (no orb)";
        }
        else
        {
            // THE HUNTER ORB.  A run costs one, and the client does not debit
            // itself here: MissionStartScene::initConnect @0x1823358 skips its
            // own `setAube(max(getAube(),1) - 1)` whenever
            // FrontierGateUtils::nowFrontierGate is true, which it is for every
            // Frontier Gate start.  So the charge is the server's, once, when
            // the run opens -- the floors inside it are not separate entries.
            //
            // Charged after the gate is validated and before the run row
            // exists, so a refused gate is free and a paid orb always has a run
            // behind it.  The client has already checked its own count in
            // FrontierGateConditionScene::startCheck, so reaching here with
            // none means the two disagreed; refuse rather than let the run open
            // for free.
            if (!co_await gme::spendHunterOrb(theDb(), identity))
            {
                LOG_WARN << "FrontierGateStart: " << identity.userId
                         << " has no Hunter Orbs; refusing to open gate " << gateId;
                co_return HandleResult::success("{}");
            }

            // frogate_num only has to be stable for the life of the run and
            // unique enough that a new run is not confused with the previous
            // one, so it advances by one each time this user opens a run.
            const int32_t runNum =
                existing.affected > 0 ? existing.front<int32_t>("frogate_num") + 1 : 1;

            // One run per user: opening a run on another gate replaces the old.
            co_await db::DatabaseInterface::upsert(
                theDb(),
                "user_frontier_gate_run",
                {
                    db::Data("user_id", identity.userId),
                    db::Data("frogate_id", gateId),
                    db::Data("frogate_num", runNum),
                    db::Data("mission_status", 0),
                    db::Data("sel_support_id", supportId),
                    // Persist the party so Continue and Retry -- which do not
                    // resend the units -- can hand it back.  Without this the
                    // next floor came up with an empty
                    // FrontierGateUserUnitInfoList and the party vanished.
                    db::Data("party_deck_json", deckJson),
                    db::Data("party_units_json", unitsJson),
                    // A new run starts at floor 0 with no score.  The upsert
                    // keeps any column it does not name, so an abandoned run's
                    // floors would otherwise carry into this one.
                    db::Data("run_score", 0),
                    db::Data("run_progress", 0),
                    db::Data("run_note", std::string{}),
                    db::Data("run_od_info", std::string{}),
                    db::Data("run_rewards", std::string{}),
                },
                /*conflict*/ {"user_id"});

            resp.num.frogate_num = runNum;
            resp.num.frogate_id = gateId;
            resp.num.sel_support_id = supportId;
            // // UNVERIFIED: the j3g5P4cq status enum is not decoded, so a
            // freshly opened run reports 0.  If the FG screen misreads the
            // run's phase, this is the first value to question.
            resp.num.mission_status = 0;
            LOG_INFO << "FrontierGateStart: " << identity.userId << " opens run " << runNum
                     << " on gate " << gateId << " (1 Hunter Orb)";
        }
    }
    catch (const drogon::orm::DrogonDbException& ex)
    {
        LOG_ERROR << "FrontierGateStart: run update failed: " << ex.base().what();
        co_return HandleResult::success("{}");
    }

    resp.num.power_up_rate_hp = kNeutralPowerUpRate;
    resp.num.power_up_rate_atk = kNeutralPowerUpRate;
    resp.num.power_up_rate_def = kNeutralPowerUpRate;

    // ECHO THE PARTY BACK — this is what makes the battle loadable.
    //
    // MissionStartScene::downloadFiles branches on GameUtils::isFrontierGate
    // and, in Frontier Gate mode, builds its asset download set from
    // FrontierGateUserUnitInfoList (52xDBRGr) rather than from the player's
    // normal unit list.  With that list empty the client requests no unit
    // files, and the battle then runs with units whose assets were never
    // fetched — the mission downloaded its shared assets and crashed.
    //
    // The client hands us both groups in this request, so echoing them is both
    // the fix and how the party survives from floor to floor: the next Start
    // sends back what this reply gave it (the run row keeps a copy for
    // Continue and Retry).
    resp.party_deck = std::move(req.party_deck);
    resp.units = std::move(req.units);

    LOG_INFO << "FrontierGateStart: echoing " << resp.party_deck.size()
             << " deck member(s) and " << resp.units.size() << " unit(s)";

    co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
