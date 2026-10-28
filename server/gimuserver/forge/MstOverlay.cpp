#include "MstOverlay.hpp"

#include "ForgeStatus.hpp"
#include "JsonText.hpp"
#include "MstChecks.hpp"

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <climits>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace forge
{

namespace
{

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxOverlayBytes = 32u << 20;

std::string g_balanceRoot;
/*! server/mst files some loader asked for (merged, refused or rejected). */
std::set<std::string> g_consumed;
/*! file -> merged text (nullopt = no overlay / refused), so a second load merges nothing. */
std::map<std::string, std::optional<std::string>> g_texts;

/*! A check failed: the whole file is refused with this plain sentence. */
struct Refusal : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

// --- Tables the Forge edits: the server knows their key (design 1.3) ---

const std::map<std::string, std::vector<std::string>, std::less<>>& knownKeys()
{
	static const std::map<std::string, std::vector<std::string>, std::less<>> keys{
		{ "gacha_mst.json", { "7Ffmi96v" } },
		{ "gacha_category_mst.json", { "vx9uyQVQ" } },
		{ "mission_mst.json", { "j28VNcUW" } },
		{ "dungeon_mst.json", { "MHx05sXt" } },
		// NOT the first column: that is the parent area (sHMai2ZG).
		{ "area_mst.json", { "VjCY7rX4" } },
		{ "unit_evo_mst.json", { "pn16CNah" } },
		{ "achievement_trade_mst.json", { "Mdgsh04u" } },
		{ "guild_point_exchange_mst.json", { "Yxo3bEic" } },
		// NOT the first column either (that one is the Brave Points step).
		{ "daily_task_prize_mst.json", { "d83aQ39U" } },
		{ "login_campaign_reward_mst.json", { "H1Dkq93v", "n0He37p1" } },
		{ "item_mst.json", { "kixHbe54" } },
		{ "unit_selector_gacha_ticket_mst.json", { "XIvaD6Jp" } },
		{ "summon_ticket_v2_mst.json", { "b0D2iq2d" } },
	};
	return keys;
}

/*! Tables whose overlay is refused: another path owns them. */
std::optional<std::string> refusedTable(std::string_view file)
{
	if (file == "unit_mst.json")
		return "Unit rows go in server/unit_mst.json (the unit balance file), not server/mst/unit_mst.json; nothing was applied.";
	if (file == "version_info_mst.json")
		return "Client table versions come from content/<table>/manifest.json, not from version_info_mst.json; nothing was applied.";
	return std::nullopt;
}

/*! Rows the game's own code needs: they may be replaced, never deleted. */
bool isProtected(std::string_view table, const std::string& id)
{
	static const std::map<std::string, std::set<std::string>, std::less<>> rows{
		// the tutorial pair, the template every missing battle borrows, the Battle Simulator sandbag
		{ "mission_mst.json", { "1", "2", "10", "6000000" } },
		// Honor, the tutorial gate, the home banner's gate
		{ "gacha_mst.json", { "1000", "2000", "11960" } },
		// the home banner routes to tile 1; the tutorial taps tile 3's spot
		{ "gacha_category_mst.json", { "1", "3" } },
	};
	const auto it = rows.find(table);
	return it != rows.end() && it->second.contains(id);
}

// --- JSON kinds: the type check compares these with the base's ---

enum class Kind { Null, Bool, Int, Real, String, Array, Object };

Kind kindOf(const Json::Value& v)
{
	switch (v.type())
	{
	case Json::nullValue: return Kind::Null;
	case Json::intValue:
	case Json::uintValue: return Kind::Int;
	case Json::realValue: return Kind::Real;
	case Json::stringValue: return Kind::String;
	case Json::booleanValue: return Kind::Bool;
	case Json::arrayValue: return Kind::Array;
	case Json::objectValue: return Kind::Object;
	}
	return Kind::Null;
}

std::string kindName(Kind k)
{
	switch (k)
	{
	case Kind::Null: return "null";
	case Kind::Bool: return "true/false";
	case Kind::Int: return "a whole number";
	case Kind::Real: return "a decimal number";
	case Kind::String: return "text";
	case Kind::Array: return "a list";
	case Kind::Object: return "an object";
	}
	return "?";
}

/*! A whole number written where the base has decimals still reads; nothing else crosses kinds. */
bool kindAllowed(const std::set<Kind>& allowed, Kind k)
{
	return allowed.contains(k) || (k == Kind::Int && allowed.contains(Kind::Real));
}

std::string kindsText(const std::set<Kind>& kinds)
{
	std::string out;
	for (const auto k : kinds)
	{
		if (!out.empty())
			out += " or ";
		out += kindName(k);
	}
	return out;
}

std::string joined(const std::vector<std::string>& parts, std::string_view sep)
{
	std::string out;
	for (const auto& p : parts)
	{
		if (!out.empty())
			out += sep;
		out += p;
	}
	return out;
}

// --- Files ---

std::string readBytes(const fs::path& path)
{
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	if (ec)
		throw Refusal("The file could not be read: " + ec.message() + ".");
	if (size > kMaxOverlayBytes)
		throw Refusal("The file is larger than 32 MiB.");
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw Refusal("The file could not be opened.");
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

Json::Value parseStrict(const std::string& bytes)
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
		throw Refusal("The file is not valid JSON: " + errors);
	}
	return out;
}

Json::Value parseBase(const std::string& bytes, std::string_view file)
{
	Json::CharReaderBuilder builder;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	Json::Value out;
	std::string errors;
	if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &out, &errors))
		throw Refusal("The game's own " + std::string(file) + " could not be read, so nothing was merged.");
	return out;
}

// --- Rows: column profile of the base table ---

struct Profile
{
	std::set<std::set<std::string>> shapes;           //!< every distinct column set
	std::map<std::string, std::set<Kind>> kinds;     //!< column -> kinds the base uses
	std::set<std::string> columns;                   //!< every column any row has
};

std::set<std::string> columnsOf(const Json::Value& row)
{
	const auto names = row.getMemberNames();
	return { names.begin(), names.end() };
}

Profile profileOf(const Json::Value& rows, std::string_view table)
{
	Profile p;
	for (const auto& row : rows)
	{
		if (!row.isObject())
			throw Refusal("The game's own " + std::string(table) + " has a row that is not an object; it cannot take an overlay.");
		auto cols = columnsOf(row);
		for (const auto& c : cols)
		{
			p.kinds[c].insert(kindOf(row[c]));
			p.columns.insert(c);
		}
		p.shapes.insert(std::move(cols));
	}
	if (p.shapes.empty())
		throw Refusal("The game's own " + std::string(table) + " has no rows, so there is nothing to check an overlay row against.");
	return p;
}

/*! Same columns as some base row, every value of a kind the base uses for that column. */
void checkRow(const Json::Value& row, const Profile& p, const std::string& label)
{
	if (!row.isObject())
		throw Refusal(label + " is not an object.");
	const auto cols = columnsOf(row);
	if (!p.shapes.contains(cols))
	{
		std::vector<std::string> extra;
		for (const auto& c : cols)
			if (!p.columns.contains(c))
				extra.push_back(c);
		if (!extra.empty())
			throw Refusal(label + " has a column the table does not have: " + joined(extra, ", ") + ".");
		// The closest base shape names what is missing.
		const std::set<std::string>* nearest = nullptr;
		size_t best = SIZE_MAX;
		for (const auto& shape : p.shapes)
		{
			size_t diff = 0;
			for (const auto& c : shape)
				diff += !cols.contains(c);
			for (const auto& c : cols)
				diff += !shape.contains(c);
			if (diff < best)
			{
				best = diff;
				nearest = &shape;
			}
		}
		std::vector<std::string> missing;
		for (const auto& c : *nearest)
			if (!cols.contains(c))
				missing.push_back(c);
		if (!missing.empty())
			throw Refusal(label + " is missing a column the table's rows carry: " + joined(missing, ", ") + ".");
		throw Refusal(label + " carries a set of columns no row of the table has together.");
	}
	for (const auto& c : cols)
	{
		const auto& allowed = p.kinds.at(c);
		const auto k = kindOf(row[c]);
		if (!kindAllowed(allowed, k))
			throw Refusal(label + ": " + c + " must be " + kindsText(allowed)
				+ " like the table's own rows, not " + kindName(k) + ".");
	}
}

std::vector<std::string> stringList(const Json::Value& v, const std::string& what)
{
	if (!v.isArray() || v.empty())
		throw Refusal(what + " must be a non-empty list of column names.");
	std::vector<std::string> out;
	for (const auto& c : v)
	{
		if (!c.isString() || c.asString().empty())
			throw Refusal(what + " must be a non-empty list of column names.");
		if (std::find(out.begin(), out.end(), c.asString()) != out.end())
			throw Refusal(what + " names " + c.asString() + " twice.");
		out.push_back(c.asString());
	}
	return out;
}

/*! Map key of a tuple (kind-tagged, so "1" and 1 never collide). */
std::string tupleKey(const std::vector<const Json::Value*>& values)
{
	std::string out;
	for (const auto* v : values)
	{
		out += static_cast<char>('0' + static_cast<int>(kindOf(*v)));
		out += valueText(*v);
		out += '\x1f';
	}
	return out;
}

std::string tupleLabel(const std::vector<const Json::Value*>& values)
{
	std::vector<std::string> parts;
	for (const auto* v : values)
		parts.push_back(valueText(*v));
	return joined(parts, " / ");
}

std::vector<const Json::Value*> keyValues(const Json::Value& row, const std::vector<std::string>& key)
{
	std::vector<const Json::Value*> out;
	for (const auto& c : key)
		out.push_back(&row[c]);
	return out;
}

bool hasKey(const Json::Value& row, const std::vector<std::string>& key)
{
	return row.isObject() && std::all_of(key.begin(), key.end(), [&row](const std::string& c) { return row.isMember(c); });
}

/*! A delete entry / group key: a list of key values (a bare value when the key has one column). */
std::vector<const Json::Value*> tupleOf(const Json::Value& entry, const std::vector<std::string>& key,
	const Profile& p, const std::string& what)
{
	std::vector<const Json::Value*> values;
	if (entry.isArray())
	{
		for (const auto& v : entry)
			values.push_back(&v);
	}
	else if (key.size() == 1)
	{
		values.push_back(&entry);
	}
	if (values.size() != key.size())
		throw Refusal(what + " must list " + std::to_string(key.size()) + " key value(s): " + joined(key, ", ") + ".");
	for (size_t i = 0; i < key.size(); ++i)
	{
		const auto& allowed = p.kinds.at(key[i]);
		if (!kindAllowed(allowed, kindOf(*values[i])))
			throw Refusal(what + ": " + key[i] + " must be " + kindsText(allowed) + " like the table's own rows.");
	}
	return values;
}

// --- The three modes ---

struct Base
{
	Json::Value root;
	std::string wrapper;   //!< "" for an object table
};

Base splitBase(Json::Value root, std::string_view table)
{
	if (!root.isObject())
		throw Refusal("The game's own " + std::string(table) + " is not a JSON object; it cannot take an overlay.");
	Base base;
	if (root.size() == 1 && root[root.getMemberNames().front()].isArray())
		base.wrapper = root.getMemberNames().front();
	base.root = std::move(root);
	return base;
}

Json::Value mergeRows(const Base& base, const Json::Value& overlay, std::string_view table, MstStatus& status)
{
	const auto& rows = base.root[base.wrapper];
	const auto profile = profileOf(rows, table);

	const auto known = knownKeys().find(table);
	std::vector<std::string> key;
	if (overlay.isMember("key"))
		key = stringList(overlay["key"], "key");
	if (known != knownKeys().end())
	{
		if (key.empty())
			key = known->second;
		else if (key != known->second)
			throw Refusal("The key for " + std::string(table) + " is " + joined(known->second, " + ")
				+ ", not " + joined(key, " + ") + ".");
	}
	else if (key.empty())
	{
		throw Refusal("Say which column(s) identify a row: \"key\": [\"<column>\"].");
	}

	// The key must name a row of the base exactly once.
	std::unordered_map<std::string, size_t> index;
	for (Json::ArrayIndex i = 0; i < rows.size(); ++i)
	{
		if (!hasKey(rows[i], key))
			throw Refusal("Not every row of " + std::string(table) + " has " + joined(key, " + ") + "; use groups or another key.");
		if (!index.emplace(tupleKey(keyValues(rows[i], key)), i).second)
			throw Refusal("The key " + joined(key, " + ") + " is not unique in " + std::string(table) + "; use groups.");
	}

	const auto& list = overlay["rows"];
	if (!list.isArray())
		throw Refusal("rows must be a list of rows.");
	std::map<std::string, const Json::Value*> byKey;
	std::vector<std::string> order;
	std::vector<const Json::Value*> overlayRows;
	for (Json::ArrayIndex n = 0; n < list.size(); ++n)
	{
		const auto& row = list[n];
		if (!hasKey(row, key))
			throw Refusal("Row #" + std::to_string(n + 1) + " of the file has no " + joined(key, " + ") + ".");
		const auto values = keyValues(row, key);
		const auto label = "Row " + tupleLabel(values);
		checkRow(row, profile, label);
		const auto k = tupleKey(values);
		if (!byKey.emplace(k, &row).second)
			throw Refusal(label + " is listed twice.");
		order.push_back(k);
		overlayRows.push_back(&row);
	}

	std::set<std::string> deletes;
	if (overlay.isMember("delete"))
	{
		const auto& del = overlay["delete"];
		if (!del.isArray())
			throw Refusal("delete must be a list of key values.");
		for (const auto& entry : del)
		{
			const auto values = tupleOf(entry, key, profile, "A delete entry");
			const auto label = tupleLabel(values);
			const auto k = tupleKey(values);
			if (!index.contains(k))
				throw Refusal("Cannot delete row " + label + ": the table has no such row.");
			if (key.size() == 1 && isProtected(table, label))
				throw Refusal("Row " + label + " cannot be deleted: the game needs it.");
			if (byKey.contains(k))
				throw Refusal("Row " + label + " is both replaced and deleted.");
			if (!deletes.insert(k).second)
				throw Refusal("Row " + label + " is deleted twice.");
		}
	}

	Json::Value merged(Json::arrayValue);
	for (const auto& row : rows)
	{
		const auto k = tupleKey(keyValues(row, key));
		if (deletes.contains(k))
		{
			++status.rowsDeleted;
			continue;
		}
		if (const auto it = byKey.find(k); it != byKey.end())
		{
			merged.append(*it->second);
			++status.rowsReplaced;
			continue;
		}
		merged.append(row);
	}
	std::set<std::string> added;
	for (const auto& k : order)
	{
		if (index.contains(k))
			continue;
		const auto& row = *byKey.at(k);
		merged.append(row);
		added.insert(tupleLabel(keyValues(row, key)));
		++status.rowsAdded;
	}

	if (const auto why = tableProblem(table, overlayRows, added, merged))
		throw Refusal(*why);
	return merged;
}

Json::Value mergeGroups(const Base& base, const Json::Value& overlay, std::string_view table, MstStatus& status)
{
	if (const auto known = knownKeys().find(table); known != knownKeys().end())
		throw Refusal(std::string(table) + " takes rows by " + joined(known->second, " + ") + "; use rows, not groups.");

	const auto& rows = base.root[base.wrapper];
	const auto profile = profileOf(rows, table);
	if (!overlay.isMember("group_key"))
		throw Refusal("Say which column(s) name a group: \"group_key\": [\"<column>\"].");
	const auto key = stringList(overlay["group_key"], "group_key");
	for (const auto& row : rows)
		if (!hasKey(row, key))
			throw Refusal("Not every row of " + std::string(table) + " has " + joined(key, " + ") + ".");

	std::set<std::string> baseGroups;
	for (const auto& row : rows)
		baseGroups.insert(tupleKey(keyValues(row, key)));

	const auto& groups = overlay["groups"];
	if (!groups.isArray())
		throw Refusal("groups must be a list of {\"key\": [...], \"rows\": [...]}.");
	std::map<std::string, const Json::Value*> byKey;
	std::vector<std::string> order;
	std::vector<const Json::Value*> overlayRows;
	for (const auto& group : groups)
	{
		if (!group.isObject() || group.size() != 2 || !group.isMember("key") || !group.isMember("rows") || !group["rows"].isArray())
			throw Refusal("Every group must be exactly {\"key\": [...], \"rows\": [...]}.");
		const auto values = tupleOf(group["key"], key, profile, "A group key");
		const auto label = "Group " + tupleLabel(values);
		const auto k = tupleKey(values);
		if (!byKey.emplace(k, &group).second)
			throw Refusal(label + " is listed twice.");
		if (group["rows"].empty() && !baseGroups.contains(k))
			throw Refusal(label + " is not in the table, so it cannot be deleted.");
		order.push_back(k);
		Json::ArrayIndex n = 0;
		for (const auto& row : group["rows"])
		{
			const auto rowLabel = label + ", row #" + std::to_string(++n);
			checkRow(row, profile, rowLabel);
			if (tupleKey(keyValues(row, key)) != k)
				throw Refusal(rowLabel + " does not carry the group's key " + tupleLabel(values) + ".");
			overlayRows.push_back(&row);
		}
	}

	Json::Value merged(Json::arrayValue);
	std::set<std::string> emitted;
	for (const auto& row : rows)
	{
		const auto k = tupleKey(keyValues(row, key));
		const auto it = byKey.find(k);
		if (it == byKey.end())
		{
			merged.append(row);
			continue;
		}
		++status.rowsDeleted;
		if (!emitted.insert(k).second)
			continue;
		// The new rows take the place of the group's first old row.
		const auto& replacement = (*it->second)["rows"];
		for (const auto& r : replacement)
		{
			merged.append(r);
			++status.rowsAdded;
		}
		if (replacement.empty())
			++status.groupsDeleted;
		else
			++status.groupsReplaced;
	}
	for (const auto& k : order)
	{
		if (baseGroups.contains(k))
			continue;
		for (const auto& r : (*byKey.at(k))["rows"])
		{
			merged.append(r);
			++status.rowsAdded;
		}
		++status.groupsAdded;
	}

	if (const auto why = tableProblem(table, overlayRows, {}, merged))
		throw Refusal(*why);
	return merged;
}

Json::Value mergeObject(const Base& base, const Json::Value& overlay, MstStatus& status)
{
	const auto& values = overlay["object"];
	if (!values.isObject() || values.empty())
		throw Refusal("object must be a non-empty object of keys to replace.");
	Json::Value merged = base.root;
	for (const auto& name : values.getMemberNames())
	{
		if (!base.root.isMember(name))
			throw Refusal("The table has no key " + name + "; an object overlay only replaces existing keys.");
		const std::set<Kind> allowed{ kindOf(base.root[name]) };
		if (!kindAllowed(allowed, kindOf(values[name])))
			throw Refusal(name + " must be " + kindsText(allowed) + " like the table's own value, not "
				+ kindName(kindOf(values[name])) + ".");
		merged[name] = values[name];
		++status.rowsReplaced;
	}
	return merged;
}

/*! The merged table text; throws Refusal with a plain reason. */
std::string merge(const std::string& baseBytes, const std::string& overlayBytes, std::string_view table, MstStatus& status)
{
	const auto overlay = parseStrict(overlayBytes);
	if (!overlay.isObject())
		throw Refusal("The file must be a JSON object: {\"format\": 1, \"table\": ..., ...}.");
	static const std::set<std::string> allowedKeys{ "format", "table", "wrapper", "key", "rows", "delete", "group_key", "groups", "object" };
	for (const auto& name : overlay.getMemberNames())
		if (!allowedKeys.contains(name))
			throw Refusal("Unknown top-level key \"" + name + "\".");
	if (!overlay.isMember("format") || !overlay["format"].isInt() || overlay["format"].asInt() != 1)
		throw Refusal("format must be 1.");
	if (!overlay.isMember("table") || !overlay["table"].isString() || overlay["table"].asString() != table)
		throw Refusal("table must be \"" + std::string(table) + "\", the file's own name.");
	const int modes = overlay.isMember("rows") + overlay.isMember("groups") + overlay.isMember("object");
	if (modes != 1)
		throw Refusal("Use exactly one of rows, groups or object.");
	status.mode = overlay.isMember("rows") ? "rows" : overlay.isMember("groups") ? "groups" : "object";
	if ((overlay.isMember("key") || overlay.isMember("delete")) && status.mode != "rows")
		throw Refusal("key and delete belong to rows.");
	if (overlay.isMember("group_key") && status.mode != "groups")
		throw Refusal("group_key belongs to groups.");

	const auto base = splitBase(parseBase(baseBytes, table), table);
	if (overlay.isMember("wrapper"))
	{
		if (base.wrapper.empty())
			throw Refusal(std::string(table) + " is one object with no wrapper; leave wrapper out.");
		if (!overlay["wrapper"].isString() || overlay["wrapper"].asString() != base.wrapper)
			throw Refusal("wrapper must be " + base.wrapper + ", the table's own wrapper.");
	}

	Json::Value out;
	if (status.mode == "object")
	{
		if (!base.wrapper.empty())
			throw Refusal(std::string(table) + " is a list of rows; use rows or groups, not object.");
		out = mergeObject(base, overlay, status);
	}
	else
	{
		if (base.wrapper.empty())
			throw Refusal(std::string(table) + " is one object, not a list of rows; use object.");
		out = Json::Value(Json::objectValue);
		out[base.wrapper] = status.mode == "rows" ? mergeRows(base, overlay, table, status)
		                                          : mergeGroups(base, overlay, table, status);
	}
	return compactJson(out);
}

void logRejected(std::string_view file, const std::string& reason)
{
	LOG_ERROR << "forge: mst " << file << " REJECTED: " << reason;
}

} // namespace

void setBalanceRoot(std::string root)
{
	g_balanceRoot = std::move(root);
}

const std::string& balanceRoot()
{
	return g_balanceRoot;
}

std::optional<std::string> mergedMstText(std::string_view mstRoot, std::string_view file)
{
	if (g_balanceRoot.empty())
		return std::nullopt;
	const std::string name(file);
	if (const auto cached = g_texts.find(name); cached != g_texts.end())
		return cached->second;

	const auto path = fs::path(g_balanceRoot) / "server" / "mst" / name;
	std::error_code ec;
	if (!fs::is_regular_file(path, ec) || ec)
	{
		g_texts.emplace(name, std::nullopt);
		return std::nullopt;
	}
	g_consumed.insert(name);

	MstStatus status;
	std::optional<std::string> result;
	try
	{
		const auto overlayBytes = readBytes(path);
		status.sha256 = sha256Hex(overlayBytes);
		if (const auto refused = refusedTable(file))
			throw Refusal(*refused);
		const auto baseBytes = readBytes(fs::path(std::string(mstRoot)) / name);
		status.baseSha256 = sha256Hex(baseBytes);
		result = merge(baseBytes, overlayBytes, file, status);
	}
	catch (const Refusal& refusal)
	{
		status.applied = false;
		status.error = refusal.what();
		status.rowsReplaced = status.rowsAdded = status.rowsDeleted = 0;
		status.groupsReplaced = status.groupsAdded = status.groupsDeleted = 0;
		logRejected(file, status.error);
		result.reset();
	}
	catch (const std::exception& ex)
	{
		status.applied = false;
		status.error = std::string("The server could not merge this file: ") + ex.what();
		logRejected(file, status.error);
		result.reset();
	}
	setMstStatus(name, status);
	g_texts.emplace(name, result);
	return result;
}

void markMstApplied(std::string_view file)
{
	const std::string name(file);
	auto status = mstStatus(name);
	if (!status || status->applied)
		return;
	status->applied = true;
	status->error.clear();
	LOG_INFO << "forge: mst " << name << " applied: "
		<< (status->mode == "groups"
			? std::to_string(status->groupsReplaced) + " group(s) replaced, " + std::to_string(status->groupsAdded)
				+ " added, " + std::to_string(status->groupsDeleted) + " deleted"
			: std::to_string(status->rowsReplaced) + " replaced, " + std::to_string(status->rowsAdded) + " added, "
				+ std::to_string(status->rowsDeleted) + " deleted")
		<< " (sha256 " << status->sha256 << ")";
	setMstStatus(name, std::move(*status));
}

void recordMstRejected(std::string_view file, const std::string& reason)
{
	const std::string name(file);
	auto status = mstStatus(name).value_or(MstStatus{});
	status.applied = false;
	status.error = reason;
	status.rowsReplaced = status.rowsAdded = status.rowsDeleted = 0;
	status.groupsReplaced = status.groupsAdded = status.groupsDeleted = 0;
	logRejected(file, reason);
	setMstStatus(name, std::move(status));
	g_texts.insert_or_assign(name, std::nullopt);
}

void finishMstLoads()
{
	g_texts.clear();
}

void reportUnusedMstOverlays()
{
	if (g_balanceRoot.empty())
		return;
	const auto dir = fs::path(g_balanceRoot) / "server" / "mst";
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
		MstStatus status;
		try
		{
			status.sha256 = sha256Hex(readBytes(entry.path()));
		}
		catch (const std::exception&)
		{
		}
		status.error = name == "unit_mst_homebrew.json"
			? "unit_mst_homebrew.json is not one of the game's tables; nothing was applied."
			: "The game server does not read " + name + "; nothing was applied.";
		logRejected(name, status.error);
		setMstStatus(name, std::move(status));
	}
}

} // namespace forge
