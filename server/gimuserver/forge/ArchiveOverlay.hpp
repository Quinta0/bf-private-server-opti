#pragma once

#include "ForgeStatus.hpp"
#include "ReadError.hpp"

#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/*!
* Balance Forge archive overlays: <balance_root>/server/archive/<file>.
*
* Two kinds, both applied at boot, both falling back to the image's file:
* - records by id (gacha.json, event_exchange.json, ai.json): each record is
*   parsed and checked on its own, so one bad record is listed as rejected
*   while the rest still apply;
* - whole-file replacements (summon_rotation.json, event_rotation.json,
*   boss_drops.json): the loader tries the overlay with strict checks, then
*   the base file with today's rules.
*
* Formats and checks: the Forge server design, section 2.
*/
namespace forge
{

/*! Unknown keys, missing (non-optional) keys and bytes after the value are all errors. */
struct StrictOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};
inline constexpr StrictOpts kStrictOpts = [] {
	StrictOpts opts{};
	opts.error_on_missing_keys = true;
	return opts;
}();

/*! Parses one record strictly; the glaze error text, or nullopt when it read. */
template <class T>
std::optional<std::string> readStrict(T& out, const std::string& text)
{
	const auto ec = glz::read<kStrictOpts>(out, text);
	if (!ec)
		return std::nullopt;
	return plainReadError(ec, text);
}

/*! <balance_root>/server/archive/<file> when it is a regular file, else nullopt.  Marks it consumed. */
std::optional<std::filesystem::path> archiveOverlayPath(std::string_view file);

/*! A whole-file overlay's bytes and hash. */
struct FileOverlay
{
	std::string bytes;
	std::string sha256;
};

/*!
* Reads server/archive/<file> for a whole-file loader: nullopt when there is
* none, or (status error recorded) when it cannot be read.
*/
std::optional<FileOverlay> readFileOverlay(std::string_view file);

/*! The whole-file loader's verdict on its overlay. */
void recordFileOverlay(std::string_view file, const std::string& sha256, bool applied, const std::string& error);

/*! Top-level keys of a JSON object the overlay must carry; the first missing one, or nullopt. */
std::optional<std::string> missingTopKey(const std::string& bytes, const std::vector<std::string>& keys);

/*! A records overlay after its envelope was accepted. */
struct RecordsFile
{
	std::string sha256;
	std::vector<std::string> records;     //!< each record as compact JSON
	std::vector<std::string> recordIds;   //!< its "id" as text ("#n" when it has none)
	std::vector<std::string> deletes;
};

/*!
* Reads server/archive/<file> as {"format": 1, "file": <file>, "records": [...],
* "delete": [...]}: nullopt when there is none or (status error recorded) the
* envelope is refused.
*/
std::optional<RecordsFile> readRecordsOverlay(std::string_view file, bool allowDelete);

/*! End of boot: server/archive/*.json files no loader asked for get an error in the status. */
void reportUnusedArchiveOverlays();

/*!
* Records-by-id merge.  A record with a base record's id replaces it whole (in
* place), a new id is appended, `delete` removes.  `parse` reads one record's
* JSON into a Record (the glaze error text on failure); `validate` returns a
* plain reason or nullopt.  Never throws; the status lists every rejected record.
*/
template <class Record>
std::vector<Record> mergeRecords(std::string_view file, std::vector<Record> base,
	const std::function<std::string(const Record&)>& idOf,
	const std::function<std::optional<std::string>(const std::string&, Record&)>& parse,
	const std::function<std::optional<std::string>(const Record&)>& validate,
	bool allowDelete)
{
	auto overlay = readRecordsOverlay(file, allowDelete);
	if (!overlay)
		return base;

	ArchiveStatus status;
	status.sha256 = overlay->sha256;
	status.mode = "records";
	status.applied = true;

	std::unordered_map<std::string, size_t> index;
	for (size_t i = 0; i < base.size(); ++i)
		index.emplace(idOf(base[i]), i);

	std::map<std::string, Record> accepted;
	std::vector<std::string> order;
	std::set<std::string> seen;
	for (size_t i = 0; i < overlay->records.size(); ++i)
	{
		const auto& id = overlay->recordIds[i];
		if (!seen.insert(id).second)
		{
			status.rejected.push_back({ id, "This id is listed twice; only the first one was used." });
			continue;
		}
		Record record{};
		if (const auto why = parse(overlay->records[i], record))
		{
			status.rejected.push_back({ id, "The record does not read: " + *why });
			continue;
		}
		if (const auto why = validate(record))
		{
			status.rejected.push_back({ id, *why });
			continue;
		}
		accepted.emplace(id, std::move(record));
		order.push_back(id);
	}

	std::set<std::string> deletes;
	for (const auto& id : overlay->deletes)
	{
		if (!index.contains(id))
			status.rejected.push_back({ id, "There is no record " + id + " to delete." });
		else if (seen.contains(id))
			status.rejected.push_back({ id, "Record " + id + " is both in records and in delete; it was not deleted." });
		else if (!deletes.insert(id).second)
			status.rejected.push_back({ id, "Record " + id + " is deleted twice." });
	}

	std::vector<Record> merged;
	merged.reserve(base.size() + accepted.size());
	for (auto& record : base)
	{
		const auto id = idOf(record);
		if (deletes.contains(id))
		{
			++status.recordsDeleted;
			continue;
		}
		if (const auto it = accepted.find(id); it != accepted.end())
		{
			merged.push_back(std::move(it->second));
			++status.recordsReplaced;
			continue;
		}
		merged.push_back(std::move(record));
	}
	for (const auto& id : order)
	{
		if (index.contains(id))
			continue;
		merged.push_back(std::move(accepted.at(id)));
		++status.recordsAdded;
	}

	for (const auto& r : status.rejected)
		LOG_ERROR << "forge: archive " << file << " record " << r.id << " REJECTED: " << r.reason;
	LOG_INFO << "forge: archive " << file << " applied: " << status.recordsReplaced << " replaced, "
		<< status.recordsAdded << " added, " << status.recordsDeleted << " deleted, "
		<< status.rejected.size() << " rejected (sha256 " << status.sha256 << ")";
	setArchiveStatus(std::string(file), std::move(status));
	return merged;
}

} // namespace forge
