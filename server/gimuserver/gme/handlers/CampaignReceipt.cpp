#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/AutoSell.hpp>
#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/RaidRequest.hpp>
#include <gimuserver/gme/common/Sandbox.hpp>

#include <algorithm>
#include <cctype>

// CampaignReceipt (5Imq3wC0) — the Serial Code screen (THE SERIAL CODE SCREEN
// below): the body is the typed code, and that is all the real client sends.
//
// NO GRAND MISSION CLAIM (ticket #166 audit).  This handler used to
// double as a Grand Mission reward claim: a request naming a cleared mission
// (j28VNcUW) whose user_campaign_missions row still had reward_claimed 0 was
// paid that mission's First Quest Clear (reward_type 4) and N% Completion
// (reward_type 1) rows straight into the account.  CampaignEnd already pays
// exactly those rows when the run is cleared, once each (init_flag 1, recorded
// in rewards_got), and never touched reward_claimed, so a crafted receipt with
// no code paid every cleared mission's bonuses a second time.  No client
// screen sends that request (CampaignCodeRequestScene is the only sender, see
// CampaignReceiptRequest::createBody @0x13A195C), so the claim is gone:
// Grand Mission rewards are CampaignEnd's alone, and a receipt without a code
// pays nothing.
//
// Response to a receipt without a code (a code is always answered with a
// Retry refusal, see redeemSerialCode):
//   "fEi17cnx" — [UserTeamInfo]          — refreshes HUD zel/karma/exp
//   "4MCxgS5p" — { "pCIRMw04": "" }      — receipt payload (stub)

// CampaignReceiptReq (login_info + mission_id) is generated from the KDL
// (packet-generator/assets/net/handlers.kdl).

// CampaignReceiptResp (fEi17cnx team_info + 4MCxgS5p receipt stub) is generated
// from the KDL (packet-generator/assets/net/handlers.kdl) and shared with
// CampaignBattleEnd.

namespace
{
// THE SERIAL CODE SCREEN.  CampaignCodeRequestScene is this request's only
// sender, and its body is just the typed code: 4MCxgS5p/pCIRMw04
// (CampaignReceiptRequest::createBody @0x13A195C).  The scene shows nothing of
// a success reply (checkConnectResult is GameScene::checkResponseMessage
// alone), so every answer is a Retry refusal: the text as written, and OK
// (notice -4000) takes the scene's noticeOK @0x175E624 to scene 41.
constexpr const char* kCodeInvalid = "This serial code is not valid.";
constexpr const char* kSandboxExplained =
	"SANDBOX MODE opens every area, quest, trial and summon banner. In multiplayer (helpers, Arena,"
	" Colosseum, raids and rankings) you will only meet other Sandbox players. This cannot be undone."
	" Enter SANDBOX again within 10 minutes to confirm.";
constexpr const char* kSandboxOn =
	"Sandbox mode is on: everything is unlocked, and multiplayer is Sandbox-only."
	" Return to the title screen to load it.";
constexpr const char* kSandboxAlreadyOn = "Sandbox mode is already on for this account.";
constexpr const char* kAutoSellOn =
	"Auto-sell is ON: after each stage, the 1-3 star units it dropped (not materials) are sold for Zel."
	" Enter AUTOSELL again to turn it off.";
constexpr const char* kAutoSellOff = "Auto-sell is OFF: stage drops stay in your unit box.";

std::string normalizedCode(const std::string& typed)
{
	std::string code;
	for (const char c : typed)
		if (!std::isspace(static_cast<unsigned char>(c)))
			code += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return code;
}

// SANDBOX twice (gme/common/Sandbox.hpp): the first arms sandbox_asked_at and
// explains, the second within kConfirmSeconds sets sandbox_at.  Nothing here or
// anywhere else clears sandbox_at.
drogon::Task<HandleResult> redeemSerialCode(const gme::UserIdentity identity, const std::string code)
{
	const auto& userId = identity.userId;
	if (code == gme::autosell::kSerialCode)
	{
		const bool on = co_await gme::autosell::toggle(theDb(), userId);
		LOG_INFO << "AutoSell: " << userId << " turned auto-sell " << (on ? "on" : "off");
		co_return HandleResult::refuseToRetry(on ? kAutoSellOn : kAutoSellOff,
			"CampaignReceipt: " + userId + " toggled auto-sell");
	}
	if (code != gme::sandbox::kSerialCode)
		co_return HandleResult::refuseToRetry(kCodeInvalid, "CampaignReceipt: unknown serial code from " + userId);

	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT sandbox_at, sandbox_asked_at FROM user_info WHERE id = $1;", userId);
	if (rows.empty())
		co_return HandleResult::refuseToRetry(kCodeInvalid, "CampaignReceipt: no user_info row for " + userId);
	if (rows[0]["sandbox_at"].as<int64_t>() > 0)
		co_return HandleResult::refuseToRetry(kSandboxAlreadyOn, "CampaignReceipt: " + userId + " is already in Sandbox mode");

	const auto now = gme::unixNow();
	const auto asked = rows[0]["sandbox_asked_at"].as<int64_t>();
	if (asked > 0 && now - asked <= gme::sandbox::kConfirmSeconds)
	{
		co_await theDb()->execSqlCoro(
			"UPDATE user_info SET sandbox_at = $1, sandbox_asked_at = 0 WHERE id = $2 AND sandbox_at = 0;",
			now, userId);
		LOG_INFO << "Sandbox: " << userId << " switched to Sandbox mode";
		co_return HandleResult::refuseToRetry(kSandboxOn, "CampaignReceipt: " + userId + " confirmed Sandbox mode");
	}
	co_await theDb()->execSqlCoro(
		"UPDATE user_info SET sandbox_asked_at = $1 WHERE id = $2;", now, userId);
	LOG_INFO << "Sandbox: " << userId << " asked for Sandbox mode; confirmation open for "
	         << gme::sandbox::kConfirmSeconds << " s";
	co_return HandleResult::refuseToRetry(kSandboxExplained, "CampaignReceipt: " + userId + " asked for Sandbox mode");
}
} // namespace

HANDLEF(CampaignReceipt)
{
    LOG_INFO << "CampaignReceipt: " << json;

    CampaignReceiptReq req{};
    {
        glz::context ctx{};
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(req, json, ctx); ec)
            LOG_WARN << "CampaignReceipt: parse error: " << glz::format_error(ec, json);
    }

    const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

    // A typed serial code: the only thing the real client sends.
    if (const auto code = normalizedCode(gme::raid::RaidRequest(json).text("4MCxgS5p", "pCIRMw04")); !code.empty())
        co_return co_await redeemSerialCode(identity, code);

    // No code: nothing is paid, whatever mission the request names (see NO
    // GRAND MISSION CLAIM at the top).  Answered as before, so a stray empty
    // receipt still gets the header back rather than a closed session.
    if (!req.mission_id.empty())
        LOG_WARN << "CampaignReceipt: " << identity.userId << " named Grand Mission '" << req.mission_id
                 << "' without a serial code; nothing paid (CampaignEnd pays Grand Mission rewards)";

    // Fetch fresh user_info so the HUD updates (team_info under fEi17cnx) and
    // return it alongside the receipt stub (4MCxgS5p) in one serialization.
    CampaignReceiptResp resp{};
    resp.team_info = std::move(
        (co_await gme::getTeamInfo(theDb(), identity)).nonEmpty());

    co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}
