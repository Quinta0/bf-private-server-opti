#include "App.hpp"
#include "ServerCache.hpp"
#include "ServerCacheMst.hpp"

#include <gimuserver/forge/MstOverlay.hpp>
#include <gimuserver/forge/RepeatClear.hpp>
#include <gimuserver/utils/BfCrypt.hpp>
#include <gimuserver/utils/JsonFile.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <vector>

// Friends added to every UserLevelMst row at load (see the progression load).
static constexpr int32_t kBonusFriendSlots = 20;

// Gem price of an SP-enhancement reset (DefineMst reset_fe_skill_dia_count; 1 in the data).
static constexpr int32_t kResetFeSkillGems = 0;

// Vortex topology bounds, used to collect m_vortexPermits at boot.
//
// Land 99 is the catch-all "special content" land shared by Vortex, Frontier
// Hunter (area 1000000+), Frontier Gate (3000000+) and Grand Quest (5000000+);
// the Vortex block itself is the low area range.  Verified against
// deploy/mst/area_mst.json: 88 land-99 areas sit below 200000 (100000
// "Gathering of Souls" .. 101800 "In the Name of Thunder").
//
// ⚠ THE RANGE IS ONLY THE TOP OF THE VORTEX.  Four of its tiles are
// categories -- 100002 Travellers, 100003 Vortex Trials, 100004/100005
// Seasons Past -- and 74 of their child areas are numbered far above the cut
// (700000 "A Dark Ritual", 800145 "Burden of Aesys", 25600001 "Full Metal
// Hare").  The client draws a category's children from AreaMst alone, so they
// showed; tapping one found no permit and dropped the player back on the
// Vortex list (ticket #19).  So a land-99 area also counts as Vortex
// when a parent_area_id chain leads into the range -- see the boot block that
// fills nestedVortexAreas.  A plain range cannot do it: the children sit
// between and above Frontier Hunter, Frontier Gate and Grand Quest.
static constexpr int32_t kVortexLandId = 99;
static constexpr int32_t kVortexAreaIdMax = 200000;

// Above this, an id belongs to event/special content rather than the Grand
// Gaia quest line.  Same floor UserInfo's PermitPlace uses for its dense
// ranges; see kSpecialIdFloor there.
static constexpr int32_t kQuestSpecialIdFloor = 100000;

// Parade Garden, and the display_order that floats it to the top of the Vortex
// tile list.
//
// It is where the evolution, sale and enhancing units come from, so it is the
// tile a player wants first, and its shipped order (100) buries it at the
// bottom.
//
// ⚠ THE VORTEX LIST IS SORTED DESCENDING — HIGHEST display_order FIRST.
// The tile order comes from AreaSPSelectScene::setDungeonList, which builds its
// OWN pair<dispOrder, AreaMst*> vector and sorts it at 0x183F2D4 with
// std::__sort<std::greater<...>>.  Do NOT reach for AreaMstList::getActiveList
// @0x1309EDC to answer this: that one sorts with std::__less and is ASCENDING,
// it is a different consumer, and trusting it put this constant at 1 and Parade
// Garden dead last.  99999 is above every shipped land-99 row (the highest is
// 9800, "Year-End Dungeon").
// "Trial of the Gods" — dungeon and area share the id.
// THE TRIAL DUNGEONS.  All above kSpecialIdFloor, so the Grand Gaia
// progression gate never reaches them and nothing else permits them.
//   800051  Trial of the Gods  -- the six elemental Trials
//   2000000 Trial Test         -- Mock Unit / EX / X trials (none built yet)
//   2100000 Strategy Zone      -- the Main Trials, incl. 2100004 The Creation
//                                 God, the Maxwell trial
// Listing a dungeon here does NOT advertise its missions: PermitPlace adds only
// the ones MissionArchiver has a battle for, so an unbuilt trial stays off the
// map and the list can name a whole series before any of it is authored.
static constexpr int32_t kTrialDungeonIds[] = { 800051, 2000000, 2100000 };
static constexpr int32_t kParadeGardenAreaId = 100600;
static constexpr int32_t kParadeGardenDispOrder = 99999;

// Where the CDN serves area/dungeon banner art from.  Relative because main()
// chdirs into the config file's directory before Drogon starts, and every other
// path in config.json (document_root, mst_root, archive_root) is relative the
// same way.
static constexpr std::string_view kBannerDir = "./game_content/content/dungeon/";

/*!
* An English banner to use in place of a Japanese one, when the set ships both.
*
* Three tiles reach the player with Japanese burnt into the artwork even though
* their MST names are English — "Mysterious Paradise" draws 神秘の楽園,
* "Ultimate Paradise" draws 究極の楽園, "Heavenly Paradise" draws 天上の楽園.
* The global build shipped translated art for exactly these three next to the
* originals (sp_quest_banner_frog{,3,_crystal}_EN.png), so the fix is to point
* at it rather than to drop three of the frog dungeons the player wants.
*
* Probed on disk rather than listed, so art added later is picked up.
*
* @param banner Banner filename from the MST.
* @return The _EN filename when one exists, otherwise banner unchanged.
*/
static std::string LocalisedBanner(const std::string& banner)
{
	if (!banner.ends_with(".png"))
		return banner;

	auto localised = banner.substr(0, banner.size() - 4) + "_EN.png";
	std::error_code ec;
	if (std::filesystem::exists(std::string(kBannerDir) + localised, ec))
		return localised;

	return banner;
}

/*!
* Whether a name is displayable text rather than Japanese or mojibake.
*
* Two shapes have to go.  Some rows are honestly Japanese ("天上の楽園",
* "禁断の石版"); others are Shift-JIS that was decoded as Latin somewhere
* upstream and reached the MST as literal question marks ("?X?g???C???C?Y").
* Neither renders as anything a player can read, and the localisation keys we
* DO want ("MST_DUNGEONS_DUNGEON_100600_NAME") are plain ASCII, so a
* non-ASCII byte or a run of question marks is enough to tell them apart.
*/
static bool IsDisplayableName(std::string_view name)
{
	int questionRun = 0;
	for (const auto ch : name)
	{
		if (static_cast<unsigned char>(ch) > 0x7F)
			return false;
		questionRun = (ch == '?') ? questionRun + 1 : 0;
		if (questionRun >= 3)
			return false;
	}

	return true;
}

/*!
* Weekday mask for a Vortex dungeon, from its banner filename.
*
* Bit 0 = Monday .. bit 6 = Sunday; 0 means "no weekday token", i.e. the
* dungeon is not part of the rotation and stays permanently open.
*
* The banner is the ONLY day signal the shipped data carries — DungeonMst has
* no day column.  It arrives in the field the KDL calls `room_asset`
* (hash 21VKZo0E), which is a misnomer: every Vortex row holds an
* `sp_quest_banner_*.png`, not a room asset.  Left alone rather than renamed
* because renaming means regenerating all.hpp through packet-generator.
*
* Real values are `sp_quest_banner_monday.png`, `..._monday2.png`,
* `..._thursday2.png`, `..._weekend2.png` and friends, so the day token is
* matched as a prefix and any trailing revision digit ignored.  Verified
* against deploy/mst/dungeon_mst.json: of the 110 Vortex dungeons exactly 7
* match, and no non-weekday banner begins with a weekday token.
*/
static uint8_t VortexDayMaskFromBanner(std::string_view banner)
{
	static constexpr std::string_view kPrefix = "sp_quest_banner_";
	if (!banner.starts_with(kPrefix))
		return 0;
	banner.remove_prefix(kPrefix.size());

	struct DayToken { std::string_view token; uint8_t mask; };
	static constexpr DayToken kDays[] = {
		{ "monday",    1 << 0 },
		{ "tuesday",   1 << 1 },
		{ "wednesday", 1 << 2 },
		{ "thursday",  1 << 3 },
		{ "friday",    1 << 4 },
		{ "saturday",  1 << 5 },
		{ "sunday",    1 << 6 },
		// The shipped data has no saturday/sunday banners; the weekend is a
		// single dungeon carrying this token.
		{ "weekend",   (1 << 5) | (1 << 6) },
	};

	for (const auto& [token, mask] : kDays)
		if (banner.starts_with(token))
			return mask;

	return 0;
}

/*!
* Builds a JSON
* @param[in] d Template class to write
* @return Parsed json string
*/
template <typename T>
static std::string BuildJson(const T& d)
{
	std::string buffer{};
	const auto& ec = glz::write_json(d, buffer);
	if (ec)
	{
		throw std::runtime_error("Cannot build a cache JSON, error:\n{}" + glz::format_error(ec, buffer));
	}

	return buffer;
}

void ServerCache::Setup(const Json::Value& serverObj)
{
	const auto& mstRoot = serverObj["mst_root"].asString();
	// Not every file the server loads at boot is a decoded MST.  features/
	// brave_slots/notice_info are server config and response fixtures, so they
	// live in system_root while mst_root holds only reference tables.
	const auto& systemRoot = serverObj.get("system_root", "./system").asString();

	// fps_cap is the client-side render cap delivered to the offline-proxy
	// libcurl shim via the /offline_mod/fps_cap endpoint. Default 60 (matches
	// the proxy's compile-time default); 0 disables the cap entirely.
	m_serverConfig.fpsCap = serverObj.get("fps_cap", 60u).asUInt();

	// See ServerConfig::vortexWeekendOpensAll — off by default because the
	// shipped MST gives the weekend its own dungeon rather than opening the
	// weekday ones.
	m_serverConfig.vortexWeekendOpensAll = serverObj.get("vortex_weekend_opens_all", false).asBool();
	m_serverConfig.vortexAlwaysOpenDungeons = { 100000, 100050, 100100, 100300 };
	if (serverObj.isMember("vortex_always_open_dungeons"))
	{
		m_serverConfig.vortexAlwaysOpenDungeons.clear();
		for (const auto& id : serverObj["vortex_always_open_dungeons"])
			m_serverConfig.vortexAlwaysOpenDungeons.insert(id.asInt());
	}
	m_serverConfig.requireSessionToken = serverObj.get("require_session_token", true).asBool();

	// Account transfer codes -- see ServerConfig for each field.
	m_serverConfig.transferCodeTtlDays = serverObj.get("transfer_code_ttl_days", 30u).asUInt();
	m_serverConfig.transferPbkdf2Iterations =
		std::max(100000u, serverObj.get("transfer_pbkdf2_iterations", 600000u).asUInt());
	m_serverConfig.transferClaimTtlSeconds = serverObj.get("transfer_claim_ttl_seconds", 86400u).asUInt();

	// Unit-balance overlay root; empty = feature off.  See ServerConfig.
	m_serverConfig.balanceRoot = serverObj.get("balance_root", "").asString();
	if (!m_serverConfig.balanceRoot.empty())
		LOG_INFO << "balance: overlay root " << m_serverConfig.balanceRoot;
	// Every mst_root table below loads through forge::LoadMst, which merges
	// <balance_root>/server/mst/<file> when the Forge wrote one (and loads
	// exactly what LoadJson would when it did not, or when the file is bad).
	// Merging here, before every derived index and cached reply is built, is
	// what lets an overlay reach all of them with no further code.
	forge::setBalanceRoot(m_serverConfig.balanceRoot);

	{
		GameDls dls{};
		// The bind address (e.g. 0.0.0.0:9960 in a container) is not something
		// the client can dial, so let the config name the host[:port] the client
		// should use.  The Android client prefixes each with "http(s)://" itself:
		// no scheme, no trailing slash ("gumilive" alone must end with '/').
		dls.game_ip = serverObj.get("public_host", GetDrogonBindHostname()).asString();
		dls.resource_ip = serverObj.get("resource_host", dls.game_ip).asString();
		dls.version = serverObj["game_version"].asUInt();
		dls.gumilive_ip = dls.game_ip + "/";
		dls.bg_image = serverObj["wallpaper_banner"].asString();

		const auto dlsJson = BuildJson(dls);
		const auto sree = BfCrypt::BuildSREE(dlsJson);

		// build SREE crypted JSON rather than constructing it everytime in the dls controller
		if (sree.has_value())
		{
			m_dls = BuildJson(sree.value());
		}
		else
		{
			throw std::runtime_error("Cannot encrypt SREE cache json");
		}
	}

	m_feature = LoadJson<FeatureCheck>(systemRoot, "features.json");
	m_controlCenterRsp = LoadJson<SlotGameInfoR>(systemRoot, "brave_slots.json");

	{
		// Cache: Initialize response
		m_initrsp.login_campagin = forge::LoadMst<LoginCampaignMst>(mstRoot, "login_campaign_mst.json");
		// "CAMPAIGN ENDS IN" is start_date + total_days days, minus now
		// (LoginCampaignScene::setMainWindow @0x1E859D0).  The MST's start is
		// 2022-03-03, so the header read 0D 0H 0M on a calendar that keeps
		// working: each player runs their own 25 days (gme/common/LoginCampaign.hpp)
		// and nothing ends globally.  Roll the window forward in whole periods
		// from the authored start so it always ends in the future (ticket #44).
		// Worked out at boot, which every deploy repeats.
		{
			auto& campaign = m_initrsp.login_campagin;
			if (campaign.total_days > 0)
			{
				const auto period = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::days(campaign.total_days));
				const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now());
				if (campaign.start_date + period <= now)
					campaign.start_date += period * ((now - campaign.start_date) / period);
				if (campaign.start_date + period <= now)
					campaign.start_date += period;
			}
		}
		m_initrsp.login_campaign_reward = forge::LoadMst<LoginCampaignRewardCache>(mstRoot, "login_campaign_reward_mst.json").data;
		m_initrsp.progression = forge::LoadMst<UserLevelMstCache>(mstRoot, "user_level_mst.json").data;
		// Owner's rule (2026-10-20): 20 more friends at every level (10 -> 30 at
		// lv 1, 50 -> 70 from lv 81).  Added to the level rows themselves, so the
		// client's copy (this Initialize reply), team_info and the server's own
		// capacity checks all agree, and the gem-bought slots on top
		// (add_friend_count) are untouched.
		for (auto& level : m_initrsp.progression)
			level.friend_count += kBonusFriendSlots;
		m_initrsp.mst = forge::LoadMst<VersionInfoCache>(mstRoot, "version_info_mst.json").data;
		m_initrsp.town_facility = forge::LoadMst<TownFacilityMstCache>(mstRoot, "town_facility_mst.json").data;
		m_initrsp.town_facility_lv = forge::LoadMst<TownFacilityLvMstCache>(mstRoot, "town_facility_lv_mst.json").data;
		m_initrsp.town_location = forge::LoadMst<TownLocationMstCache>(mstRoot, "town_location_mst.json").data;
		m_initrsp.town_location_lv = forge::LoadMst<TownLocationLvMstCache>(mstRoot, "town_location_lv_mst.json").data;
		m_initrsp.dungeon_keys = forge::LoadMst<DungeonKeyMstCache>(mstRoot, "dungeon_key_mst.json").data;
		m_initrsp.arena_ranks = forge::LoadMst<ArenaRankMstCache>(mstRoot, "arena_rank_mst.json").data;
		m_initrsp.gacha_effects = forge::LoadMst<GachaEffectMstCache>(mstRoot, "gacha_effect_mst.json").data;
		m_initrsp.gachas = forge::LoadMst<GachaMstCache>(mstRoot, "gacha_mst.json").data;
		m_initrsp.npcs = forge::LoadMst<NpcMstCache>(mstRoot, "npc_mst.json").data;
		m_initrsp.banner_info = forge::LoadMst<BannerInfoMstCache>(mstRoot, "banner_info_mst.json").data;
		m_initrsp.extra_passive_skills = forge::LoadMst<ExtraPassiveSkillMstCache>(mstRoot, "extra_passive_skill_mst.json").data;
		m_initrsp.notice_info = LoadJson<NoticeInfo>(systemRoot, "notice_info.json");
		m_initrsp.defines = forge::LoadMst<DefineMst>(mstRoot, "defines_mst.json");
		// SP-enhancement resets are free (2026-10-20, a moderator's go on the
		// "Make SP Reset cost no gems" suggestion).  The client prices the reset
		// from this value (it is in every Initialize reply) and ShopUse type 9
		// charges it, so both say 0 gems.
		m_initrsp.defines.reset_fe_skill_dia_count = kResetFeSkillGems;
		m_initrsp.video_ad_slots = forge::LoadMst<VideoAdsSlotGameInfo>(mstRoot, "video_ad_slot_game_info_mst.json");
		m_initrsp.exp_pattern = forge::LoadMst<UnitExpPatternMstCache>(mstRoot, "unit_exp_pattern_mst.json").data;
		m_initrsp.receipe = forge::LoadMst<ReceipeMstCache>(mstRoot, "recipe_mst.json").data;
		m_initrsp.trophy = forge::LoadMst<TrophyMstCache>(mstRoot, "trophy_mst.json").data;
		m_initrsp.trophy_group = forge::LoadMst<TrophyGroupMstCache>(mstRoot, "trophy_group_mst.json").data;
		m_initrsp.trophy_grade = forge::LoadMst<TrophyGradeMstCache>(mstRoot, "trophy_grade_mst.json").data;
		m_initrsp.information = forge::LoadMst<InformationMstCache>(mstRoot, "information_mst.json").data;
		m_initrsp.help = forge::LoadMst<HelpMstCache>(mstRoot, "help_mst.json").data;
		m_initrsp.help_sub = forge::LoadMst<HelpSubMstCache>(mstRoot, "help_sub_mst.json").data;
		m_initrsp.url = forge::LoadMst<UrlMstCache>(mstRoot, "url_mst.json").data;
		m_initrsp.challenge = forge::LoadMst<ChallengeMstCache>(mstRoot, "challenge_mst.json").data;

		// Keep one Frontier Hunter event running.
		//
		// All 97 rows are real 2014-2022 event windows and every one of them is
		// long over against a 2026 clock — the newest, id 97, ended
		// 2022-04-27.  With no live event the Survey Office has nothing to show
		// and, on the working theory, no Hunter Orb allowance either (the orbs
		// are Frontier Hunter's currency, which Frontier Gate shares — see the
		// in-game intro at content/event/randall_FG.txt).
		//
		// Same call as the Frontier Gate windows: this is an offline
		// preservation server with no event calendar, so the newest event is
		// advertised as permanently running rather than the catalog being left
		// entirely in the past.  Only the LAST row is touched; the other 96 keep
		// their historical windows so the event history stays intact.
		//
		// To go back to authentic windows, delete this block — nothing else
		// depends on it.
		if (!m_initrsp.challenge.empty())
		{
			auto& active = m_initrsp.challenge.back();
			const auto opened = std::chrono::time_point_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::from_time_t(1420070400));   // 2015-01-01
			const auto closes = std::chrono::time_point_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::from_time_t(2145916800));   // 2038-01-01

			active.start_data = opened;
			active.end_data = closes;
			active.cnt_start_data = opened;
			active.cnt_end_data = closes;
			active.rank_start_data = opened;
			active.rank_end_data = closes;
			m_activeChallengeId = active.id;

			LOG_INFO << "ServerCache: Frontier Hunter event " << active.id
			         << " advertised as running (all 97 rows expired by 2022)";
		}
		m_initrsp.challenge_hr = forge::LoadMst<ChallengeHrMstCache>(mstRoot, "challenge_hr_mst.json").data;
		m_initrsp.challenge_mis = forge::LoadMst<ChallengeMisMstCache>(mstRoot, "chlng_mission_mst.json").data;
		m_initrsp.challenge_grade = forge::LoadMst<ChallengeGradeMstCache>(mstRoot, "chlng_mission_grade_mst.json").data;
		m_initrsp.challenge_reward = forge::LoadMst<ChallengeRewardMstCache>(mstRoot, "chlng_mission_reward_mst.json").data;
		m_initrsp.challenge_item = forge::LoadMst<ChallengeItemMstCache>(mstRoot, "chlng_mission_item_set_mst.json").data;
		m_initrsp.challenge_rank_reward = forge::LoadMst<ChallengeRankRewardMstCache>(mstRoot, "challenge_rank_reward_mst.json").data;
		m_initrsp.challenge_mvp = forge::LoadMst<ChallengeMvpMstCache>(mstRoot, "challenge_mvp_mst.json").data;
		m_initrsp.interactive_banner = forge::LoadMst<InteractiveBannerInfoMstCache>(mstRoot, "interactive_banner_info_mst.json").data;
		m_initrsp.sound = forge::LoadMst<SoundMstCache>(mstRoot, "sound_mst.json").data;
		
		// cache: UserInfo response
		m_userrsp.notice_info = m_initrsp.notice_info;
		m_userrsp.video_ad_region = forge::LoadMst<VideoAdRegionCache>(mstRoot, "video_ad_region_mst.json").data;
		m_userrsp.video_ad_info = forge::LoadMst<VideoAdInfoCache>(mstRoot, "video_ad_info_mst.json").data;
		m_userrsp.excluded_dungeon_missions = forge::LoadMst<ExcludedDungeonMissionMstCache>(mstRoot, "excluded_dungeon_mission_mst.json").data;
		m_userrsp.gift = forge::LoadMst<GiftItemMstCache>(mstRoot, "gift_item_mst.json").data;
		m_userrsp.general_event = forge::LoadMst<GeneralEventMstCache>(mstRoot, "general_event_mst.json").data;
		m_userrsp.first_desc = forge::LoadMst<FirstDescMstCache>(mstRoot, "first_desc_mst.json").data;
		m_userrsp.summon_ticket_v2 = forge::LoadMst<SummonTicketV2MstCache>(mstRoot, "summon_ticket_v2_mst.json").data;
		m_userrsp.resummon_gacha = forge::LoadMst<ResummonGachaMstCache>(mstRoot, "resummon_gacha_mst.json").data;

		m_unitMst = forge::LoadMst<UnitMstCache>(mstRoot, "unit_mst.json").data;

		// HOMEBREW UNITS.  tools/unit-forge writes unit_mst_homebrew.json from
		// the community's packs, in the same wrapper and the same 60-field shape
		// as the shipped table -- each row is a real unit's row with a handful
		// of fields overridden, which is how a forged unit inherits artwork it
		// could never supply itself.
		//
		// Kept as a SEPARATE file and merged here rather than written into
		// unit_mst.json: the shipped data stays pristine, the overlay can be
		// deleted to undo everything, and a bad pack cannot corrupt the 2,291
		// units the game depends on.  Absent is the normal case and not an
		// error -- LoadJson throws on a missing file, so the existence check is
		// what keeps a server with no homebrew booting.
		{
			const auto overlay = std::string(mstRoot) + "/unit_mst_homebrew.json";
			std::error_code overlayEc;
			if (std::filesystem::is_regular_file(overlay, overlayEc) && !overlayEc)
			{
				try
				{
					auto forged = LoadJson<UnitMstCache>(overlay).data;
					// A forged id must not shadow a shipped unit.  The forge
					// refuses to write one, but the file is hand-editable and
					// arrives from strangers, so the server checks it too.
					size_t added = 0, skipped = 0;
					for (auto& unit : forged)
					{
						const auto clash = std::find_if(m_unitMst.begin(), m_unitMst.end(),
							[&unit](const UnitMst& u) { return u.id == unit.id; });
						if (clash != m_unitMst.end())
						{
							LOG_WARN << "homebrew unit " << unit.id
								<< " collides with a shipped unit; ignored";
							++skipped;
							continue;
						}
						m_unitMst.push_back(std::move(unit));
						++added;
					}
					LOG_INFO << "homebrew units: " << added << " added, "
						<< skipped << " ignored (" << overlay << ")";
				}
				catch (const std::exception& ex)
				{
					// A broken overlay must not take the server down with it.
					LOG_ERROR << "homebrew unit overlay could not be read, "
						"continuing without it: " << ex.what();
				}
			}
		}

		// BALANCE OVERRIDES.  The admin tooling publishes full unit rows to
		// <balance_root>/server/unit_mst.json, same wrapper and 60-field shape
		// as the shipped table.  Each REPLACES the loaded row (shipped or
		// homebrew) with its id -- unlike homebrew, which may only add.  An id
		// the table does not have is ignored: the overlay retunes units, it
		// does not invent them.
		//
		// Same robustness as the homebrew overlay: absent is normal, and a
		// broken file is logged and skipped rather than taking the server down.
		// The rows replaced are kept (m_unitMstBeforeBalance) because the
		// stat migration needs the pre-override curve for a fresh basis, and
		// UnitArchiver patches its Lv1 stats from the overridden rows.
		if (!m_serverConfig.balanceRoot.empty())
		{
			const auto overlay = m_serverConfig.balanceRoot + "/server/unit_mst.json";
			std::error_code overlayEc;
			if (std::filesystem::is_regular_file(overlay, overlayEc) && !overlayEc)
			{
				try
				{
					auto tuned = LoadJson<UnitMstCache>(overlay).data;
					size_t replaced = 0, skipped = 0;
					for (auto& unit : tuned)
					{
						const auto target = std::find_if(m_unitMst.begin(), m_unitMst.end(),
							[&unit](const UnitMst& u) { return u.id == unit.id; });
						if (target == m_unitMst.end())
						{
							LOG_WARN << "balance: unit " << unit.id
								<< " is not in the unit table; override ignored";
							++skipped;
							continue;
						}
						// First replacement wins the "before" slot, so a
						// duplicated id in the file still remembers the shipped row.
						m_unitMstBeforeBalance.try_emplace(target->id, *target);
						*target = std::move(unit);
						++replaced;
					}
					LOG_INFO << "balance: " << replaced << " unit row(s) overridden, "
						<< skipped << " ignored (" << overlay << ")";
				}
				catch (const std::exception& ex)
				{
					LOG_ERROR << "balance: unit override file could not be read, "
						"continuing without it: " << ex.what();
				}
			}
		}

		m_itemMst = forge::LoadMst<ItemMstCache>(mstRoot, "item_mst.json").data;

		// Grand Mission ("Campaign") master data — wrapper keys and field
		// maps documented in mst/grand_mission.kdl.
		// Randall Achievement master data — the list, the Merit Point shop and
		// the dedicate rates.  Ported and date-filtered by
		// tools/gen_achievement_mst.py; see mst/achievement.kdl.
		m_achievementSubjectMst = forge::LoadMst<AchievementSubjectMstCache>(mstRoot, "achievement_subject_mst.json").data;
		m_achievementTradeMst = forge::LoadMst<AchievementTradeMstCache>(mstRoot, "achievement_trade_mst.json").data;
		m_achievementDeliverRateMst = forge::LoadMst<AchievementDeliverRateMstCache>(mstRoot, "achievement_deliver_rate_mst.json").data;

		m_grandMissionMst = forge::LoadMst<GrandMissionMstCache>(mstRoot, "grand_mission_mst.json").data;
		m_grandMissionMapMst = forge::LoadMst<GrandMissionMapMstCache>(mstRoot, "grand_mission_map_mst.json").data;
		m_grandMissionSpotMst = forge::LoadMst<GrandMissionSpotMstCache>(mstRoot, "grand_mission_spot_mst.json").data;
		m_grandMissionRouteMst = forge::LoadMst<GrandMissionRouteMstCache>(mstRoot, "grand_mission_route_mst.json").data;
		m_grandMissionIconMst = forge::LoadMst<GrandMissionIconMstCache>(mstRoot, "grand_mission_icon_mst.json").data;
		m_grandMissionTreasureMst = forge::LoadMst<GrandMissionTreasureMstCache>(mstRoot, "grand_mission_treasure_mst.json").data;
		m_grandMissionFlgMst = forge::LoadMst<GrandMissionFlgMstCache>(mstRoot, "grand_mission_flg_mst.json").data;
		m_grandMissionEndCndMst = forge::LoadMst<GrandMissionEndCndMstCache>(mstRoot, "grand_mission_end_cnd_mst.json").data;
		m_grandMissionEventMst = forge::LoadMst<GrandMissionEventMstCache>(mstRoot, "grand_mission_event_mst.json").data;
		m_grandMissionRewardMst = forge::LoadMst<GrandMissionRewardMstCache>(mstRoot, "grand_mission_reward_mst.json").data;

		// Raid Battle master data — wrapper keys and field maps documented
		// in mst/raid.kdl (BOSS_PARTS is a server-side copy, see raid.kdl).
		m_raidWorldMst = forge::LoadMst<RaidWorldMstCache>(mstRoot, "raid_world_mst.json").data;
		m_raidMapMst = forge::LoadMst<RaidMapMstCache>(mstRoot, "raid_map_mst.json").data;
		m_raidRcMst = forge::LoadMst<RaidRcMstCache>(mstRoot, "raid_rc_mst.json").data;
		m_raidPlaystyleMst = forge::LoadMst<RaidPlaystyleMstCache>(mstRoot, "raid_playstyle_mst.json").data;
		m_raidBossMst = forge::LoadMst<RaidBossMstCache>(mstRoot, "raid_boss_mst.json").data;
		m_raidBossRouteMst = forge::LoadMst<RaidBossRouteMstCache>(mstRoot, "raid_boss_route_mst.json").data;
		m_raidMissionMst = forge::LoadMst<RaidMissionMstCache>(mstRoot, "raid_mission_mst.json").data;
		m_raidMissionBossMst = forge::LoadMst<RaidMissionBossMstCache>(mstRoot, "raid_mission_boss_mst.json").data;
		m_raidMissionClearCndMst = forge::LoadMst<RaidMissionClearCndMstCache>(mstRoot, "raid_mission_clear_cnd_mst.json").data;
		m_raidMissionPointMst = forge::LoadMst<RaidMissionPointMstCache>(mstRoot, "raid_mission_point_mst.json").data;
		m_raidPointMst = forge::LoadMst<RaidPointMstCache>(mstRoot, "raid_point_mst.json").data;
		m_raidUserRouteMst = forge::LoadMst<RaidUserRouteMstCache>(mstRoot, "raid_user_route_mst.json").data;
		m_raidBattleGroupMst = forge::LoadMst<RaidBattleGroupMstCache>(mstRoot, "raid_battle_group_mst.json").data;
		// Server-only copy of the client MST: boss battles build their part
		// monsters (sprites, main body) from it.
		m_raidBossPartsMst = forge::LoadMst<RaidBossPartsMstCache>(mstRoot, "raid_boss_parts_mst.json").data;
		// Colosseum (PvP arena) master data — wrapper keys and field maps
		// documented in mst/colosseum.kdl.
		m_colosseumClassMst = forge::LoadMst<ColosseumClassMstCache>(mstRoot, "colosseum_class_mst.json").data;
		m_colosseumExtraRuleMst = forge::LoadMst<ColosseumExtraRuleMstCache>(mstRoot, "colosseum_extra_rule_mst.json").data;
		m_colosseumFormationMst = forge::LoadMst<ColosseumFormationMstCache>(mstRoot, "colosseum_formation_mst.json").data;
		m_colosseumSupportMst = forge::LoadMst<ColosseumSupportMstCache>(mstRoot, "colosseum_support_mst.json").data;

		// Dual Brave Burst master data — see mst/dbb.kdl.
		m_dbbMst = forge::LoadMst<DbbMstCache>(mstRoot, "dbb_mst.json").data;
		m_dbbBondRecipeMst = forge::LoadMst<DbbBondRecipeMstCache>(mstRoot, "dbb_bond_recipe_mst.json").data;

		// Guild master data — see mst/guild.kdl (ART and SKILL_DETAILS remain
		// unported; their response classes are ambiguous).
		m_guildInfoMst = forge::LoadMst<GuildInfoMstCache>(mstRoot, "guild_info_mst.json").data;
		m_guildMemberSkillMst = forge::LoadMst<GuildMemberSkillMstCache>(mstRoot, "guild_member_skill_mst.json").data;
		m_guildPointExchangeMst = forge::LoadMst<GuildPointExchangeMstCache>(mstRoot, "guild_point_exchange_mst.json").data;

		// Purchase age-band spend caps — see mst/purchase.kdl.
		m_purchaseAgeLimitMst = forge::LoadMst<PurchaseAgeLimitMstCache>(mstRoot, "purchase_age_limit_mst.json").data;

		// Frontier Gate master data — see mst/frontier_gate.kdl (AREA remains
		// unported).  The reward table pays out the end of a run.
		m_frontierGateMst = forge::LoadMst<FrontierGateMstCache>(mstRoot, "frontier_gate_mst.json").data;
		m_frontierGateSupportMst = forge::LoadMst<FrontierGateSupportMstCache>(mstRoot, "frontier_gate_support_mst.json").data;
		m_frontierGateRewardMst = forge::LoadMst<FrontierGateRewardMstCache>(mstRoot, "frontier_gate_reward_mst.json").data;
		m_functionReleaseMst = forge::LoadMst<FunctionReleaseMstCache>(mstRoot, "function_release_mst.json").data;

		// Build the dungeon -> missions index, then drop the rows.  Only the id
		// and dungeon_id columns survive; see missionsByDungeon() for why this
		// is not the mission_mst cache that c49782e removed.
		{
			const auto missions = forge::LoadMst<MissionMstCache>(mstRoot, "mission_mst.json").data;
			for (const auto& mission : missions)
			{
				m_missionsByDungeon[mission.dungeon_id].push_back(mission.id);

				// First-clear rewards, kept for the 787 rows that have any.
				// Two ints and a short string wide -- the same reasoning as the
				// indexes below, rather than reviving the whole row cache.
				if (!mission.clear_rewards.empty())
					m_missionClearRewards.emplace(mission.id, mission.clear_rewards);
				// Repeat-clear rewards (S2ChOKo6): only the Balance Forge's
				// mission_mst overlay writes them, and its check already proved
				// they parse (forge/RepeatClear.hpp).
				forge::noteRepeatRewards(mission.id, mission.repeat_clear_bonus);

				// Mission -> its quest dungeon, for the clear Gem (see
				// missionQuestDungeon()).  Grand Gaia only: land 99 is the
				// special-content catch-all (Vortex, Frontier Gate, Frontier
				// Hunter, Grand Quest) and none of those are the "Quest map"
				// the reward belongs to.  The id floor keeps event and collab
				// numbering out for the same reason.
				if (mission.land_id > 0 && mission.land_id < kVortexLandId
					&& mission.area_id > 0 && mission.area_id < kQuestSpecialIdFloor
					&& mission.dungeon_id > 0 && mission.dungeon_id < kQuestSpecialIdFloor
					&& mission.id > 0 && mission.id < kQuestSpecialIdFloor)
				{
					m_missionQuestDungeon.emplace(mission.id, mission.dungeon_id);
				}

				// Prerequisites, for PermitPlace's progression gate.  Kept as
				// its own index for the same reason as the one above: the
				// rows themselves are dropped, and this is two ints wide.
				std::vector<int32_t> needs;
				for (const auto need : mission.need_mission_id)
					if (need != 0)
						needs.push_back(need);
				if (!needs.empty())
					m_missionNeeds.emplace(mission.id, std::move(needs));
			}
			for (auto& [dungeonId, ids] : m_missionsByDungeon)
				std::sort(ids.begin(), ids.end());
			LOG_INFO << "ServerCache: indexed " << missions.size() << " missions across "
			         << m_missionsByDungeon.size() << " dungeons";

			{
				std::set<int32_t> gemDungeons;
				for (const auto& [missionId, dungeonId] : m_missionQuestDungeon)
					gemDungeons.insert(dungeonId);
				LOG_INFO << "ServerCache: " << gemDungeons.size()
				         << " quest dungeon(s) pay a clear Gem, covering "
				         << m_missionQuestDungeon.size() << " mission(s)";
			}

			// Everything PermitPlace must allow for Frontier Gate to be
			// enterable.  UserInfo's dense ranges cover the Grand Gaia
			// numbering space only, and Frontier Gate lives entirely outside
			// it: gate 91's mission 9010001 sits in land 99, area 3000001,
			// dungeon 9000002 — none of which fall in lands 1-2, areas 1-1000,
			// missions 1-4000 or dungeons 1-2000.
			//
			// Verified 2026-08-07: permitting the gates' dungeons alone was not
			// enough; the mission loaded its assets and then crashed because
			// its parent topology was unreachable.  That is the same failure
			// the area note in UserInfo.cpp describes, one level deeper.
			//
			// Collected per gate rather than as a blanket widening so the
			// permit list grows by the ~100 ids Frontier Gate actually needs
			// instead of thousands.
			std::unordered_map<int32_t, const MissionMst*> missionById;
			missionById.reserve(missions.size());
			for (const auto& mission : missions)
				missionById.emplace(mission.id, &mission);

			for (const auto& gate : m_frontierGateMst)
			{
				if (gate.dungeon_id != 0)
					m_frontierGatePermits.dungeons.insert(gate.dungeon_id);
				if (gate.need_mission_id != 0)
					m_frontierGatePermits.missions.insert(gate.need_mission_id);

				// Guard the 0 case as well as the miss: MissionMst's sentinel
				// row is id 0 in dungeon 0, so a gate with no dungeon would
				// otherwise pull that sentinel into the permit list.
				if (gate.dungeon_id == 0)
					continue;

				const auto it = m_missionsByDungeon.find(gate.dungeon_id);
				if (it == m_missionsByDungeon.end())
					continue;

				for (const auto missionId : it->second)
				{
					m_frontierGatePermits.missions.insert(missionId);
					const auto mit = missionById.find(missionId);
					if (mit == missionById.end())
						continue;
					if (mit->second->area_id != 0)
						m_frontierGatePermits.areas.insert(mit->second->area_id);
					if (mit->second->land_id != 0)
						m_frontierGatePermits.lands.insert(mit->second->land_id);
				}
			}

			// Grand Quest.  The mode is invisible without a permit entry for the
			// first Grand Mission's dungeon: GameUtils::checkGrandPermit
			// @0x11A767C looks each Grand Mission's dungeon up in the permit
			// list and returns isEnterable() for the one whose place id is
			// "5000000".  The dungeon id is not in F_GRAND_MISSION_MST at all —
			// CampaignUtils::missionMstReflect @0x11E4F98 copies it from the
			// like-numbered F_MISSION_MST row, which is where this reads it.
			for (const auto& grand : m_grandMissionMst)
			{
				const auto mit = missionById.find(grand.mission_id);
				if (mit == missionById.end())
					continue;
				m_grandQuestPermits.missions.insert(grand.mission_id);
				if (mit->second->dungeon_id != 0)
					m_grandQuestPermits.dungeons.insert(mit->second->dungeon_id);
				if (mit->second->area_id != 0)
					m_grandQuestPermits.areas.insert(mit->second->area_id);
				if (mit->second->land_id != 0)
					m_grandQuestPermits.lands.insert(mit->second->land_id);
			}

			// FRONTIER HUNTER.  The Survey Office's Enter button routes to the
			// ordinary quest-select screen (ChallengeLobbyScene::missionScene
			// @0x155C45C -> MissionSelectScene2 with ChallengeBase::getDunMst),
			// so the mode needs no battle machinery of its own -- only its
			// topology permitted, the same way Frontier Gate and Vortex are.
			// ChallengeStartRequest exists in the binary but has ZERO callers,
			// so nothing here can fire an unregistered request.
			//
			// Only the ACTIVE challenge is collected.  Every row of
			// challenge_mst.json is a real 2014-2022 window and all of them have
			// expired; ServerCache keeps the newest one open and that is the one
			// the lobby reports, so permitting the other 96 would put 380-odd
			// unreachable missions on the map.
			for (const auto& challenge : m_initrsp.challenge)
			{
				if (challenge.id != m_activeChallengeId || challenge.dungeon_id == 0)
					continue;

				m_challengePermits.dungeons.insert(challenge.dungeon_id);

				const auto it = m_missionsByDungeon.find(challenge.dungeon_id);
				if (it == m_missionsByDungeon.end())
					continue;
				for (const auto missionId : it->second)
				{
					m_challengePermits.missions.insert(missionId);
					const auto mit = missionById.find(missionId);
					if (mit == missionById.end())
						continue;
					if (mit->second->area_id != 0)
						m_challengePermits.areas.insert(mit->second->area_id);
					if (mit->second->land_id != 0)
						m_challengePermits.lands.insert(mit->second->land_id);
				}
			}

			// THE TRIALS.  Collected by dungeon id because that is the whole of
			// it; there is no MST listing the series.  Trial of the Gods was the
			// first: the client knew its names ("Trial 01 - Burning Desire") and
			// the map had no tile, because these all sit above kSpecialIdFloor
			// where the progression gate never looks.
			//
			// ⚠ THE PREREQUISITE CHAIN IS NOT APPLIED HERE: this set is the same
			// for every player.  PermitPlace applies it per request to the
			// Research Lab's missions (gme::researchLabUnlocked), skipping any
			// prerequisite with no archive record -- a Strategy Zone trial needs
			// two before it, those are unbuilt, and an unbuilt mission can never
			// be cleared, so honouring them would lock the series behind content
			// that does not exist.  Trial of the Gods keeps no chain.
			//
			// The archive check that keeps an unbuildable tile off the map lives
			// in PermitPlace, NOT here: GimuServer::Setup runs m_cache.Setup
			// BEFORE MissionArchiver::setup, so asking the archiver anything at
			// this point gets "no" for every mission in the game.
			{
				for (const auto dungeonId : kTrialDungeonIds)
				{
					const auto it = m_missionsByDungeon.find(dungeonId);
					if (it == m_missionsByDungeon.end())
						continue;
					for (const auto missionId : it->second)
					{
						m_trialPermits.missions.insert(missionId);
						const auto mit = missionById.find(missionId);
						if (mit == missionById.end())
							continue;
						m_trialPermits.dungeons.insert(dungeonId);
						if (mit->second->area_id != 0)
							m_trialPermits.areas.insert(mit->second->area_id);
						if (mit->second->land_id != 0)
							m_trialPermits.lands.insert(mit->second->land_id);
					}
				}
			}

			LOG_INFO << "ServerCache: Trial of the Gods permits — "
			         << m_trialPermits.areas.size() << " area(s), "
			         << m_trialPermits.dungeons.size() << " dungeon(s), "
			         << m_trialPermits.missions.size() << " mission(s)";

			LOG_INFO << "ServerCache: Frontier Hunter permits — event "
			         << m_activeChallengeId << ", "
			         << m_challengePermits.lands.size() << " land(s), "
			         << m_challengePermits.areas.size() << " area(s), "
			         << m_challengePermits.dungeons.size() << " dungeon(s), "
			         << m_challengePermits.missions.size() << " mission(s)";

			LOG_INFO << "ServerCache: Grand Quest permits — "
			         << m_grandQuestPermits.lands.size() << " land(s), "
			         << m_grandQuestPermits.areas.size() << " area(s), "
			         << m_grandQuestPermits.dungeons.size() << " dungeon(s), "
			         << m_grandQuestPermits.missions.size() << " mission(s)";

			LOG_INFO << "ServerCache: Frontier Gate permits — "
			         << m_frontierGatePermits.lands.size() << " land(s), "
			         << m_frontierGatePermits.areas.size() << " area(s), "
			         << m_frontierGatePermits.dungeons.size() << " dungeon(s), "
			         << m_frontierGatePermits.missions.size() << " mission(s)";
		}

		// Summoner Unit master data — see mst/summoner.kdl (ARM_LEVEL and
		// ARM_PASSIVE remain unported; no response class in the export).
		m_summonerAbilityMst = forge::LoadMst<SummonerAbilityMstCache>(mstRoot, "summoner_ability_mst.json").data;
		m_summonerAbilityLevelMst = forge::LoadMst<SummonerAbilityLevelMstCache>(mstRoot, "summoner_ability_level_mst.json").data;
		m_summonerAbilityUpMst = forge::LoadMst<SummonerAbilityUpMstCache>(mstRoot, "summoner_ability_up_mst.json").data;
		m_summonerArmMst = forge::LoadMst<SummonerArmMstCache>(mstRoot, "summoner_arm_mst.json").data;
		m_summonerArmElementMst = forge::LoadMst<SummonerArmElementMstCache>(mstRoot, "summoner_arm_element_mst.json").data;
		m_summonerElementLevelMst = forge::LoadMst<SummonerElementLevelMstCache>(mstRoot, "summoner_element_level_mst.json").data;
		m_summonerExSkillMst = forge::LoadMst<SummonerExSkillMstCache>(mstRoot, "summoner_ex_skill_mst.json").data;
		m_summonerImageMst = forge::LoadMst<SummonerImageMstCache>(mstRoot, "summoner_image_mst.json").data;
		m_summonerLevelMst = forge::LoadMst<SummonerLevelMstCache>(mstRoot, "summoner_level_mst.json").data;

		// Skill tables — SkillMst's schema already lived in mst/skill.kdl;
		// the sidecars are in mst/skill_ext.kdl.
		m_skillMst = forge::LoadMst<SkillMstCache>(mstRoot, "skill_mst.json").data;
		m_skillLevelMst = forge::LoadMst<SkillLevelMstCache>(mstRoot, "skill_level_mst.json").data;
		m_leaderSkillMst = forge::LoadMst<LeaderSkillMstCache>(mstRoot, "leader_skill_mst.json").data;

		// Unit evolution — see mst/unit_evo.kdl.
		m_unitEvoMst = forge::LoadMst<UnitEvoMstCache>(mstRoot, "unit_evo_mst.json").data;
		m_unitEvoOmniMst = forge::LoadMst<UnitEvoOmniMstCache>(mstRoot, "unit_evo_omni_mst.json").data;
		m_unitEvoOmniTypeMst = forge::LoadMst<UnitEvoOmniTypeMstCache>(mstRoot, "unit_evo_omni_type_mst.json").data;
		m_unitEvoOmniRecipeMst = forge::LoadMst<UnitEvoOmniRecipeMstCache>(mstRoot, "unit_evo_omni_recipe_mst.json").data;

		// Unit side-tables — see mst/unit_ext.kdl (MissionEp3Mst's schema
		// already lived in mst/mission_ep3.kdl).
		m_unitTypeMst = forge::LoadMst<UnitTypeMstCache>(mstRoot, "unit_type_mst.json").data;
		m_unitExtMst = forge::LoadMst<UnitExtMstCache>(mstRoot, "unit_ext_mst.json").data;
		m_unitEp3Mst = forge::LoadMst<UnitEp3MstCache>(mstRoot, "unit_ep3_mst.json").data;
		m_unitCgsMst = forge::LoadMst<UnitCgsMstCache>(mstRoot, "unit_cgs_mst.json").data;
		m_unitCommentMst = forge::LoadMst<UnitCommentMstCache>(mstRoot, "unit_comment_mst.json").data;
		m_unitFeSkillMst = forge::LoadMst<UnitFeSkillMstCache>(mstRoot, "unit_fe_skill_mst.json").data;
		m_unitFeCategoryMst = forge::LoadMst<UnitFeCategoryMstCache>(mstRoot, "unit_fe_category_mst.json").data;
		// F_FE_SKILL_MST, the client's own copy (scripts/gen_fe_skill_mst.py):
		// what each SP enhancement costs, so FeSkillGet prices it as the
		// Enhancements screen did.
		m_feSkillMst = forge::LoadMst<FeSkillMstCache>(mstRoot, "fe_skill_mst.json").data;
		m_missionEp3Mst = forge::LoadMst<MissionEp3MstCache>(mstRoot, "mission_ep3_mst.json").data;

		// World geography — see mst/area.kdl.
		m_gateMst = forge::LoadMst<GateMstCache>(mstRoot, "gate_mst.json").data;
		m_areaMst = forge::LoadMst<AreaMstCache>(mstRoot, "area_mst.json").data;
		m_dungeonMst = forge::LoadMst<DungeonMstCache>(mstRoot, "dungeon_mst.json").data;

		// The Vortex areas numbered above kVortexAreaIdMax: every land-99 area
		// whose parent_area_id chain reaches a land-99 area inside the range.
		// 73 in this data, all children of Travellers, Vortex Trials and the
		// two Seasons Past tiles (ticket #19).
		//
		// Trial of the Gods (800051, under Travellers) is left out: the trial
		// collector above already permits its area, dungeon and missions, and
		// a second copy of each row would buy nothing.
		std::set<int32_t> nestedVortexAreas;
		{
			std::map<int32_t, const AreaMst*> areaById;
			for (const auto& area : m_areaMst)
				areaById.emplace(area.area_id, &area);

			const auto parentOf = [](const AreaMst& area) -> int32_t {
				if (!area.parent_area_id || area.parent_area_id->empty())
					return 0;
				try { return std::stoi(*area.parent_area_id); }
				catch (const std::exception&) { return 0; }
			};

			for (const auto& area : m_areaMst)
			{
				if (area.land_id != kVortexLandId || area.area_id < kVortexAreaIdMax
					|| m_trialPermits.areas.contains(area.area_id))
					continue;

				// Bounded walk: the data nests two deep (Vortex Trials ->
				// Scrolls of Urzyma -> Burden of Aesys), and a bad row must
				// not loop.
				const AreaMst* at = &area;
				for (int depth = 0; depth < 8; ++depth)
				{
					const auto it = areaById.find(parentOf(*at));
					if (it == areaById.end() || it->second->land_id != kVortexLandId)
						break;
					at = it->second;
					if (at->area_id > 0 && at->area_id < kVortexAreaIdMax)
					{
						nestedVortexAreas.insert(area.area_id);
						break;
					}
				}
			}

			LOG_INFO << "ServerCache: Vortex — " << nestedVortexAreas.size()
			         << " nested area(s) above " << kVortexAreaIdMax
			         << " belong to a Vortex category";
		}
		const auto isVortexArea = [&nestedVortexAreas](const int32_t areaId) {
			return areaId < kVortexAreaIdMax || nestedVortexAreas.contains(areaId);
		};

		// Which of the 88 Vortex areas are worth a tile.  Three ways to lose
		// one, all of them things the player sees as clutter:
		//
		//  - nothing behind it.  Ten areas have no dungeon at all, because
		//    this data version consolidated the weekday dungeons under area
		//    100001 "Enhancement" and left the old per-day area shells
		//    ("Garden of God", "Cave of Greed", "Souls Training Ground", ...)
		//    empty.  They still draw, and open onto nothing.
		//  - Japanese or mojibake naming.  34 areas, mostly the untranslated
		//    collab dungeons (Tales of, FFBE, Guilty Gear, Megami Tensei) plus
		//    the 至高の楽園 run.
		//  - the same tile again.  13 areas repeat a tile the player already
		//    has: "Heavenly Paradise" is six areas, "Ultimate Paradise" four,
		//    "Year-End Dungeon" three, and "Winter Paradise" arrives three
		//    times over as Travellers / Vortex Trials / Seasons Past.
		//
		// Duplicates are matched on the BANNER SET rather than the area name,
		// because the Winter Paradise trio carries three different area names
		// and plate images over one identical dungeon.  The lowest
		// display_order in each group is the one kept, so the survivor is
		// wherever the player already expected the tile to be.
		std::map<std::vector<std::string>, int32_t> bannersSeen;
		{
			std::map<int32_t, std::vector<const DungeonMst*>> vortexDungeons;
			for (const auto& dungeon : m_dungeonMst)
				if (dungeon.land_id == kVortexLandId && isVortexArea(dungeon.area_id))
					vortexDungeons[dungeon.area_id].push_back(&dungeon);

			std::vector<const AreaMst*> vortexAreas;
			for (const auto& area : m_areaMst)
				if (area.land_id == kVortexLandId && isVortexArea(area.area_id))
					vortexAreas.push_back(&area);

			std::sort(vortexAreas.begin(), vortexAreas.end(),
				[](const AreaMst* lhs, const AreaMst* rhs) {
					return std::tie(lhs->display_order, lhs->area_id)
					     < std::tie(rhs->display_order, rhs->area_id);
				});

			// ⚠ A PARENT CATEGORY IS NEVER HIDDEN.  Every rule below judges an
			// area by its OWN dungeons, which is right for a leaf tile and
			// wrong for a category: area 100002 "Travellers" has a single
			// dungeon whose banner duplicates 100004's, so it was dropped — and
			// it is the tile the player opens to reach 47 other areas, Trial of
			// the Gods among them.  That is why the Trials had a permitted
			// area, a permitted dungeon and six permitted missions and still
			// drew no tile, and the same was true of four more categories
			// covering 84 child areas in total.
			std::set<int32_t> parentAreas;
			for (const auto& area : m_areaMst)
			{
				const auto parent = area.parent_area_id;
				if (!parent || parent->empty() || *parent == "0")
					continue;
				try { parentAreas.insert(std::stoi(*parent)); }
				catch (const std::exception&) {}
			}

			size_t empty = 0, untranslated = 0, duplicate = 0, parents = 0;
			for (const auto* area : vortexAreas)
			{
				if (parentAreas.contains(area->area_id))
				{
					++parents;
					continue;
				}
				const auto it = vortexDungeons.find(area->area_id);
				if (it == vortexDungeons.end())
				{
					m_vortexHiddenAreas.insert(area->area_id);
					++empty;
					continue;
				}

				const auto& dungeons = it->second;
				const auto readable = IsDisplayableName(area->name)
					&& std::all_of(dungeons.begin(), dungeons.end(),
						[](const DungeonMst* d) { return IsDisplayableName(d->name); });
				if (!readable)
				{
					m_vortexHiddenAreas.insert(area->area_id);
					++untranslated;
					continue;
				}

				std::vector<std::string> banners;
				banners.reserve(dungeons.size());
				for (const auto* dungeon : dungeons)
					banners.push_back(dungeon->room_asset);
				std::sort(banners.begin(), banners.end());

				if (!bannersSeen.emplace(std::move(banners), area->area_id).second)
				{
					m_vortexHiddenAreas.insert(area->area_id);
					++duplicate;
					continue;
				}
			}

			LOG_INFO << "ServerCache: Vortex tiles — "
			         << (vortexAreas.size() - m_vortexHiddenAreas.size()) << " kept of "
			         << vortexAreas.size() << " (" << empty << " empty, "
			         << untranslated << " untranslated, " << duplicate << " duplicate, "
			         << parents << " kept as parent categories)";
		}

		// The area table the client is served, which is what actually draws the
		// Vortex list: the hidden areas are dropped, Parade Garden is floated
		// to the top, and any tile with translated art gets it.  Every other
		// row is passed through untouched — the key is a full replace (see
		// UserInfoResp::area_mst), so anything missing here stops existing for
		// the client, Grand Gaia included.
		{
			size_t localised = 0;
			m_clientAreaMst.reserve(m_areaMst.size());
			for (const auto& area : m_areaMst)
			{
				if (m_vortexHiddenAreas.contains(area.area_id))
					continue;

				auto& emitted = m_clientAreaMst.emplace_back(area);
				if (emitted.area_id == kParadeGardenAreaId)
					emitted.display_order = kParadeGardenDispOrder;

				auto banner = LocalisedBanner(emitted.plate_img);
				if (banner != emitted.plate_img)
				{
					emitted.plate_img = std::move(banner);
					++localised;
				}
			}

			LOG_INFO << "ServerCache: serving " << m_clientAreaMst.size()
			         << " area(s) to the client, " << localised
			         << " with translated banner art";
		}

		// Everything PermitPlace must allow before the Vortex draws a tile.
		// See vortexPermits() for why this is scoped rather than a widening.
		//
		// Runs here, after the geography load, rather than in the mission block
		// above: the mission rows are dropped there, but m_missionsByDungeon
		// survives and is all this needs to walk dungeon -> missions.
		{
			for (const auto& area : m_areaMst)
				if (area.land_id == kVortexLandId && isVortexArea(area.area_id)
					&& !m_vortexHiddenAreas.contains(area.area_id))
					m_vortexPermits.areas.insert(area.area_id);

			for (const auto& dungeon : m_dungeonMst)
			{
				if (dungeon.land_id != kVortexLandId || !isVortexArea(dungeon.area_id))
					continue;

				// A hidden area has no tile, so nothing inside it is reachable.
				// Leaving its dungeons permitted would not draw anything, but it
				// would keep them in PermitPlace for no reason.
				if (m_vortexHiddenAreas.contains(dungeon.area_id))
					continue;

				// The area is permitted unconditionally even for a rotating
				// dungeon, so the area tile stays put and only the dungeon
				// inside it comes and goes.
				m_vortexPermits.areas.insert(dungeon.area_id);

				// An always-open dungeon keeps its weekday banner but drops the
				// day gate (ServerConfig::vortexAlwaysOpenDungeons).
				const auto dayMask = m_serverConfig.vortexAlwaysOpenDungeons.contains(dungeon.dungeon_id)
					? uint8_t{ 0 }
					: VortexDayMaskFromBanner(dungeon.room_asset);

				// A dungeon a DungeonKey opens is emitted by UserInfo instead,
				// which alone knows whether the window is live and can attach
				// C1vG0iKh/qY49LBjw.  Emitting it here too would put a BARE
				// entry earlier in the list, and getObjectDungeon takes the
				// first match -- whose isEnterable() is false, so the parade
				// would draw locked no matter what we appended later.
				const bool dungeonKeyed = std::any_of(
					m_initrsp.dungeon_keys.begin(), m_initrsp.dungeon_keys.end(),
					[&dungeon](const DungeonKeyMst& k) { return k.dungeon_id == dungeon.dungeon_id; });

				// Not in the rotation -> permanently open.
				if (dayMask == 0 && !dungeonKeyed)
					m_vortexPermits.dungeons.insert(dungeon.dungeon_id);

				const auto it = m_missionsByDungeon.find(dungeon.dungeon_id);
				const auto* missionIds =
					it == m_missionsByDungeon.end() ? nullptr : &it->second;

				// A mission with a need_mission_id is held back into
				// gatedMissions instead of being permitted outright, so
				// PermitPlace can test it against the player's cleared set.
				// The parade ladders (Metal base -> six elements -> Super ->
				// six Super elements -> Mega, and Jewel base -> Super -> Mega)
				// are expressed this way; Vortex ids are above kSpecialIdFloor,
				// so the Grand Gaia gate never sees them and every tier used to
				// be open from the start.
				//
				// m_missionNeeds is already built above, so this reuses it
				// rather than rescanning the MST.
				// Dungeons a DungeonKey opens (the parades) are gated by the
				// key tier, not by the topology: the player buys a rung and
				// gets that rung's missions for limit_sec.  Their missions are
				// held back wholesale and emitted by UserInfo against the
				// active tier.
				const bool keyGated = std::any_of(
					m_initrsp.dungeon_keys.begin(), m_initrsp.dungeon_keys.end(),
					[&dungeon](const DungeonKeyMst& k) { return k.dungeon_id == dungeon.dungeon_id; });

				const auto place = [this, keyGated](TopologyPermits& permits, int32_t missionId) {
					// Every mission reachable inside the Vortex, however it is
					// gated.  The daily task code VV asks "did you clear a
					// Vortex mission", and MissionEnd has only the mission id —
					// the MissionMst rows that carry land_id are dropped after
					// this block, so the answer has to be collected here.
					m_vortexMissions.insert(missionId);
					if (keyGated)
					{
						permits.keyGatedMissions.insert(missionId);
						return;
					}
					const auto need = m_missionNeeds.find(missionId);
					if (need != m_missionNeeds.end() && !need->second.empty())
						permits.gatedMissions.emplace(missionId, need->second);
					else
						permits.missions.insert(missionId);
				};

				// A dungeon none of whose missions is open yet waits with them
				// (TopologyPermits::gatedDungeons).
				const auto holdIfLocked = [&dungeon, missionIds](TopologyPermits& permits) {
					if (!missionIds || missionIds->empty() || !permits.dungeons.contains(dungeon.dungeon_id))
						return;
					const bool locked = std::all_of(missionIds->begin(), missionIds->end(),
						[&permits](const int32_t id) { return permits.gatedMissions.contains(id); });
					if (!locked)
						return;
					permits.dungeons.erase(dungeon.dungeon_id);
					permits.gatedDungeons.emplace(dungeon.dungeon_id, *missionIds);
				};

				if (dayMask == 0)
				{
					if (missionIds)
						for (const auto missionId : *missionIds)
							place(m_vortexPermits, missionId);
					holdIfLocked(m_vortexPermits);
					continue;
				}

				for (size_t day = 0; day < m_vortexDayPermits.size(); ++day)
				{
					if ((dayMask & (1u << day)) == 0)
						continue;

					auto& permits = m_vortexDayPermits[day];
					if (!dungeonKeyed)
						permits.dungeons.insert(dungeon.dungeon_id);
					if (missionIds)
						for (const auto missionId : *missionIds)
							place(permits, missionId);
					holdIfLocked(permits);
				}
			}

			// Player recollection is that weekends opened every weekday
			// dungeon; the shipped MST only gives the weekend its own.  Fold
			// Mon-Fri into Sat/Sun when the operator asks for the former.
			if (m_serverConfig.vortexWeekendOpensAll)
			{
				for (size_t day = 5; day < m_vortexDayPermits.size(); ++day)
					for (size_t weekday = 0; weekday < 5; ++weekday)
					{
						m_vortexDayPermits[day].dungeons.insert(
							m_vortexDayPermits[weekday].dungeons.begin(),
							m_vortexDayPermits[weekday].dungeons.end());
						m_vortexDayPermits[day].missions.insert(
							m_vortexDayPermits[weekday].missions.begin(),
							m_vortexDayPermits[weekday].missions.end());
					}
			}

			if (!m_vortexPermits.areas.empty())
				m_vortexPermits.lands.insert(kVortexLandId);

			LOG_INFO << "ServerCache: Vortex permits — "
			         << m_vortexPermits.lands.size() << " land(s), "
			         << m_vortexPermits.areas.size() << " area(s), "
			         << m_vortexPermits.dungeons.size() << " always-open dungeon(s), "
			         << m_vortexPermits.missions.size() << " always-open mission(s)";

			static constexpr std::string_view kDayNames[] = {
				"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
			};
			for (size_t day = 0; day < m_vortexDayPermits.size(); ++day)
				LOG_INFO << "ServerCache: Vortex rotation " << kDayNames[day] << " — "
				         << m_vortexDayPermits[day].dungeons.size() << " dungeon(s), "
				         << m_vortexDayPermits[day].missions.size() << " mission(s)"
				         << (m_serverConfig.vortexWeekendOpensAll && day >= 5
				             ? "  (weekend-opens-all)" : "");
		}

		// Shop / medal / help / fixed-PvP singletons — see mst/shop.kdl.
		m_shopItemMst = forge::LoadMst<ShopItemMstCache>(mstRoot, "shop_item_mst.json").data;
		m_medalMst = forge::LoadMst<MedalMstCache>(mstRoot, "medal_mst.json").data;
		m_helpDetailMst = forge::LoadMst<HelpDetailMstCache>(mstRoot, "help_detail_mst.json").data;
		m_pvpFixedSettingMst = forge::LoadMst<PvpFixedSettingMstCache>(mstRoot, "pvp_fixed_setting_mst.json").data;

		// NOTE: gacha_info_mst.json is deliberately NOT loaded here.  The
		// GachaInfoMst struct in mst/gacha.kdl is the per-request RESPONSE
		// shape (12 fields) and GachaArchiver serves it at request time; the
		// MST file has 23 columns, so a strict load into that struct throws.

		m_unitSelectorGacha = forge::LoadMst<UnitSelectorGachaMstCache>(mstRoot, "unit_selector_gacha_ticket_mst.json").data;
		m_missionMst = forge::LoadMst<MissionMstCache>(mstRoot, "mission_mst.json").data;
		m_eventTokenMst = forge::LoadMst<EventTokenInfoCache>(mstRoot, "event_token_mst.json").data;

		// cache: GachaList response (gacha_info comes from GachaArchiver at
		// request time; only the category banners are cached here)
		m_gachaListRsp.gacha_categories = forge::LoadMst<GachaCategoryCache>(mstRoot, "gacha_category_mst.json").data;

		// TODO(arves): move this to generated per-used as there's no support for the claim
		m_initrsp.daily_task_bonuses = forge::LoadMst<DailyTaskBonusMst>(mstRoot, "daily_task_bonus_mst.json");
		m_initrsp.daily_task_prizes = forge::LoadMst<DailyTaskPrizeMstCache>(mstRoot, "daily_task_prize_mst.json").data;
		m_initrsp.daily_tasks = forge::LoadMst<DailyTaskMstCache>(mstRoot, "daily_task_mst.json").data;
		// ---
	}
}
