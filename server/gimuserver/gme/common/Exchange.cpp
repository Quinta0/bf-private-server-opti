#include "App.hpp"
#include "Exchange.hpp"
#include <gimuserver/gme/common/EventRotation.hpp>
#include <gimuserver/gme/common/EventTokens.hpp>
#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/utils/JsonFile.hpp>
#include <gimuserver/archive/UnitArchiver.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace gme
{
namespace
{
std::vector<ExchangeOffer> eventOffers;
bool validReward(const ExchangeOffer& offer)
{
    if (offer.price <= 0 || offer.limit <= 0 || offer.quantity <= 0) return false;
    if (offer.reward_type == 6)
        return UnitArchiver::instance().lookup(offer.target_id).has_value();
    if (offer.reward_type != 4 && offer.reward_type != 5 && offer.reward_type != 7)
        return false;
    for (const auto& item : theServer()->cache().itemMst())
        if (item.id == offer.target_id) return true;
    return false;
}

/*!
* One Bazaar offer as the Forge writes it: every field of the image's offers
* is required (a typo must not become a 0), `source` and the Elgif skill are
* optional as they are there.
*/
struct ForgeOffer
{
    std::string id;
    int32_t token_id = 0;
    std::string name;
    int32_t price = 0;
    int32_t limit = 0;
    int32_t reward_type = 0;
    int32_t target_id = 0;
    int32_t quantity = 0;
    std::optional<std::string> source;
    std::optional<int32_t> extra_skill;
};

/*! Why the Forge's offer cannot be sold, or nullopt (the boot check below, per offer, in plain words). */
std::optional<std::string> forgeOfferProblem(const ExchangeOffer& offer)
{
    const auto who = "Offer " + offer.id;
    if (offer.id.empty() || offer.id.size() > 64)
        return std::string("An offer id must be 1 to 64 characters.");
    if (offer.token_id <= 0 || !isKnownEventToken(std::to_string(offer.token_id)))
        return who + " is paid with token " + std::to_string(offer.token_id) + ", which is not an event token the game knows.";
    if (offer.price <= 0 || offer.limit <= 0 || offer.quantity <= 0)
        return who + ": the price, the limit and the quantity must each be at least 1.";
    if (offer.reward_type != 4 && offer.reward_type != 5 && offer.reward_type != 6 && offer.reward_type != 7)
        return who + ": the reward type must be 4 (item), 5 (material), 6 (unit) or 7 (sphere).";
    if (!validReward(offer))
        return who + (offer.reward_type == 6 ? " gives unit " + std::to_string(offer.target_id) + ", which is not in the unit archive."
                                             : " gives item " + std::to_string(offer.target_id) + ", which is not in the item table.");
    if (offer.extra_skill != 0)
    {
        if (offer.reward_type != 6)
            return who + ": only a unit offer can carry an Elgif skill.";
        const auto& skills = theServer()->cache().initializeResp().extra_passive_skills;
        if (std::none_of(skills.begin(), skills.end(), [&offer](const auto& s) { return s.id == offer.extra_skill; }))
            return who + ": Elgif skill " + std::to_string(offer.extra_skill) + " is not in extra_passive_skill_mst.";
    }
    for (const auto& featured : rotationOffers())
        if (featured.id == offer.id)
            return who + ": the id is already used by a featured event's Bazaar (event_rotation.json).";
    return std::nullopt;
}

/*! The catalog with the featured events' Bazaars, if every offer passes the boot check; else why not. */
std::optional<std::string> tryCatalog(std::vector<ExchangeOffer> offers)
{
    // The featured events' Bazaars (event_rotation.json): one catalog, so the
    // id and reward checks below cover them too.
    offers.insert(offers.end(), rotationOffers().begin(), rotationOffers().end());
    std::set<std::string> ids;
    for (const auto& offer : offers)
        if (offer.id.empty() || !ids.insert(offer.id).second || offer.token_id <= 0 || !validReward(offer))
            return "Invalid event exchange offer: " + offer.id;
    eventOffers = std::move(offers);
    return std::nullopt;
}

/*! The Forge's event_exchange.json was accepted record by record but the catalog would not load with it. */
void blameForgeExchange(const std::string& why)
{
    auto status = forge::archiveStatus("event_exchange.json").value_or(forge::ArchiveStatus{});
    status.applied = false;
    status.recordsReplaced = status.recordsAdded = status.recordsDeleted = 0;
    status.error = "With these offers the Bazaar does not load (" + why + "); the game's own offers are served.";
    LOG_ERROR << "forge: archive event_exchange.json REJECTED: " << status.error;
    forge::setArchiveStatus("event_exchange.json", std::move(status));
}
}

void loadExchangeArchive(const std::string& root)
{
    // A bad offer in the image's own files still stops the boot, as always.
    // The Forge's files never do: each one that makes the catalog fail is
    // reported and dropped, the exchange overlay first, then the rotation.
    const auto base = LoadJson<std::vector<ExchangeOffer>>(root, "event_exchange.json");
    auto merged = forge::mergeRecords<ExchangeOffer>("event_exchange.json", base,
        [](const ExchangeOffer& o) { return o.id; },
        [](const std::string& text, ExchangeOffer& out) -> std::optional<std::string> {
            ForgeOffer offer{};
            if (auto why = forge::readStrict(offer, text))
                return why;
            out = ExchangeOffer{ offer.id, offer.token_id, offer.name, offer.price, offer.limit, offer.reward_type,
                                 offer.target_id, offer.quantity, offer.source.value_or(""), offer.extra_skill.value_or(0) };
            return std::nullopt;
        },
        forgeOfferProblem, /*allowDelete=*/true);
    const auto status = forge::archiveStatus("event_exchange.json");
    const bool forgeExchange = status && status->applied;

    auto why = tryCatalog(forgeExchange ? merged : base);
    if (why && forgeExchange)
    {
        if (!tryCatalog(base))
        {
            blameForgeExchange(*why);
            why.reset();
        }
    }
    if (why && eventRotationFromForge())
    {
        fallBackToBaseEventRotation(root, "Its Bazaar offers are invalid or clash with the Bazaar (" + *why
            + "); the game's own rotation is served.");
        why = tryCatalog(forgeExchange ? merged : base);
        if (why && forgeExchange && !tryCatalog(base))
        {
            blameForgeExchange(*why);
            why.reset();
        }
    }
    if (why)
        throw std::runtime_error(*why);
    LOG_INFO << "Event exchange: " << eventOffers.size() << " offers";
}

const std::vector<ExchangeOffer>& eventExchangeCatalog() { return eventOffers; }

std::optional<ExchangeOffer> guildExchangeOffer(int32_t id)
{
    for (const auto& row : theServer()->cache().guildPointExchangeMst())
    {
        if (row.id != id || row.reward_info.size() < 3) continue;
        ExchangeOffer offer{};
        offer.id = std::to_string(id);
        offer.name = row.name;
        offer.price = row.price;
        offer.limit = row.limit_count;
        try
        {
            offer.reward_type = std::stoi(row.reward_info[0]);
            offer.target_id = std::stoi(row.reward_info[1]);
            offer.quantity = std::stoi(row.reward_info[2]);
        }
        catch (const std::exception&) { return std::nullopt; }
        if (validReward(offer)) return offer;
        return std::nullopt;
    }
    return std::nullopt;
}

drogon::Task<std::vector<GuildExchangeStock>> loadGuildExchangeStock(
    db::Database database, UserIdentity identity)
{
    std::map<std::string, int32_t> counts;
    for (const auto& row : co_await database->execSqlCoro(
        "SELECT offer_id, count FROM user_exchange_purchases WHERE user_id=$1 AND shop='guild';", identity.userId))
        counts[row["offer_id"].as<std::string>()] = row["count"].as<int32_t>();
    std::vector<GuildExchangeStock> result;
    for (const auto& offer : theServer()->cache().guildPointExchangeMst())
        if (guildExchangeOffer(offer.id))
            result.push_back({offer.id, counts[std::to_string(offer.id)], -1, 0});
    co_return result;
}

drogon::Task<std::vector<EventExchangeOffer>> loadEventExchangeOffers(
    db::Database database, UserIdentity identity, int32_t token)
{
    // A featured event's Bazaar books each run under its own shop, so the
    // counts (and the remaining stock) are this run's; a closed one lists nothing.
    const auto shop = eventShopFor(token, unixNow());
    if (!shop) co_return std::vector<EventExchangeOffer>{};
    std::map<std::string, int32_t> counts;
    for (const auto& row : co_await database->execSqlCoro(
        "SELECT offer_id, count FROM user_exchange_purchases WHERE user_id=$1 AND shop=$2;", identity.userId, *shop))
        counts[row["offer_id"].as<std::string>()] = row["count"].as<int32_t>();
    std::vector<EventExchangeOffer> result;
    for (const auto& offer : eventOffers)
    {
        if (offer.token_id != token) continue;
        EventExchangeOffer row{};
        row.id = offer.id;
        row.name = offer.name;
        row.order = static_cast<int32_t>(result.size() + 1);
        row.price = offer.price;
        row.remaining = std::max(0, offer.limit - counts[offer.id]);
        row.reward = std::to_string(offer.reward_type) + ':' + std::to_string(offer.target_id)
            + ':' + std::to_string(offer.quantity) + ":0:0";
        result.push_back(std::move(row));
    }
    co_return result;
}

drogon::Task<std::string> purchaseExchange(db::Database database,
    UserIdentity identity, ExchangeOffer offer, int32_t count, bool guild,
    GrantedRewards& granted)
{
    if (count <= 0 || count > offer.limit || !validReward(offer))
        co_return "Invalid purchase quantity or reward";
    const auto quantity = static_cast<int64_t>(count) * offer.quantity;
    if (quantity > 100000) co_return "Purchase quantity too large";
    const auto cost = static_cast<int64_t>(count) * offer.price;
    const auto eventShop = guild ? std::optional<std::string>("guild") : eventShopFor(offer.token_id, unixNow());
    if (!eventShop) co_return "This Bazaar has closed for the week";
    const auto& shop = *eventShop;
    const auto bought = co_await database->execSqlCoro(
        "INSERT INTO user_exchange_purchases(user_id,shop,offer_id,count) VALUES($1,$2,$3,$4)"
        " ON CONFLICT(user_id,shop,offer_id) DO UPDATE SET count=count+$4"
        " WHERE count+$4 <= $5 RETURNING count;",
        identity.userId, shop, offer.id, count, offer.limit);
    if (bought.empty()) co_return "Purchase limit reached";
    if (guild)
    {
        const auto paid = co_await database->execSqlCoro(
            "UPDATE user_info SET guild_tokens=guild_tokens-$1 WHERE id=$2 AND guild_tokens >= $1 RETURNING guild_tokens;",
            cost, identity.userId);
        if (paid.empty()) co_return "Not enough Guild Tokens";
    }
    else
    {
        const auto paid = co_await database->execSqlCoro(
            "UPDATE user_event_tokens SET count=count-$1 WHERE user_id=$2 AND token_id=$3 AND count >= $1 RETURNING count;",
            cost, identity.userId, std::to_string(offer.token_id));
        if (paid.empty()) co_return "Not enough event tokens";
    }
    if (offer.reward_type == 6 && offer.extra_skill > 0)
    {
        // An Elgif carries its skill on the unit row, which only the present
        // box path writes (PresentReceipt, as for first-clear Elgifs).
        co_await addUserPresent(database, identity, 6, std::to_string(offer.target_id),
            static_cast<int32_t>(quantity), 0, {}, offer.extra_skill);
    }
    else if (offer.reward_type == 6)
    {
        for (int64_t i = 0; i < quantity; ++i)
        {
            const auto unit = fromArchivedUnit(offer.target_id, UnitArchiver::getRandomType());
            if (!unit) co_return "Missing unit archive";
            const auto added = (co_await addUserUnit(database, identity, *unit)).nonEmpty();
            granted.userUnitIds.push_back(added.user_unit_id);
        }
    }
    else
    {
        (co_await addUserItem(database, identity, offer.target_id, static_cast<uint32_t>(quantity))).nonEmpty();
        granted.items = true;
    }
    co_return std::string{};
}
}
