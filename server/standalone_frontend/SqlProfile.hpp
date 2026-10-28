#pragma once
// Per-statement SQLite time, for load tests: set BF_SQL_PROFILE=<file> and every 10 s the
// file is rewritten with each statement's count and total ms, slowest first.  Off (and free)
// when the variable is unset.  Hooks every connection through sqlite3_auto_extension, so
// drogon needs no change.

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sqlprof
{
struct Entry
{
	uint64_t count = 0;
	uint64_t ns = 0;
};

inline std::mutex g_mu;
inline std::unordered_map<std::string, Entry> g_stats;

inline int onTrace(unsigned type, void*, void* stmt, void* elapsed)
{
	if (type != SQLITE_TRACE_PROFILE)
		return 0;
	const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
	const auto ns = static_cast<uint64_t>(*static_cast<sqlite3_int64*>(elapsed));
	std::lock_guard lock(g_mu);
	auto& e = g_stats[sql ? std::string(sql, std::min<size_t>(strlen(sql), 400)) : std::string("?")];
	e.count++;
	e.ns += ns;
	return 0;
}

inline int onOpen(sqlite3* db, const char**, const sqlite3_api_routines*)
{
	sqlite3_trace_v2(db, SQLITE_TRACE_PROFILE, onTrace, nullptr);
	return SQLITE_OK;
}

inline void installIfRequested()
{
	const char* path = std::getenv("BF_SQL_PROFILE");
	if (!path || !*path)
		return;
	sqlite3_auto_extension(reinterpret_cast<void (*)()>(onOpen));
	std::thread([out = std::string(path)]() {
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(10));
			std::vector<std::pair<std::string, Entry>> rows;
			{
				std::lock_guard lock(g_mu);
				rows.assign(g_stats.begin(), g_stats.end());
			}
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
			std::ofstream f(out, std::ios::trunc);
			for (const auto& [sql, e] : rows)
				f << e.count << '\t' << e.ns / 1000000.0 << '\t' << sql << '\n';
		}
	}).detach();
}
} // namespace sqlprof
