#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <drogon/drogon.h>

#include <cstdint>
#include <string>

/*!
* The difficulty log: one line per finished quest run, for
* GET /bf/gme/forgeClears.php.
*
* A run opens at MissionStart (started) and finishes as
*   clear    MissionEnd with mission_status 2 (or none, read as a win like MissionEnd does);
*   fail     MissionEnd with any other status (lost, or given up);
*   abandon  the same player started another battle first.
* Each finished run appends one JSON line to forge-clears.jsonl in the save
* directory (BF_FORGE_CLEARS_FILE overrides; /data when it exists, else the
* working directory): the time, the mission, the result, the real seconds from
* start to end, the turns the client reported, and the squad's summed
* HP/ATK/DEF/REC (helper included) with its top rarity.  NO USER IDS: neither
* the line nor the endpoint names a player.  Open runs live in memory only, so
* a restart mid-battle logs that battle without seconds or squad.
*
* The file is capped (8 MiB) with one rotation (.1); the per-mission
* aggregates are rebuilt from both files at boot and kept up to date in memory.
* Nothing here may fail a battle: every entry point swallows and logs its errors.
*/
namespace forge::clears
{

/*! The squad a run fought with, summed (helper included); all 0 when unknown. */
struct Strength
{
	int64_t hp = 0, atk = 0, def = 0, rec = 0;
	int32_t topRarity = 0;
	int32_t units = 0;   //!< units summed, helper included
};

/*!
* The squad MissionStart opened a battle with, read the way the squad rules
* read it (forge::checkSquad): user_decks squad `deckNum`, or for a trial the
* strongest of its trial squads (the client does not say which one fights).
* The helper: an own unit (MYUNIT, `rawHelperId`) from user_units, a real
* player's lent leader from their card, a computer helper at its unit_mst max
* stats.  A fixed-party quest (not the player's squad) reads nothing.
* Never throws.
*/
drogon::Task<Strength> readStrength(gme::UserIdentity identity, int32_t missionId, int32_t deckNum,
	std::string rawHelperId, std::string recordedHelper, bool fixedParty);

/*! MissionStart opened battle `serial`: the player's previous open run, if any, is logged as abandoned. */
void started(const std::string& userId, int32_t missionId, uint32_t serial, const Strength& strength);

/*!
* MissionEnd settled battle `serial` (past the once-per-battle guard).
* @param status MissionEnd j3g5P4cq (2 = won); -1 when the client sent none.
* @param turns The client's turn total (Lu0pAk1R summed), 0 when unknown.
*/
void ended(const std::string& userId, int32_t missionId, uint32_t serial, int32_t status, int64_t turns);

/*! End of boot: the aggregates from forge-clears.jsonl(.1). */
void loadAtBoot();

/*! The body of GET /bf/gme/forgeClears.php: per-mission aggregates, no per-user data. */
std::string aggregatesJson();

} // namespace forge::clears
