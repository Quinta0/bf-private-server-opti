#include "App.hpp"

#include <gimuserver/gme/common/RaidRewards.hpp>
#include <gimuserver/gme/common/Raids.hpp>

#include <algorithm>
#include <map>
#include <sstream>

namespace gme::raid
{

namespace
{

constexpr int64_t kMaxZelKarma = 99'999'999LL;
constexpr int32_t kMaxRc = 7;
// Present types (shared vocabulary, Present.cpp): zel, item, karma, medal.
constexpr int32_t kPresentZel = 3;
constexpr int32_t kPresentItem = 4;
constexpr int32_t kPresentKarma = 11;
constexpr int32_t kPresentMedal = 12;
// RaidMissionRewardInfo reward types, as the result scenes sort them
// (RaidFieldQuestResultScene ctor @0x197C940).  1 only feeds the zel/karma/
// friend point count-up of page 1262 and, at present type 12, the medal scene
// 1266: items sent as 1 were paid but never shown (ticket #154).
// The materials page 1285 lists 2, 5 and 6; the Raid Class-up scene 1263
// shows 3 and the clear-reward scene 1265 shows 4.
constexpr int32_t kRewardBase = 1;
constexpr int32_t kRewardMaterial = 2;
constexpr int32_t kRewardRcUp = 3;

std::vector<std::string> split(const std::string& text, char separator)
{
	std::vector<std::string> out;
	std::stringstream stream(text);
	std::string item;
	while (std::getline(stream, item, separator))
		out.push_back(item);
	return out;
}

::RaidMissionRewardInfo rewardRow(int32_t presentType, std::string targetId, int32_t count, int32_t rewardType)
{
	::RaidMissionRewardInfo row{};
	row.present_type = presentType;
	row.target_id = std::move(targetId);
	row.target_cnt = count;
	row.reward_type = rewardType;
	return row;
}

/*! The bag goes to the present box: the raid bag only exists for the run. */
drogon::Task<void> payBag(
	const db::Database database, const UserIdentity identity, const RaidRun run, RaidPayout& payout)
{
	const auto bag = co_await database->execSqlCoro(
		"SELECT item_id, num FROM raid_mission_bag WHERE room_mission_id = $1 AND user_id = $2 AND num > 0"
		" ORDER BY frame_id;",
		run.id, identity.userId);
	for (const auto& slot : bag)
	{
		const auto itemId = slot["item_id"].as<std::string>();
		const auto num = slot["num"].as<int32_t>();
		co_await addUserPresent(database, identity, kPresentItem, itemId, num, 0, "Raid Battle: found in the field");
		payout.rewards.push_back(rewardRow(kPresentItem, itemId, num, kRewardMaterial));
	}
}

bool itemExists(int32_t itemId)
{
	for (const auto& item : theServer()->cache().itemMst())
		if (item.id == itemId)
			return true;
	return false;
}

/*!
* Battle drops banked by BattleEnd ("itemId:count,...", BattleRewardList::
* getItemCsv @0x10DFA24) to the present box.  The client reports them, so
* only real items count and every count is capped: raid battles are built
* without drop tables (RaidBattles stripDrops), so honest reports are rare.
*/
drogon::Task<void> payDrops(
	const db::Database database, const UserIdentity identity, const RaidRun run, RaidPayout& payout)
{
	constexpr int32_t kMaxDropRows = 20;
	constexpr int32_t kMaxDropCount = 99;
	const auto rows = co_await database->execSqlCoro(
		"SELECT drops FROM raid_mission_user WHERE room_mission_id = $1 AND user_id = $2;", run.id, identity.userId);
	if (rows.empty())
		co_return;
	std::map<int32_t, int32_t> totals;
	for (const auto& entry : split(rows[0]["drops"].as<std::string>(), ','))
	{
		const auto fields = split(entry, ':');
		if (fields.size() != 2 || static_cast<int32_t>(totals.size()) >= kMaxDropRows)
			continue;
		const auto itemId = std::atoi(fields[0].c_str());
		const auto count = std::clamp(std::atoi(fields[1].c_str()), 0, kMaxDropCount);
		if (count > 0 && itemExists(itemId))
			totals[itemId] = std::min(kMaxDropCount, totals[itemId] + count);
	}
	for (const auto& [itemId, count] : totals)
	{
		co_await addUserPresent(database, identity, kPresentItem, std::to_string(itemId), count, 0,
			"Raid Battle: battle drop");
		payout.rewards.push_back(rewardRow(kPresentItem, std::to_string(itemId), count, kRewardMaterial));
	}
}

bool medalExists(int32_t medalId)
{
	for (const auto& medal : theServer()->cache().medalMst())
		if (medal.medal_id == medalId)
			return true;
	return false;
}

/*!
* RaidMissionMst b5yeVr61 (`unk3`, "medalId:count") is the Brave Medals a won
* run pays: the client never reads it, but the counts climb with the RC
* (0-1 at RC 1 to 11 at RC 7) and the Medal Rush missions, whose descriptions
* promise medals, hold the largest (5 .. 28).  CoinScene (1266) shows the row
* and looks the medal up without a null check, so an unknown id is skipped.
* Paid to the present box, not the medal store: the MissionEnd reply cannot
* refresh the client's medal count, a present claim does (PresentReceipt
* medalInfo), and Brave Slots spends against that count.
*/
drogon::Task<void> payMedals(
	const db::Database database, const UserIdentity identity, const ::RaidMissionMst& mission, RaidPayout& payout)
{
	if (mission.unk3.size() < 2 || mission.unk3[1] <= 0)
		co_return;
	if (!medalExists(mission.unk3[0]))
	{
		LOG_WARN << "Raid: mission " << mission.id << " pays unknown medal " << mission.unk3[0] << ", skipped";
		co_return;
	}
	const auto medalId = std::to_string(mission.unk3[0]);
	co_await addUserPresent(database, identity, kPresentMedal, medalId, mission.unk3[1], 0, "Raid Battle: Brave Medals");
	payout.rewards.push_back(rewardRow(kPresentMedal, medalId, mission.unk3[1], kRewardBase));
}

/*!
* RaidRcMst JQ23rIvk is "presentType:targetId:count:?:?,..."; only the first
* three fields are understood (RAIDS_ROUND2_CORE 7.5), so the rest is ignored.
*/
drogon::Task<void> payRcUp(
	const db::Database database, const UserIdentity identity, const int32_t newRc, RaidPayout& payout)
{
	for (const auto& rc : theServer()->cache().raidRcMst())
	{
		if (rc.rc_id != newRc)
			continue;
		for (const auto& entry : rc.reward_info)
		{
			const auto fields = split(entry, ':');
			if (fields.size() < 3)
				continue;
			const auto type = std::atoi(fields[0].c_str());
			const auto count = std::max(1, std::atoi(fields[2].c_str()));
			co_await addUserPresent(database, identity, type, fields[1], count, 0,
				"Raid Battle: Raid Class " + std::to_string(newRc) + " reward");
			payout.rewards.push_back(rewardRow(type, fields[1], count, kRewardRcUp));
		}
	}
}

} // namespace

drogon::Task<RaidPayout> payRun(
	const db::Database database, const UserIdentity identity, const RaidRun run)
{
	RaidPayout payout{};
	const auto user = co_await loadRaidUser(database, identity.userId);
	payout.rcBefore = user.rc;
	const auto mission = runMission(run);
	if (run.result != kRunWon || !mission)
		co_return payout;

	co_await database->execSqlCoro(
		"UPDATE user_info SET zel = MAX(zel, MIN(zel + $1, $2)), karma = MAX(karma, MIN(karma + $3, $4)) WHERE id = $5;",
		mission->earn_zel, kMaxZelKarma, mission->karma, kMaxZelKarma, identity.userId);
	payout.rewards.push_back(rewardRow(kPresentZel, "", mission->earn_zel, kRewardBase));
	payout.rewards.push_back(rewardRow(kPresentKarma, "", mission->karma, kRewardBase));
	co_await payMedals(database, identity, *mission, payout);
	co_await payBag(database, identity, run, payout);
	co_await payDrops(database, identity, run, payout);

	const auto first = co_await database->execSqlCoro(
		"INSERT OR IGNORE INTO raid_clear(user_id, mission_id, cleared_at) VALUES ($1, $2, $3);",
		identity.userId, run.missionId, nowSeconds());
	payout.firstClear = first.affectedRows() > 0;

	// A class-up mission only counts at the player's own class: clearing an
	// RC-1 class-up mission again at RC 3 must not skip ahead.
	if (mission->unk2 == 1 && mission->rc_id == user.rc && user.rc < kMaxRc)
	{
		payout.rcUp = true;
		co_await database->execSqlCoro(
			"UPDATE raid_user SET rc = $1 WHERE user_id = $2;", user.rc + 1, identity.userId);
		co_await payRcUp(database, identity, user.rc + 1, payout);
		LOG_INFO << "Raid: " << identity.userId << " raised Raid Class " << user.rc << " -> " << user.rc + 1;
	}
	co_return payout;
}

} // namespace gme::raid
