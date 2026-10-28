#include <drogon/drogon.h>

#include <gimuserver/App.hpp>
#include <gimuserver/db/MigrationManager.hpp>
#include <gimuserver/drogon/GimuServer.hpp>
#include <gimuserver/gme/common/BalanceSync.hpp>
#include <gimuserver/gme/common/GameDay.hpp>

// The optional developer console is enabled by GIMU_ENABLE_DEBUG_CLI.
// Portable release presets disable it.
#ifdef BF_DEBUG_CLI
#include "DebugCli.hpp"
#endif

#include <filesystem>

#include "SqlProfile.hpp"

int main(int argc, char** argv)
{
    // Unbuffered stdout so log lines reach redirected files immediately -
    // otherwise a wedged/killed headless run loses everything still sitting
    // in the CRT block buffer and boot failures become undiagnosable.
    setvbuf(stdout, nullptr, _IONBF, 0);
    sqlprof::installIfRequested();

#ifdef BF_DEBUG_CLI
    // CLI-client mode: launched by the server to host the debug console window.
    // Must be checked before any Drogon / config setup so this process never
    // starts the web server or runs migrations.
    if (argc >= 3 && std::string(argv[1]) == "--debug-cli")
    {
        RunDebugCliClient(argv[2]);
        return 0;
    }
#endif

#ifdef _WIN32
    SetConsoleTitleW(L"GimuFrontier standalone server");
#endif

    // Config path resolution priority:
    //   1. Command-line argument (argv[1]) — explicit always wins.
    //   2. GIMU_DEFAULT_CONFIG_PATH — baked in at compile time for Debug
    //      builds so F5 from VS works without touching the launch config.
    //   3. Windows Release: config.json beside this executable.
    //      Other platforms: ./config.json in the caller's working directory.
#ifndef GIMU_DEFAULT_CONFIG_PATH
#define GIMU_DEFAULT_CONFIG_PATH "./config.json"
#endif
    std::filesystem::path configArg = (argc > 1) ? argv[1] : GIMU_DEFAULT_CONFIG_PATH;
#ifdef _WIN32
#ifndef _DEBUG
    if (argc <= 1)
    {
        wchar_t executable[32768];
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        if (length == 0 || length >= 32768) return 1;
        configArg = std::filesystem::path(executable).parent_path() / "config.json";
    }
#endif
#endif

    try
    {
        // Resolve to an absolute path so the chdir below works regardless of
        // what the current directory was at launch.
        auto absConfig = std::filesystem::absolute(configArg);

        // Change into the config file's directory.  Every path in config.json
        // is relative to its own location (mst_root, document_root, etc.) so
        // this makes the server portable without editing config.json.
        std::filesystem::current_path(absConfig.parent_path());

        drogon::app()
            .loadConfigFile(absConfig.filename().string())
            .registerBeginningAdvice([]() {
                auto db = drogon::app().getDbClient();
                // Wait for other processes' locks instead of failing at once.  Drogon
                // never rolls back a COMMIT that failed with SQLITE_BUSY, so without
                // this one outside reader (the hourly bf-backup, the admin tooling's
                // Accounts tab) overlapping a COMMIT leaves our single connection
                // stuck inside an open transaction until a restart.  The setting
                // lives on the connection, and SQLite has exactly one (config
                // connection_numbr 1), opened once for the life of the process.
                // 10 s, not 5.  Every game transaction takes the write lock with its
                // first statement (gme::newTransaction, ticket #72), the one
                // place SQLite waits this long for another writer; in WAL the
                // admin tooling and bf-daily-gift hold it for milliseconds.
                const auto busy = db->execSqlSync("PRAGMA busy_timeout = 10000;");
                LOG_INFO << "SQLite busy_timeout: "
                         << (busy.empty() ? std::string("?") : busy[0][0].as<std::string>()) << " ms";
                // WAL: a reader (bf-backup's copy, the admin tooling) no longer blocks our
                // writes and our writes never block it, so only another writer can make
                // a request wait.  In rollback-journal mode the hourly backup had to hold
                // the write lock for its whole copy, and every MissionEnd/UnitMix that
                // waited past busy_timeout failed ("database is locked").  The mode is
                // stored in the file, so it sticks (an older image opens it fine).  If
                // another process holds the file right now, SQLite keeps the old mode;
                // the next start tries again.
                const auto wal = db->execSqlSync("PRAGMA journal_mode = WAL;");
                const auto mode = wal.empty() ? std::string("?") : wal[0][0].as<std::string>();
                if (mode == "wal")
                    LOG_INFO << "SQLite journal_mode: wal";
                else
                    LOG_WARN << "SQLite journal_mode is still " << mode << "; WAL is retried at the next start";
                // Page cache for the one connection: 128 MiB, more than the whole save
                // (110 MB on 2026-10-20).  With the 2 MiB default every request read its
                // pages back from the OS (pread + copy, ~8% of the SQLite thread under load).
                db->execSqlSync("PRAGMA cache_size = -131072;");
                MigrationManager::RunMigrations(db);
                // The daily features' recorded day shifts (GameDay.hpp), read
                // before any request asks what day it is.
                gme::loadDayShifts(db);
                // Plugins (and so the unit table, balance overrides included)
                // are loaded before beginning advices run, and listeners only
                // open after them: owned units are re-curved before any
                // request can read them.  See BalanceSync.hpp.
                gme::syncUnitStatBasis(db);
                // Legacy SeedDefaultUnits/SeedDefaultTown removed: the
                // tutorial (CreateUser + UnitArchiver) provisions the user
                // under the fresh-DB model adopted from upstream dev.
#ifdef BF_DEBUG_CLI
                StartDebugCli();
#endif
            })
            .run();
    }
    catch (const std::exception& ex)
    {
        printf("Fatal exception during execution: %s\n", ex.what());
#ifdef _WIN32
        // Also write to the VS Output window so the message is visible
        // without needing to watch the console window.
        OutputDebugStringA("Fatal exception during execution: ");
        OutputDebugStringA(ex.what());
        OutputDebugStringA("\n");
#endif
        return 1;
    }

    drogon::HttpAppFramework::instance().getLoop()->queueInLoop([]()
        {
            drogon::HttpAppFramework::instance().quit();
    });

/*#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#endif*/
    return 0;
}
