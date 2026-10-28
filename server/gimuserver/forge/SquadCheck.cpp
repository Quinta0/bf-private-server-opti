#include "SquadCheck.hpp"

#include "App.hpp"

#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace forge
{

namespace
{

struct Member
{
	int32_t unitId = 0;
	bool reserve = false;   // Strategy Zone reserve (member_type 2)
};

using Squad = std::vector<Member>;

/*! What the checks know of the helper; the unit and the friend flag are read only when a rule asks. */
struct Helper
{
	bool used = false;
	bool own = false;
	int32_t unitId = 0;
};

std::string unitName(const int32_t unitId)
{
	const auto* facts = unitFacts(unitId);
	return facts && !facts->name.empty() ? facts->name : "Unit " + std::to_string(unitId);
}

int32_t elementOf(const int32_t unitId)
{
	const auto* facts = unitFacts(unitId);
	return facts ? facts->element : 0;
}

/*! "Fire", "Fire and Water", "Fire, Water and Earth" (joiner "and" or "or"). */
std::string elementList(const std::vector<int32_t>& elements, const std::string& joiner)
{
	std::string out;
	for (size_t i = 0; i < elements.size(); ++i)
	{
		if (i > 0)
			out += i + 1 == elements.size() ? " " + joiner + " " : ", ";
		out += elementName(elements[i]);
	}
	return out;
}

bool contains(const std::vector<int32_t>& list, const int32_t value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

/*! One squad against one rule's squad checks (banned through required). */
std::optional<std::string> squadProblem(const SquadRule& rule, const Squad& squad)
{
	for (const auto& m : squad)
		if (rule.banned.contains(m.unitId))
			return unitName(m.unitId) + " can't be used in this quest. Change your squad.";
	int32_t units = 0, cost = 0;
	for (const auto& m : squad)
	{
		if (m.reserve)
			continue;
		++units;
		const auto* facts = unitFacts(m.unitId);
		cost += facts ? facts->cost : 0;
	}
	if (rule.maxUnits > 0 && units > rule.maxUnits)
		return "This quest allows at most " + std::to_string(rule.maxUnits)
			+ (rule.maxUnits == 1 ? " unit" : " units") + " in your squad, not counting the helper.";
	if (rule.maxCost > 0 && cost > rule.maxCost)
		return "Your squad costs " + std::to_string(cost) + "; this quest allows at most "
			+ std::to_string(rule.maxCost) + ". Change your squad.";
	if (!rule.elements.empty())
		for (const auto& m : squad)
			if (!contains(rule.elements, elementOf(m.unitId)))
				return "Only " + elementList(rule.elements, "and") + " units can fight in this quest. Change your squad.";
	if (rule.oneElement && !squad.empty())
	{
		const auto first = elementOf(squad.front().unitId);
		for (const auto& m : squad)
			if (elementOf(m.unitId) != first)
				return std::string("All units in your squad must share one element in this quest.");
	}
	for (size_t i = 0; i < rule.requiredLines.size(); ++i)
	{
		const auto& line = rule.requiredLines[i];
		if (std::none_of(squad.begin(), squad.end(), [&line](const Member& m) { return line.contains(m.unitId); }))
			return "This quest needs " + unitName(rule.required[i]) + " (any form) in your squad.";
	}
	return std::nullopt;
}

drogon::Task<std::vector<Squad>> readSquads(const gme::UserIdentity identity, const int32_t missionId,
	const int32_t deckNum)
{
	std::vector<Squad> squads;
	if (const auto trial = trialDeckRange(missionId))
	{
		const auto rows = co_await theDb()->execSqlCoro(
			"SELECT t.deck_num, t.member_type, u.unit_id FROM user_trial_decks t"
			" JOIN user_units u ON u.user_id = t.user_id AND u.user_unit_id = t.user_unit_id"
			" WHERE t.user_id = $1 AND t.deck_num BETWEEN $2 AND $3 AND t.user_unit_id > 0"
			" ORDER BY t.deck_num, t.disp_order;",
			identity.userId, trial->first, trial->second);
		std::map<int32_t, Squad> byDeck;
		for (const auto& row : rows)
			byDeck[row["deck_num"].as<int32_t>()].push_back(
				{ row["unit_id"].as<int32_t>(), row["member_type"].as<int32_t>() == 2 });
		for (auto& [_, squad] : byDeck)
			squads.push_back(std::move(squad));
		co_return squads;
	}
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT d.member_type, u.unit_id FROM user_decks d"
		" JOIN user_units u ON u.user_id = d.user_id AND u.user_unit_id = d.user_unit_id"
		" WHERE d.user_id = $1 AND d.deck_type = 1 AND d.deck_num = $2 AND d.user_unit_id > 0"
		" ORDER BY d.disp_order;",
		identity.userId, deckNum);
	Squad squad;
	for (const auto& row : rows)
		squad.push_back({ row["unit_id"].as<int32_t>(), false });
	squads.push_back(std::move(squad));
	co_return squads;
}

} // namespace

drogon::Task<std::optional<Refusal>> checkSquad(const std::shared_ptr<const RulesSnapshot> snapshot,
	const gme::UserIdentity identity, const int32_t missionId, const int32_t deckNum, const std::string helperId)
{
	const auto rules = snapshot->squadRulesFor(missionId);
	if (rules.empty())
		co_return std::nullopt;
	const bool trial = trialDeckRange(missionId).has_value();
	const auto squads = co_await readSquads(identity, missionId, deckNum);

	Helper helper;
	helper.used = gme::borrowedHelper(helperId);
	helper.own = helper.used && gme::isOwnHelperId(helperId);
	bool unitRead = false;
	std::optional<bool> isFriend;

	for (const auto* rule : rules)
	{
		const auto refuse = [&rule](std::string message) { return Refusal{ rule->id, rule->message.empty() ? std::move(message) : rule->message }; };
		if (helper.used)
		{
			if (rule->helper == HelperRule::None)
				co_return refuse("This quest must be done without a helper. Start it without one.");
			if (rule->helper == HelperRule::OwnUnits && !helper.own)
				co_return refuse("In this quest the helper must be one of your own units.");
			if (rule->helper == HelperRule::FriendsOnly)
			{
				if (!helper.own && !isFriend)
					isFriend = co_await gme::isFriend(theDb(), identity, helperId);
				if (helper.own || !*isFriend)
					co_return refuse("In this quest the helper must be one of your friends.");
			}
			if ((!rule->helperElements.empty() || !rule->banned.empty()) && !unitRead)
			{
				unitRead = true;
				if (const auto own = gme::ownHelperUnitId(helperId))
				{
					const auto row = co_await theDb()->execSqlCoro(
						"SELECT unit_id FROM user_units WHERE user_id = $1 AND user_unit_id = $2;",
						identity.userId, *own);
					helper.unitId = row.empty() ? 0 : row[0]["unit_id"].as<int32_t>();
				}
				else if (!helper.own)
				{
					helper.unitId = (co_await gme::helperLeader(theDb(), identity, helperId)).unit_id;
				}
			}
			if (!rule->helperElements.empty() && !contains(rule->helperElements, elementOf(helper.unitId)))
			{
				const auto first = rule->helperElements.front();
				const std::string article = first == 3 ? "an " : "a ";   // "an Earth unit"
				co_return refuse("In this quest the helper must be " + article
					+ elementList(rule->helperElements, "or") + " unit.");
			}
			if (rule->banned.contains(helper.unitId))
				co_return refuse(unitName(helper.unitId) + " can't be used in this quest. Change your squad.");
		}
		for (const auto& squad : squads)
		{
			if (trial && squad.empty())
				continue;
			if (auto why = squadProblem(*rule, squad))
				co_return refuse(trial ? *why + " Every trial squad must follow this." : *why);
		}
	}
	co_return std::nullopt;
}

} // namespace forge
