#pragma once

#include <json/json.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/*!
* The per-table checks an MST overlay must pass on top of the column checks
* (the Forge server design, section 1.4 rule 5): the things the game's own
* code assumes about these tables.
*/
namespace forge
{

/*!
* A mission reward list (mission_mst SiYs27Cj, and S2ChOKo6 with an optional
* leading daily:N): type:id:amount:0:0[:elgif skill], comma-separated, every
* type one the present box pays.  Why it does not parse, or nullopt.
*/
std::optional<std::string> rewardListError(const std::string& text, bool allowDaily);

/*!
* Why the merged table breaks one of the game's assumptions, or nullopt.
* `overlayRows` are the rows the file wrote, `addedIds` the keys it added,
* `merged` the whole table after the merge.
*/
std::optional<std::string> tableProblem(std::string_view table, const std::vector<const Json::Value*>& overlayRows,
	const std::set<std::string>& addedIds, const Json::Value& merged);

} // namespace forge
