// Hand-built battles: <balance_root>/server/missions/<mission id>.json.
//
// Each file holds ONE MissionRecord, exactly the shape of one element of
// archive/mission.json, and replaces (or adds) the archive record with that id
// while it is there.  An external tool writes them, or an operator drops one
// in by hand.  The folder is re-read on the main loop every 2 s,
// so a publish goes live without a restart, and lookup()/archived() never do
// file IO: they read an immutable snapshot that a refresh swaps in whole.
//
// A file is served only after it passed every check MissionStart and
// MissionEnd depend on, ending with a dry run of the seven MissionStart
// blocks.  A file that fails keeps whatever was served before (the previous
// good version of that file, else the archive record) and is not re-read
// until its bytes change.  docs/features/BATTLE_BUILDER.md has the contract.

#include "MissionArchiver.hpp"

#include "UnitArchiver.hpp"

#include <gimuserver/App.hpp>
#include <gimuserver/forge/Tuning.hpp>

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#ifdef _WIN32
#include <fstream>
#include <iterator>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
namespace fs = std::filesystem;
using mission_overlay::FileKey;
using mission_overlay::Listing;
using Problem = std::optional<std::string>;

// Every pass lists the folder and stats each exactly-named file.  It does not
// trust the folder's own mtime to say "nothing changed": Docker Desktop's file
// sharing serves a stale folder mtime for good (measured 2026-10-18: still
// stale 10 s after an os.replace on the host), while per-file stats refresh
// within about a second.  This runs on the main loop, never on a request.
constexpr double kPollSeconds = 2.0;
// A bulk change (restoring a backup) is spread over several passes.
constexpr size_t kMaxParsesPerPass = 200;
constexpr uintmax_t kMaxFileBytes = 4u << 20;

constexpr size_t kMaxStages = 30;
constexpr size_t kMaxEnemiesPerWave = 6;          // the client's layouts stop at 6
constexpr uint32_t kMaxWireInt = INT32_MAX;       // hp, which the client reads as int32
constexpr size_t kMaxTreasureRows = 64;           // 64 rows x weight 1000 cannot wrap the u32 sum
constexpr uint32_t kMaxTreasureWeight = 1000;
// Chest amounts.  Zel/Karma/crystal chests add to the battle totals the client
// keeps in int32 and reports back (the zel_max_drop bound); an item chest
// spawns one pickup per unit and MissionEnd adds the count to an int32 stack.
// The archive's largest are 6000 Zel, 5000 Karma, 100 crystals and 1 item.
constexpr uint32_t kMaxTreasureAmount = 1'000'000;
constexpr uint32_t kMaxTreasureItems = 9'999;
constexpr uint32_t kMaxReward = 100'000'000;      // MissionEnd adds zel/karma in u32
constexpr uint32_t kMaxDropSize = 1'000'000;      // the client reports these back as Zel/Karma
constexpr uint32_t kMaxDropCount = 30;

// Never replaceable: the tutorial pair (scripted captures), the template
// every missing battle borrows, and the Battle Simulator sandbag.
constexpr std::array<uint32_t, 4> kReservedIds{ 1, 2, 10, 6000000 };

// Unknown keys (glaze's default), missing keys and bytes after the object are
// all errors: a typo must reject the file, never silently become a 0.
// std::optional members stay optional under error_on_missing_keys.
struct OverlayOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};
constexpr OverlayOpts kOverlayOpts = [] {
	OverlayOpts opts{};
	opts.error_on_missing_keys = true;
	return opts;
}();

// --- Platform: the only system calls (Windows falls back to std::filesystem) ---

#ifndef _WIN32
std::string errnoText(int err)
{
	return std::error_code(err, std::generic_category()).message();
}

int64_t nanos(const timespec& t)
{
	return static_cast<int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

FileKey keyOf(const struct stat& st)
{
	return { static_cast<uint64_t>(st.st_ino), static_cast<uintmax_t>(st.st_size),
		nanos(st.st_mtim), nanos(st.st_ctim) };
}

std::optional<FileKey> statKey(const fs::path& path)
{
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0)
		return std::nullopt;
	return keyOf(st);
}

// Closes the descriptor on every way out of readOverlayFile.
struct Descriptor
{
	int fd;
	~Descriptor() { ::close(fd); }
};

// Reads a file through ONE descriptor, so `key` describes exactly the bytes
// that were parsed.  nullopt with `why` empty: it vanished since the listing.
std::optional<std::string> readOverlayFile(const fs::path& path, FileKey& key, std::string& why)
{
	// O_NONBLOCK: a FIFO named like an overlay file must not park the main loop.
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
	{
		const int err = errno;
		if (err != ENOENT)
			why = "cannot open it: " + errnoText(err);
		return std::nullopt;
	}
	const Descriptor guard{ fd };
	struct stat st{};
	if (::fstat(fd, &st) != 0)
	{
		why = "cannot stat it: " + errnoText(errno);
		return std::nullopt;
	}
	key = keyOf(st);
	if (!S_ISREG(st.st_mode))
	{
		why = "not a regular file";
		return std::nullopt;
	}
	if (key.size > kMaxFileBytes)
	{
		why = std::format("{} bytes, at most {} are read", key.size, kMaxFileBytes);
		return std::nullopt;
	}

	std::string text(static_cast<size_t>(key.size), '\0');
	size_t got = 0;
	while (got < text.size())
	{
		const auto n = ::read(fd, text.data() + got, text.size() - got);
		const int err = errno;
		if (n < 0 && err == EINTR)
			continue;
		if (n < 0)
		{
			why = "cannot read it: " + errnoText(err);
			return std::nullopt;
		}
		if (n == 0)
			break;   // shrank while reading: its new key brings it back next pass
		got += static_cast<size_t>(n);
	}
	text.resize(got);
	return text;
}
#else
// Windows dev builds have no inode or ctime: size + mtime key the bytes.
std::optional<FileKey> statKey(const fs::path& path)
{
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	const auto mtime = ec ? fs::file_time_type{} : fs::last_write_time(path, ec);
	if (ec)
		return std::nullopt;
	return FileKey{ 0, size, static_cast<int64_t>(mtime.time_since_epoch().count()), 0 };
}

std::optional<std::string> readOverlayFile(const fs::path& path, FileKey& key, std::string& why)
{
	std::error_code ec;
	const auto listed = statKey(path);
	if (!listed)
		return std::nullopt;
	key = *listed;
	if (!fs::is_regular_file(path, ec) || key.size > kMaxFileBytes)
	{
		why = std::format("not a regular file of at most {} bytes", kMaxFileBytes);
		return std::nullopt;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		why = "cannot open it";
		return std::nullopt;
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
#endif

// --- Folder listing ---

// The mission id an exactly-named file serves: "<id>.json" with no sign and
// no leading zero, so 0100700.json can never race 100700.json for an id.
std::optional<uint32_t> overlayId(std::string_view name)
{
	constexpr std::string_view kExt = ".json";
	if (name.size() <= kExt.size() || !name.ends_with(kExt))
		return std::nullopt;
	const auto stem = name.substr(0, name.size() - kExt.size());
	uint32_t id = 0;
	const auto [end, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), id);
	if (ec != std::errc{} || end != stem.data() + stem.size() || std::to_string(id) != stem)
		return std::nullopt;
	return id;
}

// An entry's name for the stray report when path::string() refused it.
std::string unrepresentableName(const fs::path& name)
{
	try
	{
		const auto utf8 = name.u8string();
		return std::string(utf8.begin(), utf8.end()) + " (not in the system code page)";
	}
	catch (const std::exception&)
	{
		return "<a name that is not valid Unicode>";
	}
}

// Lists the folder with the error_code overloads: a missing folder is an
// empty listing, any other error abandons the pass.  Dotfiles (a
// writer's temp files are .<name>.XXXX) are skipped silently, strays reported.
Listing listOverlay(const fs::path& dir, std::set<std::string>& misnamed, std::string& error)
{
	Listing listing;
	std::error_code ec;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		// On Windows string() converts to the ANSI code page and throws for a
		// character it lacks (an emoji, CJK on cp1252).  Such a name can never
		// be <id>.json: report it as a stray instead of failing every pass.
		std::string name;
		try
		{
			name = it->path().filename().string();
		}
		catch (const std::exception&)
		{
			misnamed.insert(unrepresentableName(it->path().filename()));
			continue;
		}
		if (name.empty() || name.front() == '.')
			continue;
		if (!overlayId(name))
		{
			misnamed.insert(name);
			continue;
		}
		listing.emplace(name, statKey(it->path()));
	}
	if (ec && ec != std::errc::no_such_file_or_directory)
		error = std::format("cannot list {}: {}", dir.string(), ec.message());
	return listing;
}

// Whether any file is new, changed or gone since it was last read.  A file
// whose stat failed counts as unchanged this pass.
bool listingChanged(const Listing& listing, const std::map<std::string, FileKey>& seen)
{
	for (const auto& [name, key] : seen)
		if (!listing.contains(name))
			return true;
	for (const auto& [name, key] : listing)
	{
		const auto it = seen.find(name);
		if (key && (it == seen.end() || it->second != *key))
			return true;
	}
	return false;
}

// Warns about a misnamed file once, not on every pass, until it goes away.
void reportMisnamed(std::set<std::string> now, std::set<std::string>& reported)
{
	for (const auto& name : now)
		if (!reported.contains(name))
			LOG_WARN << "balance: ignoring server/missions/" << name
				<< " (only files named <mission id>.json are read)";
	reported = std::move(now);
}

// --- Record checks (no IO) ---

bool digits(std::string_view text)
{
	return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "x:y", the only shape BattleMonsterGroupMst.position takes.
bool validPosition(std::string_view position)
{
	const auto colon = position.find(':');
	return colon != std::string_view::npos && digits(position.substr(0, colon))
		&& digits(position.substr(colon + 1));
}

// A type-4 chest names an item the way MissionEnd re-parses it (dropNumber,
// then item_mst): a plain decimal id, no sign, no leading zero.
bool knownItem(const std::string& text, const mission_overlay::Refs& refs)
{
	uint32_t id = 0;
	const auto* last = text.data() + text.size();
	const auto [end, ec] = std::from_chars(text.data(), last, id);
	return ec == std::errc{} && end == last && digits(text) && text.front() != '0'
		&& refs.itemIds.contains(id);
}

// Static treasure rules: encodeTreasureDrop sums weights in u32 and
// RandomUInt(1, 0) throws when that wraps -- randomly, inside MissionStart.
Problem treasureProblem(const BattleMonster& monster, const mission_overlay::Refs& refs)
{
	if (monster.treasure_chest_chance > 100)
		return std::format("treasure_chest_chance {} is above 100", monster.treasure_chest_chance);
	if (monster.treasure_drops.size() > kMaxTreasureRows)
		return std::format("{} treasure rows, at most {}", monster.treasure_drops.size(), kMaxTreasureRows);
	for (const auto& drop : monster.treasure_drops)
	{
		if (drop.target_type < 1 || drop.target_type > 4)
			return std::format("treasure target_type {} (1-4)", drop.target_type);
		if (drop.weight < 1 || drop.weight > kMaxTreasureWeight)
			return std::format("treasure weight {} (1-{})", drop.weight, kMaxTreasureWeight);
		const auto maxAmount = drop.target_type == 4 ? kMaxTreasureItems : kMaxTreasureAmount;
		if (drop.amount < 1 || drop.amount > maxAmount)
			return std::format("treasure amount {} (1-{} for target_type {})", drop.amount, maxAmount,
				drop.target_type);
		if (drop.amount_max && (*drop.amount_max < drop.amount || *drop.amount_max > maxAmount))
			return std::format("treasure amount_max {} ({}-{} for target_type {})", *drop.amount_max,
				drop.amount, maxAmount, drop.target_type);
		if (drop.target_type == 4 && !knownItem(drop.target_id, refs))
			return std::format("treasure item \"{}\" is not in item_mst",
				std::string_view(drop.target_id).substr(0, 24));
	}
	return std::nullopt;
}

// passive_procs / passive_params (#162): what the client splits on '@' and ','.  Ids are whole
// numbers, params numbers; as many param groups as ids; a handful at most.
Problem passiveProblem(const BattleMonster& m)
{
	const auto ids = m.passive_procs.value_or("");
	const auto params = m.passive_params.value_or("");
	if (ids.empty() && params.empty())
		return std::nullopt;
	constexpr size_t kMaxPassives = 8, kMaxPassiveText = 400;
	if (ids.size() > kMaxPassiveText || params.size() > kMaxPassiveText)
		return std::format("passive_procs/params longer than {} characters", kMaxPassiveText);
	if (ids.empty() || !std::ranges::all_of(ids, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '@'; }))
		return std::format("passive_procs \"{}\" is not ids joined by @", std::string_view(ids).substr(0, 40));
	if (!std::ranges::all_of(params, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '@' || c == ',' || c == '-' || c == '.'; }))
		return std::format("passive_params \"{}\" has a character other than digits, -, ., ',' and @", std::string_view(params).substr(0, 40));
	const auto groups = [](const std::string& t) { return static_cast<size_t>(std::ranges::count(t, '@')) + 1; };
	if (groups(ids) > kMaxPassives)
		return std::format("more than {} passives", kMaxPassives);
	if (groups(ids) != groups(params))
		return std::format("{} passive ids but {} param groups", groups(ids), groups(params));
	return std::nullopt;
}

Problem monsterProblem(const BattleMonster& m, const mission_overlay::Refs& refs)
{
	if (m.id == 0)
		return "monster id 0";
	if (!validPosition(m.position))
		return std::format("position \"{}\" is not x:y", std::string_view(m.position).substr(0, 24));
	if (m.hp < 1 || m.hp > kMaxWireInt)
		return std::format("hp {} (1-{})", m.hp, kMaxWireInt);
	if (m.act_min > m.act_max)
		return std::format("act_min {} is above act_max {}", m.act_min, m.act_max);
	if (m.unit_drop_chance > 100)
		return std::format("unit_drop_chance {} is above 100", m.unit_drop_chance);
	if (m.unit_drop_type > 6)
		return std::format("unit_drop_type {} (0-6)", m.unit_drop_type);
	if (m.zel_max_drop > kMaxDropSize || m.karma_max_drop > kMaxDropSize)
		return std::format("zel/karma_max_drop above {}", kMaxDropSize);
	if (m.zel_drop_count > kMaxDropCount || m.karma_drop_count > kMaxDropCount)
		return std::format("zel/karma_drop_count above {}", kMaxDropCount);
	if (auto why = passiveProblem(m))
		return why;
	return treasureProblem(m, refs);
}

// Runs `check` over every monster the record carries -- waves, fixed and
// random mimics, scripted swaps -- and names the first one it refuses.
template <class Check>
Problem firstMonsterProblem(const MissionRecord& record, const Check& check)
{
	for (size_t s = 0; s < record.stages.size(); ++s)
		for (size_t i = 0; i < record.stages[s].battle_monsters.size(); ++i)
			if (auto p = check(record.stages[s].battle_monsters[i]))
				return std::format("wave {} enemy {} (monster {}): {}", s + 1, i + 1,
					record.stages[s].battle_monsters[i].id, *p);
	if (record.mimic_chests)
		for (size_t i = 0; i < record.mimic_chests->size(); ++i)
			if (auto p = check((*record.mimic_chests)[i].monster))
				return std::format("mimic chest {}: {}", i + 1, *p);
	if (record.random_mimics)
		for (size_t i = 0; i < record.random_mimics->monsters.size(); ++i)
			if (auto p = check(record.random_mimics->monsters[i]))
				return std::format("random mimic {}: {}", i + 1, *p);
	if (record.script_monsters)
		for (size_t i = 0; i < record.script_monsters->size(); ++i)
			if (auto p = check((*record.script_monsters)[i]))
				return std::format("script monster {}: {}", i + 1, *p);
	return std::nullopt;
}

Problem structureProblem(const MissionRecord& record, const mission_overlay::Refs& refs)
{
	if (record.stages.empty() || record.stages.size() > kMaxStages)
		return std::format("{} waves (1-{})", record.stages.size(), kMaxStages);
	for (size_t s = 0; s < record.stages.size(); ++s)
	{
		const auto& stage = record.stages[s];
		if (stage.battle_monsters.empty() || stage.battle_monsters.size() > kMaxEnemiesPerWave)
			return std::format("wave {} has {} enemies (1-{})", s + 1, stage.battle_monsters.size(),
				kMaxEnemiesPerWave);
		if (stage.first_attack_rate > 100)
			return std::format("wave {} first_attack_rate {} is above 100", s + 1, stage.first_attack_rate);
	}
	if (record.zel > kMaxReward || record.karma > kMaxReward || record.exp > kMaxReward)
		return std::format("zel/karma/exp above {}", kMaxReward);
	return firstMonsterProblem(record,
		[&refs](const BattleMonster& monster) { return monsterProblem(monster, refs); });
}

// MissionEnd re-reads a capture through fromArchivedUnit and unit_mst; a
// reference that cannot resolve throws there, after the energy was spent.
Problem captureProblem(const BattleMonster& m, const mission_overlay::Refs& refs)
{
	if (m.unit_drop_id == 0 || m.unit_drop_chance == 0)
		return std::nullopt;
	if (m.unit_drop_level < 1)
		return std::format("capture of unit {} at level 0", m.unit_drop_id);
	if (!UnitArchiver::instance().lookup(m.unit_drop_id))
		return std::format("capture unit {} is not in unit.json", m.unit_drop_id);
	const auto mst = refs.unitMaxLv.find(m.unit_drop_id);
	if (mst == refs.unitMaxLv.end() || mst->second < 1)
		return std::format("capture unit {} has no unit_mst row", m.unit_drop_id);
	return std::nullopt;
}

// The seven blocks MissionStart builds (Mission.cpp), into throwaway objects,
// from the record MissionStart would serve: an AI, unit, sprite, skill or
// mimic reference that cannot be served fails here instead of closing a
// player's session.  Each failing populatePacket logs its own ERROR first.
Problem dryRunProblem(const MissionRecord& served)
{
	MissionNumInfo missionNum{};
	std::vector<AiMst> ais;
	std::vector<MonsterMst> monsters;
	std::optional<std::vector<MonsterCgsMst>> monsterCgs;
	std::optional<std::vector<SkillMst>> battleSkills;
	std::vector<BattleMonsterGroupMst> monsterGroups;
	std::vector<BattleGroupMst> battleGroups;
	if (!MissionArchiver::populatePacket(served, missionNum))
		return "dry run: mission_num (mimic chests)";
	if (!MissionArchiver::populatePacket(served, ais))
		return "dry run: ais";
	if (!MissionArchiver::populatePacket(served, monsters))
		return "dry run: monsters";
	if (!MissionArchiver::populatePacket(served, monsterCgs))
		return "dry run: monster_cgs";
	if (!MissionArchiver::populatePacket(served, battleSkills))
		return "dry run: battle_skills";
	if (!MissionArchiver::populatePacket(served, monsterGroups))
		return "dry run: battle_monster_groups";
	if (!MissionArchiver::populatePacket(served, battleGroups))
		return "dry run: battle_groups";
	return std::nullopt;
}

std::set<uint32_t> scriptIds(const MissionRecord& record)
{
	std::set<uint32_t> ids;
	if (record.script_monsters)
		for (const auto& monster : *record.script_monsters)
			ids.insert(monster.id);
	return ids;
}

// glaze's message is "line:col: error", the context and a caret line: keep
// the first two, on one line.
std::string parseError(const glz::error_ctx& err, const std::string& text)
{
	const auto message = glz::format_error(err, text);
	const auto first = message.find('\n');
	if (first == std::string::npos)
		return message;
	const auto second = message.find('\n', first + 1);
	auto context = message.substr(first + 1, second == std::string::npos ? second : second - first - 1);
	context.erase(0, context.find_first_not_of(' '));
	return message.substr(0, first) + " near: " + context;
}

void addPass(mission_overlay::ScanResult& total, const mission_overlay::ScanResult& pass)
{
	total.accepted += pass.accepted;
	total.rejected += pass.rejected;
	total.removed += pass.removed;
}

mission_overlay::Refs buildRefs()
{
	const auto& cache = theServer()->cache();
	mission_overlay::Refs refs;
	for (const auto& row : cache.missionMst())
		refs.missionEnergy.emplace(static_cast<uint32_t>(row.id), row.energy_use);
	for (const auto& row : cache.itemMst())
		refs.itemIds.insert(static_cast<uint32_t>(row.id));
	// emplace keeps the first row, the one MissionEnd's find_if would use.
	for (const auto& row : cache.unitMst())
		refs.unitMaxLv.emplace(static_cast<uint32_t>(row.id), row.max_lv);
	return refs;
}
} // namespace

void MissionArchiver::setupOverlay()
{
	const auto& root = theServer()->cache().serverConfig().balanceRoot;
	if (root.empty())
		return;

	overlayDir_ = fs::path(root) / "server" / "missions";
	overlayRefs_ = buildRefs();
	// Every file before the first request, however many: a pass that hit
	// the per-pass cap is followed by another (bounded, in case files keep
	// changing under it).
	mission_overlay::ScanResult result;
	for (int pass = 0; pass < 100; ++pass)
	{
		const auto done = refreshOverlay();
		addPass(result, done);
		if (done.accepted + done.rejected < kMaxParsesPerPass)
			break;
	}
	const auto served = snapshot();
	const auto replaced = static_cast<size_t>(std::count_if(served->begin(), served->end(),
		[this](const auto& entry) { return missionCache_.contains(entry.first); }));
	LOG_INFO << "balance: " << served->size() << " mission record(s) from " << overlayDir_.string()
		<< " (" << replaced << " replace archive records, " << served->size() - replaced << " new), "
		<< result.rejected << " rejected";

	// On the main loop, not an IO loop: a slow folder never holds up a
	// request.  Plugins are set up before drogon's main loop starts; runEvery
	// is thread-safe and only arms the timer, which first fires once it runs.
	drogon::app().getLoop()->runEvery(kPollSeconds, [] { MissionArchiver::instance().refreshOverlay(); });
}

std::shared_ptr<const MissionArchiver::OverlayMap> MissionArchiver::snapshot() const
{
	std::lock_guard guard(snapshotMutex_);
	return overlay_;
}

mission_overlay::ScanResult MissionArchiver::refreshOverlay() noexcept
{
	// A timer callback must never throw; the next pass simply tries again.  An
	// unexpected failure is logged like a listing error, once until it changes
	// (the inner handler can throw too, hence the outer try).
	try
	{
		try
		{
			return scanOverlay();
		}
		catch (const std::exception& ex)
		{
			std::lock_guard scanGuard(scanMutex_);   // scanOverlay's lock is released by now
			reportScanError(std::format("reading {} failed: {}", overlayDir_.string(), ex.what()));
		}
	}
	catch (...)
	{
		LOG_ERROR << "balance: reading " << overlayDir_.string() << " failed";
	}
	return {};
}

mission_overlay::ScanResult MissionArchiver::scanOverlay()
{
	std::lock_guard scanGuard(scanMutex_);
	if (overlayDir_.empty())
		return {};

	std::string error;
	std::set<std::string> misnamed;
	const auto listing = listOverlay(overlayDir_, misnamed, error);
	if (!error.empty())
	{
		// Never read a failed listing as "every file was removed".
		reportScanError(error);
		return {};
	}
	reportMisnamed(std::move(misnamed), misnamed_);

	mission_overlay::ScanResult result;
	if (listingChanged(listing, seen_))
	{
		auto next = std::make_shared<OverlayMap>(*snapshot());   // copies pointers only
		const bool removed = forgetRemoved(listing, *next, result);
		const bool parsed = parseChanged(listing, *next, result);
		if (removed || parsed)
		{
			std::lock_guard publish(snapshotMutex_);
			overlay_ = std::move(next);
		}
		// Files left over (more than one pass parses, or one that vanished
		// mid-read) are still unseen, so the next pass picks them up.
	}
	// Only a pass that got this far ends a reported failure, so one that
	// throws after the listing is not re-reported every 2 s either.
	if (!lastScanError_.empty())
		LOG_INFO << "balance: " << overlayDir_.string() << " can be read again";
	lastScanError_.clear();
	return result;
}

bool MissionArchiver::forgetRemoved(const Listing& listing, OverlayMap& next,
	mission_overlay::ScanResult& result)
{
	bool changed = false;
	for (auto it = seen_.begin(); it != seen_.end();)
	{
		if (listing.contains(it->first))
		{
			++it;
			continue;
		}
		// Forget the last good version too: the file is the only source.
		const auto id = *overlayId(it->first);
		const bool wasServed = next.erase(id) > 0;
		changed = changed || wasServed;
		++result.removed;
		LOG_INFO << "balance: mission " << id << " file removed" << (missionCache_.contains(id)
			? "; serving the archive record"
			: wasServed ? "; it has no record now (MissionStart falls back to mission 10)" : "");
		it = seen_.erase(it);
	}
	return changed;
}

bool MissionArchiver::parseChanged(const Listing& listing, OverlayMap& next,
	mission_overlay::ScanResult& result)
{
	bool changed = false;
	size_t parsed = 0;
	for (const auto& [name, listed] : listing)
	{
		const auto seen = seen_.find(name);
		if (!listed || (seen != seen_.end() && seen->second == *listed))
			continue;   // unchanged, or its stat failed: neither re-read nor removed
		if (parsed++ == kMaxParsesPerPass)
			break;

		const auto id = *overlayId(name);
		auto key = *listed;
		std::string why;
		auto record = acceptFile(overlayDir_ / name, id, key, why);
		if (!record && why.empty())
			continue;   // vanished between the listing and the open
		seen_[name] = key;   // rejected files too: the same bytes are not re-read
		const bool inArchive = missionCache_.contains(id);
		if (!record)
		{
			++result.rejected;
			LOG_INFO << "balance: mission " << id << " rejected (" << why << "); "
				<< (next.contains(id) ? "keeping the previous version"
					: inArchive ? "serving the archive record" : "the mission has no record");
			continue;
		}
		next.insert_or_assign(id, std::move(record));
		changed = true;
		++result.accepted;
		LOG_INFO << "balance: mission " << id << " from server/missions/" << name
			<< (inArchive ? " (replaces the archive record)" : " (new mission)");
	}
	return changed;
}

MissionArchiver::RecordPtr MissionArchiver::acceptFile(const fs::path& path, MissionId stemId,
	FileKey& key, std::string& why) const
{
	const auto reject = [&why](std::string problem) {
		why = std::move(problem);
		return RecordPtr{};
	};
	try
	{
		auto text = readOverlayFile(path, key, why);
		if (!text)
			return nullptr;

		MissionRecord record{};
		if (const auto err = glz::read<kOverlayOpts>(record, *text); err)
			return reject("does not parse: " + parseError(err, *text));
		if (auto problem = identityProblem(record, stemId))
			return reject(std::move(*problem));
		if (auto problem = structureProblem(record, overlayRefs_))
			return reject(std::move(*problem));

		// Checked as MissionStart will serve it: with the capture floor, which MissionStart
		// applies per battle (tuning.json capture_floor_pct, #164).  Stored without it.
		auto checked = record;
		applyCaptureFloor(checked, forge::tuning().captureFloorPct);
		const auto capture = [this](const BattleMonster& monster) { return captureProblem(monster, overlayRefs_); };
		if (auto problem = firstMonsterProblem(checked, capture))
			return reject(std::move(*problem));

		auto served = checked;
		applyMimicPolicy(served);
		if (auto problem = dryRunProblem(served))
			return reject(std::move(*problem));

		// Stored without the mimic merge, exactly like the archive: lookup() adds it.
		return std::make_shared<const MissionRecord>(std::move(record));
	}
	catch (const std::exception& ex)
	{
		return reject(std::string("check failed: ") + ex.what());
	}
}

std::optional<std::string> MissionArchiver::identityProblem(const MissionRecord& record,
	MissionId stemId) const
{
	if (record.id != stemId)
		return std::format("its id is {} but the file is named {}.json", record.id, stemId);
	if (std::ranges::contains(kReservedIds, record.id))
		return "missions 1, 2, 10 and 6000000 (tutorial, fallback template, sandbag) cannot be replaced";
	const auto mst = overlayRefs_.missionEnergy.find(record.id);
	if (mst == overlayRefs_.missionEnergy.end())
		return "the id is not in mission_mst";
	// The quest screen and the client's start check use energy_use, MissionStart
	// charges energy_cost: a higher cost throws for a player in the gap (after a
	// Frontier Hunter orb was spent), a lower one undercharges.
	if (static_cast<int64_t>(record.energy_cost) != mst->second)
		return std::format("energy_cost {} but mission_mst energy_use is {}", record.energy_cost, mst->second);
	// The client's mission script swaps monsters in by id alone
	// (MonsterParty::changeMonster): dropping or renaming one crashes mid-battle.
	const auto base = missionCache_.find(record.id);
	if (base != missionCache_.end() && base->second.script_monsters
		&& scriptIds(base->second) != scriptIds(record))
		return "script_monsters must keep the archive record's scripted monster ids";
	return std::nullopt;
}

void MissionArchiver::reportScanError(const std::string& error)
{
	if (error != lastScanError_)
		LOG_ERROR << "balance: " << error << "; still serving the " << snapshot()->size()
			<< " hand-built mission(s) loaded before";
	lastScanError_ = error;
}
