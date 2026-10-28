// Frontier Hunter: the running event's data, indexed once.
#include "FrontierHunterDetail.hpp"

#include "Common.hpp"
#include "GameClock.hpp"
#include "HunterOrbs.hpp"
#include "RaidRequest.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <unordered_map>

namespace gme::frohun
{

using namespace detail;

namespace detail
{

EventData buildEventData()
{
	EventData data;
	const auto& cache = theServer()->cache();
	const auto& init = cache.initializeResp();
	data.eventId = cache.activeChallengeId();

	for (const auto& event : init.challenge)
	{
		if (event.id == data.eventId)
			data.endComment = event.comment_3;
	}

	for (const auto& hr : init.challenge_hr)
	{
		data.ladder[hr.id] = LadderRow{ hr.present_typ, hr.target_id, hr.target_cnt };
		data.maxHr = std::max(data.maxHr, hr.id);
	}

	if (data.eventId == 0)
		return data;

	for (const auto& mis : init.challenge_mis)
	{
		if (mis.id != data.eventId)
			continue;
		QuestRules rules;
		rules.eventId = data.eventId;
		rules.missionId = mis.mission_id;
		const auto rates = split(mis.score_rate, ',');
		// [17] is the Esteem -> Frontier Service rate; the client parses only
		// [0..16] (GameUtils::setNormalChallengeMissionSetting), so the
		// conversion was always the server's.  1 % when a row lacks it.
		rules.servicePercent = rates.size() > 17 ? std::strtod(rates[17].c_str(), nullptr) : 1.0;
		if (!(rules.servicePercent > 0.0))
			rules.servicePercent = 1.0;
		data.quests[mis.mission_id] = rules;
	}

	for (const auto& grade : init.challenge_grade)
	{
		if (data.quests.contains(grade.mission_id))
			data.grades[grade.mission_id].emplace_back(grade.scope, grade.grade_id);
	}
	for (auto& [mission, list] : data.grades)
		std::sort(list.begin(), list.end());

	for (const auto& reward : init.challenge_reward)
	{
		if (!data.quests.contains(reward.mission_id))
			continue;
		data.pointRewards[reward.mission_id].push_back(PointReward{
			reward.reward_id, reward.point_start, reward.reward_type,
			reward.target_id, reward.target_cnt, reward.reward_param });
	}

	// The score the client reports is its own count, so it is capped, but
	// generously: today's units score far past the 2022 tables (a real run on
	// the Entrance with a Lv 999 helper reported 1.9M, against a top reward of
	// 400k).  The battle's score counter has seven digits, so 9,999,999, or
	// three times the quest's highest point reward or top grade when that is
	// more (13.5M on event 97's Terminus, whose last reward is at 4.5M).
	constexpr int64_t kScoreCounterMax = 9'999'999;
	for (auto& [mission, rules] : data.quests)
	{
		int64_t top = 0;
		for (const auto& row : data.pointRewards[mission])
			top = std::max(top, row.pointStart);
		for (const auto& [score, grade] : data.grades[mission])
			top = std::max(top, score);
		rules.scoreCap = std::max(top * 3, kScoreCounterMax);
	}

	// Some events carry every rank-reward row twice (48, 83, 91); a set keeps
	// one of each so nothing is paid double.
	std::set<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>> seen;
	for (const auto& row : init.challenge_rank_reward)
	{
		if (row.fronhun_id != data.eventId)
			continue;
		if (!seen.emplace(row.rank_type, row.rank_min, row.rank_max, row.reward_type,
			row.target_id, row.target_cnt).second)
			continue;
		data.rankRows.push_back(RankRow{ row.rank_type, row.rank_min, row.rank_max,
			row.reward_type, row.target_id, row.target_cnt, row.commentm });
	}

	for (const auto& mvp : init.challenge_mvp)
	{
		if (mvp.id == data.eventId)
			data.mvpBoards.push_back(MvpBoard{ mvp.rank_type, std::to_string(mvp.mission_id) });
	}
	return data;
}

const EventData& eventData()
{
	static const EventData data = buildEventData();
	return data;
}

} // namespace detail

} // namespace gme::frohun
