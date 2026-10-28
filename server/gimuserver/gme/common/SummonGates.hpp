#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/SummonRotation.hpp>
#include <gimuserver/packets/all.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Which summon gates a player may pull, and how — the rules the original client
// already enforces on its side, kept here so GachaAction, GachaList and
// UserInfo agree with each other.
//
// GachaAction used to trust the request: any gate with an archive record could
// be pulled with any count and paid for with any kind of ticket, whether or
// not the rail offered it.  That is how the 1-gem "Test Gate" pools (99005-99008,
// removed) stayed pullable by a crafted request even after the rail tile went.

namespace gme
{

// Gate 2000 — the client labels type 20000 "Tutorial Gacha" and it is the only
// row with that type across all 1305 original gacha_mst rows.  Tutorial-only:
// tuto15 sends the player into it once, and opening it outside the tutorial
// crashes the client.
constexpr uint32_t kTutorialGachaId = 2000;

// The most pulls one GachaAction may carry.  SummonsDetailScene::setGachaInfo
// @0x16A6894-0x16A68A8 stores min(affordable pulls, 10) as the Multi Summon
// count, and the single Summon button sends 1.
constexpr uint32_t kMaxSummonsPerRequest = 10;

// Below this id the client never offers the "use Summon Tickets" checkbox
// (SummonsDetailScene::setGachaInfo @0x16A1C24, cmp w0,#0x7d0): those are the
// Honor gates, paid in Honor points only.
constexpr uint32_t kFirstTicketGachaId = 2000;

// How a pull is paid: UserState::usingGachaTicket, which GachaActionRequest
// sends as `324b023k`.  SummonsDetailScene::touchBegan @0x16A7F08-0x16A812C
// sets it when the "use Summon Tickets" box is ticked: on a gate that has
// SummonTicketV2Mst types of its own, 2 when the player holds any of them; on
// any other gate, 1 when the plain counter is not zero; 0 otherwise.  So the
// plain counter never pays on a gate with its own ticket types, and a gate's
// own tickets never pay anywhere else.
enum class SummonPayment : uint32_t
{
	Currency = 0,     // need_gems, or need_friend_point on Honor gates, per pull
	PlainTicket = 1,  // user_info.summon_tickets, one per pull
	GateTicket = 2,   // the gate's own V2 tickets, user_summon_tickets_v2
};

/*!
* Whether a gate takes Summon Tickets in place of its currency, as the client
* decides it (@0x16A1C24-0x16A1C4C): gem gates from id 2000 on, except the
* 10004/10005 gem-summon types.
*
* @param gachaId Summon gate id.
* @param gachaType GachaMst type of the gate.
* @return true when a ticket may pay for a pull.
*/
inline bool summonTicketsAccepted(uint32_t gachaId, int32_t gachaType)
{
	return gachaId >= kFirstTicketGachaId && gachaType != 10004 && gachaType != 10005;
}

/*!
* Whether a permanent rail tile (gacha_category_mst.json) pages through a gate.
* Selector tiles are per player and are handled by their own ticket path.
* A gate in the weekly rotation pool is open only in its featured weeks,
* whatever static tile names it (SummonRotation.hpp).
*
* @param gachaId Summon gate id.
* @param wholePool A Sandbox account (gme/common/Sandbox.hpp), whose rail holds
*        every pool gate.
* @return true when some tile's comma-separated 3rCmq58M names the gate.
*/
inline bool isOnSummonRail(uint32_t gachaId, bool wholePool = false)
{
	if (isRotatingGate(gachaId))
		return wholePool || isFeaturedNow(gachaId, unixNow());
	const auto wanted = std::to_string(gachaId);
	for (const auto& category : theServer()->cache().gachaListRsp().gacha_categories)
	{
		std::string_view list = category.gacha_id_list;
		while (!list.empty())
		{
			const auto comma = list.find(',');
			if (list.substr(0, comma) == wanted)
				return true;
			if (comma == std::string_view::npos)
				break;
			list.remove_prefix(comma + 1);
		}
	}
	return false;
}

/*!
* Whether the player is still inside the tutorial, and so may still need the
* tutorial gate: the stored end flag is 0.  That is the client's own test —
* LoginScene::changeNextScene @0x174C720 enters the tutorial only when
* UserInfo::isTutoPlaying @0x1295AFC finds TutoEndFlg at 0.
*
* The flag is safe to trust here.  The client sends sv6BEI8X as
* `getTutoEndFlg() ? 1 : chapter > 11` (BaseRequest::createTutoUserInfoTag
* @0x13A0B34-0x13A0B54), and the main tutorial runs chapters 1..12 with
* chapter 10 as tuto15, the free summon (GameUtils::getTutorialScriptFile
* @0x1EB8758).  TutorialStepScene::checkChapter @0x1B927A4 raises the flag
* only for a chapter above 11, turning it into 12, and initConnect
* @0x1B9270C sends TutorialUpdate only at 12.  Chapters 50..54 (tuto10-14)
* are the "triggerable" tutorial after chapter 12 (GameScene::checkTouchFooter
* @0x1616740), switched off in this client: UserInfo::setTriggerableTutorial
* @0x1296ED4 always clears its bit.  So no flag of 1 comes before tuto15.
*
* @param database Database client or transaction to use.
* @param identity Resolved user identity to read.
* @return true while tutorial_end_flag is 0; false once it is set, or when
*         the row is missing.
*/
inline drogon::Task<bool> tutorialInProgress(
	const db::Database database,
	const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT tutorial_end_flag FROM user_info WHERE id = $1;", identity.userId);
	co_return !rows.empty() && rows[0]["tutorial_end_flag"].as<int32_t>() == 0;
}

/*!
* Takes the tutorial gate and its rail tile out of a summon list.  Leaving it
* there after the tutorial displaces the event banner from the top of the rail,
* and opening it crashes the client.
*
* @param categories Rail tiles to filter in place.
* @param gates Gate rows to filter in place.
*/
inline void hideTutorialGacha(
	std::vector<::GachaCategory>& categories,
	std::vector<::GachaInfoMst>& gates)
{
	std::erase_if(categories, [](const ::GachaCategory& c) {
		return c.gacha_id_list == std::to_string(kTutorialGachaId);
	});
	std::erase_if(gates, [](const ::GachaInfoMst& g) {
		return static_cast<uint32_t>(g.id) == kTutorialGachaId;
	});
}

} // namespace gme
