#pragma once

#include <cstdint>
#include <set>
#include <string>

/*!
* Server configuration.
*/
struct ServerConfig
{
	/*!
	* Initial level.
	*/
	uint32_t initialLevel;

	/*!
	* Initial ZEL.
	*/
	uint32_t initialZel;

	/*!
	* Initial Karma.
	*/
	uint32_t initialKarma;

	/*!
	* Initial brave coins.
	*/
	uint32_t initialBraveCoins;

	/*!
	* Frame-rate cap delivered to the offline-proxy client at startup. 0
	* disables the client-side cap. Read from plugins[0].config.server.fps_cap
	* in deploy/config.json; defaults to 60 if absent.
	*/
	uint32_t fpsCap;

	/*!
	* When true, Saturday and Sunday open every Vortex weekday dungeon instead
	* of only the weekend one.
	*
	* Defaults to FALSE, which is what the shipped MST describes: exactly one
	* dungeon carries a weekend banner (100300 "Garden of God"), and the
	* Mon-Fri dungeons carry only their own day. Player recollection of the
	* live game is that weekends opened everything; no evidence for that
	* survives in the data we hold, so it is a switch rather than the default.
	* Read from plugins[0].config.server.vortex_weekend_opens_all.
	*/
	bool vortexWeekendOpensAll;

	/*!
	* Vortex rotation dungeons that stay open every day, whatever weekday
	* their banner names.  Defaults to { 100000, 100050, 100100, 100300 }:
	*   100000 Gathering of Souls and 100050 Souls Training Ground, the two
	*          Monday (imp) dungeons.  The day gate reads the container's
	*          clock, which prod sets to Asia/Tokyo, so a Japanese Monday is
	*          Sunday afternoon to Monday morning in the Americas and players
	*          there never saw them (ticket #120);
	*   100100 Faerie Paradise (Global: Enchanted Paradise), the Tuesday
	*          "Evolution Units Abound!" dungeon -- the balancing team wants a
	*          steady source of the low-tier evolution units, which otherwise
	*          come only from random quests and the Merit shop;
	*   100300 Garden of God (Global: Oasis of the Gods), the weekend
	*          "Totem Units Abound!" dungeon, open every day on Global since 2018.
	* The banners and rewards are left alone; only the day gate goes.
	* Read from plugins[0].config.server.vortex_always_open_dungeons (an array
	* of dungeon ids; [] restores the weekday rotation).
	*/
	std::set<int32_t> vortexAlwaysOpenDungeons;

	/*!
	* When true (the default), every GME request must carry the session token
	* issued by the last guest login for its gumi user. Turn off only for
	* clients that cannot send it (e.g. a legacy offline proxy).
	* Read from plugins[0].config.server.require_session_token.
	*/
	bool requireSessionToken;

	/*!
	* Days an account transfer code stays valid after it is issued; 0 means
	* it never expires.  A code is single-use either way: a completed
	* transfer deletes it.
	* Read from plugins[0].config.server.transfer_code_ttl_days (default 30).
	*/
	uint32_t transferCodeTtlDays;

	/*!
	* PBKDF2-HMAC-SHA256 iterations for new transfer passwords.  Stored
	* hashes carry their own count, so changing this never breaks an issued
	* code.  Clamped to at least 100000: the password is only 4-12
	* alphanumerics, so the work factor is what protects a leaked database.
	* Read from plugins[0].config.server.transfer_pbkdf2_iterations
	* (default 600000).
	*/
	uint32_t transferPbkdf2Iterations;

	/*!
	* Seconds a no-session transfer claim waits for the claiming device's
	* next guest login before it lapses.
	* Read from plugins[0].config.server.transfer_claim_ttl_seconds
	* (default 86400).
	*/
	uint32_t transferClaimTtlSeconds;

	/*!
	* Root of the unit-balance publishing overlay, or empty when the feature
	* is off.  Relative to the config file's directory like every other root.
	*
	*   <balance_root>/content/<TABLE>/manifest.json + parts
	*       client download tables; announced and served alongside (and in
	*       preference to) <document_root>/content, rescanned per Initialize.
	*   <balance_root>/server/unit_mst.json
	*       full unit_mst rows that replace the loaded row with the same id;
	*       read once at boot.
	*   <balance_root>/server/missions/<mission id>.json
	*       one hand-built MissionRecord per file (the shape of one element of
	*       archive/mission.json) that replaces or adds the archive record with
	*       that id; checked per file, read at boot and re-read every 2 s
	*       (MissionArchiver, docs/features/BATTLE_BUILDER.md).
	*   <balance_root>/news/posts.json
	*       the news board; re-read when it changes.
	*
	* Read from plugins[0].config.server.balance_root (default: absent/off).
	*/
	std::string balanceRoot;

};
