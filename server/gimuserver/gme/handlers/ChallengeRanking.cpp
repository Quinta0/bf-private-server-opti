#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FrontierHunter.hpp>

#include <string>

// ChallengeRanking (2Kxi7rIB / v1PzNE9f) — the Frontier Hunter ranking board,
// reached from the Ranking button of the Survey Office lobby.
//
// Request : group cvg8hzp9 carrying c5yZnpB4, the event id
//           (ChallengeRankingRequest::createBody @0x13A20C4).  No board type,
//           mission or page: ChallengeNewRankingScene sends it once per visit
//           and draws EVERY tab from the one reply.
// Response: key Hmiwj75u (ChallengeRankingResponse), one array holding all the
//           boards, told apart by NH65Wj0f: 0 friends, 1 total, 2 an MVP
//           category (9w5e3ZJB = its challenge_mvp_mst rank_type, j28VNcUW its
//           mission id).  The client sorts each board by U4pMNjy0, keeps
//           RankOrder <= 30 (<= 1000 for friends) and appends the viewer's own
//           row last on the total and MVP boards.
//
// REAL PLAYERS ONLY since ticket #8 (gme/common/FrontierHunter.hpp):
// the week's Frontier Service for the total and friend boards, the best run
// per quest and the note counters for the MVP boards.  The emulated rivals are
// gone; the client needs no filler, and a viewer with no service still gets
// their own row (point 0 draws "---").

HANDLEF(ChallengeRanking)
{
    LOG_INFO << "ChallengeRanking: " << json;

    ::ChallengeRankingReq req{};
    {
        glz::context ctx{};
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(req, json, ctx); ec)
            LOG_WARN << "ChallengeRanking: parse error: " << glz::format_error(ec, json);
    }

    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();
    const auto boards = co_await gme::frohun::rankingJson(theDb(), identity.userId);

    LOG_INFO << "ChallengeRanking: event " << req.challenge.frohun_id << " boards for " << identity.userId
             << " (" << boards.size() << " bytes)";

    co_return HandleResult::success("{\"Hmiwj75u\":" + boards + "}");
}
