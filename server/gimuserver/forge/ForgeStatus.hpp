#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/*!
* What the game server did with the Balance Forge's files, for
* GET /bf/gme/forgeStatus.php.
*
* The admin tooling writes overlays under <balance_root>/server/ and can only tell
* a balancer "the game server accepted your change" when this registry says
* so for the exact bytes it wrote (sha256).  Every loader in forge/ reports
* here; nothing else reads it.  It holds file names, counts, hashes and plain
* reasons only: no paths, no file contents, no secrets, because the endpoint
* is public.
*/
namespace forge
{

/*! One MST overlay, server/mst/<file>. */
struct MstStatus
{
	std::string sha256;       //!< of the overlay's bytes, lowercase hex
	std::string baseSha256;   //!< of the image's deploy/mst/<file>; empty when it was not read
	std::string mode;         //!< "rows", "groups", "object", or empty when the envelope was refused
	int32_t rowsReplaced = 0;
	int32_t rowsAdded = 0;
	int32_t rowsDeleted = 0;
	int32_t groupsReplaced = 0;
	int32_t groupsAdded = 0;
	int32_t groupsDeleted = 0;
	bool applied = false;
	std::string error;        //!< plain sentence; empty when applied
};

/*! One record an archive overlay could not take. */
struct RejectedRecord
{
	std::string id;
	std::string reason;
};

/*! One archive overlay, server/archive/<file>. */
struct ArchiveStatus
{
	std::string sha256;
	std::string mode;         //!< "records" or "file"
	int32_t recordsReplaced = 0;
	int32_t recordsAdded = 0;
	int32_t recordsDeleted = 0;
	bool applied = false;
	std::vector<RejectedRecord> rejected;
	std::string error;
};

/*! A rule rules.json could not take. */
struct RuleError
{
	std::string rule;
	std::string message;
};

/*!
* server/rules.json, filled by the rules engine (forge/Rules.hpp).  `engine`
* false (a build without it) makes the endpoint say that nothing in the file
* is enforced, so the admin tooling never reports a rule as live that is not.
*/
struct RulesStatus
{
	bool engine = false;      //!< false: this build does not read rules.json
	std::string sha256;       //!< of the live snapshot's file; empty when no rules are live
	int64_t loadedAt = 0;     //!< when the live snapshot was built; 0 = never
	int32_t squads = 0;
	int32_t unlocks = 0;
	int32_t schedules = 0;
	std::vector<RuleError> errors;
	std::string error;        //!< why the newest file was not taken
};

/*!
* server/starter.json: the tutorial-completion presents (forge/StarterPack.hpp).
* The pack fields are the pack in force: the file's when it was taken, else
* the server's own defaults.
*/
struct StarterStatus
{
	bool engine = false;      //!< false: this build does not read starter.json
	bool present = false;     //!< the file was there at the last look
	std::string sha256;       //!< of the file last read; empty when there was none
	bool applied = false;     //!< the file's pack is the one in force
	int64_t loadedAt = 0;     //!< the last look that found a change; 0 = never
	std::string error;        //!< why the file was not taken
	int32_t gems = 0;
	int32_t zel = 0;
	int32_t karma = 0;
	std::vector<std::pair<int32_t, int32_t>> items;   //!< (item id, count)
	std::vector<int32_t> units;
};

void setMstStatus(const std::string& file, MstStatus status);
std::optional<MstStatus> mstStatus(const std::string& file);

void setArchiveStatus(const std::string& file, ArchiveStatus status);
std::optional<ArchiveStatus> archiveStatus(const std::string& file);

void setRulesStatus(RulesStatus status);

void setStarterStatus(StarterStatus status);

/*! Called once at the end of boot.  An empty balance root = the feature is off. */
void setBootInfo(const std::string& balanceRoot, int64_t bootedAt);

/*! env BF_SERVER_COMMIT, else the commit CMake saw at configure time, else "unknown". */
std::string serverCommit();

/*! The endpoint's body (format 1). */
std::string statusJson();

/*! Lowercase hex SHA-256: the hash the admin tooling compares with the file it wrote. */
std::string sha256Hex(std::string_view bytes);

/*! The commit baked in at configure time (forge/BuildInfo.cpp). */
const char* buildCommit();

} // namespace forge
