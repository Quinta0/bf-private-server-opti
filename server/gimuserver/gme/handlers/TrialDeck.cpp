#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <charconv>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The Research Lab's trial squads (old trials, dungeon_type 2: Trial No. 001
// onward).  Tapping a trial opens TrialPartyTopScene, whose initConnect
// @0x1AA7D14 sends TrialDeckGet whenever the client's UserTrialPartyDeckList
// holds no squad (the first visit of every app session), and
// MissionCheckScene::updateEvent @0x1883BC0 queues TrialDeckEdit before EVERY
// trial MissionStart (the squad screen also queues it when a squad changed).
// Both were unregistered, so every player was sent Home before the first
// trial battle.
//
// Rows travel under PnjQ49wK both ways (UserTrialPartyDeckInfoResponse::
// readParam @0x140460C, TrialDeckEditRequest::createBody @0x13ADE10): the
// normal party keys without the deck type, every number quoted.  The reply
// replaces the client's whole list (removeAllObjects on the first row), and
// the edit sends the whole list back.  Deck numbers
// (UserTrialPartyDeckList::getCount / getCountForEp3Trial / getCountForNewTrial):
// 0-2 are the three Trial Zone squads, 10 the Ep3 trial's (Battle Hall),
// 99 the new trial's (Strategy Zone).

namespace
{

/// One squad slot as the client writes and reads it.
struct TrialDeckRow
{
	std::string deck_num;      // zsiAn9P1
	std::string user_unit_id;  // edy7fq3L
	std::string member_type;   // gr48vsdJ (0 = leader, 1 = member, 2 = Strategy Zone reserve)
	std::string disp_order;    // XuJL4pc5
};

/// TrialDeckGet (login block only) and TrialDeckEdit (login block + squads).
struct TrialDeckReq
{
	LoginInfoReq login_info;          // IKqx1Cn9
	std::vector<TrialDeckRow> decks;  // PnjQ49wK
};

struct TrialDeckResp
{
	std::vector<TrialDeckRow> decks;  // PnjQ49wK
};

/// Slots per squad.  A Strategy Zone squad holds 5 units plus up to
/// TrialMissionMst max_reserve_num reserves (10 in F_TRIAL_MISSION_MST v72),
/// numbered on from slot 5 with member type 2 (NewTrialPartySetScene::checkDeck
/// @0x1A9371C); the bound only keeps junk out.
constexpr int64_t kMaxTrialSlots = 32;

/// The squads a TrialDeckEdit row may name, grouped as the client counts
/// them: an edit that carries any row of a group replaces that whole group.
enum class TrialDeckGroup { TrialZone, Ep3, NewTrial };

std::optional<TrialDeckGroup> trialDeckGroup(int64_t deckNum)
{
	if (deckNum >= 0 && deckNum <= 2)
		return TrialDeckGroup::TrialZone;
	if (deckNum == 10)
		return TrialDeckGroup::Ep3;
	if (deckNum == 99)
		return TrialDeckGroup::NewTrial;
	return std::nullopt;
}

/// The deck numbers DELETE clears for a group.
const char* groupFilter(TrialDeckGroup group)
{
	switch (group)
	{
	case TrialDeckGroup::TrialZone: return "deck_num BETWEEN 0 AND 2";
	case TrialDeckGroup::Ep3: return "deck_num = 10";
	case TrialDeckGroup::NewTrial: return "deck_num = 99";
	}
	return "0";
}

std::optional<int64_t> parseInt(const std::string& raw)
{
	int64_t value = 0;
	const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
	if (raw.empty() || ec != std::errc{} || ptr != raw.data() + raw.size())
		return std::nullopt;
	return value;
}

} // namespace

template <> struct glz::meta<TrialDeckRow>
{
	using T = TrialDeckRow;
	static constexpr auto value = glz::object(
		"zsiAn9P1", &T::deck_num,
		"edy7fq3L", &T::user_unit_id,
		"gr48vsdJ", &T::member_type,
		"XuJL4pc5", &T::disp_order);
};

template <> struct glz::meta<TrialDeckReq>
{
	using T = TrialDeckReq;
	static constexpr auto value = glz::object(
		"IKqx1Cn9", pkg::glaze::single_array<&T::login_info>(),
		"PnjQ49wK", &T::decks);
};

template <> struct glz::meta<TrialDeckResp>
{
	using T = TrialDeckResp;
	static constexpr auto value = glz::object(
		"PnjQ49wK", &T::decks);
};

// TrialDeckGet (gY63Zvy7) — the player's trial squads.  A player with none
// gets squad 1 = the active normal squad, the state the client's own fallback
// (TrialPartyTopScene::copyActiveDeck @0x1AA7ED4) would build.  Units sold
// since are left out; the client re-picks a leader if that was one
// (checkHaveLeader @0x1AAE770).
HANDLEF(TrialDeckGet)
{
	(void)session;

	TrialDeckReq req{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json); ec)
		LOG_WARN << "TrialDeckGet: parse error: " << glz::format_error(ec, json);

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	// TrialDeckGetRequest::createBody @0x13AE054 carries the client's current
	// cutscene progress (createDungeonEventUserInfoTag).
	co_await gme::storeClientScenarioMarkers(theDb(), identity,
		req.login_info.special_scenario_info,
		req.login_info.scenario_info.value_or(std::string{}));

	// Read before writing: a failed read must reach the player as an error, not
	// as "no squads", or the client would rebuild squad 1 and its next
	// TrialDeckEdit would overwrite the stored squads.
	const auto stored = co_await theDb()->execSqlCoro(
		"SELECT EXISTS (SELECT 1 FROM user_trial_decks WHERE user_id = $1) AS any_row;", identity.userId);
	if (stored.empty() || stored[0]["any_row"].as<int64_t>() == 0)
	{
		try
		{
			const auto seeded = co_await theDb()->execSqlCoro(
				"INSERT OR IGNORE INTO user_trial_decks (user_id, deck_num, disp_order, member_type, user_unit_id)"
				" SELECT d.user_id, 0, d.disp_order, d.member_type, d.user_unit_id"
				" FROM user_decks d JOIN user_info i ON i.id = d.user_id"
				" WHERE d.user_id = $1 AND d.deck_type = 1 AND d.deck_num = i.active_deck AND d.user_unit_id > 0;",
				identity.userId);
			if (seeded.affectedRows() > 0)
				LOG_INFO << "TrialDeckGet: seeded trial squad 1 (" << seeded.affectedRows()
					<< " unit(s)) from the active squad for " << identity.userId;
		}
		catch (const drogon::orm::DrogonDbException& ex)
		{
			// Nothing stored to lose: with no rows the client copies its
			// active squad itself and saves it with TrialDeckEdit.
			LOG_WARN << "TrialDeckGet: could not seed trial squad 1 for " << identity.userId << ": " << ex.base().what();
		}
	}

	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT t.deck_num, t.user_unit_id, t.member_type, t.disp_order FROM user_trial_decks t"
		" JOIN user_units u ON u.user_unit_id = t.user_unit_id AND u.user_id = t.user_id"
		" WHERE t.user_id = $1 ORDER BY t.deck_num, t.disp_order;",
		identity.userId);

	TrialDeckResp resp{};
	resp.decks.reserve(rows.size());
	for (const auto& r : rows)
	{
		resp.decks.push_back(TrialDeckRow{
			.deck_num = r["deck_num"].as<std::string>(),
			.user_unit_id = r["user_unit_id"].as<std::string>(),
			.member_type = r["member_type"].as<std::string>(),
			.disp_order = r["disp_order"].as<std::string>(),
		});
	}

	LOG_INFO << "TrialDeckGet: " << resp.decks.size() << " squad slot(s) for " << identity.userId;
	co_return HandleResult::success(glz::write_json(resp).value_or("{}"));
}

// TrialDeckEdit (MbdL5D4K) — stores the trial squads.  It precedes every
// trial MissionStart, so a malformed row is dropped, never refused: a refusal
// here would send the player Home instead of into the battle.
HANDLEF(TrialDeckEdit)
{
	(void)session;

	TrialDeckReq req{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json); ec)
	{
		// A half-read body must not replace the stored squads.
		LOG_ERROR << "TrialDeckEdit: parse error, squads left unchanged: " << glz::format_error(ec, json);
		co_return HandleResult::success("{}");
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info)).nonEmpty();

	auto transaction = co_await gme::newTransaction(theDb());
	try
	{
		// TrialDeckEditRequest::createBody @0x13ADE10 carries the client's
		// current cutscene progress (createDungeonEventUserInfoTag).
		co_await gme::storeClientScenarioMarkers(transaction, identity,
			req.login_info.special_scenario_info,
			req.login_info.scenario_info.value_or(std::string{}));

		std::set<int64_t> owned;
		for (const auto& row : co_await transaction->execSqlCoro(
			"SELECT user_unit_id FROM user_units WHERE user_id = $1;", identity.userId))
		{
			owned.insert(row["user_unit_id"].as<int64_t>());
		}

		// The client sends its whole list, so a squad it emptied is simply
		// absent.  A group with no row at all is left alone: that path did not
		// load it (e.g. a Battle Hall edit carries only deck 10).
		std::set<TrialDeckGroup> groups;
		for (const auto& member : req.decks)
		{
			if (const auto deckNum = parseInt(member.deck_num))
			{
				if (const auto group = trialDeckGroup(*deckNum))
					groups.insert(*group);
			}
		}
		for (const auto group : groups)
		{
			co_await transaction->execSqlCoro(
				std::string("DELETE FROM user_trial_decks WHERE user_id = $1 AND ") + groupFilter(group) + ";",
				identity.userId);
		}

		size_t stored = 0, dropped = 0;
		for (const auto& member : req.decks)
		{
			const auto deckNum = parseInt(member.deck_num);
			const auto unit = parseInt(member.user_unit_id);
			const auto memberType = parseInt(member.member_type);
			const auto dispOrder = parseInt(member.disp_order);
			if (!deckNum || !trialDeckGroup(*deckNum) || !unit || !owned.contains(*unit)
				|| !memberType || *memberType < 0 || *memberType > 2
				|| !dispOrder || *dispOrder < 0 || *dispOrder >= kMaxTrialSlots)
			{
				LOG_WARN << "TrialDeckEdit: slot dropped (deck '" << member.deck_num << "', unit '"
					<< member.user_unit_id << "', type '" << member.member_type << "', order '"
					<< member.disp_order << "')";
				++dropped;
				continue;
			}
			co_await transaction->execSqlCoro(
				"INSERT INTO user_trial_decks (user_id, deck_num, disp_order, member_type, user_unit_id)"
				" VALUES ($1, $2, $3, $4, $5)"
				" ON CONFLICT(user_id, deck_num, disp_order) DO UPDATE SET"
				" member_type = excluded.member_type, user_unit_id = excluded.user_unit_id;",
				identity.userId, *deckNum, *dispOrder, *memberType, *unit);
			++stored;
		}

		LOG_INFO << "TrialDeckEdit: stored " << stored << " squad slot(s), dropped " << dropped
			<< " for " << identity.userId;
	}
	catch (...)
	{
		transaction->rollback();
		throw;
	}

	co_return HandleResult::success("{}");
}
