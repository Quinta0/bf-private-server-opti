#include "DailyChallenges.hpp"

#include "Achievements.hpp"
#include "App.hpp"
#include "Common.hpp"
#include "GameClock.hpp"
#include "RaidRequest.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace gme
{

namespace challenge
{

namespace
{

/*!
* No battle reports more than this of anything.  The client sends the counts;
* the cap keeps a forged body from overflowing a column, and every condition
* clamps at its own target anyway.
*/
constexpr int64_t kCountCap = 1'000'000'000LL;

int64_t clampCount(const int64_t value)
{
	return std::clamp<int64_t>(value, 0, kCountCap);
}

/*! The numbers in a cond_param, which packs "N", "0,N", "<elem>,N" or "<elem>:N". */
std::vector<int64_t> paramNumbers(const std::string& packed)
{
	std::vector<int64_t> out;
	int64_t value = 0;
	bool inNumber = false;
	for (const char c : packed)
	{
		if (c >= '0' && c <= '9')
		{
			value = std::min<int64_t>(value * 10 + (c - '0'), kCountCap);
			inNumber = true;
		}
		else if (inNumber)
		{
			out.push_back(value);
			value = 0;
			inNumber = false;
		}
	}
	if (inNumber)
		out.push_back(value);
	return out;
}

/*! "<a>:<n>,<b>:<m>,..." summed into per-key totals. */
std::map<int64_t, int64_t> pairCounts(const std::string& csv)
{
	std::map<int64_t, int64_t> out;
	for (const auto& entry : detail::splitCsv(csv))
	{
		const auto colon = entry.find(':');
		if (colon == std::string::npos)
			continue;
		const auto key = paramNumbers(entry.substr(0, colon));
		const auto count = paramNumbers(entry.substr(colon + 1));
		if (key.size() == 1 && count.size() == 1)
			out[key.front()] = clampCount(out[key.front()] + count.front());
	}
	return out;
}

int32_t unitElement(const int64_t unitId)
{
	const auto& units = theServer()->cache().unitMst();
	const auto it = std::find_if(units.begin(), units.end(),
		[unitId](const ::UnitMst& u) { return u.id == unitId; });
	return it == units.end() ? 0 : it->element;
}

bool validElement(const int64_t element)
{
	return element >= 1 && element <= 6;
}

} // namespace

Condition conditionOf(const ::AchievementSubjectMst& subject)
{
	Condition condition{};
	const auto numbers = paramNumbers(subject.cond_param);
	if (numbers.empty())
		return condition;
	condition.target = numbers.back();

	switch (subject.disp_order)
	{
	case 8500: condition.kind = Kind::QuestClears; break;
	case 9900: condition.kind = Kind::RaidClears; break;
	case 7600: condition.kind = Kind::ArenaBattles; break;
	case 7700: condition.kind = Kind::ArenaWins; break;
	case 8100: condition.kind = Kind::ArenaFlawless; break;
	case 8200: condition.kind = Kind::ArenaBbKills; break;
	case 8300: condition.kind = Kind::ArenaOverkills; break;
	case 6500: condition.kind = Kind::QuestEnemies; break;
	case 5701: condition.kind = Kind::Overkills; break;
	case 7201: condition.kind = Kind::MultiKills; break;
	case 7301: condition.kind = Kind::Ailments; break;
	case 7401: condition.kind = Kind::Criticals; break;
	case 7001: condition.kind = Kind::StrongerElemDamage; break;
	case 6201: condition.kind = Kind::BestSparks; condition.highWater = true; break;
	case 6301: condition.kind = Kind::BestBattleCrystals; condition.highWater = true; break;
	// The element is the FIRST number: "3,5" is five Earth units in the arena,
	// "3:30" thirty Earth enemies in quests.
	case 8000:
	case 6606:
		if (numbers.size() != 2 || !validElement(numbers.front()))
			return {};
		condition.kind = subject.disp_order == 8000 ? Kind::ArenaElementKills : Kind::QuestElementKills;
		condition.element = static_cast<int32_t>(numbers.front());
		break;
	default:
		return {};
	}
	if (condition.target <= 0)
		return {};
	return condition;
}

bool Progress::empty() const
{
	const auto none = [](const std::array<int64_t, 7>& a)
	{ return std::all_of(a.begin(), a.end(), [](const int64_t v) { return v == 0; }); };
	return questClears == 0 && raidClears == 0 && arenaBattles == 0 && arenaWins == 0
		&& arenaFlawless == 0 && arenaBbKills == 0 && arenaOverkills == 0 && questEnemies == 0
		&& overkills == 0 && multiKills == 0 && ailments == 0 && criticals == 0
		&& strongerElemDamage == 0 && bestSparks == 0 && bestBattleCrystals == 0
		&& none(questElementKills) && none(arenaElementKills);
}

int64_t amountFor(const Condition& condition, const Progress& progress)
{
	const auto element = static_cast<size_t>(validElement(condition.element) ? condition.element : 0);
	switch (condition.kind)
	{
	case Kind::QuestClears:        return progress.questClears;
	case Kind::RaidClears:         return progress.raidClears;
	case Kind::ArenaBattles:       return progress.arenaBattles;
	case Kind::ArenaWins:          return progress.arenaWins;
	case Kind::ArenaFlawless:      return progress.arenaFlawless;
	case Kind::ArenaBbKills:       return progress.arenaBbKills;
	case Kind::ArenaOverkills:     return progress.arenaOverkills;
	case Kind::ArenaElementKills:  return element ? progress.arenaElementKills[element] : 0;
	case Kind::QuestElementKills:  return element ? progress.questElementKills[element] : 0;
	case Kind::QuestEnemies:       return progress.questEnemies;
	case Kind::Overkills:          return progress.overkills;
	case Kind::MultiKills:         return progress.multiKills;
	case Kind::Ailments:           return progress.ailments;
	case Kind::Criticals:          return progress.criticals;
	case Kind::StrongerElemDamage: return progress.strongerElemDamage;
	case Kind::BestSparks:         return progress.bestSparks;
	case Kind::BestBattleCrystals: return progress.bestBattleCrystals;
	case Kind::Unknown:            break;
	}
	return 0;
}

// The battle log keys are paired with their BattleLog getters in
// MissionEndRequest::createBody @0x13A78F8 (net/mission.kdl,
// MissionEndReqBattleResult).
Progress progressFromMissionEnd(const std::string& json, const bool won)
{
	const raid::RaidRequest req(json);
	const auto* log = req.node("rXvA1E5y");
	const auto count = [log](const char* key)
	{ return clampCount(raid::RaidRequest::numberOf(log, key)); };

	Progress progress{};
	progress.questClears = won ? 1 : 0;
	progress.overkills = count("EcDR3f6M");            // getOverKillAttackCnt: hits on a downed enemy
	// getMultiKillCnt: enemies downed in turns that downed two or more
	// (BattleParty::exitTurn @0x10B2AD8), the client's own measure.
	progress.multiKills = count("xFH5i4o8");
	progress.ailments = count("J3EBd90b");             // getBadStateCnt
	progress.criticals = count("2z8kTtBy");            // getCriticalCnt
	// "Damage to Units of stronger elements" is the DISADVANTAGED hit -- the
	// original's own example is "Earth Unit damages Fire Unit".  The client
	// files that under getWeakElemDamage (the attacker is weak against the
	// target, DamageData::update @0x103BFE0); getStrongElemDamage (CwN87TyL)
	// is the opposite, advantaged hit.
	progress.strongerElemDamage = count("32edsbP0");   // getWeakElemDamage
	progress.bestSparks = count("U8uZLA34");           // getSparkCnt
	progress.bestBattleCrystals = count("hoG2ieT5");   // getBattleCrystalNum

	// getKoElemMonsterCsv: "<element>:<count>" for every enemy knocked out.
	for (const auto& [element, kills] : pairCounts(raid::RaidRequest::textOf(log, "Fw2dr06x")))
	{
		progress.questEnemies = clampCount(progress.questEnemies + kills);
		if (validElement(element))
			progress.questElementKills[static_cast<size_t>(element)] = kills;
	}
	return progress;
}

// ArenaBattleEndRequest::createBody @0x139E898 (net/arena.kdl).
Progress progressFromArenaEnd(const std::string& json, const bool won)
{
	const raid::RaidRequest req(json);
	const auto* result = req.node("yTEvm85f");
	const auto count = [result](const char* key)
	{ return clampCount(raid::RaidRequest::numberOf(result, key)); };

	Progress progress{};
	progress.arenaBattles = 1;
	progress.arenaWins = won ? 1 : 0;
	// getPerfect: 1 only on a win, not a time-up, with none of the player's units down.
	progress.arenaFlawless = won && count("s92RgSwp") == 1 ? 1 : 0;
	progress.arenaOverkills = count("Wnc6A31E");
	// getBbKnockDownCnt: enemies downed by a BB, SBB or UBB.  It is bumped
	// with exactly one of the three split counters (DamageData::update
	// @0x103C714), so their sum stands in when it is missing.
	progress.arenaBbKills = count("XRoD9Hg7");
	if (progress.arenaBbKills == 0)
		progress.arenaBbKills = clampCount(count("GRCVD1i8") + count("Uy15wSJd") + count("EfwspXXo"));

	// getDefeatUnitsInfo: the unit id of every enemy unit downed, comma-joined.
	for (const auto& id : detail::splitCsv(raid::RaidRequest::textOf(result, "sQ2aMk8B")))
	{
		const auto numbers = paramNumbers(id);
		if (numbers.size() != 1)
			continue;
		const auto element = unitElement(numbers.front());
		if (validElement(element))
		{
			auto& kills = progress.arenaElementKills[static_cast<size_t>(element)];
			kills = clampCount(kills + 1);
		}
	}
	return progress;
}

} // namespace challenge

namespace
{

/*! The cache's MST row for a challenge id, or null when this data lacks it. */
const ::AchievementSubjectMst* challengeRow(const int64_t subjectId)
{
	if (!challenge::isChallengeId(subjectId))
		return nullptr;
	const auto& table = theServer()->cache().achievementSubjectMst();
	const auto it = std::find_if(table.begin(), table.end(),
		[subjectId](const ::AchievementSubjectMst& s)
		{ return s.id == subjectId && s.category == challenge::kCategory; });
	return it == table.end() ? nullptr : &*it;
}

int64_t nowSeconds()
{
	return static_cast<int64_t>(GameClock::nowSeconds());
}

int32_t toInt32(const int64_t value)
{
	return static_cast<int32_t>(std::clamp<int64_t>(
		value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

/*!
* How many challenges may be held at once: DefineMst p3pXbuHA, which is what
* RandallAchievementCommon::getMaxAchievementAcceptCnt @0x1A319BC reads for
* category 2.  0 or less means no cap, as there.
*/
int32_t acceptCap()
{
	return theServer()->cache().initializeResp().defines.max_achievement_challenge_accept_count;
}

/*! One stored attempt. */
struct Attempt
{
	int64_t windowDay = 0;
	int32_t subjectId = 0;
	int32_t state = 1;
	int64_t progress = 0;
	bool rewarded = false;
	bool failNotified = false;
};

drogon::Task<std::vector<Attempt>> loadAttempts(
	const db::Database database, const UserIdentity& identity)
{
	std::vector<Attempt> attempts;
	for (const auto& row : co_await database->execSqlCoro(
		"SELECT window_day, subject_id, state, progress, reward_received, fail_notified"
		" FROM user_daily_challenges WHERE user_id = $1 ORDER BY window_day, subject_id;",
		identity.userId))
	{
		Attempt attempt{};
		attempt.windowDay = row["window_day"].as<int64_t>();
		attempt.subjectId = row["subject_id"].as<int32_t>();
		attempt.state = row["state"].as<int32_t>();
		attempt.progress = row["progress"].as<int64_t>();
		attempt.rewarded = row["reward_received"].as<int32_t>() != 0;
		attempt.failNotified = row["fail_notified"].as<int32_t>() != 0;
		attempts.push_back(attempt);
	}
	co_return attempts;
}

/*!
* What the Challenges bucket holds right now, before it is turned into rows.
* Shared by the list, the accept cap and the badge so the three agree.
*/
struct Board
{
	challenge::Window window;
	std::vector<Attempt> leftovers;   //!< finished in an earlier window, unclaimed
	std::map<int32_t, Attempt> open;  //!< attempts in the open window, by id
	std::vector<Attempt> expired;     //!< started earlier, ran out, not yet told
};

Board boardOf(const std::vector<Attempt>& attempts, const int64_t now)
{
	Board board{};
	board.window = challenge::windowAt(now);
	std::set<int32_t> leftoverIds;
	for (const auto& attempt : attempts)
	{
		if (attempt.windowDay == board.window.startDay)
		{
			board.open[attempt.subjectId] = attempt;
			continue;
		}
		if (attempt.windowDay > board.window.startDay)
			continue;   // written under a clock that has since gone back
		if (attempt.state == 2 && !attempt.rewarded)
		{
			// The OLDEST unclaimed one per id is what a claim of that id pays,
			// so it is the one shown.
			if (leftoverIds.insert(attempt.subjectId).second)
				board.leftovers.push_back(attempt);
		}
		else if (attempt.state == 1 && !attempt.failNotified)
		{
			board.expired.push_back(attempt);
		}
	}
	return board;
}

/*!
* How many rows the client counts against the cap: getAcceptCount @0x12C2850
* counts state 1, and state 2 with the reward still to claim.
*/
int32_t heldCount(const Board& board)
{
	int32_t held = static_cast<int32_t>(board.leftovers.size());
	for (const auto& [id, attempt] : board.open)
	{
		const bool shadowed = std::any_of(board.leftovers.begin(), board.leftovers.end(),
			[id](const Attempt& a) { return a.subjectId == id; });
		if (!shadowed && (attempt.state == 1 || (attempt.state == 2 && !attempt.rewarded)))
			++held;
	}
	return held;
}

::UserAchievementSubjectInfo rowFor(
	const int32_t subjectId, const int64_t target, const int64_t progress,
	const int32_t state, const int32_t rewardReceived, const int64_t secondsLeft)
{
	::UserAchievementSubjectInfo row{};
	row.subject_id = std::to_string(subjectId);
	row.progress = std::clamp<int64_t>(progress, 0, target);
	row.need_count = toInt32(target);
	row.state = state;
	row.reward_received = rewardReceived;
	// >= 1 prints the countdown, 0 "Time Expired", < 0 nothing at all.
	row.time_left = toInt32(secondsLeft);
	row.new_flag = 0;
	return row;
}

} // namespace

drogon::Task<void> advanceDailyChallenges(
	const db::Database database,
	const UserIdentity& identity,
	const challenge::Progress& progress)
{
	if (progress.empty())
		co_return;

	const auto window = challenge::windowAt(nowSeconds());
	const auto running = co_await database->execSqlCoro(
		"SELECT subject_id, progress FROM user_daily_challenges"
		" WHERE user_id = $1 AND window_day = $2 AND state = 1;",
		identity.userId, window.startDay);
	for (const auto& row : running)
	{
		const auto subjectId = row["subject_id"].as<int32_t>();
		const auto* subject = challengeRow(subjectId);
		if (subject == nullptr)
			continue;
		const auto condition = challenge::conditionOf(*subject);
		const auto amount = challenge::amountFor(condition, progress);
		if (amount <= 0)
			continue;

		const auto before = row["progress"].as<int64_t>();
		const auto after = std::min(condition.target,
			condition.highWater ? std::max(before, amount) : before + amount);
		if (after == before)
			continue;
		co_await database->execSqlCoro(
			"UPDATE user_daily_challenges SET progress = $1, state = $2"
			" WHERE user_id = $3 AND window_day = $4 AND subject_id = $5 AND state = 1;",
			after, after >= condition.target ? 2 : 1,
			identity.userId, window.startDay, subjectId);
		LOG_INFO << "DailyChallenges: " << identity.userId << " " << subjectId << " "
			<< before << " -> " << after << "/" << condition.target;
	}
}

drogon::Task<std::vector<::UserAchievementSubjectInfo>> loadDailyChallengeRows(
	const db::Database database,
	const UserIdentity& identity)
{
	const auto now = nowSeconds();
	const auto board = boardOf(co_await loadAttempts(database, identity), now);
	const auto secondsLeft = std::max<int64_t>(board.window.endsAt - now, 1);

	std::vector<::UserAchievementSubjectInfo> rows;
	std::set<int32_t> sent;
	for (const auto& attempt : board.leftovers)
	{
		const auto* subject = challengeRow(attempt.subjectId);
		if (subject == nullptr)
			continue;
		rows.push_back(rowFor(attempt.subjectId, challenge::conditionOf(*subject).target,
			attempt.progress, 2, 1, -1));
		sent.insert(attempt.subjectId);
	}

	for (const auto subjectId : board.window.subjectIds)
	{
		const auto* subject = challengeRow(subjectId);
		if (subject == nullptr || sent.count(subjectId))
			continue;
		const auto target = challenge::conditionOf(*subject).target;
		if (target <= 0)
			continue;   // a condition this server cannot count is not offered
		const auto it = board.open.find(subjectId);
		if (it == board.open.end())
			rows.push_back(rowFor(subjectId, target, 0, 0, 0, secondsLeft));
		else if (it->second.state == 2)
			rows.push_back(rowFor(subjectId, target, it->second.progress, 2,
				it->second.rewarded ? 2 : 1, -1));
		else
			rows.push_back(rowFor(subjectId, target, it->second.progress, 1, 0, secondsLeft));
		sent.insert(subjectId);
	}

	// THE EXPIRED NOTICE, ONCE.  A state-3 row goes to the client's fail list
	// (addObject @0x12C2CF0), and FailState @0x1A5CE38 shows one "has expired"
	// notice per row and clears it.  It is latched here so the next visit does
	// not repeat it.  The fail list is a vector of its own beside the rows on
	// screen (Data+0x10 vs +0x08), and nothing reads across the two, so last
	// week's attempt is reported even when this week offers the same id again.
	std::set<int32_t> expired;
	for (const auto& attempt : board.expired)
	{
		const auto* subject = challengeRow(attempt.subjectId);
		if (subject != nullptr && expired.insert(attempt.subjectId).second)
		{
			rows.push_back(rowFor(attempt.subjectId, challenge::conditionOf(*subject).target,
				attempt.progress, 3, 0, 0));
		}
		co_await database->execSqlCoro(
			"UPDATE user_daily_challenges SET fail_notified = 1"
			" WHERE user_id = $1 AND window_day = $2 AND subject_id = $3;",
			identity.userId, attempt.windowDay, attempt.subjectId);
	}
	co_return rows;
}

drogon::Task<ChallengeAcceptResult> acceptDailyChallenge(
	const db::Database database,
	const UserIdentity& identity,
	const int32_t subjectId,
	const int32_t acceptFlag)
{
	const auto now = nowSeconds();
	const auto window = challenge::windowAt(now);
	const auto& ids = window.subjectIds;
	if (std::find(ids.begin(), ids.end(), subjectId) == ids.end() || challengeRow(subjectId) == nullptr)
		co_return ChallengeAcceptResult::NotOffered;
	const auto board = boardOf(co_await loadAttempts(database, identity), now);
	const auto held = board.open.find(subjectId);

	if (acceptFlag == 2)
	{
		if (held == board.open.end() || held->second.state != 1)
			co_return ChallengeAcceptResult::NotGivenUp;
		co_await database->execSqlCoro(
			"DELETE FROM user_daily_challenges"
			" WHERE user_id = $1 AND window_day = $2 AND subject_id = $3 AND state = 1;",
			identity.userId, board.window.startDay, subjectId);
		co_return ChallengeAcceptResult::GaveUp;
	}

	// A finished attempt of an earlier window with the same id is what the
	// list shows in its place, so there is no Start on screen to press.
	const bool shadowed = std::any_of(board.leftovers.begin(), board.leftovers.end(),
		[subjectId](const Attempt& a) { return a.subjectId == subjectId; });
	if (held != board.open.end() || shadowed)
		co_return ChallengeAcceptResult::AlreadyHeld;
	const auto cap = acceptCap();
	if (cap > 0 && heldCount(board) >= cap)
		co_return ChallengeAcceptResult::CapReached;

	// Settled attempts of earlier windows are history nobody reads: claimed,
	// or expired and already told.  Dropped here, where the table grows.
	co_await database->execSqlCoro(
		"DELETE FROM user_daily_challenges WHERE user_id = $1 AND window_day < $2"
		" AND (reward_received = 1 OR (state = 1 AND fail_notified = 1));",
		identity.userId, board.window.startDay);
	co_await database->execSqlCoro(
		"INSERT INTO user_daily_challenges"
		" (user_id, window_day, subject_id, state, progress, reward_received, fail_notified, accepted_at)"
		" VALUES ($1, $2, $3, 1, 0, 0, 0, $4)"
		" ON CONFLICT(user_id, window_day, subject_id) DO NOTHING;",
		identity.userId, board.window.startDay, subjectId, now);
	co_return ChallengeAcceptResult::Started;
}

drogon::Task<bool> claimDailyChallenge(
	const db::Database database,
	const UserIdentity& identity,
	const int32_t subjectId)
{
	const auto window = challenge::windowAt(nowSeconds());
	// Guarded on reward_received = 0, so a replayed body pays once.
	const auto latched = co_await database->execSqlCoro(
		"UPDATE user_daily_challenges SET reward_received = 1"
		" WHERE user_id = $1 AND subject_id = $2 AND window_day = ("
		"   SELECT MIN(window_day) FROM user_daily_challenges"
		"   WHERE user_id = $1 AND subject_id = $2 AND window_day <= $3"
		"     AND state = 2 AND reward_received = 0)"
		" AND state = 2 AND reward_received = 0"
		" RETURNING window_day;",
		identity.userId, subjectId, window.startDay);
	co_return !latched.empty();
}

drogon::Task<int32_t> dailyChallengesClaimable(
	const db::Database database,
	const UserIdentity& identity)
{
	const auto board = boardOf(co_await loadAttempts(database, identity), nowSeconds());
	auto claimable = static_cast<int32_t>(board.leftovers.size());
	for (const auto& [id, attempt] : board.open)
	{
		const bool shadowed = std::any_of(board.leftovers.begin(), board.leftovers.end(),
			[id](const Attempt& a) { return a.subjectId == id; });
		if (!shadowed && attempt.state == 2 && !attempt.rewarded)
			++claimable;
	}
	co_return claimable;
}

} // namespace gme
