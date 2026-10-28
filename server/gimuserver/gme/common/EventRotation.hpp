#pragma once

#include <gimuserver/gme/common/Common.hpp>
#include <gimuserver/gme/common/Exchange.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Weekly featured event: event-token payouts and the event's Bazaar.
//
// The event dungeons are open all the time (they sit in the Vortex under
// Seasons Past).  What rotates is the currency: each game week, Monday 08:00
// UTC (GameWeek.hpp), ONE event of the pool in deploy/archive/event_rotation.json
// is featured, the same for every player.  Only that event's missions pay its
// token, and only its Bazaar is shown and sells.  No event is featured before
// the pool's first_week.
//
// NO STORED STATE, like SummonRotation.hpp: the featured event is a pure
// function of the week number, the pool sits in one fixed order and each week
// takes the next event, wrapping.
//
// PAYOUTS (payFeaturedEventTokens, from MissionEnd on a win) come in three
// modes: "each" pays every clear (the farm missions), "daily" the first clear
// of a game day, "run" the first clear in each featured week (the one-off
// amounts, earned again whenever the event comes back).  The daily and run
// claims are kept in user_exchange_purchases under shop "event-claim", which
// account reset and delete already cover, so no new table is needed.
//
// THE BAZAAR.  A pool token's EventTokenInfo carries the seconds to the end
// of its featured week as `timeleft`, so EventTokenTopScene shows a countdown,
// and 0 in other weeks, which makes the client skip the token entirely
// (EventTokenTopScene::setLayout).  Balances are kept between runs.  Purchases
// are booked under shop "event@<week>", so every run starts with full stock.
namespace gme
{

/*! One token payout: clearing `mission` pays `amount` of the event's token. */
struct EventPayout
{
	uint32_t mission{};
	int32_t amount{};
	/*! "each", "daily" or "run" (see above). */
	std::string mode;
};

/*!
* Load deploy/archive/event_rotation.json.  Needs the ServerCache (mission ->
* dungeon).  Call before loadExchangeArchive, which merges the pool's offers
* into the event catalog and validates them.
*/
void loadEventRotation(const std::string& archiveRoot);

/*! The Balance Forge's event_rotation.json overlay is the live rotation. */
bool eventRotationFromForge();

/*!
* The Forge's rotation was accepted on its own but its Bazaar offers fail the
* exchange catalog's checks: report why and load the image's file instead.
*/
void fallBackToBaseEventRotation(const std::string& archiveRoot, const std::string& why);

/*! Every Bazaar offer of every pool event, for loadExchangeArchive. */
const std::vector<ExchangeOffer>& rotationOffers();

/*! Whether a mission is a pool event's "daily" payout (Daily Tan, Training Grounds, ...). */
bool isDailyPayoutMission(uint32_t missionId);

/*! Whether a token belongs to a pool event (shown and sold only in its featured weeks). */
bool isRotatingToken(int32_t tokenId);

/*!
* The game week whose featured event owns `tokenId`, when that is this week,
* or last week and the week turned over less than the grace period ago.  The
* grace covers a battle or a Bazaar visit that started before Monday 08:00.
*
* @return nullopt for a token that is not featured now, or not in the pool.
*/
std::optional<int64_t> featuredRunWeek(int32_t tokenId, int64_t now);

/*! Seconds until `tokenId`'s featured week ends, 0 when it is not featured this week. */
int64_t featuredSecondsLeft(int32_t tokenId, int64_t now);

/*! The dungeons that pay `tokenId`, as the comma list EventTokenInfo.dungeon_ids wants. */
std::string rotationTokenDungeons(int32_t tokenId);

/*!
* The purchase-ledger shop for a token's Bazaar now: "event" for a token
* outside the pool, "event@<week>" for a featured pool token, nullopt for a
* pool token whose event is not featured (the Bazaar is closed).
*/
std::optional<std::string> eventShopFor(int32_t tokenId, int64_t now);

/*!
* This week's events as JSON for the Discord bot's weekly post: the featured
* event and next week's, and the Vortex dungeons each weekday opens on top of
* the every-day ones.  Read from the same tables the game serves, so the post
* cannot drift from what players see.
*
* {"week","starts","ends","event":{"name","token","at"},"next_event":{...},
*  "every_day":[names],"vortex":[[Mon names]..[Sun names]],"today","next_day"}
* `event.at` is the end of its featured week, `next_event.at` the start of its.
* The Vortex weekday turns at the server's local midnight (PermitPlace), which
* `next_day` gives in Unix seconds; `today` is 0 = Monday.
*
* @param now Unix seconds.
*/
std::string eventScheduleJson(int64_t now);

/*!
* Pay the featured event's token for a won mission.
*
* @return Whether any token was paid, so the caller knows to resend the balances.
*/
drogon::Task<bool> payFeaturedEventTokens(db::Database database, UserIdentity identity,
	uint32_t missionId, int64_t now);

} // namespace gme
