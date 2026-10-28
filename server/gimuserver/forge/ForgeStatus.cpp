#include "ForgeStatus.hpp"

#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha.h>
#include <json/json.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace forge
{

namespace
{

struct Registry
{
	std::mutex mutex;
	std::map<std::string, MstStatus> mst;
	std::map<std::string, ArchiveStatus> archive;
	RulesStatus rules;
	StarterStatus starter;
	std::string balanceRoot;
	int64_t bootedAt = 0;
};

Registry& registry()
{
	static Registry instance;
	return instance;
}

Json::Value textOrNull(const std::string& text)
{
	return text.empty() ? Json::Value(Json::nullValue) : Json::Value(text);
}

Json::Value mstJson(const MstStatus& s)
{
	Json::Value out(Json::objectValue);
	out["sha256"] = textOrNull(s.sha256);
	out["base_sha256"] = textOrNull(s.baseSha256);
	out["mode"] = textOrNull(s.mode);
	out["rows_replaced"] = s.rowsReplaced;
	out["rows_added"] = s.rowsAdded;
	out["rows_deleted"] = s.rowsDeleted;
	out["groups_replaced"] = s.groupsReplaced;
	out["groups_added"] = s.groupsAdded;
	out["groups_deleted"] = s.groupsDeleted;
	out["applied"] = s.applied;
	out["error"] = textOrNull(s.error);
	return out;
}

Json::Value archiveJson(const ArchiveStatus& s)
{
	Json::Value out(Json::objectValue);
	out["sha256"] = textOrNull(s.sha256);
	out["mode"] = textOrNull(s.mode);
	out["records_replaced"] = s.recordsReplaced;
	out["records_added"] = s.recordsAdded;
	out["records_deleted"] = s.recordsDeleted;
	out["applied"] = s.applied;
	Json::Value rejected(Json::arrayValue);
	for (const auto& r : s.rejected)
	{
		Json::Value row(Json::objectValue);
		row["id"] = r.id;
		row["reason"] = r.reason;
		rejected.append(std::move(row));
	}
	out["rejected"] = std::move(rejected);
	out["error"] = textOrNull(s.error);
	return out;
}

Json::Value rulesJson(const RulesStatus& s, const std::string& balanceRoot)
{
	// Present = the file is there now (one stat per request), whatever the
	// engine made of it: the admin tooling compares it with what it wrote.
	bool present = false;
	if (!balanceRoot.empty())
	{
		std::error_code ec;
		present = std::filesystem::is_regular_file(
			std::filesystem::path(balanceRoot) / "server" / "rules.json", ec) && !ec;
	}
	Json::Value out(Json::objectValue);
	out["present"] = present;
	out["sha256"] = textOrNull(s.sha256);
	out["loaded_at"] = s.loadedAt > 0 ? Json::Value(static_cast<Json::Int64>(s.loadedAt)) : Json::Value(Json::nullValue);
	Json::Value counts(Json::objectValue);
	counts["squads"] = s.squads;
	counts["unlocks"] = s.unlocks;
	counts["schedules"] = s.schedules;
	out["counts"] = std::move(counts);
	Json::Value errors(Json::arrayValue);
	for (const auto& e : s.errors)
	{
		Json::Value row(Json::objectValue);
		row["rule"] = e.rule;
		row["message"] = e.message;
		errors.append(std::move(row));
	}
	out["errors"] = std::move(errors);
	std::string error = s.error;
	if (!s.engine && present)
		error = "This game server does not read rules.json yet; nothing in it is enforced.";
	out["error"] = textOrNull(error);
	return out;
}

Json::Value starterJson(const StarterStatus& s)
{
	Json::Value out(Json::objectValue);
	out["engine"] = s.engine;
	out["present"] = s.present;
	out["sha256"] = textOrNull(s.sha256);
	out["applied"] = s.applied;
	out["loaded_at"] = s.loadedAt > 0 ? Json::Value(static_cast<Json::Int64>(s.loadedAt)) : Json::Value(Json::nullValue);
	out["error"] = textOrNull(s.error);
	Json::Value pack(Json::objectValue);
	pack["gems"] = s.gems;
	pack["zel"] = s.zel;
	pack["karma"] = s.karma;
	Json::Value items(Json::arrayValue);
	for (const auto& [id, count] : s.items)
	{
		Json::Value row(Json::objectValue);
		row["id"] = id;
		row["count"] = count;
		items.append(std::move(row));
	}
	pack["items"] = std::move(items);
	Json::Value units(Json::arrayValue);
	for (const auto id : s.units)
	{
		Json::Value row(Json::objectValue);
		row["id"] = id;
		units.append(std::move(row));
	}
	pack["units"] = std::move(units);
	out["pack"] = std::move(pack);
	return out;
}

} // namespace

void setMstStatus(const std::string& file, MstStatus status)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	r.mst.insert_or_assign(file, std::move(status));
}

std::optional<MstStatus> mstStatus(const std::string& file)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	const auto it = r.mst.find(file);
	if (it == r.mst.end())
		return std::nullopt;
	return it->second;
}

void setArchiveStatus(const std::string& file, ArchiveStatus status)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	r.archive.insert_or_assign(file, std::move(status));
}

std::optional<ArchiveStatus> archiveStatus(const std::string& file)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	const auto it = r.archive.find(file);
	if (it == r.archive.end())
		return std::nullopt;
	return it->second;
}

void setRulesStatus(RulesStatus status)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	r.rules = std::move(status);
}

void setStarterStatus(StarterStatus status)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	r.starter = std::move(status);
}

void setBootInfo(const std::string& balanceRoot, const int64_t bootedAt)
{
	auto& r = registry();
	std::lock_guard lock(r.mutex);
	r.balanceRoot = balanceRoot;
	r.bootedAt = bootedAt;
}

std::string serverCommit()
{
	// The prod image can name the commit it was built from at run time; a
	// local build only knows what CMake saw when it configured.
	if (const char* env = std::getenv("BF_SERVER_COMMIT"); env != nullptr && *env != '\0')
		return env;
	const std::string baked = buildCommit();
	return baked.empty() ? "unknown" : baked;
}

std::string statusJson()
{
	Json::Value out(Json::objectValue);
	{
		auto& r = registry();
		std::lock_guard lock(r.mutex);
		out["format"] = 1;
		out["server_commit"] = serverCommit();
		out["booted_at"] = r.bootedAt > 0 ? Json::Value(static_cast<Json::Int64>(r.bootedAt)) : Json::Value(Json::nullValue);
		out["balance_root"] = !r.balanceRoot.empty();
		Json::Value mst(Json::objectValue);
		for (const auto& [file, status] : r.mst)
			mst[file] = mstJson(status);
		out["mst"] = std::move(mst);
		Json::Value archive(Json::objectValue);
		for (const auto& [file, status] : r.archive)
			archive[file] = archiveJson(status);
		out["archive"] = std::move(archive);
		out["rules"] = rulesJson(r.rules, r.balanceRoot);
		out["starter"] = starterJson(r.starter);
	}
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	writer["emitUTF8"] = true;
	return Json::writeString(writer, out);
}

std::string sha256Hex(const std::string_view bytes)
{
	CryptoPP::SHA256 hash;
	std::string out;
	CryptoPP::StringSource(
		reinterpret_cast<const CryptoPP::byte*>(bytes.data()), bytes.size(), true,
		new CryptoPP::HashFilter(hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(out), false)));
	return out;
}

} // namespace forge
