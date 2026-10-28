#include "AccountReset.hpp"

#include <gimuserver/gme/common/GameClock.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <map>
#include <mutex>
#include <random>

namespace gme
{
namespace
{

struct PendingCode
{
	std::string code;
	int64_t expiresAt = 0;
};

// One code per profile, in memory: a restart only means "search 000000 again".
std::mutex g_mutex;
std::map<std::string, PendingCode> g_pending;

int64_t now()
{
	return static_cast<int64_t>(GameClock::nowSeconds());
}

/*! 6 digits from the OS CSPRNG, never starting with 0 (so never kResetStartCode). */
std::string drawCode()
{
	std::random_device rd;
	return std::to_string(100000u + rd() % 900000u);
}

/*! What became of a search for a 6-digit code. */
enum class CodeCheck { None, Match, Wrong, Expired };

CodeCheck takeCode(const std::string& userId, const std::string& digits)
{
	const std::lock_guard lock(g_mutex);
	const auto t = now();
	const auto it = g_pending.find(userId);
	if (it == g_pending.end())
		return CodeCheck::None;
	if (it->second.expiresAt <= t)
	{
		g_pending.erase(it);
		return digits.size() == 6 ? CodeCheck::Expired : CodeCheck::None;
	}
	if (it->second.code == digits)
	{
		g_pending.erase(it);
		return CodeCheck::Match;
	}
	return digits.size() == 6 ? CodeCheck::Wrong : CodeCheck::None;
}

std::string issueCode(const std::string& userId)
{
	const std::lock_guard lock(g_mutex);
	const auto t = now();
	for (auto it = g_pending.begin(); it != g_pending.end();)
		it = it->second.expiresAt <= t ? g_pending.erase(it) : std::next(it);
	auto code = drawCode();
	g_pending[userId] = PendingCode{ code, t + kResetCodeLifeSec };
	return code;
}

/*! The name as the popup may show it: the client reads < and > as markup. */
std::string shownName(const std::string& name)
{
	std::string out;
	for (const unsigned char c : name)
		if (c >= 0x20 && c != 0x7f && c != '<' && c != '>')
			out += static_cast<char>(c);
	if (out.size() > 32)
		out.resize(32);
	return out;
}

drogon::Task<bool> resetQueued(db::Database database, const std::string& userId)
{
	const auto row = co_await database->execSqlCoro(
		"SELECT 1 FROM account_reset_queue WHERE user_id = $1 AND state = 'queued' LIMIT 1;", userId);
	co_return !row.empty();
}

const std::string kOnItsWay =
	"Your reset is on its way. Close the game now and start it again in a few minutes: "
	"you begin at the tutorial.";

} // namespace

drogon::Task<std::optional<std::string>> accountResetSearch(
	db::Database database, UserIdentity identity, std::string digits)
{
	if (digits == kResetStartCode)
	{
		if (co_await resetQueued(database, identity.userId))
			co_return kOnItsWay;
		const auto card = co_await loadPlayerCard(database, identity.userId);
		if (!card)
			co_return std::optional<std::string>{};
		const auto code = issueCode(identity.userId);
		const auto name = shownName(card->handle_name);
		LOG_INFO << "AccountReset: " << identity.userId << " asked for a reset code";
		co_return "Reset " + (name.empty() ? std::string("your account") : name)
			+ " (Lv " + std::to_string(card->level) + ", ID " + card->friend_id + ")? "
			"Everything on this account is deleted for good: units, items, Gems and progress. "
			"You start again at the tutorial. To confirm, search " + code + " within 10 minutes.";
	}

	switch (takeCode(identity.userId, digits))
	{
	case CodeCheck::None:
		co_return std::optional<std::string>{};
	case CodeCheck::Wrong:
		co_return std::string("That is not your reset code. To get a new one, search ")
			+ kResetStartCode + ".";
	case CodeCheck::Expired:
		co_return std::string("That reset code has run out. To get a new one, search ")
			+ kResetStartCode + ".";
	case CodeCheck::Match:
		break;
	}

	if (!(co_await resetQueued(database, identity.userId)))
		co_await database->execSqlCoro(
			"INSERT INTO account_reset_queue(user_id, gumi_user_id, requested_at, state) "
			"VALUES ($1, $2, $3, 'queued');",
			identity.userId, identity.gumiUserId, now());
	LOG_INFO << "AccountReset: " << identity.userId << " (login " << identity.gumiUserId
		<< ") confirmed a reset; queued";
	co_return std::string("Reset confirmed. Close the game now. In about 2 minutes, start it again: "
		"you begin at the tutorial.");
}

} // namespace gme
