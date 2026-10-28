#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FrontierHunter.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>

// ChallengeRankReward (m3fr7vxo / 4BuRdgr2, /actionSymbol/dzoV8Z5i.php) — the
// Frontier Hunter reward claim (ticket #8).
//
// Sent by the lobby's main button when frohun_stat is 3, the blue Reward
// button (ChallengeLobbyScene::missionScene @0x155C45C -> rankRewardRequest
// @0x155CC64, the request's only constructor).  The body is group cvg8hzp9
// with c5yZnpB4 = the event id and nothing else: the client never says which
// reward it wants, the server decides what was earned.
//
// The reply's p73SJiWv rows are the receipt cards ChallengeReceiptScene shows
// one by one (rankRewardCheque @0x155C37C opens it, or says "There are no
// Rewards to receive." for an empty list).  The client applies nothing itself,
// so everything is paid in gme::frohun::claimRankRewards before the reply goes
// out: the Hunter Rank steps (each with its challenge_hr_mst reward), the rank
// tier's items and the MVP spheres, all into the present box.  The reply also
// carries s7A3bGLe with the lobby back on the running week (the receipt
// returns to the lobby without asking again) and kN2i7qds with the new HR.

HANDLEF(ChallengeRankReward)
{
    (void)session;
    LOG_INFO << "ChallengeRankReward: " << json;

    ::FriendGetReq idOnly{};   // identity-only shape: IKqx1Cn9 and nothing else
    {
        glz::context ctx{};
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(idOnly, json, ctx); ec)
            LOG_WARN << "ChallengeRankReward: parse error: " << glz::format_error(ec, json);
    }
    const auto identity = (co_await gme::getUserIdentity(theDb(), idOnly.login_info)).nonEmpty();

    const gme::raid::RaidRequest request(json);
    const auto eventId = request.text("cvg8hzp9", "c5yZnpB4");
    if (!eventId.empty() && eventId != std::to_string(gme::frohun::activeEventId()))
    {
        LOG_WARN << "ChallengeRankReward: " << identity.userId << " claimed for event " << eventId
                 << ", the running one is " << gme::frohun::activeEventId() << "; settling the weeks anyway";
    }

    co_return HandleResult::success(co_await gme::frohun::claimRankRewards(identity));
}
