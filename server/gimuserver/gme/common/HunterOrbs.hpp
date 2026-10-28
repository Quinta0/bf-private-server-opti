#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Transactions.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// Hunter Orbs — the attempt currency Frontier Gate and Frontier Hunter share.
//
// They are NOT `team_info.fight_point`.  That is the ARENA Orbs
// (ShopHelFightScene's title string is "Arena Orbs"), and an earlier session
// spent a while restoring it and watching the Frontier Gate prompt keep
// appearing.  Hunter Orbs are internally "Aube" and live in the client's
// `ChallengeHeaderInfo` singleton, whose ONLY writer is the `kN2i7qds`
// ChallengeUserTeam response — a response this server had never sent.  So the
// count sat at the default 0 forever and every Frontier Gate entry took
// FrontierGateConditionScene::startCheck's no-orbs branch (@0x15DE978: it calls
// entryCheck only when getAube() > 0).
//
// Which of that response's three unnamed fields was the count had been left to
// a probe, and the probe came back inconclusive because every value it tried
// sat above the cap.  Disassembling ChallengeUserTeamResponse::readParam
// @0x13D3F94 settles it outright — see the citations in net/handlers.kdl.
//
// THE CLIENT RUNS THE CLOCK ITSELF.  `decFightRestTimer` @0x1258270 ticks the
// rest timer down one second at a time and, each time the remainder divides
// evenly by DefineMst recover_time_frohun, adds an orb and reloads the timer,
// stopping at max_frohun_p.  So the server's job is only to hand over an
// honest starting state — how many orbs are held, and how many seconds are left
// on the one currently refilling.  Both sides then agree without either
// polling the other.
//
// SPENDING IS THE SERVER'S.  MissionStartScene::initConnect @0x1823240 has the
// client's only two debits (setAube(max(getAube(),1)-1) at 0x1823380 and
// 0x1823468), and both are FRONTIER GATE: GameUtils::isFrontierGate (dungeon
// type 5) or isFGPlus (1000), and only while FrontierGateUtils::nowFrontierGate
// is false (getNowScore() == 0, the first floor).  A FRONTIER HUNTER quest
// (dungeon type 1, GameUtils::isNormalChallengeMission @0x1EC1620) is never
// debited by the client at all -- it is an ordinary MissionStart -- so its orb
// is charged here, at MissionStart (chargeHunterOrbForStart).  Frontier Gate
// pays at FrontierGateStart / FrontierGateRetry instead.  Until that charge
// existed nothing spent an orb on a Frontier Hunter entry, and every Survey
// Office refresh (kN2i7qds) reported the full, never-decremented count.

namespace gme
{

/*!
* The orb cap, from DefineMst `max_frohun_p` (`SziK4Xg1`, 3 in this data).
*
* ChallengeHeaderInfo::getAubeMax @0x125819C forwards straight to it, and the
* client clamps to it on every read — which is exactly why the earlier 4/5/6
* probe read back as 3/3/3 and could not tell the three fields apart.
*/
inline int32_t hunterOrbCap()
{
	const auto cap = theServer()->cache().initializeResp().defines.max_frohun_p;
	return cap > 0 ? cap : 3;
}

/*! Seconds to regenerate one orb, from DefineMst `recover_time_frohun` (10800). */
inline int32_t hunterOrbRegenSeconds()
{
	const auto secs = theServer()->cache().initializeResp().defines.recover_time_frohun;
	return secs > 0 ? secs : 10800;
}

/*!
* Whether a mission is a FRONTIER HUNTER quest: its dungeon is DungeonMst
* dungeon_type 1, the client's own test (GameUtils::isNormalChallengeMission
* @0x1EC1620).  Not 3 (the SP challenge), 5 or 1000 (Frontier Gate, FG+).
* The MST decides, not the request: MissionStart's optional cvg8hzp9 group is
* the client's say-so, and leaving it out must not make an entry free.
*/
inline bool isFrontierHunterMission(const int32_t missionId)
{
	const auto& cache = theServer()->cache();
	for (const auto& [dungeonId, missions] : cache.missionsByDungeon())
	{
		if (std::find(missions.begin(), missions.end(), missionId) == missions.end())
			continue;
		for (const auto& dungeon : cache.dungeonMst())
		{
			if (dungeon.dungeon_id == dungeonId)
				return dungeon.dungeon_type == 1;
		}
		return false;
	}
	return false;
}

/*!
* How long a charged Frontier Hunter start is recognised when the SAME request
* arrives again.  The client resends a request byte for byte when the player
* taps Retry after a lost reply; that must not take a second orb.
*/
inline constexpr int64_t kHunterOrbResendWindowSec = 30 * 60;

/*!
* The orb state as the client should see it right now.
*
* Stored state is the count at the time of the last write plus `restTs`, the
* unix time the orb then regenerating is due.  Everything since is caught up
* here rather than by a background job, the same way UserEnergy derives from
* `energy_full_ts`.
*
* @param orbs   Stored count.  Mutated to the derived current count.
* @param restTs Stored unix time the next orb lands; 0 when nothing is pending.
*               Mutated to the still-pending deadline (0 once at the cap).
* @return Seconds remaining on the orb being refilled, 0 when at the cap.
*/
inline int32_t deriveHunterOrbs(int32_t& orbs, int64_t& restTs)
{
	const auto cap = hunterOrbCap();
	const auto regen = hunterOrbRegenSeconds();

	if (orbs >= cap || restTs <= 0)
	{
		// At the cap, or nothing was ever spent: no clock running.  Clamp
		// rather than trusting an overcapped column — the client clamps too, so
		// a stored 5 would only ever read as 3 and the two would disagree.
		orbs = std::min(orbs, cap);
		restTs = 0;
		return 0;
	}

	const auto now = static_cast<int64_t>(
		std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
	if (now < restTs)
	{
		return static_cast<int32_t>(restTs - now);
	}

	// The pending orb landed, and possibly several more behind it.
	const auto elapsed = now - restTs;
	const auto extra = static_cast<int64_t>(elapsed / regen);
	orbs = static_cast<int32_t>(std::min<int64_t>(orbs + 1 + extra, cap));
	if (orbs >= cap)
	{
		restTs = 0;
		return 0;
	}

	restTs = restTs + (extra + 1) * regen;
	return static_cast<int32_t>(restTs - now);
}

// Read-only partial refresh for polls and purchases. The APK reader does not
// reset the singleton; omitted HR/score fields retain the client's current values.
inline drogon::Task<::HunterOrbRefresh> loadHunterOrbRefresh(
	const db::Database database, const UserIdentity identity)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT hunter_orbs, hunter_orb_rest_ts FROM user_info WHERE id = $1;", identity.userId);
	if (rows.empty()) throw std::runtime_error("Missing Hunter Orb account");
	auto orbs = rows[0]["hunter_orbs"].as<int32_t>();
	auto deadline = rows[0]["hunter_orb_rest_ts"].as<int64_t>();
	::HunterOrbRefresh result{};
	result.aube_rest_timer = deriveHunterOrbs(orbs, deadline);
	result.aube = orbs;
	co_return result;
}

/*!
* Fills the `kN2i7qds` header block for a user.
*
* Reads the stored orb state, catches it up, and writes the caught-up values
* back so the next read starts from them.  HR and the Frontier Gate score ride
* the same block because the client keeps them in the same singleton.
*
* @param database Database client or transaction to use.
* @param identity Resolved user identity.
* @param hrId Hunter Rank to report.  Read by FrontierGateUtils::entryCheck and
*             drawn in the header.
* @param frogateScore Frontier Gate score for the header.
* @return The block, ready to assign to a `user_team` field.
*/
inline drogon::Task<::ChallengeUserTeam> loadChallengeHeader(
	const db::Database database,
	const UserIdentity identity,
	const int32_t hrId,
	const int32_t frogateScore)
{
	::ChallengeUserTeam team = {};
	team.hr_id = hrId;
	team.frogate_score = frogateScore;
	team.aube = hunterOrbCap();
	team.aube_timer = 0;
	team.aube_rest_timer = 0;

	try
	{
		const auto rows = co_await database->execSqlCoro(
			"SELECT hunter_orbs, hunter_orb_rest_ts FROM user_info WHERE id = $1;",
			identity.userId);
		if (rows.empty())
		{
			co_return team;
		}

		auto orbs = rows[0]["hunter_orbs"].as<int32_t>();
		auto restTs = rows[0]["hunter_orb_rest_ts"].as<int64_t>();
		const auto storedOrbs = orbs;
		const auto storedTs = restTs;

		team.aube_rest_timer = deriveHunterOrbs(orbs, restTs);
		team.aube = orbs;
		// aube_timer (+0x44) is write-only in this build — getAubeTimer has no
		// callers — so it stays 0 rather than carrying an invented number.

		if (orbs != storedOrbs || restTs != storedTs)
		{
			co_await database->execSqlCoro(
				"UPDATE user_info SET hunter_orbs = $1, hunter_orb_rest_ts = $2 WHERE id = $3;",
				orbs, restTs, identity.userId);
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		// A header must never fail the screen it decorates.
		LOG_WARN << "loadChallengeHeader: " << ex.base().what();
	}

	co_return team;
}

/*!
* Spends one Hunter Orb, starting the refill clock if it was not already running.
*
* The Frontier Gate charge (FrontierGateStart / FrontierGateRetry).  Frontier
* Hunter pays through chargeHunterOrbForStart, which also guards resends.
*
* @param database Database client or transaction to use.
* @param identity Resolved user identity.
* @return True when an orb was spent, false when the player had none.
*/
inline drogon::Task<bool> spendHunterOrb(
	const db::Database database,
	const UserIdentity identity)
{
	try
	{
		const auto rows = co_await database->execSqlCoro(
			"SELECT hunter_orbs, hunter_orb_rest_ts FROM user_info WHERE id = $1;",
			identity.userId);
		if (rows.empty())
		{
			co_return false;
		}

		auto orbs = rows[0]["hunter_orbs"].as<int32_t>();
		auto restTs = rows[0]["hunter_orb_rest_ts"].as<int64_t>();
		deriveHunterOrbs(orbs, restTs);

		if (orbs <= 0)
		{
			co_return false;
		}

		const auto wasFull = orbs >= hunterOrbCap();
		--orbs;
		if (wasFull || restTs <= 0)
		{
			// The clock only starts on the drop BELOW the cap; spending while
			// already refilling must not push the pending orb further away.
			// (restTs <= 0 below the cap would be a clock that never ran, and
			// those orbs would never come back.)
			const auto now = static_cast<int64_t>(
				std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
			restTs = now + hunterOrbRegenSeconds();
		}

		co_await database->execSqlCoro(
			"UPDATE user_info SET hunter_orbs = $1, hunter_orb_rest_ts = $2 WHERE id = $3;",
			orbs, restTs, identity.userId);
		co_return true;
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_WARN << "spendHunterOrb: " << ex.base().what();
		co_return false;
	}
}

/*! The charged run a resent start matches: its serial and when it was paid. */
struct HunterOrbPaidRun
{
	int64_t serial = 0;
	int64_t paidAt = 0;
};

/*!
* The charged run this Frontier Hunter start is a RESEND of, if any.
*
* A resend is the same request object (GmeHeader.client_id, `requestTag`) for
* the same mission while the battle that request paid for is STILL OPEN
* (user_info.open_mission_serial): the client re-sending a start whose reply
* it never got.  The tag alone is not enough -- it sits in the plaintext header,
* so a client that kept re-using one could otherwise enter again and again on
* one orb.  Once that battle settles (MissionEnd zeroes open_mission_serial) or
* another start supersedes it, the tag buys nothing.  The window bounds a tag
* that a relaunch happens to reuse.  An empty tag (no session) never matches.
*/
inline drogon::Task<std::optional<HunterOrbPaidRun>> findHunterOrbResend(
	const db::Database database,
	const UserIdentity identity,
	const int32_t missionId,
	const std::string requestTag)
{
	if (requestTag.empty())
		co_return std::nullopt;
	const auto now = static_cast<int64_t>(
		std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
	const auto rows = co_await database->execSqlCoro(
		"SELECT r.serial, r.hunter_orb_paid_at FROM user_mission_runs r"
		" JOIN user_info u ON u.id = r.user_id AND u.open_mission_serial = r.serial"
		" WHERE r.user_id = $1 AND r.mission_id = $2 AND r.hunter_orb_tag = $3"
		" AND r.hunter_orb_paid_at >= $4 LIMIT 1;",
		identity.userId, static_cast<int64_t>(missionId), requestTag,
		now - kHunterOrbResendWindowSec);
	if (rows.empty())
		co_return std::nullopt;
	co_return HunterOrbPaidRun{ rows[0]["serial"].as<int64_t>(), rows[0]["hunter_orb_paid_at"].as<int64_t>() };
}

/*! Whether this start is a resend of one already charged (see findHunterOrbResend). */
inline drogon::Task<bool> isHunterOrbResend(
	const db::Database database,
	const UserIdentity identity,
	const int32_t missionId,
	const std::string requestTag)
{
	co_return (co_await findHunterOrbResend(database, identity, missionId, requestTag)).has_value();
}

/*!
* Charges the Hunter Orb for a Frontier Hunter MissionStart.
*
* One transaction, so the check, the spend, the mark on the run it paid for
* and that run becoming the OPEN battle cannot interleave with a second copy of
* the same request: the copy that runs second finds the open, marked run and is
* a resend.  A resend is not charged again; its new serial takes over the mark
* (with the original payment time, so the window does not slide) and becomes
* the open battle, so one orb always covers exactly one battle.  MissionStart
* records the open battle again further on; the same values, written early here.
* No refunds afterwards -- the client has no refund path either, and a give-up
* or a loss settles the battle like a clear (MissionEnd status 3).
*
* @param identity   The player.
* @param missionId  The quest being entered.
* @param runSerial  The serial this start issued (user_mission_runs).
* @param requestTag GmeHeader.client_id of this request, "" when unknown.
* @return The orb state to send back, or nullopt when the player has none.
*/
inline drogon::Task<std::optional<::HunterOrbRefresh>> chargeHunterOrbForStart(
	const UserIdentity identity,
	const int32_t missionId,
	const uint32_t runSerial,
	const std::string requestTag)
{
	auto transaction = co_await gme::newTransaction(theDb());

	const auto rows = co_await transaction->execSqlCoro(
		"SELECT hunter_orbs, hunter_orb_rest_ts FROM user_info WHERE id = $1;", identity.userId);
	if (rows.empty())
		co_return std::nullopt;
	auto orbs = rows[0]["hunter_orbs"].as<int32_t>();
	auto restTs = rows[0]["hunter_orb_rest_ts"].as<int64_t>();
	auto restTimer = deriveHunterOrbs(orbs, restTs);

	const auto now = static_cast<int64_t>(
		std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
	const auto serial = static_cast<int64_t>(runSerial);
	auto paidAt = now;

	const auto resent = co_await findHunterOrbResend(transaction, identity, missionId, requestTag);
	if (resent)
	{
		paidAt = resent->paidAt;
		if (resent->serial != serial)
		{
			co_await transaction->execSqlCoro(
				"UPDATE user_mission_runs SET hunter_orb_tag = '' WHERE serial = $1 AND user_id = $2;",
				resent->serial, identity.userId);
		}
		LOG_INFO << "chargeHunterOrbForStart: " << identity.userId << " resent the start of "
			<< missionId << "; already paid, " << orbs << " orb(s) left";
	}
	else
	{
		if (orbs <= 0)
			co_return std::nullopt;
		if (orbs >= hunterOrbCap() || restTs <= 0)
			restTs = now + hunterOrbRegenSeconds();   // the clock starts on the drop below the cap
		--orbs;
		restTimer = static_cast<int32_t>(std::max<int64_t>(restTs - now, 0));

		co_await transaction->execSqlCoro(
			"UPDATE user_info SET hunter_orbs = $1, hunter_orb_rest_ts = $2 WHERE id = $3;",
			orbs, restTs, identity.userId);
		LOG_INFO << "chargeHunterOrbForStart: " << identity.userId << " spent a Hunter Orb on "
			<< missionId << ", " << orbs << " left, next in " << restTimer << "s";
	}

	co_await transaction->execSqlCoro(
		"UPDATE user_mission_runs SET hunter_orb_paid_at = $1, hunter_orb_tag = $2"
		" WHERE serial = $3 AND user_id = $4;",
		paidAt, requestTag, serial, identity.userId);
	co_await transaction->execSqlCoro(
		"UPDATE user_info SET open_mission_id = $1, open_mission_serial = $2 WHERE id = $3;",
		static_cast<int64_t>(missionId), serial, identity.userId);

	::HunterOrbRefresh state{};
	state.aube = orbs;
	state.aube_rest_timer = restTimer;
	co_return state;
}

/*!
* Appends the orb block (kN2i7qds) to a serialised reply.
*
* The client parses kN2i7qds from ANY reply (GameResponseParser::
* getResponseObject @0x13935E4) and ChallengeUserTeamResponse::readParam
* @0x13D3F94 sets only the fields present, so the count/timer pair leaves Hunter
* Rank and the Frontier Gate score alone.  Spliced rather than declared on the
* response so the count rides the reply without a packet change.  Never throws:
* the orb is already spent when this runs, so a reply without the block (the
* Survey Office refreshes it anyway) beats an error.
*/
inline void appendHunterOrbs(std::string& body, const ::HunterOrbRefresh& orbs)
{
	std::string block;
	if (glz::write_json(orbs, block) || block.size() < 2 || body.size() < 2
		|| body.front() != '{' || body.back() != '}')
	{
		LOG_WARN << "appendHunterOrbs: reply left without the orb block";
		return;
	}
	body.pop_back();
	if (body.size() > 1)
		body += ',';
	body += "\"kN2i7qds\":";
	body += block;
	body += '}';
}

/*!
* Refills Hunter Orbs to the cap and stops the clock — the ShopUse type 5 grant.
*
* @param database Database client or transaction to use.
* @param identity Resolved user identity.
*/
inline drogon::Task<void> restoreHunterOrbsToCap(
	const db::Database database,
	const UserIdentity identity)
{
	try
	{
		co_await database->execSqlCoro(
			"UPDATE user_info SET hunter_orbs = $1, hunter_orb_rest_ts = 0 WHERE id = $2;",
			hunterOrbCap(), identity.userId);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_WARN << "restoreHunterOrbsToCap: " << ex.base().what();
	}
	co_return;
}

} // namespace gme
