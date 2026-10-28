#include "StarterPack.hpp"

#include "ForgeStatus.hpp"
#include "MstOverlay.hpp"
#include "ReadError.hpp"

#include <gimuserver/App.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/gme/common/GameDay.hpp>

#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <climits>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace forge
{

namespace
{

// starter.json as written.  Strict: an unknown or a missing key refuses the file.
struct RawStarterItem
{
	int64_t id = 0;
	int64_t count = 0;
};

struct RawStarterUnit
{
	int64_t id = 0;
};

struct RawStarter
{
	int64_t format = 0;
	int64_t gems = 0;
	int64_t zel = 0;
	int64_t karma = 0;
	std::vector<RawStarterItem> items;
	std::vector<RawStarterUnit> units;
};

struct StarterOpts : glz::opts
{
	bool validate_trailing_whitespace = true;
};

constexpr StarterOpts kStarterOpts = [] {
	StarterOpts opts{};
	opts.error_on_missing_keys = true;
	return opts;
}();

constexpr uintmax_t kMaxStarterBytes = 64u << 10;

} // namespace
} // namespace forge

template <> struct glz::meta<forge::RawStarterItem>
{
	using T = forge::RawStarterItem;
	static constexpr auto value = glz::object("id", &T::id, "count", &T::count);
};

template <> struct glz::meta<forge::RawStarterUnit>
{
	using T = forge::RawStarterUnit;
	static constexpr auto value = glz::object("id", &T::id);
};

template <> struct glz::meta<forge::RawStarter>
{
	using T = forge::RawStarter;
	static constexpr auto value = glz::object(
		"format", &T::format, "gems", &T::gems, "zel", &T::zel, "karma", &T::karma,
		"items", &T::items, "units", &T::units);
};

namespace forge
{

namespace
{

std::string withCommas(int64_t n)
{
	auto s = std::to_string(n);
	for (auto at = static_cast<int>(s.size()) - 3; at > 0; at -= 3)
		s.insert(static_cast<size_t>(at), ",");
	return s;
}

/*! Why a currency amount is refused, or empty. */
std::string amountProblem(const char* what, int64_t n, int64_t max)
{
	if (n < 0 || n > max)
		return std::string("The starter ") + what + " must be from 0 to " + withCommas(max) + ".";
	return {};
}

struct FileKey
{
	uint64_t ino = 0;
	uintmax_t size = 0;
	int64_t mtimeNs = 0;
	int64_t ctimeNs = 0;
	bool operator==(const FileKey&) const = default;
};

/*! The file's identity (a new inode on every atomic write), or nullopt when it is not a regular file. */
std::optional<FileKey> fileKey(const std::filesystem::path& path)
{
#ifndef _WIN32
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return std::nullopt;
	const auto ns = [](const timespec& t) { return static_cast<int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec; };
	return FileKey{ static_cast<uint64_t>(st.st_ino), static_cast<uintmax_t>(st.st_size), ns(st.st_mtim), ns(st.st_ctim) };
#else
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec) || ec)
		return std::nullopt;
	const auto size = std::filesystem::file_size(path, ec);
	const auto mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
	if (ec)
		return std::nullopt;
	return FileKey{ 0, size, static_cast<int64_t>(mtime.time_since_epoch().count()), 0 };
#endif
}

struct Live
{
	std::mutex mutex;
	bool looked = false;
	std::optional<FileKey> seen;
	StarterPack pack;
	StarterStatus status;
};

Live& live()
{
	static Live instance;
	return instance;
}

void report(Live& l)
{
	auto& s = l.status;
	s.engine = true;
	s.gems = l.pack.gems;
	s.zel = l.pack.zel;
	s.karma = l.pack.karma;
	s.items.clear();
	for (const auto& i : l.pack.items)
		s.items.emplace_back(i.id, i.count);
	s.units = l.pack.units;
	setStarterStatus(s);
}

bool unitInGame(int32_t id)
{
	// A unit present is claimed through the archive (gme::fromArchivedUnit).
	return UnitArchiver::instance().contains(static_cast<UnitArchiver::UnitId>(id));
}

bool itemInGame(int32_t id)
{
	for (const auto& i : theServer()->cache().itemMst())
		if (static_cast<int64_t>(i.id) == id)
			return true;
	return false;
}

/*! Called with l.mutex held: look at the file, re-read it when it changed. */
void refresh(Live& l)
{
	const auto& root = balanceRoot();
	const auto path = root.empty() ? std::filesystem::path() : std::filesystem::path(root) / "server" / "starter.json";
	const auto key = root.empty() ? std::nullopt : fileKey(path);
	if (l.looked && key == l.seen)
		return;
	l.looked = true;
	l.seen = key;
	l.status = StarterStatus{};
	l.status.loadedAt = gme::unixNow();
	l.pack = StarterPack{};
	if (!key)
	{
		LOG_INFO << "forge: no starter.json; the tutorial gives the server's own starter pack";
		return report(l);
	}
	l.status.present = true;
	const auto refuse = [&l](const std::string& why) {
		LOG_WARN << "forge: starter.json REJECTED: " << why << "; the server's own starter pack stays in force";
		l.status.error = why;
		report(l);
	};
	if (key->size > kMaxStarterBytes)
		return refuse("starter.json is " + std::to_string(key->size) + " bytes; at most 64 KiB is read.");
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return refuse("starter.json cannot be opened.");
	std::ostringstream bytes;
	bytes << in.rdbuf();
	const auto text = bytes.str();
	l.status.sha256 = sha256Hex(text);
	auto parsed = parseStarter(text, unitInGame, itemInGame);
	if (!parsed.pack)
		return refuse(parsed.error);
	l.pack = std::move(*parsed.pack);
	l.status.applied = true;
	LOG_INFO << "forge: starter.json taken: " << l.pack.gems << " gems, " << l.pack.zel << " zel, "
		<< l.pack.karma << " karma, " << l.pack.items.size() << " item line(s), " << l.pack.units.size() << " unit(s)";
	report(l);
}

} // namespace

StarterParse parseStarter(const std::string& text, const std::function<bool(int32_t)>& unitKnown,
	const std::function<bool(int32_t)>& itemKnown)
{
	StarterParse out;
	RawStarter raw{};
	if (const auto ec = glz::read<kStarterOpts>(raw, text))
	{
		out.error = "starter.json does not read: " + plainReadError(ec, text);
		return out;
	}
	if (raw.format != 1)
	{
		out.error = "starter.json must say \"format\": 1.";
		return out;
	}
	for (const auto& [what, n, max] : { std::tuple{ "gems", raw.gems, kStarterMaxGems },
		     std::tuple{ "zel", raw.zel, kStarterMaxCurrency }, std::tuple{ "karma", raw.karma, kStarterMaxCurrency } })
	{
		if (auto why = amountProblem(what, n, max); !why.empty())
		{
			out.error = why;
			return out;
		}
	}
	if (raw.items.size() > kStarterMaxItems)
	{
		out.error = "The starter pack has " + std::to_string(raw.items.size()) + " item lines; at most "
			+ std::to_string(kStarterMaxItems) + ".";
		return out;
	}
	if (raw.units.size() > kStarterMaxUnits)
	{
		out.error = "The starter pack has " + std::to_string(raw.units.size()) + " units; at most "
			+ std::to_string(kStarterMaxUnits) + ".";
		return out;
	}
	StarterPack pack;
	pack.gems = static_cast<int32_t>(raw.gems);
	pack.zel = static_cast<int32_t>(raw.zel);
	pack.karma = static_cast<int32_t>(raw.karma);
	pack.items.clear();
	std::set<int64_t> seen;
	for (const auto& i : raw.items)
	{
		if (i.id < 1 || i.id > INT32_MAX || !itemKnown(static_cast<int32_t>(i.id)))
		{
			out.error = "Item " + std::to_string(i.id) + " is not in the game.";
			return out;
		}
		if (!seen.insert(i.id).second)
		{
			out.error = "Item " + std::to_string(i.id) + " is listed twice; give one line a bigger count.";
			return out;
		}
		if (i.count < 1 || i.count > kStarterMaxItemCount)
		{
			out.error = "Item " + std::to_string(i.id) + " must count from 1 to "
				+ std::to_string(kStarterMaxItemCount) + ".";
			return out;
		}
		pack.items.push_back({ static_cast<int32_t>(i.id), static_cast<int32_t>(i.count) });
	}
	for (const auto& u : raw.units)
	{
		if (u.id < 1 || u.id > INT32_MAX || !unitKnown(static_cast<int32_t>(u.id)))
		{
			out.error = "Unit " + std::to_string(u.id) + " is not one the game server can give.";
			return out;
		}
		pack.units.push_back(static_cast<int32_t>(u.id));
	}
	if (pack.gems == 0 && pack.zel == 0 && pack.karma == 0 && pack.items.empty() && pack.units.empty())
	{
		out.error = "The starter pack gives nothing; the tutorial's last dialog promises gifts.";
		return out;
	}
	out.pack = std::move(pack);
	return out;
}

StarterPack starterPack()
{
	auto& l = live();
	std::lock_guard lock(l.mutex);
	refresh(l);
	return l.pack;
}

void loadStarterAtBoot()
{
	(void)starterPack();
}

} // namespace forge
