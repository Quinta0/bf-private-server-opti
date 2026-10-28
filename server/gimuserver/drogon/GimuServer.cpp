#include "App.hpp"
#include "GimuServer.hpp"

#include <gimuserver/archive/GachaArchiver.hpp>
#include <gimuserver/archive/MissionArchiver.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <gimuserver/gme/common/BraveSlots.hpp>
#include <gimuserver/gme/common/SummonerJournal.hpp>
#include <gimuserver/gme/common/Friends.hpp>
#include <gimuserver/gme/common/LoginCampaign.hpp>
#include <gimuserver/gme/common/SelectorRotation.hpp>
#include <gimuserver/gme/common/SummonRotation.hpp>
#include <gimuserver/gme/common/EventRotation.hpp>
#include <gimuserver/gme/common/UnitPick.hpp>
#include <gimuserver/gme/common/DailySpin.hpp>
#include <gimuserver/gme/common/MysteryChest.hpp>
#include <gimuserver/gme/common/Exchange.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/forge/ClearLog.hpp>
#include <gimuserver/forge/ForgeStatus.hpp>
#include <gimuserver/forge/MstOverlay.hpp>
#include <gimuserver/forge/Rules.hpp>
#include <gimuserver/forge/StarterPack.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

GimuServer::GimuServer() : m_dlc_error_log(), m_have_log(false), m_cache() {}

void GimuServer::initAndStart(const Json::Value& config)
{
	const auto& extralog = config["extra_log"];
	if (extralog["enable"].asBool())
	{
		std::string dir = extralog["directory"].asCString();
		std::string log_prefix = extralog["http_log_prefix"].asString();

		std::filesystem::path logPrefixPath(dir);
		logPrefixPath /= log_prefix;
		m_http_log_prefix = logPrefixPath.string();

		m_http_log_suffix = extralog["http_log_suffix"].asString();
		std::string dlcLogName = extralog["dlc_error_log_name"].asString();

		if (!std::filesystem::is_directory(dir))
		{
			if (!std::filesystem::create_directory(dir))
			{
				LOG_WARN << "Cannot create log directory!";
				m_have_log = false;
			}
			else
			{
				m_have_log = true;
			}
		}
		else
		{
			m_have_log = true;
		}

		if (m_have_log)
		{
			std::filesystem::path dlcPath(dir);
			dlcPath /= dlcLogName;
			m_dlc_error_log.open(dlcPath.string());
		}
	}

	const auto& server = config["server"];
	m_cache.Setup(server);
	UnitArchiver::instance().setup(server);
	// Balance overrides replaced unit rows in m_cache above; the archive keeps
	// its own Lv1 copy of those stats, so point it at the same numbers before
	// anything (the friend roster, selector rotation) reads it.
	{
		std::vector<UnitMst> tuned;
		for (const auto& [id, _] : m_cache.unitMstBeforeBalance())
		{
			const auto& units = m_cache.unitMst();
			const auto it = std::find_if(units.begin(), units.end(),
				[id](const UnitMst& u) { return u.id == id; });
			if (it != units.end())
				tuned.push_back(*it);
		}
		UnitArchiver::instance().applyBalanceStats(tuned);
	}
	MissionArchiver::instance().setup(server);
	GachaArchiver::instance().setup(server);
	// Daily Spin reward table.  Authored data rather than MST, so it lives in
	// the archive alongside the other curated tables (handbook §6.15 rule 3).
	gme::loadDailySpinArchive(server["archive_root"].asString());
	gme::loadMysteryChestArchive(server["archive_root"].asString());
	gme::loadBraveSlotArchive(server["archive_root"].asString());
	gme::loadSummonerJournalArchive(server["archive_root"].asString());
	// The friend roster needs the unit archive already set up: it sweeps the
	// id space for evolution-chain bases, so UnitArchiver::setup must run first.
	gme::loadFriendArchive(server["archive_root"].asString());
	gme::loadLoginCampaignArchive(server["archive_root"].asString());
	// Needs UnitArchiver::setup above: it sweeps the archive for 6-star units.
	gme::loadSelectorRotation();
	// Needs UnitArchiver and GachaArchiver: the 50-gem unit pick's pickers and
	// the level 999 race pick's (archive race_pick.json).
	gme::loadUnitPick(server["archive_root"].asString());
	// Needs GachaArchiver and the cache's static rail: the weekly featured banners.
	gme::loadSummonRotation(server["archive_root"].asString());
	// Needs the cache's mission -> dungeon index.  Before the exchange archive,
	// which merges the featured events' Bazaar offers in and validates them.
	gme::loadEventRotation(server["archive_root"].asString());
	gme::loadExchangeArchive(server["archive_root"].asString());

	// Balance Forge: every loader has had its chance at its overlay.  A file
	// in server/mst or server/archive that none of them read is reported in
	// forgeStatus.php, so a misnamed table never looks applied.
	forge::finishMstLoads();
	forge::reportUnusedMstOverlays();
	forge::reportUnusedArchiveOverlays();
	// rules.json is read here once (every table it is checked against is
	// loaded), then re-read live by forge::rules().
	forge::loadRulesAtBoot();
	// starter.json: checked against the unit archive and item_mst, both loaded.
	forge::loadStarterAtBoot();
	// The difficulty log's per-quest aggregates (forgeClears.php).
	forge::clears::loadAtBoot();
	forge::setBootInfo(forge::balanceRoot(), std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

void GimuServer::shutdown() {}
