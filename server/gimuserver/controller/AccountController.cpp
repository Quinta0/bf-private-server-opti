#include "App.hpp"
#include "AccountController.hpp"

#include <gimuserver/db/DatabaseInterface.h>
#include <gimuserver/gme/common/Transactions.hpp>
#include <gimuserver/utils/AccountSecurity.hpp>
#include <gimuserver/utils/LogRedaction.hpp>

#include <ctime>
#include <optional>
#include <sstream>
#include <stdexcept>

using namespace drogon;
using namespace drogon::orm;

namespace
{

/*!
* Whether a device identifier can identify a device: empty and all-zero ids
* (a limited-ad-tracking advertising id) cannot.
*/
bool usableDeviceId(const std::string& value)
{
    return value.size() >= 8 && value.find_first_not_of("0-") != std::string::npos;
}

std::string paramValue(const drogon::SafeStringMap<std::string>& params, const char* name)
{
    const auto it = params.find(name);
    return it == params.end() ? std::string{} : it->second;
}

/*!
* Stable per-device key: SHA-256 of the first identifier the client provides,
* tagged with its kind.  Android's ANDROID_ID (sent as JSON in "identifiers")
* is scoped to the APK signing key, so it survives a reinstall of the same
* build; the advertising ids are fallbacks for clients that omit it.
*/
std::optional<std::string> deriveDeviceKey(const drogon::SafeStringMap<std::string>& params)
{
    const auto param = [&params](const char* name) { return paramValue(params, name); };

    std::string androidId;
    if (const auto identifiers = param("identifiers"); !identifiers.empty())
    {
        Json::Value json;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream in(identifiers);
        if (Json::parseFromStream(builder, in, &json, &errors) && json.isObject())
            androidId = json.get("android_id", "").asString();
    }

    const std::pair<const char*, std::string> candidates[] = {
        { "android_id", androidId },
        { "device_id", param("device_id") },
        { "altvid", param("altvid") },
        { "vid", param("vid") },
    };
    for (const auto& [kind, value] : candidates)
    {
        if (usableDeviceId(value))
            return account::Sha256Hex(std::string(kind) + ":" + value);
    }
    return std::nullopt;
}

/*!
* Second way to recognize a device: SHA-256 of the advertising id together
* with altvid, or nothing when either is unusable.  BlueStacks and MuMu
* sometimes hand the game a new ANDROID_ID after a crash or a long session
* while both of these stay (ticket #69); altvid itself changes on a
* reinstall, so the pair does not follow an account to a fresh install.
*/
std::optional<std::string> deriveDeviceHint(const drogon::SafeStringMap<std::string>& params)
{
    const auto vid = paramValue(params, "vid");
    const auto altvid = paramValue(params, "altvid");
    if (!usableDeviceId(vid) || !usableDeviceId(altvid))
        return std::nullopt;
    return account::Sha256Hex("vid+altvid:" + vid + ":" + altvid);
}

/*!
* The account this device played before its device key changed, moved onto
* the new key; nothing when the hint does not pick out exactly one account.
*
* A row's hint counts only while the row still has the device key it arrived
* with (device_hint_key): a transfer or an admin tooling move that gives the row
* another phone's key leaves the old phone's hint behind, and that phone must
* not pull the account back before the new phone has logged in.  Only a row
* with a player qualifies, and two of them (a cloned emulator instance, say)
* leave the device to a new login as before.
*/
drogon::Task<std::optional<std::string>> rebindByDeviceHint(const std::string& deviceKey, const std::string& hint)
{
    const auto found = co_await theDb()->execSqlCoro(
        "SELECT g.id AS id FROM gumi_live_users g"
        " WHERE g.device_hint=$1 AND g.device_hint_key=g.device_key AND g.unbound_at=0"
        " AND EXISTS (SELECT 1 FROM user_info u WHERE u.gumi_user_id=g.id) LIMIT 2;", hint);
    if (found.size() != 1)
    {
        if (!found.empty())
            LOG_WARN << "AccountController: new device key, but its advertising id and altvid match several accounts; new login";
        co_return std::nullopt;
    }

    const auto id = found[0]["id"].as<std::string>();
    const auto moved = co_await theDb()->execSqlCoro(
        "UPDATE gumi_live_users SET device_key=$1 WHERE id=$2 AND device_hint=$3;", deviceKey, id, hint);
    if (moved.affectedRows() != 1)
        co_return std::nullopt;
    LOG_INFO << "AccountController: gumi user " << id
        << " kept its account on a new device key (same advertising id and altvid)";
    co_return id;
}

/*!
* Binds this device to the gumi user an account transfer claimed for it.
*
* A Transfer sent without a session (a fresh install that has not tapped the
* title yet) cannot move a device key it does not know, so it leaves a claim
* keyed on the advertising id -- the only identifier TransferRequest carries,
* sent here as "vid" (gme/handlers/Transfer.cpp, path B).  The claim is
* single-use.
*
* The advertising id is not a secret (every ad SDK on the device sees it), so
* anyone holding some account's code could aim a claim at someone else's
* device.  A claim is therefore only redeemed on a device that has nothing to
* lose: no gumi user yet, or one that never created a player (no user_info
* row).  On a device whose account has progress the claim is dropped instead;
* the target account stays where it was, and a real transfer to such a device
* goes through path A (Options -> Transfer, with a session).
*/
drogon::Task<> applyTransferClaim(const drogon::SafeStringMap<std::string>& params, const std::string& deviceKey)
{
    const auto vid = params.find("vid");
    if (vid == params.end() || !usableDeviceId(vid->second))
        co_return;

    const auto vidHash = account::Sha256Hex("vid:" + vid->second);
    const auto now = static_cast<int64_t>(std::time(nullptr));
    const auto claim = co_await theDb()->execSqlCoro(
        "SELECT to_gumi_user_id FROM transfer_device_claims WHERE vid_hash=$1 AND expires_at>$2;",
        vidHash, now);
    if (claim.empty())
        co_return;
    const auto target = claim[0]["to_gumi_user_id"].as<std::string>();

    const auto current = co_await theDb()->execSqlCoro(
        "SELECT g.id AS id FROM gumi_live_users g JOIN user_info u ON u.gumi_user_id=g.id"
        " WHERE g.device_key=$1 AND g.id<>$2 LIMIT 1;", deviceKey, target);
    if (!current.empty())
    {
        co_await theDb()->execSqlCoro("DELETE FROM transfer_device_claims WHERE vid_hash=$1;", vidHash);
        LOG_WARN << "AccountController: transfer claim for gumi user " << target
            << " dropped: this device already plays account of gumi user " << current[0]["id"].as<std::string>();
        co_return;
    }

    auto tx = co_await gme::newTransaction(theDb());
    try
    {
        // Order matters: the unique index on device_key forbids two rows
        // holding the key even for a moment.  Only a row without a player
        // can match here (checked above).
        co_await tx->execSqlCoro(
            "UPDATE gumi_live_users SET device_key=NULL, token_hash=NULL, unbound_at=$1"
            " WHERE device_key=$2 AND id<>$3"
            " AND NOT EXISTS (SELECT 1 FROM user_info WHERE user_info.gumi_user_id=gumi_live_users.id);",
            now, deviceKey, target);
        co_await tx->execSqlCoro(
            "UPDATE gumi_live_users SET device_key=$1, unbound_at=0 WHERE id=$2;", deviceKey, target);
        co_await tx->execSqlCoro("DELETE FROM transfer_device_claims WHERE vid_hash=$1;", vidHash);
    }
    catch (...)
    {
        tx->rollback();
        throw;
    }
    if (!(co_await gme::CommitTransaction(std::move(tx))))
        throw std::runtime_error("could not commit the transfer claim");
    LOG_INFO << "AccountController: transfer claim bound this device to gumi user " << target;
}

/*!
* Gumi user for this device, creating it on first login.  A save made before
* per-device accounts existed has a single row without a device key; the first
* device to log in adopts it so that player keeps their account.  A device
* whose key is new but whose hint is known keeps the account it had.
*/
drogon::Task<std::string> findOrCreateGumiUser(const std::string& deviceKey, const std::optional<std::string>& hint)
{
    const auto existing = co_await db::DatabaseInterface::read(
        theDb(), "gumi_live_users",
        { db::Data("id"), db::Lookup("device_key", deviceKey) });
    if (existing.affected > 0)
        co_return existing.front<std::string>("id");

    if (hint)
    {
        if (const auto rebound = co_await rebindByDeviceHint(deviceKey, *hint))
            co_return *rebound;
    }

    // unbound_at marks a row an account transfer took the device key from;
    // that account belongs to whoever transferred away, never to a newcomer.
    const auto all = co_await db::DatabaseInterface::read(
        theDb(), "gumi_live_users", { db::Data("id"), db::Data("device_key"), db::Data("unbound_at") });
    if (all.affected == 1 && all.data.front()["device_key"].isNull()
        && all.data.front()["unbound_at"].as<int64_t>() == 0)
    {
        const auto legacyId = all.front<std::string>("id");
        (co_await db::DatabaseInterface::update(
            theDb(), "gumi_live_users",
            { db::Data("device_key", deviceKey), db::Lookup("id", legacyId) })).nonEmpty();
        LOG_INFO << "AccountController: device adopted legacy gumi user " << legacyId;
        co_return legacyId;
    }

    const auto now = static_cast<int64_t>(std::time(nullptr));
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto id = account::SecureRandomId(8);
        const auto inserted = co_await db::DatabaseInterface::insert(
            theDb(), "gumi_live_users",
            {
                db::Data("id", id),
                db::Data("device_key", deviceKey),
                db::Data("created_at", now),
                db::Data("last_login_at", now),
            });
        if (inserted.affected > 0)
        {
            LOG_INFO << "AccountController: new gumi user " << id;
            co_return id;
        }
    }
    throw std::runtime_error("could not allocate a gumi user id");
}

} // namespace

Task<> AccountController::HandleGuest(HttpRequestPtr rq, std::function<void(const HttpResponsePtr&)> callback)
{
    DumpLog log;
    theServer()->tryOpenHttpDumpLog("guestLogin", log);
    log << rq << "\n";

    // Extract parameters
    const auto& params = rq->getParameters();
    GuestLogin login{};
    login.status = StatusEnum::Error;
    login.status_number = 1; // TODO: what was this again?

    try
    {
        // First server call of the login flow.  The client sends device
        // identifiers but no gumi user id, so the device decides the account:
        // the same device always gets the same gumi user, and every login mints
        // a new session token that GME requests must present (see
        // gme::getUserIdentity).  Only hashes of the identifier and token are
        // stored.
        const auto deviceKey = deriveDeviceKey(params);
        if (!deviceKey)
        {
            LOG_WARN << "AccountController: guest login without a usable device identifier";
            throw std::runtime_error("no device identifier");
        }

        co_await applyTransferClaim(params, *deviceKey);
        const auto hint = deriveDeviceHint(params);
        const auto gumiUserId = co_await findOrCreateGumiUser(*deviceKey, hint);
        const auto token = account::SecureRandomHex(16);
        const auto now = static_cast<int64_t>(std::time(nullptr));
        // The hint is stored with the key it came with (see rebindByDeviceHint).
        const auto updated = co_await theDb()->execSqlCoro(
            "UPDATE gumi_live_users SET token_hash=$1, last_login_at=$2,"
            " device_hint=NULLIF($3, ''), device_hint_key=$4 WHERE id=$5;",
            account::Sha256Hex(token), now, hint.value_or(""), *deviceKey, gumiUserId);
        if (updated.affectedRows() != 1)
            throw std::runtime_error("the gumi user row is gone");

        login.user_id = gumiUserId;
        login.token = token;
        login.status = StatusEnum::Success;
        login.status_number = 0;
    }
    catch (const DrogonDbException& ex)
    {
        LOG_ERROR << "AccountController: login failed " << ex.base().what();
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR << "AccountController: login failed " << ex.what();
    }

    auto resp = HttpResponse::newHttpResponse();
    resp->setCloseConnection(true); // we live the session torugh the gme controllers from the token...

    std::string output{};
    auto ec = glz::write_json(login, output);
    if (ec)
    {
        LOG_ERROR << "AccountController: Cannot serialize GuestLogin " << ec;
        resp->setStatusCode(k500InternalServerError);
        log << "Serialization error!";
    }
    else
    {
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(ContentType::CT_APPLICATION_JSON);
        resp->setBody(output);
        // The fresh session token stays out of the dump; only its hash is
        // kept anywhere.
        log << logredact::RedactJsonStrings(output, { "token" });
    }

    callback(resp);
}


static HttpResponsePtr jsonStatus(const char* status)
{
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(ContentType::CT_APPLICATION_JSON);
    resp->setBody(std::string(R"({"status":")") + status + R"("})");
    return resp;
}

void AccountController::HandleGameUserUpdate(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
    DumpLog log;
    theServer()->tryOpenHttpDumpLog("gameUserUpdate", log);
    log << rq << "\n";
    callback(jsonStatus("successful"));
}

void AccountController::HandleGuestGet(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
    DumpLog log;
    theServer()->tryOpenHttpDumpLog("guestGet", log);
    log << rq << "\n";
    callback(jsonStatus("not exist"));
}
