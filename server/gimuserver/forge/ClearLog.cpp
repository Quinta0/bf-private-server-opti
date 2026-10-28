#include "ClearLog.hpp"

#include "App.hpp"
#include "JsonText.hpp"
#include "Rules.hpp"

#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/Relations.hpp>

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace forge::clears
{

namespace
{

/*! One file's cap; past it the file becomes .1 (replacing the old .1). */
constexpr std::uintmax_t kMaxBytes = 8u << 20;
/*! The medians read a mission's most recent runs only; the counts are exact. */
constexpr size_t kSamplesPerMission = 1000;

enum class Result { Clear, Fail, Abandon };

constexpr const char* resultName(const Result r)
{
	return r == Result::Clear ? "clear" : r == Result::Fail ? "fail" : "abandon";
}

struct Record
{
	int64_t at = 0;            //!< unix seconds the run finished
	int32_t mission = 0;
	Result result = Result::Clear;
	int64_t seconds = -1;      //!< start -> end; -1 unknown (abandons, a restart mid-battle)
	int64_t turns = 0;         //!< 0 unknown
	Strength squad;
	int32_t status = -1;       //!< MissionEnd j3g5P4cq of a fail
};

struct Sample
{
	bool clear = false;
	int64_t seconds = -1;
	int64_t turns = 0;
	int64_t atk = 0;           //!< 0 when the squad is unknown
	int64_t hp = 0;
};

struct Aggregate
{
	int64_t runs = 0, clears = 0, fails = 0, abandons = 0;
	int64_t since = 0, last = 0;
	std::map<int32_t, int64_t> topRarity;   //!< runs with a known squad, by its top rarity
	std::deque<Sample> samples;
};

struct OpenRun
{
	int32_t mission = 0;
	uint32_t serial = 0;
	std::chrono::steady_clock::time_point startedAt;
	Strength squad;
};

struct State
{
	std::mutex mutex;
	std::unordered_map<std::string, OpenRun> open;   //!< by user id; never leaves this file
	std::map<int32_t, Aggregate> missions;
	int64_t since = 0;
	int64_t records = 0;
	int64_t skippedLines = 0;
	std::string cached;
	bool dirty = true;
};

State& state()
{
	static State s;
	return s;
}

int64_t unixNow()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

const std::filesystem::path& logPath()
{
	static const std::filesystem::path path = []
	{
		if (const char* env = std::getenv("BF_FORGE_CLEARS_FILE"); env && *env)
			return std::filesystem::path(env);
		std::error_code ec;
		if (std::filesystem::is_directory("/data", ec))
			return std::filesystem::path("/data/forge-clears.jsonl");
		return std::filesystem::path("forge-clears.jsonl");
	}();
	return path;
}

std::filesystem::path rotatedPath()
{
	auto old = logPath();
	old += ".1";
	return old;
}

std::string lineOf(const Record& r)
{
	Json::Value v(Json::objectValue);
	v["t"] = static_cast<Json::Int64>(r.at);
	v["m"] = r.mission;
	v["r"] = resultName(r.result);
	if (r.seconds >= 0)
		v["s"] = static_cast<Json::Int64>(r.seconds);
	if (r.turns > 0)
		v["turns"] = static_cast<Json::Int64>(r.turns);
	if (r.squad.units > 0)
	{
		v["hp"] = static_cast<Json::Int64>(r.squad.hp);
		v["atk"] = static_cast<Json::Int64>(r.squad.atk);
		v["def"] = static_cast<Json::Int64>(r.squad.def);
		v["rec"] = static_cast<Json::Int64>(r.squad.rec);
		v["rar"] = r.squad.topRarity;
		v["n"] = r.squad.units;
	}
	if (r.status >= 0)
		v["st"] = r.status;
	return compactJson(v) + "\n";
}

int64_t whole(const Json::Value& v, const char* key, const int64_t fallback)
{
	const auto& field = v[key];
	return field.isInt64() ? field.asInt64() : fallback;
}

std::optional<Record> parseLine(const std::string& line)
{
	Json::CharReaderBuilder builder;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	Json::Value v;
	std::string errors;
	if (line.empty() || !reader->parse(line.data(), line.data() + line.size(), &v, &errors) || !v.isObject())
		return std::nullopt;
	Record r;
	r.at = whole(v, "t", 0);
	r.mission = static_cast<int32_t>(whole(v, "m", 0));
	const auto result = v["r"].isString() ? v["r"].asString() : std::string{};
	if (result == "clear")
		r.result = Result::Clear;
	else if (result == "fail")
		r.result = Result::Fail;
	else if (result == "abandon")
		r.result = Result::Abandon;
	else
		return std::nullopt;
	if (r.at <= 0 || r.mission <= 0)
		return std::nullopt;
	r.seconds = whole(v, "s", -1);
	r.turns = whole(v, "turns", 0);
	r.squad.units = static_cast<int32_t>(whole(v, "n", 0));
	r.squad.hp = whole(v, "hp", 0);
	r.squad.atk = whole(v, "atk", 0);
	r.squad.def = whole(v, "def", 0);
	r.squad.rec = whole(v, "rec", 0);
	r.squad.topRarity = static_cast<int32_t>(whole(v, "rar", 0));
	r.status = static_cast<int32_t>(whole(v, "st", -1));
	return r;
}

/*! Called with the mutex held. */
void fold(State& s, const Record& r)
{
	auto& a = s.missions[r.mission];
	++a.runs;
	if (r.result == Result::Clear)
		++a.clears;
	else if (r.result == Result::Fail)
		++a.fails;
	else
		++a.abandons;
	if (a.since == 0 || r.at < a.since)
		a.since = r.at;
	a.last = std::max(a.last, r.at);
	if (r.squad.units > 0)
		++a.topRarity[r.squad.topRarity];
	a.samples.push_back({ r.result == Result::Clear, r.seconds, r.turns,
		r.squad.units > 0 ? r.squad.atk : 0, r.squad.units > 0 ? r.squad.hp : 0 });
	if (a.samples.size() > kSamplesPerMission)
		a.samples.pop_front();
	if (s.since == 0 || r.at < s.since)
		s.since = r.at;
	++s.records;
	s.dirty = true;
}

/*! Called with the mutex held: fold it in, then append the line (rotating first when full). */
void append(State& s, const Record& r)
{
	fold(s, r);
	const auto line = lineOf(r);
	const auto& path = logPath();
	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (!ec && size + line.size() > kMaxBytes)
	{
		std::filesystem::rename(path, rotatedPath(), ec);
		if (ec)
			LOG_WARN << "forge clears: could not rotate " << path.string() << ": " << ec.message();
	}
	std::ofstream out(path, std::ios::app | std::ios::binary);
	out << line;
	out.flush();
	if (!out)
		LOG_WARN << "forge clears: could not append to " << path.string();
}

void loadFile(State& s, const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return;
	for (std::string line; std::getline(in, line);)
	{
		if (const auto r = parseLine(line))
			fold(s, *r);
		else if (!line.empty())
			++s.skippedLines;
	}
}

/*! Median of the values (the mean of the middle two for an even count); null when none. */
Json::Value median(std::vector<int64_t> v)
{
	if (v.empty())
		return Json::Value(Json::nullValue);
	std::sort(v.begin(), v.end());
	const auto n = v.size();
	if (n % 2 == 1)
		return static_cast<Json::Int64>(v[n / 2]);
	const auto sum = v[n / 2 - 1] + v[n / 2];
	if (sum % 2 == 0)
		return static_cast<Json::Int64>(sum / 2);
	return static_cast<double>(sum) / 2.0;
}

/*! Nearest-rank 90th percentile; null when none. */
Json::Value p90(std::vector<int64_t> v)
{
	if (v.empty())
		return Json::Value(Json::nullValue);
	std::sort(v.begin(), v.end());
	const auto rank = (v.size() * 9 + 9) / 10;   // ceil(0.9 n)
	return static_cast<Json::Int64>(v[rank - 1]);
}

Json::Value missionJson(const Aggregate& a)
{
	std::vector<int64_t> seconds, turns, atk, hp;
	for (const auto& sample : a.samples)
	{
		if (sample.clear && sample.seconds >= 0)
			seconds.push_back(sample.seconds);
		if (sample.clear && sample.turns > 0)
			turns.push_back(sample.turns);
		if (sample.atk > 0)
			atk.push_back(sample.atk);
		if (sample.hp > 0)
			hp.push_back(sample.hp);
	}
	Json::Value m(Json::objectValue);
	m["runs"] = static_cast<Json::Int64>(a.runs);
	m["clears"] = static_cast<Json::Int64>(a.clears);
	m["fails"] = static_cast<Json::Int64>(a.fails);
	m["abandons"] = static_cast<Json::Int64>(a.abandons);
	m["clear_seconds_median"] = median(seconds);
	m["clear_seconds_p90"] = p90(seconds);
	m["clear_turns_median"] = median(turns);
	m["squad_atk_median"] = median(atk);
	m["squad_hp_median"] = median(hp);
	Json::Value rarity(Json::objectValue);
	for (const auto& [r, count] : a.topRarity)
		rarity[std::to_string(r)] = static_cast<Json::Int64>(count);
	m["top_rarity"] = std::move(rarity);
	m["samples"] = static_cast<Json::Int64>(a.samples.size());
	m["since"] = static_cast<Json::Int64>(a.since);
	m["last"] = static_cast<Json::Int64>(a.last);
	return m;
}

void addUnit(Strength& s, const int32_t unitId, const int64_t hp, const int64_t atk, const int64_t def, const int64_t rec)
{
	s.hp += hp;
	s.atk += atk;
	s.def += def;
	s.rec += rec;
	++s.units;
	if (const auto* facts = unitFacts(unitId))
		s.topRarity = std::max(s.topRarity, facts->rarity);
}

void addRow(Strength& s, const drogon::orm::Row& row)
{
	addUnit(s, row["unit_id"].as<int32_t>(), row["hp"].as<int64_t>(), row["atk"].as<int64_t>(),
		row["def"].as<int64_t>(), row["rec"].as<int64_t>());
}

constexpr const char* kStats =
	"u.unit_id, u.base_hp + u.ext_hp AS hp, u.base_atk + u.ext_atk AS atk,"
	" u.base_def + u.ext_def AS def, u.base_rec + u.ext_rec AS rec";

} // namespace

drogon::Task<Strength> readStrength(const gme::UserIdentity identity, const int32_t missionId, const int32_t deckNum,
	const std::string rawHelperId, const std::string recordedHelper, const bool fixedParty)
{
	Strength out;
	if (fixedParty)
		co_return out;
	try
	{
		if (const auto trial = trialDeckRange(missionId))
		{
			// Strategy Zone reserves (member_type 2) wait on the bench: not summed.
			const auto rows = co_await theDb()->execSqlCoro(
				std::string("SELECT t.deck_num, ") + kStats + " FROM user_trial_decks t"
				" JOIN user_units u ON u.user_id = t.user_id AND u.user_unit_id = t.user_unit_id"
				" WHERE t.user_id = $1 AND t.deck_num BETWEEN $2 AND $3 AND t.user_unit_id > 0"
				" AND t.member_type != 2 ORDER BY t.deck_num, t.disp_order;",
				identity.userId, trial->first, trial->second);
			std::map<int32_t, Strength> byDeck;
			for (const auto& row : rows)
				addRow(byDeck[row["deck_num"].as<int32_t>()], row);
			for (const auto& [_, squad] : byDeck)
				if (squad.atk > out.atk || (squad.atk == out.atk && squad.hp > out.hp))
					out = squad;
		}
		else
		{
			const auto rows = co_await theDb()->execSqlCoro(
				std::string("SELECT ") + kStats + " FROM user_decks d"
				" JOIN user_units u ON u.user_id = d.user_id AND u.user_unit_id = d.user_unit_id"
				" WHERE d.user_id = $1 AND d.deck_type = 1 AND d.deck_num = $2 AND d.user_unit_id > 0;",
				identity.userId, deckNum);
			for (const auto& row : rows)
				addRow(out, row);
		}

		if (const auto own = gme::ownHelperUnitId(rawHelperId))
		{
			const auto rows = co_await theDb()->execSqlCoro(
				std::string("SELECT ") + kStats + " FROM user_units u WHERE u.user_id = $1 AND u.user_unit_id = $2;",
				identity.userId, *own);
			if (!rows.empty())
				addRow(out, rows[0]);
		}
		else if (gme::borrowedHelper(recordedHelper))
		{
			if (co_await gme::isRealPlayer(theDb(), recordedHelper))
			{
				if (const auto card = co_await gme::loadPlayerCard(theDb(), recordedHelper); card && card->unit_id != 0)
					addUnit(out, card->unit_id, card->base_hp + card->ext_hp, card->base_atk + card->ext_atk,
						card->base_def + card->ext_def, card->base_rec + card->ext_rec);
			}
			else if (const auto leader = co_await gme::helperLeader(theDb(), identity, recordedHelper); leader.unit_id != 0)
			{
				if (const auto* facts = unitFacts(leader.unit_id))
					addUnit(out, leader.unit_id, facts->maxHp, facts->maxAtk, facts->maxDef, facts->maxRec);
			}
		}
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_WARN << "forge clears: squad of " << identity.userId << " for quest " << missionId
			<< " not read: " << ex.base().what();
		co_return Strength{};
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "forge clears: squad of " << identity.userId << " for quest " << missionId
			<< " not read: " << ex.what();
		co_return Strength{};
	}
	co_return out;
}

void started(const std::string& userId, const int32_t missionId, const uint32_t serial, const Strength& strength)
{
	try
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		const auto it = s.open.find(userId);
		if (it != s.open.end())
		{
			// The same battle again (a resent start keeps its serial): still the one run.
			if (it->second.serial == serial)
				return;
			Record r;
			r.at = unixNow();
			r.mission = it->second.mission;
			r.result = Result::Abandon;
			r.squad = it->second.squad;
			append(s, r);
		}
		s.open[userId] = OpenRun{ missionId, serial, std::chrono::steady_clock::now(), strength };
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "forge clears: start of quest " << missionId << " not logged: " << ex.what();
	}
}

void ended(const std::string& userId, const int32_t missionId, const uint32_t serial, const int32_t status,
	const int64_t turns)
{
	try
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		Record r;
		r.at = unixNow();
		r.mission = missionId;
		r.result = status < 0 || status == 2 ? Result::Clear : Result::Fail;
		r.turns = std::max<int64_t>(turns, 0);
		if (r.result == Result::Fail)
			r.status = status;
		if (const auto it = s.open.find(userId); it != s.open.end() && it->second.serial == serial)
		{
			r.seconds = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - it->second.startedAt).count();
			r.squad = it->second.squad;
			s.open.erase(it);
		}
		append(s, r);
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "forge clears: end of quest " << missionId << " not logged: " << ex.what();
	}
}

void loadAtBoot()
{
	try
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		loadFile(s, rotatedPath());
		loadFile(s, logPath());
		LOG_INFO << "forge clears: " << s.records << " runs over " << s.missions.size() << " quests from "
			<< logPath().string() << (s.skippedLines > 0 ? " (" + std::to_string(s.skippedLines) + " unreadable lines skipped)" : "");
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "forge clears: the log could not be read: " << ex.what();
	}
}

std::string aggregatesJson()
{
	auto& s = state();
	std::lock_guard lock(s.mutex);
	if (!s.dirty)
		return s.cached;
	Json::Value out(Json::objectValue);
	out["format"] = 1;
	out["since"] = s.since > 0 ? Json::Value(static_cast<Json::Int64>(s.since)) : Json::Value(Json::nullValue);
	out["runs"] = static_cast<Json::Int64>(s.records);
	out["samples_per_quest"] = static_cast<Json::Int64>(kSamplesPerMission);
	Json::Value missions(Json::objectValue);
	for (const auto& [id, a] : s.missions)
		missions[std::to_string(id)] = missionJson(a);
	out["missions"] = std::move(missions);
	s.cached = compactJson(out);
	s.dirty = false;
	return s.cached;
}

} // namespace forge::clears
