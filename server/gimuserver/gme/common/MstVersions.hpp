#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The MST version/download channel -- `KeC10fuL` on InitializeResp.
//
// VersionInfoResponse::readParam @0x140649C hands every row this declares to
// DownloadMstFileList::add, and VersionInfo::isVersionUp @0x12BD5C4 decides
// whether to fetch it.  That test is `server != local` (cmp; cset ne), NOT
// `server > local`: ANY version that differs from the one the device saved
// triggers a download -- a lower one included (withdraw a table and every
// client re-fetches the baseline).  GameScene::requestMstFiles @0x1610F04
// fetches the parts, which DataMstManager caches under LocalState and reloads
// at every boot -- so a table delivered this way stays delivered without being
// resent.
//
// ⚠ SO A VERSION NUMBER MUST NEVER BE REUSED.  A device whose saved version
// equals the announced one never re-downloads, whatever the bytes are now.
// Every publish of a table needs a fresh number, monotonically increasing,
// and a revert is a NEW version carrying the old rows -- never a re-announce
// of an old number.
//
// It is the only road wide enough for the big tables: filenames carry a part
// index (`Ver<ver>_<fileKey>_<n>.dat`) and the largest table in the client's own
// cache is 23.7 MB across nine of them.
//
// ⚠ THE DANGEROUS FAILURE IS DECLARING A VERSION WE CANNOT SERVE.
// requestMstFiles runs during boot, so a table announced without its files on
// disk sends the client chasing a 404 at startup.  That is why nothing here is
// hardcoded: this reads the manifests tools/gen_mst_download.py writes BESIDE
// the files it generates, so a declaration cannot exist without the bytes.
// Deleting a table's folder withdraws the declaration; no code change needed.
//
// The per-table filename and decryption key are NOT sent -- the client holds
// them as constants in that 102 KB readParam chain.  tools/mst_download_map.py
// recovers all 133 pairs (127 verified by decrypting the client's own cache).

namespace gme
{

/*!
* What tools/gen_mst_download.py writes beside each generated table.
*
* At namespace scope deliberately: Glaze reflects an aggregate declared here,
* but a struct local to a function has no reflection and every read of it fails
* -- silently, because a failed parse is indistinguishable from "no manifest"
* unless it is logged.
*/
struct MstDownloadManifest
{
	std::string table;
	std::string file_key;
	int32_t version = 0;
	int32_t file_count = 0;
	int32_t record_num = 0;
};

/*!
* One table found under a content root, and which root it came from.
*/
struct StagedMstTable
{
	::VersionInfo row;
	std::filesystem::path root;
};

/*!
* Reads every servable `<root>/<TABLE>/manifest.json` under one content root.
*
* A folder without a readable manifest, or whose declared parts are not all
* present, is SKIPPED rather than announced -- the whole point of reading from
* disk is that the declaration and the bytes cannot drift apart.
*
* @param root Content directory to scan; must exist.
* @return One entry per servable table under root.
*/
inline std::vector<StagedMstTable> scanMstRoot(const std::filesystem::path& root)
{
	namespace fs = std::filesystem;
	std::vector<StagedMstTable> tables;

	// One error_code shared across every call was a real bug here: the value
	// persists until the next call overwrites it, so a single failed probe made
	// every later `if (ec || ...)` true and skipped the whole scan in silence.
	// Each check now gets its own, and the iteration error is checked once.
	std::error_code iterEc;
	auto it = fs::directory_iterator(root, iterEc);
	if (iterEc)
	{
		LOG_WARN << "mstVersions: cannot list " << root.string()
			<< " (" << iterEc.message() << "); offering nothing from it";
		return tables;
	}

	int32_t looked = 0;
	for (const auto& entry : it)
	{
		std::error_code dirEc;
		if (!entry.is_directory(dirEc) || dirEc)
			continue;

		const auto manifestPath = entry.path() / "manifest.json";
		std::error_code manEc;
		if (!fs::is_regular_file(manifestPath, manEc) || manEc)
			continue;
		++looked;

		std::string text;
		{
			std::ifstream in(manifestPath, std::ios::binary);
			if (!in)
				continue;
			text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		MstDownloadManifest manifest{};

		if (const auto err = glz::read<glz::opts{ .error_on_unknown_keys = false }>(manifest, text); err)
		{
			LOG_WARN << "mstVersions: " << manifestPath.string() << " is not readable as a manifest; skipping";
			continue;
		}
		if (manifest.table.empty() || manifest.file_key.empty()
			|| manifest.version <= 0 || manifest.file_count <= 0)
		{
			LOG_WARN << "mstVersions: " << manifestPath.string() << " is incomplete; skipping";
			continue;
		}

		// EVERY part must exist.  A half-generated table would otherwise be
		// announced and then 404 partway through the client's boot download.
		bool complete = true;
		for (int32_t i = 1; i <= manifest.file_count && complete; ++i)
		{
			const auto name = manifest.file_count == 1
				? "Ver" + std::to_string(manifest.version) + "_" + manifest.file_key + ".dat"
				: "Ver" + std::to_string(manifest.version) + "_" + manifest.file_key
					+ "_" + std::to_string(i) + ".dat";
			std::error_code partEc;
			if (!fs::is_regular_file(entry.path() / name, partEc) || partEc)
			{
				LOG_WARN << "mstVersions: " << manifest.table << " declares " << manifest.file_count
					<< " part(s) but " << name << " is missing; not offering this table";
				complete = false;
			}
		}
		if (!complete)
			continue;

		// The folder name is what the client fetches from, so a manifest whose
		// table disagrees with it would point the download at the wrong place.
		if (entry.path().filename().string() != manifest.table)
		{
			LOG_WARN << "mstVersions: " << manifestPath.string() << " names table "
				<< manifest.table << " but sits in " << entry.path().filename().string()
				<< "; skipping rather than misdirecting the download";
			continue;
		}

		::VersionInfo row{};
		row.id = manifest.table;
		row.description = {};          // not read by readParam
		row.version = manifest.version;
		row.file_count = manifest.file_count;
		row.record_num = manifest.record_num;
		tables.push_back({ std::move(row), root });
	}

	LOG_DEBUG << "mstVersions: " << looked << " manifest(s) under " << root.string()
		<< ", " << tables.size() << " servable";
	return tables;
}

/*!
* Every MST table this server is actually able to serve, as `KeC10fuL` rows.
*
* Scans `game_content/content/<TABLE>/manifest.json`, then -- when
* balance_root is configured -- `<balance_root>/content/<TABLE>/manifest.json`
* the same way.  A table staged in both is offered from balance_root: that is
* where the admin tooling publishes, and its version supersedes whatever
* the content mirror carries.
*
* Cheap enough to run per Initialize: it is one directory listing plus a small
* read per table, and it means a regenerated table takes effect on the next
* login instead of a restart.
*
* @return One row per servable table, empty when none are staged.
*/
inline std::vector<::VersionInfo> mstVersions()
{
	namespace fs = std::filesystem;
	std::vector<StagedMstTable> staged;

	std::error_code ec;

	// Drogon's getDocumentRoot() is not a reliable base here -- it can come back
	// empty or normalised differently from the config's "./game_content", and
	// the first cut of this silently scanned a path that did not exist and
	// declared nothing.  Try the candidates in order and say which one answered,
	// so "no tables offered" can never again be indistinguishable from "wrong
	// directory".
	fs::path root;
	for (const auto& base : {
		fs::path(drogon::app().getDocumentRoot()) / "content",
		fs::path("./game_content/content"),
		fs::path("game_content/content") })
	{
		if (!base.empty() && fs::is_directory(base, ec))
		{
			root = base;
			break;
		}
	}
	if (root.empty())
	{
		LOG_WARN << "mstVersions: no content directory found (tried the document "
			"root and ./game_content/content)";
	}
	else
	{
		LOG_DEBUG << "mstVersions: scanning " << root.string();
		staged = scanMstRoot(root);
	}

	// The balance overlay.  Absent root = feature off; an absent content/
	// under it is simply "nothing published yet".
	const auto& balanceRoot = theServer()->cache().serverConfig().balanceRoot;
	if (!balanceRoot.empty())
	{
		const auto balanceContent = fs::path(balanceRoot) / "content";
		std::error_code balEc;
		if (fs::is_directory(balanceContent, balEc) && !balEc)
		{
			for (auto& table : scanMstRoot(balanceContent))
			{
				const auto clash = std::find_if(staged.begin(), staged.end(),
					[&table](const StagedMstTable& t) { return t.row.id == table.row.id; });
				if (clash != staged.end())
				{
					LOG_INFO << "mstVersions: " << table.row.id << " v" << table.row.version
						<< " in " << balanceContent.string() << " supersedes v"
						<< clash->row.version << " in " << clash->root.string();
					*clash = std::move(table);
				}
				else
				{
					staged.push_back(std::move(table));
				}
			}
		}
	}

	std::vector<::VersionInfo> rows;
	rows.reserve(staged.size());
	for (auto& table : staged)
	{
		LOG_INFO << "mstVersions: offering " << table.row.id << " v" << table.row.version
			<< " (" << table.row.file_count << " part(s), " << table.row.record_num
			<< " row(s)) from " << table.root.string();
		rows.push_back(std::move(table.row));
	}

	LOG_INFO << "mstVersions: offering " << rows.size() << " table(s)";
	return rows;
}

} // namespace gme
