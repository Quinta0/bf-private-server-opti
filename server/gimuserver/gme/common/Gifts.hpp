#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/FriendPoints.hpp>
#include <gimuserver/gme/common/GameDay.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <map>
#include <random>
#include <string>
#include <vector>

// The gift inbox -- what a friend has sent you today.
//
// WHAT GIFTS ACTUALLY ARE (checked, not assumed): Honor crafting materials, the
// purple items obtainable ONLY this way.  A player nominates up to three gifts
// they want, friends send them once per day each, and -- the part that makes
// this implementable -- A GIFT IS NOT DEDUCTED FROM THE SENDER.  It costs the
// giver nothing, so a gift arriving from the simulated friend is not taking
// anything from anybody.
//
// That is why this exists at all.  The gift screen renders straight from
// UserGiftInfoList with no request of its own (GiftRecieveListScene2::
// initConnect is a 16-byte stub), and on an EMPTY list setGiftList @0x166D330
// null-derefs -- a deterministic client crash, identical in two minidumps
// (+0x84F533, read 0x14).  There is no server-side gate on the gift button, so
// an empty inbox is not a safe state: the inbox has to have something in it.
//
// THE DESIGN THIS IMPLEMENTS:
// One gift per game day from the simulated friend, WEIGHTED so the gifts the
// player asked for (up to three wish slots) land more often than the rest.
// That is the hook -- you set a preference, and logging in tomorrow is worth
// something because you might get it.
//
// REAL FRIENDS send through the same table (gme::sendGifts): one row per
// sender, recipient and game day, claimed by the same path.
//
// HOW THE CLIENT KNOWS WHO IT ALREADY GIFTED.  GiftSendScene2::setFriendInfoList
// @0x1679e5c drops a friend from the Send list when UserGiftInfoList (30uygM9m)
// holds a row with user_id_to == that friend, or when the local send list does.
// The local list is wiped by UserState::clear("gLRIn74v") as soon as the
// FixGiftInfo reply is parsed (@0x1391cc0), so the lasting signal is the
// server's: loadGiftInbox appends today's SENT rows (user_id_from = me) after
// the received ones.  The inbox and the footer badge filter on user_id_to == me
// (getCountRecieve @0x1294f94, GiftRecieveScene2::setGiftInfoList @0x1673234),
// so sent rows are never shown as gifts to claim.  There is no other limit in
// the client -- no DefineMst value and no counter in the gift scenes.
namespace gme
{

/*! Who the gifts come from until real friends exist. */
inline constexpr const char* kGiftSenderId = kSyntheticHelperUserId;
inline constexpr const char* kGiftSenderName = "DecompFriend";

/*!
* How much likelier each gift the player ASKED for is than any other.
*
* Per wanted gift, not shared between them: with the 10-row catalogue and all
* three wish slots filled, each wish is 4/19 and one of the three lands on
* about 63% of days -- likely, never certain.
*/
inline constexpr int32_t kWantedGiftWeight = 4;

/*! Wish slots the gift screen draws (GiftRecieveScene2::setWantGiftList, see gme::padWantGift). */
inline constexpr size_t kWantedGiftSlots = 3;

/*!
* Most `toUserId:giftId` pairs read from one FixGiftInfo.
*
* The client batches a whole visit into one request (ConnectRequestList::
* addObject @0x13914f8 drops a second queued gLRIn74v), so a real request
* carries at most one pair per friend -- 100 at the top friend capacity.
*/
inline constexpr size_t kMaxGiftSendsPerRequest = 200;

/*! Name drawn on a gift whose real sender no longer exists. */
inline constexpr const char* kGiftUnknownSenderName = "Summoner";

/*!
* Today as the shared day key, "YYYY-MM-DD": the gift inbox, the login bonus,
* helper lends and the friend picker all count days by it.
*
* THE GAME DAY, turning at 08:00 UTC (the client's midnight), numbered with
* DayClock::Gifts' recorded shift — so the key never steps back at the switch,
* and with a shift of 1 it reads one date ahead of the UTC date from 08:00 on.
* It is a day KEY, not a calendar date to show anyone (utcDateToday is).  The
* zero-padded form sorts as dates do, which is what the "a later day is never
* rewound" checks below rely on.  One server clock, so a player's timezone
* cannot mint a second gift.
*/
inline std::string giftToday()
{
	return dayKey(currentDay(DayClock::Gifts));
}

namespace detail
{

/*!
* The gifts a stored want_gift asks for.
*
* want_gift is a COMMA LIST, not one id: FixGiftInfo stores s2WnRw9N as the
* client holds it, and after any UserInfo the client holds gme::padWantGift's
* three ids ("a,b,c").  rollGift used to compare the whole string with one id,
* which never matched, so no wish was ever weighted.
*
* Read the way the gift screen reads it, so the weighted gifts are the ones its
* three wish slots draw (GiftRecieveScene2::getGiftMstList @0x16734BC and
* setWantGiftList @0x1671DA4): split on ',', each piece a number (StrToInt:
* leading blanks skipped, digits up to the first non-digit).  "0", a blank or
* a non-number is an EMPTY slot -- delWantGift @0x16739B0 writes "0".  A
* non-zero id the catalogue lacks is dropped and the next piece moves up.
* Only the first kWantedGiftSlots slots count, so a crafted list naming the
* whole catalogue weights three.  The client allows one gift in two slots;
* it is weighted once, so stacking slots cannot buy a near-certain drop.
*
* A player who never picked a wish holds padWantGift's defaults once a
* FixGiftInfo has echoed them back; those are what their slots show, so they
* count as asked for.  Nothing stored (never on the gift screens) weights
* nothing.
*
* @return Catalogue ids in slot order, at most kWantedGiftSlots, no repeats.
*/
inline std::vector<int32_t> wantedGiftIds(
	const std::vector<::GiftItemMst>& catalogue,
	const std::string& wantGift)
{
	std::vector<int32_t> wanted;
	size_t slots = 0;
	for (size_t at = 0; at <= wantGift.size() && slots < kWantedGiftSlots; )
	{
		const auto comma = wantGift.find(',', at);
		const auto end = comma == std::string::npos ? wantGift.size() : comma;
		const char* first = wantGift.data() + at;
		const char* const last = wantGift.data() + end;
		at = end + 1;

		while (first != last && std::isspace(static_cast<unsigned char>(*first)))
			++first;
		int32_t id = 0;
		if (std::from_chars(first, last, id).ec == std::errc::result_out_of_range)
			continue;   // non-zero to StrToInt, and in no catalogue: dropped
		if (id == 0)
		{
			++slots;    // an empty slot
			continue;
		}

		const bool known = std::any_of(catalogue.begin(), catalogue.end(),
			[id](const ::GiftItemMst& g) { return g.id == id; });
		if (!known)
			continue;   // dropped; the next piece takes this slot
		++slots;
		if (std::find(wanted.begin(), wanted.end(), id) == wanted.end())
			wanted.push_back(id);
	}
	return wanted;
}

/*!
* Pick today's gift, biased toward the ones the player asked for.
*
* Deterministic per (user, day) on purpose: the daily drop is decided once, and
* re-rolling it on a second login would let a player reload until they liked the
* result.  The seed is the user id and the date, so the same day always yields
* the same gift for that player -- and a different one tomorrow.
*/
inline int32_t rollGift(
	const std::vector<::GiftItemMst>& catalogue,
	const std::string& wantGift,
	const std::string& userId,
	const std::string& day)
{
	if (catalogue.empty())
		return 0;

	const auto wanted = wantedGiftIds(catalogue, wantGift);

	std::vector<int32_t> pool;
	pool.reserve(catalogue.size() + wanted.size() * (kWantedGiftWeight - 1));
	for (const auto& row : catalogue)
	{
		pool.push_back(row.id);
		// Each wanted gift goes in several times rather than being forced: the
		// point is a better chance, not a guarantee, or there is no reason to
		// look.
		if (std::find(wanted.begin(), wanted.end(), row.id) != wanted.end())
		{
			for (int32_t i = 1; i < kWantedGiftWeight; ++i)
				pool.push_back(row.id);
		}
	}

	std::seed_seq seed{ static_cast<uint32_t>(std::hash<std::string>{}(userId)),
						static_cast<uint32_t>(std::hash<std::string>{}(day)) };
	std::mt19937 rng(seed);
	std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
	return pool[pick(rng)];
}

} // namespace detail

namespace detail
{

/*! The catalogue row for a gift id, or nullptr when the catalogue no longer has it. */
inline const ::GiftItemMst* giftItem(const std::vector<::GiftItemMst>& catalogue, const int32_t giftId)
{
	const auto it = std::find_if(catalogue.begin(), catalogue.end(),
		[giftId](const ::GiftItemMst& g) { return g.id == giftId; });
	return it == catalogue.end() ? nullptr : &*it;
}

/*!
* A player's party leader, for the thumbnail on a gift row.
*
* 0 when they lead with nothing; that is what the simulated friend's rows have
* always sent, and the gift screen draws it.
*/
inline drogon::Task<int32_t> giftLeaderUnit(const db::Database database, const std::string& userId)
{
	const auto rows = co_await database->execSqlCoro(
		"SELECT CAST(u.unit_id AS INTEGER) AS unit_id FROM user_info i"
		" JOIN user_units u ON u.user_id = i.id"
		" LEFT JOIN user_decks d ON d.user_id = i.id AND d.user_unit_id = u.user_unit_id"
		"  AND d.deck_type = 1 AND d.deck_num = i.active_deck AND d.member_type = 0"
		" WHERE i.id = $1"
		" ORDER BY (d.user_unit_id IS NOT NULL) DESC, u.unit_lvl DESC LIMIT 1;",
		userId);
	co_return rows.empty() ? 0 : rows[0]["unit_id"].as<int32_t>();
}

} // namespace detail

/*!
* Make sure today's gift exists, then read the inbox back.
*
* The reply carries two kinds of row (see the header): the unclaimed gifts
* sent TO this player, and the gifts this player SENT today, which is how the
* Send Gift list knows to leave those friends out.
*
* @param database Database client or transaction.
* @param identity Resolved user.
* @return Gift rows, received first and sent after, ready for the wire.
*/
inline drogon::Task<std::vector<::UserGiftInfo>> loadGiftInbox(
	const db::Database database,
	const UserIdentity identity)
{
	std::vector<::UserGiftInfo> inbox;

	const auto& catalogue = theServer()->cache().userInfoResp().gift;
	if (catalogue.empty())
	{
		LOG_WARN << "loadGiftInbox: no GiftItemMst rows loaded; the gift screen "
			"would open on an empty inbox -- check deploy/mst/gift_item_mst.json";
		co_return inbox;
	}

	const auto day = giftToday();

	// THE SIMULATED FRIEND'S DAILY GIFT.  Counted by sender: a real friend's
	// gift that landed first must not use up the day's simulated one.  A row
	// dated LATER than today counts as today's (`day >= $`): only a rollback
	// past the 08:00 UTC switch or a clock step back makes one, and it must not
	// let an earlier day be minted after it (GameDay.hpp, DayClock).  The
	// INSERT carries its own NOT EXISTS, so two loads racing (UserInfo and
	// GetGiftInfo) cannot both mint it.  It also keeps the list non-empty on
	// a new day, which is what replaces yesterday's sent rows on the client
	// (an empty 30uygM9m leaves the old list standing).
	const auto existing = co_await database->execSqlCoro(
		"SELECT COUNT(*) AS n FROM user_gifts WHERE user_id = $1 AND from_user_id = $2 AND day >= $3;",
		identity.userId, std::string(kGiftSenderId), day);
	if (!existing.empty() && existing[0]["n"].as<int64_t>() == 0)
	{
		std::string wantGift;
		const auto want = co_await database->execSqlCoro(
			"SELECT want_gift FROM user_info WHERE id = $1;", identity.userId);
		if (!want.empty())
			wantGift = want[0]["want_gift"].as<std::string>();

		const auto giftId = detail::rollGift(catalogue, wantGift, identity.userId, day);
		if (giftId != 0)
		{
			const auto minted = co_await database->execSqlCoro(
				"INSERT INTO user_gifts (user_id, from_user_id, gift_id, day, received)"
				" SELECT $1, $2, $3, $4, 0 WHERE NOT EXISTS (SELECT 1 FROM user_gifts"
				" WHERE user_id = $1 AND from_user_id = $2 AND day >= $4);",
				identity.userId, std::string(kGiftSenderId), giftId, day);
			if (minted.affectedRows() > 0)
			{
				LOG_INFO << "loadGiftInbox: " << kGiftSenderName << " sent gift "
					<< giftId << " to " << identity.userId << " for " << day
					<< (wantGift.empty() ? "" : " (wanted: " + wantGift + ")");
			}
		}
	}

	// Leader thumbnails, one lookup per real sender however many gifts they sent.
	std::map<std::string, int32_t> leaders;

	// RECEIVED: unclaimed gifts to this player, with the sender's own name.
	for (const auto& row : co_await database->execSqlCoro(
		"SELECT g.id, g.from_user_id, g.gift_id, g.day, i.username"
		" FROM user_gifts g LEFT JOIN user_info i ON i.id = g.from_user_id"
		" WHERE g.user_id = $1 AND g.received = 0 ORDER BY g.id;",
		identity.userId))
	{
		const auto giftId = row["gift_id"].as<int32_t>();
		const auto* item = detail::giftItem(catalogue, giftId);
		if (item == nullptr)
			continue;   // catalogue changed under a stored row; skip rather than send a half-row

		const auto from = row["from_user_id"].as<std::string>();
		const bool simulated = from == kGiftSenderId;
		const bool realSender = !simulated && !row["username"].isNull();

		::UserGiftInfo gift{};
		gift.gift_identify_id = row["id"].as<int32_t>();
		gift.user_id_from = from;
		gift.user_id_to = identity.userId;
		gift.gift_id = giftId;
		gift.gift_type = item->type;
		gift.item_id = item->item_id;
		gift.possession = item->possession;
		gift.gift_date = row["day"].as<std::string>() + " 00:00:00";  // chrono_time takes the string form
		gift.gift_ymd = 0;
		gift.recieve_flg = 0;
		gift.handle_name = simulated ? std::string(kGiftSenderName)
			: realSender ? row["username"].as<std::string>()
			: std::string(kGiftUnknownSenderName);
		if (realSender)
		{
			auto leader = leaders.find(from);
			if (leader == leaders.end())
				leader = leaders.emplace(from, co_await detail::giftLeaderUnit(database, from)).first;
			gift.unit_id = leader->second;
		}
		inbox.push_back(std::move(gift));
	}

	// SENT TODAY: what keeps a friend off the Send Gift list after the reply
	// wipes the client's own send list.  Claimed or not, a sent gift is sent.
	// GiftRecieveScene2::downloadFiles fetches every row's unit, filtered or
	// not, so these carry the sender's own leader like a received row would.
	const auto sent = co_await database->execSqlCoro(
		"SELECT g.id, g.user_id AS to_id, g.gift_id, g.day, i.username"
		" FROM user_gifts g LEFT JOIN user_info i ON i.id = g.from_user_id"
		" WHERE g.from_user_id = $1 AND g.day >= $2 ORDER BY g.id;",
		identity.userId, day);
	if (!sent.empty())
	{
		const auto myLeader = co_await detail::giftLeaderUnit(database, identity.userId);
		for (const auto& row : sent)
		{
			const auto giftId = row["gift_id"].as<int32_t>();
			const auto* item = detail::giftItem(catalogue, giftId);

			::UserGiftInfo gift{};
			gift.gift_identify_id = row["id"].as<int32_t>();
			gift.user_id_from = identity.userId;
			gift.user_id_to = row["to_id"].as<std::string>();
			gift.gift_id = giftId;
			// A catalogue miss still sends the row: the friend has been gifted
			// today whatever the gift turned out to be.
			gift.gift_type = item ? item->type : 0;
			gift.item_id = item ? item->item_id : 0;
			gift.possession = item ? item->possession : 0;
			gift.gift_date = row["day"].as<std::string>() + " 00:00:00";
			gift.gift_ymd = 0;
			gift.recieve_flg = 1;
			gift.handle_name = row["username"].isNull() ? std::string(kGiftUnknownSenderName)
				: row["username"].as<std::string>();
			gift.unit_id = myLeader;
			inbox.push_back(std::move(gift));
		}
	}

	co_return inbox;
}

/*! One `toUserId:giftId` pair from FixGiftInfo's `qD74LPXb`. */
struct GiftSend
{
	std::string toUserId;
	int32_t giftId = 0;
};

/*!
* Parse FixGiftInfo's send list.
*
* FixGiftInfoRequest::createBody @0x13A37E4 joins the client's send list as
* `getUserIDTo + ":" + giftId`, comma-separated with no trailing comma.  The
* recipient is FriendInfo.user_id (h7eY3sAK) -- the account id, never the
* public friend ID.  Malformed pieces are dropped, a recipient named twice
* keeps the first gift, and the list is capped.
*/
inline std::vector<GiftSend> parseGiftSends(const std::string& pairs)
{
	std::vector<GiftSend> sends;
	size_t at = 0;
	while (at <= pairs.size() && sends.size() < kMaxGiftSendsPerRequest)
	{
		const auto comma = pairs.find(',', at);
		auto piece = pairs.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		at = comma == std::string::npos ? pairs.size() + 1 : comma + 1;

		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front())))
			piece.erase(piece.begin());
		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back())))
			piece.pop_back();

		const auto colon = piece.rfind(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 >= piece.size())
			continue;

		GiftSend send{};
		send.toUserId = piece.substr(0, colon);
		try { send.giftId = std::stoi(piece.substr(colon + 1)); }
		catch (const std::exception&) { continue; }
		if (send.giftId <= 0 || send.toUserId.size() > 64)
			continue;

		const bool seen = std::any_of(sends.begin(), sends.end(),
			[&send](const GiftSend& s) { return s.toUserId == send.toUserId; });
		if (!seen)
			sends.push_back(std::move(send));
	}
	return sends;
}

/*!
* Deliver gifts to real friends: one row in each recipient's inbox.
*
* Gifts cost the sender nothing (see the header), so the limits are the
* server's own, all in the one INSERT:
*   - the recipient must be a REAL friend right now (user_relations state 1;
*     both rows are written together, so my row is enough).  A stranger, a
*     pending request, a public friend ID or a made-up id inserts nothing;
*   - one gift per sender, recipient and game day, which also makes a resent
*     request and a second visit the same day harmless (a row dated later
*     than today blocks too, see loadGiftInbox);
*   - the gift must be in the catalogue, and never to yourself.
* NPC roster friends (user_friends) are skipped: there is no inbox behind them.
* Nothing here refuses the request -- a FixGiftInfo error closes the session.
*
* @param database Transaction to run in -- the caller owns the commit.
* @param identity The sender.
* @param sends    Parsed `qD74LPXb` pairs.
* @return How many gifts were delivered.
*/
inline drogon::Task<int32_t> sendGifts(
	const db::Database database,
	const UserIdentity identity,
	const std::vector<GiftSend> sends)
{
	int32_t delivered = 0;
	if (sends.empty())
		co_return delivered;

	const auto& catalogue = theServer()->cache().userInfoResp().gift;
	const auto day = giftToday();

	for (const auto& send : sends)
	{
		if (send.toUserId == identity.userId || detail::giftItem(catalogue, send.giftId) == nullptr)
			continue;

		const auto result = co_await database->execSqlCoro(
			"INSERT INTO user_gifts (user_id, from_user_id, gift_id, day, received)"
			" SELECT $1, $2, $3, $4, 0"
			" WHERE EXISTS (SELECT 1 FROM user_relations WHERE user_id = $2 AND other_id = $1 AND state = 1)"
			" AND NOT EXISTS (SELECT 1 FROM user_gifts WHERE user_id = $1 AND from_user_id = $2 AND day >= $4);",
			send.toUserId, identity.userId, send.giftId, day);
		if (result.affectedRows() > 0)
		{
			++delivered;
			LOG_INFO << "sendGifts: " << identity.userId << " sent gift " << send.giftId
				<< " to " << send.toUserId << " for " << day;
		}
	}

	co_return delivered;
}

/*!
* Claim the named gifts, paying each into the right place.
*
* The gift TYPE decides where it lands, from GiftItemMst's own rows:
*   1 Zel, 2 Honor Points, 3 an item (the Honor crafting materials), 4 Karma.
*
* Idempotent by design: the client sends its whole selection and will resend it
* if the screen is reopened, so the UPDATE carries `received = 0` and a row that
* has already paid out simply matches nothing.
*
* @param database Transaction to run in -- the caller owns the commit.
* @param identity Resolved user.
* @param ids      Comma-separated gift_identify_ids from `23Xi0jom`.
* @return How many gifts actually paid out.
*/
inline drogon::Task<int32_t> claimGifts(
	const db::Database database,
	const UserIdentity identity,
	const std::string& ids)
{
	int32_t paid = 0;
	if (ids.empty())
		co_return paid;

	const auto& catalogue = theServer()->cache().userInfoResp().gift;

	// Same split idiom AchievementDeliver uses on its id lists; blanks dropped.
	std::vector<std::string> tokens;
	for (size_t at = 0; at <= ids.size(); )
	{
		const auto comma = ids.find(',', at);
		auto piece = ids.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front())))
			piece.erase(piece.begin());
		while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back())))
			piece.pop_back();
		if (!piece.empty())
			tokens.push_back(std::move(piece));
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}

	for (const auto& token : tokens)
	{
		int32_t identifyId = 0;
		try { identifyId = std::stoi(token); }
		catch (const std::exception&) { continue; }
		if (identifyId <= 0)
			continue;

		// Claim the row FIRST.  If the grant below throws, the transaction
		// rolls the whole thing back together -- but within one pass this stops
		// a duplicated id in the list from paying twice.
		const auto claimed = co_await database->execSqlCoro(
			"UPDATE user_gifts SET received = 1"
			" WHERE id = $1 AND user_id = $2 AND received = 0;",
			identifyId, identity.userId);
		if (claimed.affectedRows() == 0)
			continue;

		const auto row = co_await database->execSqlCoro(
			"SELECT gift_id FROM user_gifts WHERE id = $1;", identifyId);
		if (row.empty())
			continue;
		const auto giftId = row[0]["gift_id"].as<int32_t>();
		const auto it = std::find_if(catalogue.begin(), catalogue.end(),
			[giftId](const ::GiftItemMst& g) { return g.id == giftId; });
		if (it == catalogue.end())
			continue;

		switch (it->type)
		{
		case 1:
			co_await database->execSqlCoro(
				"UPDATE user_info SET zel = zel + $1 WHERE id = $2;",
				it->possession, identity.userId);
			break;
		case 2:
			co_await database->execSqlCoro(
				"UPDATE user_info SET friend_points = friend_points + $1 WHERE id = $2;",
				it->possession, identity.userId);
			break;
		case 3:
			// The Honor crafting materials.  user_items stacks by species, so
			// add to the stack when one exists and open a new one when it does
			// not.
			{
				const auto stack = co_await database->execSqlCoro(
					"SELECT instance_id FROM user_items"
					" WHERE user_id = $1 AND item_id = $2 LIMIT 1;",
					identity.userId, it->item_id);
				if (stack.empty())
				{
					co_await database->execSqlCoro(
						"INSERT INTO user_items (user_id, item_id, item_num)"
						" VALUES ($1, $2, $3);",
						identity.userId, it->item_id, it->possession);
				}
				else
				{
					co_await database->execSqlCoro(
						"UPDATE user_items SET item_num = item_num + $1"
						" WHERE instance_id = $2;",
						it->possession, stack[0]["instance_id"].as<int64_t>());
				}
			}
			break;
		case 4:
			co_await database->execSqlCoro(
				"UPDATE user_info SET karma = karma + $1 WHERE id = $2;",
				it->possession, identity.userId);
			break;
		default:
			LOG_WARN << "claimGifts: gift " << giftId << " has unhandled type "
				<< it->type << "; the row is claimed but nothing was paid";
			break;
		}

		++paid;
		LOG_INFO << "claimGifts: " << identity.userId << " claimed gift "
			<< identifyId << " (gift_id " << giftId << ", type " << it->type
			<< ", x" << it->possession << ")";
	}

	co_return paid;
}

} // namespace gme
