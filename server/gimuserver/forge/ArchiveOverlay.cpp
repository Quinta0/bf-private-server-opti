#include "ArchiveOverlay.hpp"

#include "JsonText.hpp"
#include "MstOverlay.hpp"

#include <json/json.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace forge
{

namespace
{

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxOverlayBytes = 32u << 20;

/*! server/archive files some loader asked for. */
std::set<std::string> g_consumed;

std::string readBytes(const fs::path& path)
{
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	if (ec)
		throw std::runtime_error("The file could not be read: " + ec.message() + ".");
	if (size > kMaxOverlayBytes)
		throw std::runtime_error("The file is larger than 32 MiB.");
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("The file could not be opened.");
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

Json::Value parseStrictJson(const std::string& bytes)
{
	Json::CharReaderBuilder builder;
	Json::CharReaderBuilder::strictMode(&builder.settings_);
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	Json::Value out;
	std::string errors;
	if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &out, &errors))
	{
		while (!errors.empty() && (errors.back() == '\n' || errors.back() == ' '))
			errors.pop_back();
		throw std::runtime_error("The file is not valid JSON: " + errors);
	}
	return out;
}

void recordError(std::string_view file, const std::string& sha256, const std::string& mode, const std::string& error)
{
	ArchiveStatus status;
	status.sha256 = sha256;
	status.mode = mode;
	status.applied = false;
	status.error = error;
	LOG_ERROR << "forge: archive " << file << " REJECTED: " << error;
	setArchiveStatus(std::string(file), std::move(status));
}

} // namespace

std::optional<fs::path> archiveOverlayPath(std::string_view file)
{
	if (balanceRoot().empty())
		return std::nullopt;
	const auto path = fs::path(balanceRoot()) / "server" / "archive" / std::string(file);
	std::error_code ec;
	if (!fs::is_regular_file(path, ec) || ec)
		return std::nullopt;
	g_consumed.insert(std::string(file));
	return path;
}

std::optional<FileOverlay> readFileOverlay(std::string_view file)
{
	const auto path = archiveOverlayPath(file);
	if (!path)
		return std::nullopt;
	try
	{
		FileOverlay overlay;
		overlay.bytes = readBytes(*path);
		overlay.sha256 = sha256Hex(overlay.bytes);
		return overlay;
	}
	catch (const std::exception& ex)
	{
		recordError(file, "", "file", ex.what());
		return std::nullopt;
	}
}

void recordFileOverlay(std::string_view file, const std::string& sha256, const bool applied, const std::string& error)
{
	if (!applied)
	{
		recordError(file, sha256, "file", error);
		return;
	}
	ArchiveStatus status;
	status.sha256 = sha256;
	status.mode = "file";
	status.applied = true;
	LOG_INFO << "forge: archive " << file << " applied: the whole file replaces the image's (sha256 " << sha256 << ")";
	setArchiveStatus(std::string(file), std::move(status));
}

std::optional<std::string> missingTopKey(const std::string& bytes, const std::vector<std::string>& keys)
{
	try
	{
		const auto root = parseStrictJson(bytes);
		if (!root.isObject())
			return "The file must be a JSON object.";
		for (const auto& key : keys)
			if (!root.isMember(key))
				return "The file has no \"" + key + "\".";
		return std::nullopt;
	}
	catch (const std::exception& ex)
	{
		return std::string(ex.what());
	}
}

std::optional<RecordsFile> readRecordsOverlay(std::string_view file, const bool allowDelete)
{
	const auto path = archiveOverlayPath(file);
	if (!path)
		return std::nullopt;
	RecordsFile out;
	try
	{
		const auto bytes = readBytes(*path);
		out.sha256 = sha256Hex(bytes);
		const auto root = parseStrictJson(bytes);
		if (!root.isObject())
			throw std::runtime_error("The file must be a JSON object: {\"format\": 1, \"file\": ..., \"records\": [...]}.");
		for (const auto& name : root.getMemberNames())
			if (name != "format" && name != "file" && name != "records" && name != "delete")
				throw std::runtime_error("Unknown top-level key \"" + name + "\".");
		if (!root.isMember("format") || !root["format"].isInt() || root["format"].asInt() != 1)
			throw std::runtime_error("format must be 1.");
		if (!root.isMember("file") || !root["file"].isString() || root["file"].asString() != file)
			throw std::runtime_error("file must be \"" + std::string(file) + "\", the file's own name.");
		if (!root.isMember("records") || !root["records"].isArray())
			throw std::runtime_error("records must be a list of records.");
		if (root.isMember("delete"))
		{
			if (!allowDelete)
				throw std::runtime_error(std::string(file) + " takes no delete list.");
			if (!root["delete"].isArray())
				throw std::runtime_error("delete must be a list of ids.");
			for (const auto& id : root["delete"])
			{
				if (!id.isString() && !id.isIntegral())
					throw std::runtime_error("delete must be a list of ids.");
				out.deletes.push_back(valueText(id));
			}
		}
		Json::ArrayIndex n = 0;
		for (const auto& record : root["records"])
		{
			++n;
			out.records.push_back(compactJson(record));
			out.recordIds.push_back(record.isObject() && record.isMember("id") && (record["id"].isString() || record["id"].isIntegral())
				? valueText(record["id"]) : "#" + std::to_string(n));
		}
	}
	catch (const std::exception& ex)
	{
		recordError(file, out.sha256, "records", ex.what());
		return std::nullopt;
	}
	return out;
}

void reportUnusedArchiveOverlays()
{
	if (balanceRoot().empty())
		return;
	const auto dir = fs::path(balanceRoot()) / "server" / "archive";
	std::error_code ec;
	if (!fs::is_directory(dir, ec) || ec)
		return;
	for (const auto& entry : fs::directory_iterator(dir, ec))
	{
		const auto name = entry.path().filename().string();
		if (name.empty() || name.front() == '.' || entry.path().extension() != ".json" || g_consumed.contains(name))
			continue;
		std::error_code fileEc;
		if (!entry.is_regular_file(fileEc) || fileEc)
			continue;
		std::string sha;
		try
		{
			sha = sha256Hex(readBytes(entry.path()));
		}
		catch (const std::exception&)
		{
		}
		recordError(name, sha, "", "The game server does not take an overlay of " + name + "; nothing was applied.");
	}
}

} // namespace forge
