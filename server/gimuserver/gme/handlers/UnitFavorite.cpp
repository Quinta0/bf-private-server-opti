#include "App.hpp"
#include "Handlers.hpp"
#include <gimuserver/gme/common/Relations.hpp>

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>

// UnitFavorite (3kcmQy7B) — the favourite (lock) flags of the player's units.
//
// The request is the client's WHOLE favourite list, not a toggle: createBody
// @0x13AEDC8 lists every unit whose getFavorite is 1 (or whose image type is
// 2), so a unit missing from the list is one the player unfavourited.  The old
// handler only wrote the listed rows, so unfavourited units stayed locked here
// (the Randall Merit trade then crashed on units the client thought free), and
// it echoed one entry where the client replaces its whole UnitFavoriteList
// from the reply, which dropped the other stars (ticket #39).
//
// IT ALSO CARRIES THE ART EACH UNIT SHOWS.  UnitDetailUserUnitScene::
// unitImgTypeChange @0x1BC3984 flips UserUnitInfo.unit_img_type between 1
// (base) and 2 (alternate art) and only on the client; the change reaches the
// server here, as 2pAyFjmZ on every listed unit (createBody reads 0 as 1).  It
// was dropped, so login sent every unit back on its base art and the choice
// was gone after a restart (ticket #145).  Stored as 2 only when the
// player owns that unit's alternate art (user_unit_alt_art, the same unlock
// that puts the swap arrow on the unit page), 0 otherwise.
HANDLEF(UnitFavorite)
{
	UnitFavoriteReq req = {};
	// Lenient read: the request also carries login_info / MST-version blocks the
	// struct doesn't declare (§4.4) — strict parsing rejects them as unknown_key.
	if (const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json); ec)
	{
		const auto& fmte = glz::format_error(ec, json);
		LOG_WARN << "UnitFavorite: parse error, nothing changed: " << fmte;
		co_return HandleResult::error("Deserialization error", fmte);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// One transaction: clear every flag, then set the listed ones, so a failure
	// part-way leaves the old set rather than half of the new one.
	auto transaction = co_await gme::newTransaction(theDb());
	try
	{
		co_await transaction->execSqlCoro(
			"UPDATE user_units SET favorite_flg = 0, unit_img_type = 0"
			" WHERE user_id = $1 AND (favorite_flg <> 0 OR unit_img_type <> 0);",
			identity.userId);
		for (const auto& e : req.entries)
		{
			co_await transaction->execSqlCoro(
				"UPDATE user_units SET favorite_flg = $1,"
				" unit_img_type = CASE WHEN $2 = 2 AND EXISTS (SELECT 1 FROM user_unit_alt_art a"
				"  WHERE a.user_id = $3 AND a.unit_id = CAST(user_units.unit_id AS INTEGER)) THEN 2 ELSE 0 END"
				" WHERE user_id = $3 AND user_unit_id = $4;",
				e.favorite != 0 ? 1 : 0, e.unit_img_type, identity.userId, e.user_unit_id);
		}
	}
	catch (...)
	{
		transaction->rollback();
		throw;
	}

	const auto favourites = std::count_if(req.entries.begin(), req.entries.end(),
		[](const auto& e) { return e.favorite != 0; });
	const auto altArt = std::count_if(req.entries.begin(), req.entries.end(),
		[](const auto& e) { return e.unit_img_type == 2; });
	LOG_INFO << "UnitFavorite: " << identity.userId << " now has " << favourites
		<< " favourite unit(s), " << altArt << " on alternate art (" << req.entries.size() << " listed)";

	UnitFavoriteResp resp = {};
	resp.entries = std::move(req.entries);
	// The helper picker shows favourited units as own cards: refresh it now, or the
	// picker keeps the favourites it got at login (Relations.hpp buildHelperLists).
	{
	    auto lists = co_await gme::buildHelperLists(transaction, identity);
	    resp.helper_list = std::move(lists.reinforce_info);
	    resp.helper_friends = std::move(lists.friend_info);
	}

	std::string buffer{};
	if (const auto& ec2 = glz::write_json(resp, buffer); ec2)
	{
		const auto& glze = glz::format_error(ec2, buffer);
		LOG_ERROR << "UnitFavorite: reply serialization failed: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}

	co_return HandleResult::success(buffer);
}
