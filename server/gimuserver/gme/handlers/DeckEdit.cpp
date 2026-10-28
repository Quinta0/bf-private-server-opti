#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <charconv>
#include <optional>
#include <string>

namespace
{

/// Deck numbers run 0-9 (ten party slots); anything else is ignored rather
/// than stored, so a garbled value cannot point the arena at a missing deck.
std::optional<int32_t> parseArenaDeck(const std::optional<std::string>& raw)
{
	if (!raw || raw->empty())
		return std::nullopt;
	int32_t value = -1;
	const auto [ptr, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
	if (ec != std::errc{} || ptr != raw->data() + raw->size() || value < 0 || value > 9)
		return std::nullopt;
	return value;
}

/*!
* The new player name a rename sends, "" when this DeckEdit carries none.
*
* PlayerInfoScene2::confirmAnswerYes queues a DeckEdit for a rename, and
* DeckEditRequest::createBody @0x13A2F94 adds the name as B5JQyV8j to the team
* node (Ti62XfZK) only while UserState::getChangeHandleName is set.  The
* generated DeckEditTeamInfo does not declare the key, so it is read from the
* raw body; the client had already shown the new name, and the old one came
* back with the next login (ticket #144).
*/
std::string requestedHandleName(const std::string& json)
{
	glz::generic parsed{};
	if (glz::read_json(parsed, json) || !parsed.is_object() || !parsed.contains("Ti62XfZK"))
		return {};
	const glz::generic* team = &parsed["Ti62XfZK"];
	if (team->is_array())
	{
		const auto& rows = team->get_array();
		if (rows.empty())
			return {};
		team = &rows.front();
	}
	if (!team->is_object() || !team->contains("B5JQyV8j"))
		return {};
	const auto& name = (*team)["B5JQyV8j"];
	return name.is_string() ? name.get<std::string>() : std::string{};
}

/// The rename box's own limit: PlayerInfoScene2::nameEditBox @0x179DF74
/// calls setMaxLength(8).  Counted in characters, not bytes.
constexpr size_t kMaxHandleNameChars = 8;

/*! The next code point of a strictly valid UTF-8 string, or -1 (overlong, surrogate, > U+10FFFF, cut short). */
int32_t nextCodePoint(const std::string& s, size_t& i)
{
	const auto byte = [&s](size_t at) { return static_cast<unsigned char>(s[at]); };
	const auto lead = byte(i);
	const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
	if (len == 0 || i + len > s.size())
		return -1;
	int32_t cp = len == 1 ? lead : len == 2 ? (lead & 0x1F) : len == 3 ? (lead & 0x0F) : (lead & 0x07);
	for (size_t k = 1; k < len; ++k)
	{
		if ((byte(i + k) & 0xC0) != 0x80)
			return -1;
		cp = (cp << 6) | (byte(i + k) & 0x3F);
	}
	constexpr int32_t kMinForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
	if (cp < kMinForLength[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return -1;
	i += len;
	return cp;
}

/*!
* Whether a requested name can be stored: strictly valid UTF-8, 1-8
* characters, at least one that is not a space, and none that the name lists
* and cards would draw wrongly -- control characters (C0, C1), '<' '>' (names
* are drawn in text that treats those as markup, the trap a "<3" present
* message fell into), zero-width and bidi formatting characters.
*/
bool acceptableHandleName(const std::string& name)
{
	size_t chars = 0;
	bool visible = false;
	for (size_t i = 0; i < name.size(); ++chars)
	{
		const auto cp = nextCodePoint(name, i);
		if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == '<' || cp == '>'
			|| (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E)
			|| (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF)
			return false;
		if (cp != ' ' && cp != 0xA0 && cp != 0x3000)
			visible = true;
	}
	return visible && chars <= kMaxHandleNameChars;
}

} // namespace

HANDLEF(DeckEdit)
{
	DeckEditReq req = {};
	const auto& ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json);
	if (ec)
	{
		const auto error = glz::format_error(ec, json);
		LOG_ERROR << "DeckEditReq deserialization failed:\n" << error;
		co_return HandleResult::error("Deserialization error", error);
	}

	const auto identity = (co_await gme::getUserIdentity(theDb(), req.login_info, true)).nonEmpty();

	// A rejected name is logged and skipped, never refused: a DeckEdit error
	// closes the session, and the deck change must still save.
	const auto newName = requestedHandleName(json);
	const bool rename = !newName.empty() && acceptableHandleName(newName);
	if (!newName.empty() && !rename)
		LOG_WARN << "DeckEdit: " << identity.userId << " asked for an unusable name; kept the old one";

	{
		auto transaction = co_await gme::newTransaction(theDb());
		try
		{
			(co_await db::DatabaseInterface::update(
				transaction,
				"user_info",
				{
					db::Data("active_deck", req.team_info.active_deck),
					db::Lookup("id", identity.userId),
				})).nonEmpty();

			// The arena deck rides along in the same team node; dropping it
			// meant the Arena always fought with deck 0 after a relaunch.
			if (const auto arenaDeck = parseArenaDeck(req.team_info.arena_deck))
			{
				co_await transaction->execSqlCoro(
					"UPDATE user_info SET active_arena_deck = $1 WHERE id = $2;",
					*arenaDeck, identity.userId);
			}

			if (rename)
			{
				co_await transaction->execSqlCoro(
					"UPDATE user_info SET username = $1 WHERE id = $2;", newName, identity.userId);
				LOG_INFO << "DeckEdit: " << identity.userId << " is now named " << newName;
			}

			if (!req.party_deck_info.empty())
			{
				(co_await gme::updateDecks(
					transaction,
					identity,
					req.party_deck_info)).nonEmpty();
			}

			// DeckEditRequest::createBody @0x13A2D8C builds its login block with
			// createDungeonEventUserInfoTag, so it carries the client's current
			// cutscene progress; keep it (see storeClientScenarioMarkers).
			co_await gme::storeClientScenarioMarkers(transaction, identity,
				req.login_info.special_scenario_info,
				req.login_info.scenario_info.value_or(std::string{}));
		}
		catch (...)
		{
			transaction->rollback();
			throw;
		}
	}

	co_return HandleResult::success("{}");
}
