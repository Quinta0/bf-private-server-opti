#pragma once

#include <gimuserver/forge/ReadError.hpp>
#include <gimuserver/utils/JsonFile.hpp>

#include <glaze/glaze.hpp>

#include <optional>
#include <string>
#include <string_view>

/*!
* Balance Forge MST overlays: <balance_root>/server/mst/<deploy mst file>.
*
* The admin tooling writes full rows (or whole groups, or object keys) of one
* deploy/mst table; at boot every mst_root load goes through LoadMst, which
* merges them onto the image's table at JSON level, checks that every row
* still has exactly the columns and JSON types the base rows have, and then
* runs the same typed parse LoadJson does.  A file that fails any check is
* logged, reported in forgeStatus.php and skipped: the base table loads
* exactly as it does without the Forge, and boot continues.
*
* File format and the per-table keys: the Forge server design, section 1.
*/
namespace forge
{

/*! The configured balance root ("" = the Forge is off).  Set before the first LoadMst. */
void setBalanceRoot(std::string root);
const std::string& balanceRoot();

/*!
* The base text merged with server/mst/<file>, or nullopt when there is no
* overlay or it was refused (the caller then loads the base unchanged).
* Records the result in the forge status.  Never throws.  A table loaded twice
* (mission_mst) is merged once.
*/
std::optional<std::string> mergedMstText(std::string_view mstRoot, std::string_view file);

/*! The merged text parsed: the overlay is live. */
void markMstApplied(std::string_view file);

/*! The merged text did not parse: the base is loaded instead. */
void recordMstRejected(std::string_view file, const std::string& reason);

/*! End of boot: drops the merged texts kept for a second load. */
void finishMstLoads();

/*! End of boot: server/mst/*.json files no loader asked for get an error in the status. */
void reportUnusedMstOverlays();

/*!
* Drop-in for LoadJson<T>(mstRoot, file) at every mst_root load site.
* Without an overlay this IS LoadJson (same file, same parse, same errors).
*/
template <typename T>
T LoadMst(std::string_view mstRoot, std::string_view file)
{
	if (auto text = mergedMstText(mstRoot, file))
	{
		T obj{};
		// Same default options as LoadJson's read_file_json.
		const auto ec = glz::read<glz::opts{}>(obj, *text);
		if (!ec)
		{
			markMstApplied(file);
			return obj;
		}
		recordMstRejected(file, "After merging, the table no longer reads: " + plainReadError(ec, *text));
	}
	return LoadJson<T>(mstRoot, file);
}

} // namespace forge
