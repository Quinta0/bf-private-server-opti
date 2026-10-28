#pragma once

#include <gimuserver/gme/common/Common.hpp>

#include <cstdint>
#include <string>
#include <vector>

// THE 50-GEM UNIT PICK (owner, 2026-10-19): once per account, pay 50 gems and
// choose any unit in the archive.  A reset or delete makes it available again.
//
// WHY 24 PICKERS AND NOT ONE.  The client cannot show the whole archive in one
// selector.  UnitSelectorGachaUnitSelectScene::downloadFiles @0x1D95AF4 asks
// requestUnitFiles(id, 1, 1, 1) for EVERY unit in the pool before the picker
// opens (thumbnail, full and battle art, animation sheet, SAM and skill files,
// 100-500 KB a unit on our CDN), and UnitSelectorGachaUnitObjManager::initialise
// @0x1CCD5E8 builds every icon up front with its texture loaded.  The original
// selectors held 6-33 units; all 2,259 would be about half a gigabyte on first
// open.  So the archive is split by rarity band and element: four rail tiles
// (Omni, 7★, 5-6★, 1-4★), each paging through Fire..Dark, 70-136 units a gate.
//
// NOTHING HERE IS DATA.  The pools, gates, tiles and selector rows are built at
// boot from the unit archive and added to each reply, so no MST or archive file
// changes and an image can carry this without touching the data files the live
// server runs (its temporary prices and banners).
//
// THE TICKETS ARE VIRTUAL.  The picker only lets a pick through while the
// player holds the selector's ticket (UnitSelectorGachaUserInfo, CGHaOZda), so
// an account that has not used the offer is sent one of each of the 24, and an
// account that has is sent them at 0.  Nothing is stored until the pick:
// user_unit_pick holds one row per account that has used it, and a reset
// deletes that row.
//
// THE LEVEL 999 RACE PICK (owner, 2026-10-22) is a second, free family of the
// same pickers: the first ten accounts to reach level 999 each get ONE pick
// from every 7★ and Omni unit (deploy/archive/race_pick.json, written by
// scripts/gen_race_pick.py with the summon gates' filters).  Twelve pickers,
// Omni and 7★ per element, on gates of their own, so a gate never carries two
// selector rows.  An operator grants it by adding a user_race_pick row; its tiles and
// tickets show while that row has no pick yet.
//
// THE GEMS ARE TAKEN AT THE PICK.  No shop screen in the client can sell a
// selector ticket (shop_item_mst is four summoner boosts), so the price is
// charged by UnitSelectorGachaTicket when the pick arrives.  Its reply carries
// team_info, which is what moves the client's gem counter.
namespace gme
{

/*! What the pick costs.  AUTHORED (owner). */
inline constexpr int32_t kUnitPickPrice = 50;

/*! One of the 24 pickers. */
struct UnitPickOffer
{
	int32_t ticketId = 0;  //!< UnitSelectorGachaMst XIvaD6Jp, 9001-9024
	int32_t gateId = 0;    //!< the gate the picker opens from
	std::string label;     //!< "Fire Omni"
	std::vector<int32_t> pool;
};

/*!
* Build the 24 pickers from the unit archive, and the race's 12 from
* `archiveRoot`/race_pick.json.
*
* Needs UnitArchiver and GachaArchiver set up first.  If a gate id is already
* an archive gate, or its GachaMst row is missing, that family is turned off
* with an error in the log rather than shown half-built.
*/
void loadUnitPick(const std::string& archiveRoot);

/*! The picker a selector ticket id belongs to, or nullptr if it is not one. */
const UnitPickOffer* unitPickOffer(uint32_t ticketId);

/*! The race picker a selector ticket id belongs to, or nullptr. */
const UnitPickOffer* racePickOffer(uint32_t ticketId);

/*! True when this account has used its pick. */
drogon::Task<bool> unitPickUsed(const db::Database database, const UserIdentity identity);

/*!
* Add the pick to a GachaList or UserInfo reply.
*
* The gates and selector rows always go out (they are the same for everyone);
* the four rail tiles only while the account has not used its pick.
*/
drogon::Task<void> addUnitPick(
	const db::Database database,
	const UserIdentity identity,
	std::vector<::GachaCategory>& categories,
	std::vector<::GachaInfoMst>& gates,
	std::vector<::UnitSelectorGachaMst>& selectors);

/*!
* Add the 24 virtual tickets, and the race's 12, to a selector inventory (CGHaOZda).
*
* Every reply that sends loadSelectorTickets has to call this too: the list is
* a full replace, so a reply without them takes the pick off the picker.
*/
drogon::Task<void> addUnitPickTickets(
	const db::Database database,
	const UserIdentity identity,
	std::vector<::UnitSelectorGachaUserInfo>& tickets);


} // namespace gme
