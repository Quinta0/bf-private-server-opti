#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/db/Types.h>
#include <gimuserver/gme/common/Transactions.hpp>
#include <gimuserver/utils/AccountSecurity.hpp>

#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// Account transfer codes: move a save to another device.
//
//   TransferCodeIssue  LK9e0WyX / B6siV3AT  Options -> Transfer, old device
//   TransferCodeCheck  Fn1UzTo3 / A6QiFT7D  Confirm screen, new device
//   Transfer           I9DyF2aU / 2hp90fZ1  after Confirm OK, new device
//
// Spec, client evidence and open questions: re/notes/https_transfer_re.json,
// entry "transfer".  Wire shapes: net/transfer.kdl.
//
// What the client forces on this design:
//
// * Every refusal goes out as HandleResult::refuseToRetry.  The client shows
//   it as notice -4000, and each transfer scene's own noticeOK routes that
//   back to its input screen.  A thrown exception would instead become
//   ReturnToGame (a Home rebuild, useless at the title) or, for SessionError,
//   Close.  So no handler here lets anything escape.
// * Confirm OK swaps the TARGET id into the client's keychain BEFORE Transfer
//   is sent.  A Transfer that fails after a successful Check strands the
//   player, so Transfer forgives what a recent Check already judged: a lock
//   placed after that Check (with the right password), a code that expired in
//   between, and a retried request.  Without a Check in the last 15 minutes a
//   lock holds for Transfer too.
// * Nothing secret reaches the HTTP dump logs: the dispatcher masks the code,
//   password and session token (utils/LogRedaction.hpp).
// * The account moves by moving gumi_live_users.device_key to the target's
//   gumi user, never by re-pointing user_info.gumi_user_id: Energy, Gacha,
//   Present, Mission and Tutorial address user_info by gumi_user_id alone and
//   need one row per gumi user.
// * Check and Transfer do not call gme::getUserIdentity.  Transfer's
//   h7eY3sAK is already the target, which would throw a mismatch, and a fresh
//   install may have no session at all.

namespace
{

constexpr const char* kBadCodeMessage = "The Transfer ID or password is incorrect.";
constexpr const char* kRateLimitedMessage = "Too many attempts. Please wait 15 minutes and try again.";
constexpr const char* kLockedMessage =
	"This Transfer ID is locked after too many failed attempts. Issue a new code on the original device.";
constexpr const char* kOwnCodeMessage = "This Transfer ID belongs to the account you are already using.";
constexpr const char* kExpiredMessage = "This Transfer ID has expired. Issue a new code on the original device.";
constexpr const char* kNoSessionMessage = "Please restart the game, tap the title screen once, then try again.";
constexpr const char* kBadPasswordMessage = "Password must be 4-12 letters or numbers.";
constexpr const char* kServerErrorMessage = "Server error, please try again.";

// Echoed signal key, as the other handlers that have no request key to copy.
constexpr const char* kSignalKey = "5EdKHavF";

// Failed Check/Transfer attempts allowed per requester per window.  A
// requester is the gumi user, or without a session the device identifier the
// request carries (push token on Check, advertising id on Transfer).  Only a
// request with neither falls into the server-wide "anon" bucket: handlers
// cannot see the peer address, so a per-IP limit is not possible yet.
constexpr int64_t kAttemptWindowSeconds = 900;
constexpr int64_t kMaxFailuresPerUser = 5;
constexpr int64_t kMaxFailuresAnonymous = 30;
constexpr int64_t kIssueWindowSeconds = 3600;
constexpr int64_t kMaxIssuesPerWindow = 5;
constexpr int64_t kAttemptRetentionSeconds = 86400;

// A code locks for a day after this many wrong passwords.
constexpr int64_t kMaxFailedPasswords = 10;
constexpr int64_t kLockSeconds = 86400;

// How long Transfer still honours a code after Check accepted it, and how
// long a retried Transfer is answered from the audit trail.
constexpr int64_t kTransferGraceSeconds = 900;
constexpr int64_t kReplayWindowSeconds = 600;

constexpr int kIssueCodeTries = 8;
constexpr const char* kAnonymousRequester = "anon";

// Derivations allowed queued or running on the KDF thread at once (each takes
// about 0.6 s at the default work factor).  Past this, requests are refused
// with M_RATE instead of piling up as suspended coroutines with open
// connections behind an attacker's burst.
constexpr int kMaxKdfJobs = 8;

int64_t nowSeconds()
{
	return static_cast<int64_t>(std::time(nullptr));
}

std::optional<std::string> optionalText(const drogon::orm::Field& field)
{
	if (field.isNull())
		return std::nullopt;
	return field.as<std::string>();
}

/*!
* The single thread PBKDF2 runs on.  Each derivation takes a noticeable
* fraction of a second by design; on an IO loop it would stall every request
* that loop serves.  Started on first use.
*/
trantor::EventLoop* kdfLoop()
{
	static trantor::EventLoopThread thread("TransferKdf");
	static std::once_flag started;
	std::call_once(started, [] { thread.run(); });
	return thread.getLoop();
}

std::atomic<int> g_kdfJobs{ 0 };

/*!
* Runs `work` on the KDF thread and resumes on the caller's loop; nullopt
* (nothing run) when kMaxKdfJobs derivations are already queued or running.
*/
template <typename T>
drogon::Task<std::optional<T>> offLoop(std::function<T()> work)
{
	if (g_kdfJobs.fetch_add(1) >= kMaxKdfJobs)
	{
		g_kdfJobs.fetch_sub(1);
		LOG_WARN << "Transfer: KDF queue full, request refused";
		co_return std::nullopt;
	}
	struct Release
	{
		~Release() { g_kdfJobs.fetch_sub(1); }
	} release;

	T result{};
	co_await drogon::queueInLoopCoro(
		kdfLoop(), [&result, &work] { result = work(); },
		trantor::EventLoop::getEventLoopOfCurrentThread());
	co_return result;
}

uint32_t kdfIterations()
{
	return theServer()->cache().serverConfig().transferPbkdf2Iterations;
}

/*!
* One throwaway derivation, so an unknown code costs the same time as a wrong
* password and the reply time does not reveal which codes exist.  False when
* the KDF queue is full.
*/
drogon::Task<bool> dummyDerivation(const std::string& password)
{
	const auto iterations = kdfIterations();
	const auto ran = co_await offLoop<bool>([&password, iterations] {
		account::HashTransferPassword(password, iterations);
		return true;
	});
	co_return ran.has_value();
}

template <typename Req>
std::optional<Req> parseRequest(const std::string& json, const char* name)
{
	Req req{};
	if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json); ec)
	{
		LOG_WARN << name << ": parse error: " << glz::format_error(ec, json);
		return std::nullopt;
	}
	return req;
}

template <typename Resp>
HandleResult reply(const Resp& resp)
{
	std::string body;
	if (const auto ec = glz::write_json(resp, body); ec)
		return HandleResult::refuseToRetry(kServerErrorMessage, glz::format_error(ec, body));
	return HandleResult::success(body);
}

/*!
* The requester's gumi session, when it has a valid one.
*/
struct TransferSession
{
	std::string gumiUserId;
	std::optional<std::string> deviceKey;
	std::optional<std::string> tokenHash;
	// user_info.id bound to gumiUserId; '' before the tutorial created one.
	std::string userId;
};

/*!
* Validates the request's gumi pair the way gme::getUserIdentity does, but
* answers nullopt instead of throwing.  The client's h7eY3sAK is not
* compared: on Transfer it is already the target.
*/
drogon::Task<std::optional<TransferSession>> resolveSession(
	const std::string& gumiUserId, const std::string& token)
{
	if (gumiUserId.empty())
		co_return std::nullopt;
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT device_key, token_hash FROM gumi_live_users WHERE id=$1;", gumiUserId);
	if (rows.empty())
		co_return std::nullopt;

	auto tokenHash = optionalText(rows[0]["token_hash"]);
	if (theServer()->cache().serverConfig().requireSessionToken
		&& (!tokenHash || token.empty()
			|| !account::ConstantTimeEquals(*tokenHash, account::Sha256Hex(token))))
		co_return std::nullopt;

	const auto user = co_await theDb()->execSqlCoro(
		"SELECT id FROM user_info WHERE gumi_user_id=$1;", gumiUserId);
	co_return TransferSession{
		.gumiUserId = gumiUserId,
		.deviceKey = optionalText(rows[0]["device_key"]),
		.tokenHash = std::move(tokenHash),
		.userId = user.empty() ? std::string{} : user[0]["id"].as<std::string>(),
	};
}

/*!
* Whether a client-supplied identifier can key a rate-limit bucket: the same
* rule as a usable advertising id (not empty, not the all-zero opt-out id).
*/
bool usableClientId(const std::string& value)
{
	return value.size() >= 8 && value.find_first_not_of("0-") != std::string::npos;
}

/*!
* Rate-limit bucket of a request: its gumi user, else its device identifier,
* else the shared "anon" bucket.  The identifier is the client's word, so a
* determined attacker can rotate it (as he can mint gumi users); what bounds
* guessing is the per-code lock, and what bounds load is kMaxKdfJobs.  The
* bucket only keeps one honest no-session device from exhausting the budget
* of every other one.
*/
std::string requesterOf(const std::optional<TransferSession>& session, const std::string& clientId)
{
	if (session)
		return session->gumiUserId;
	if (!usableClientId(clientId))
		return kAnonymousRequester;
	return "anon:" + account::Sha256Hex("client:" + clientId).substr(0, 32);
}

/*!
* Opens an attempt, counted as a failure until settleAttempt says otherwise,
* and drops rows older than a day.  Opening it BEFORE the slow derivation is
* what makes the limits hold against a burst of concurrent requests: each one
* is already counted when the next one checks the limit.
*/
drogon::Task<int64_t> openAttempt(const std::string& requester, const char* kind)
{
	const auto now = nowSeconds();
	const auto inserted = co_await theDb()->execSqlCoro(
		"INSERT INTO account_transfer_attempts(requester, kind, ok, at) VALUES($1,$2,0,$3);",
		requester, std::string(kind), now);
	co_await theDb()->execSqlCoro(
		"DELETE FROM account_transfer_attempts WHERE at<$1;", now - kAttemptRetentionSeconds);
	co_return static_cast<int64_t>(inserted.insertId());
}

/*!
* Marks an open attempt as a success (it stops counting as a failure).
*/
drogon::Task<> settleAttempt(int64_t attemptId)
{
	co_await theDb()->execSqlCoro("UPDATE account_transfer_attempts SET ok=1 WHERE id=$1;", attemptId);
}

/*!
* Forgets an open attempt that was refused before any code was judged (rate
* limit, full KDF queue), so waiting out the window is enough.
*/
drogon::Task<> dropAttempt(int64_t attemptId)
{
	co_await theDb()->execSqlCoro("DELETE FROM account_transfer_attempts WHERE id=$1;", attemptId);
}

/*!
* Whether the requester is over its failure budget, counting the attempt it
* has just opened.
*/
drogon::Task<bool> failuresExhausted(const std::string& requester)
{
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT COUNT(*) AS n FROM account_transfer_attempts"
		" WHERE requester=$1 AND kind IN ('check','transfer') AND ok=0 AND at>$2;",
		requester, nowSeconds() - kAttemptWindowSeconds);
	const auto limit = requester == kAnonymousRequester ? kMaxFailuresAnonymous : kMaxFailuresPerUser;
	co_return rows[0]["n"].as<int64_t>() > limit;
}

/*!
* Whether the requester issued too many codes, counting the issue it has just
* opened (pending or failed issues count too).
*/
drogon::Task<bool> issuesExhausted(const std::string& requester)
{
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT COUNT(*) AS n FROM account_transfer_attempts"
		" WHERE requester=$1 AND kind='issue' AND at>$2;",
		requester, nowSeconds() - kIssueWindowSeconds);
	co_return rows[0]["n"].as<int64_t>() > kMaxIssuesPerWindow;
}

/*!
* Outcome of judging a typed code and password: the account it opens (and the
* code row that proved it), or the message to refuse with.
*/
struct CodeVerdict
{
	std::string userId;
	std::string codeHash;
	const char* error = nullptr;
	// Refused because the KDF queue was full; nothing was judged.
	bool busy = false;

	bool ok() const { return error == nullptr; }
};

CodeVerdict busyVerdict()
{
	return CodeVerdict{ .error = kRateLimitedMessage, .busy = true };
}

drogon::Task<> recordWrongPassword(const std::string& codeHash, int64_t now)
{
	// SET expressions read the row's old values, so failed_attempts+1 is the
	// new count in both places.
	co_await theDb()->execSqlCoro(
		"UPDATE account_transfer_codes SET failed_attempts=failed_attempts+1,"
		" locked_until=CASE WHEN failed_attempts+1>=$1 THEN $2 ELSE locked_until END"
		" WHERE code_hash=$3;",
		kMaxFailedPasswords, now + kLockSeconds, codeHash);
}

/*!
* Judges a code and password.
*
* Check enforces the lock and the expiry date.  Transfer forgives what a
* recent Check already judged -- the client has switched accounts by then, so
* a refusal strands the player -- but only that: a lock or an expiry is
* overlooked when Check accepted this code within kTransferGraceSeconds (an
* expiry also within kTransferGraceSeconds of the date).  Without a recent
* Check, a locked code stays locked for Transfer too, so the lock bounds
* password guessing for anyone who learned a code.
*
* A malformed code is refused without a derivation: the format is public, so
* its timing hides nothing.  An unknown code costs one dummy derivation.
*/
drogon::Task<CodeVerdict> verifyCode(const std::string& typedCode, const std::string& password, bool forTransfer)
{
	const auto code = account::NormalizeTransferCode(typedCode);
	if (!code)
		co_return CodeVerdict{ .error = kBadCodeMessage };
	auto codeHash = account::TransferCodeHash(*code);
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT user_id, password_hash, expires_at, locked_until, last_check_ok_at"
		" FROM account_transfer_codes WHERE code_hash=$1;", codeHash);
	if (rows.empty())
	{
		if (!(co_await dummyDerivation(password)))
			co_return busyVerdict();
		co_return CodeVerdict{ .error = kBadCodeMessage };
	}

	const auto& row = rows[0];
	const auto now = nowSeconds();
	const auto expiresAt = row["expires_at"].as<int64_t>();
	const auto lastCheckOk = row["last_check_ok_at"].as<int64_t>();
	const bool checkedRecently = lastCheckOk != 0 && now - lastCheckOk <= kTransferGraceSeconds;

	if (row["locked_until"].as<int64_t>() > now && !(forTransfer && checkedRecently))
		co_return CodeVerdict{ .error = kLockedMessage };
	const bool live = forTransfer
		? expiresAt == 0 || now <= expiresAt + kTransferGraceSeconds || checkedRecently
		: expiresAt == 0 || now <= expiresAt;
	if (!live)
		co_return CodeVerdict{ .error = kExpiredMessage };

	const auto stored = row["password_hash"].as<std::string>();
	const auto passwordOk = co_await offLoop<bool>(
		[&password, &stored] { return account::VerifyTransferPassword(password, stored); });
	if (!passwordOk)
		co_return busyVerdict();
	if (!*passwordOk)
	{
		co_await recordWrongPassword(codeHash, nowSeconds());
		co_return CodeVerdict{ .error = kBadCodeMessage };
	}

	if (!forTransfer)
		co_await theDb()->execSqlCoro(
			"UPDATE account_transfer_codes SET failed_attempts=0, last_check_ok_at=$1 WHERE code_hash=$2;",
			nowSeconds(), codeHash);
	co_return CodeVerdict{ .userId = row["user_id"].as<std::string>(), .codeHash = std::move(codeHash) };
}

/*!
* Judges a Check or Transfer under the requester's failure budget: the
* attempt is opened (as a failure) before the budget is checked and before the
* derivation, then settled by the verdict.
*/
drogon::Task<CodeVerdict> judgeAttempt(
	const std::string& requester, const char* kind, const TransferLoginInfoReq& li, bool forTransfer)
{
	const auto attemptId = co_await openAttempt(requester, kind);
	if (co_await failuresExhausted(requester))
	{
		co_await dropAttempt(attemptId);
		co_return CodeVerdict{ .error = kRateLimitedMessage };
	}
	auto verdict = co_await verifyCode(li.transfer_code, li.password, forTransfer);
	if (verdict.busy)
		co_await dropAttempt(attemptId);
	else if (verdict.ok())
		co_await settleAttempt(attemptId);
	co_return verdict;
}

// ---------------------------------------------------------------- Issue

/*!
* Stores a fresh code for `userId` (replacing any earlier one, and its lock)
* and returns the plaintext.  A colliding code is looked up rather than
* caught: Drogon rolls the whole transaction back on any failed statement.
*/
drogon::Task<std::string> storeNewCode(
	const db::Database tx, const std::string& userId, const std::string& passwordHash,
	int64_t now, int64_t expiresAt)
{
	for (int attempt = 0; attempt < kIssueCodeTries; ++attempt)
	{
		auto code = account::NewTransferCode();
		const auto codeHash = account::TransferCodeHash(code);
		const auto taken = co_await tx->execSqlCoro(
			"SELECT 1 FROM account_transfer_codes WHERE code_hash=$1 AND user_id<>$2;", codeHash, userId);
		if (!taken.empty())
			continue;
		co_await tx->execSqlCoro(
			"INSERT INTO account_transfer_codes(user_id, code_hash, password_hash, issued_at, expires_at,"
			" failed_attempts, locked_until, last_check_ok_at) VALUES($1,$2,$3,$4,$5,0,0,0)"
			" ON CONFLICT(user_id) DO UPDATE SET code_hash=excluded.code_hash,"
			" password_hash=excluded.password_hash, issued_at=excluded.issued_at,"
			" expires_at=excluded.expires_at, failed_attempts=0, locked_until=0, last_check_ok_at=0;",
			userId, codeHash, passwordHash, now, expiresAt);
		co_return code;
	}
	throw std::runtime_error("no free transfer code after several tries");
}

drogon::Task<HandleResult> issueCode(const std::string& json)
{
	const auto req = parseRequest<TransferCodeIssueReq>(json, "TransferCodeIssue");
	if (!req)
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "unreadable request");
	const auto& li = req->login_info;

	const auto session = co_await resolveSession(li.gumi_live_userid, li.gumi_live_token);
	if (!session || session->userId.empty())
		co_return HandleResult::refuseToRetry(kNoSessionMessage, "no session or no account");
	if (!account::ValidTransferPassword(li.password))
		co_return HandleResult::refuseToRetry(kBadPasswordMessage);
	const auto attemptId = co_await openAttempt(session->gumiUserId, "issue");
	if (co_await issuesExhausted(session->gumiUserId))
	{
		co_await dropAttempt(attemptId);
		co_return HandleResult::refuseToRetry(kRateLimitedMessage);
	}

	const auto iterations = kdfIterations();
	const auto& password = li.password;
	const auto passwordHash = co_await offLoop<std::string>(
		[&password, iterations] { return account::HashTransferPassword(password, iterations); });
	if (!passwordHash)
	{
		co_await dropAttempt(attemptId);
		co_return HandleResult::refuseToRetry(kRateLimitedMessage, "KDF queue full");
	}

	const auto now = nowSeconds();
	const auto ttlDays = static_cast<int64_t>(theServer()->cache().serverConfig().transferCodeTtlDays);
	const auto expiresAt = ttlDays == 0 ? int64_t{ 0 } : now + ttlDays * 86400;

	std::string code;
	auto tx = co_await gme::newTransaction(theDb());
	try
	{
		code = co_await storeNewCode(tx, session->userId, *passwordHash, now, expiresAt);
	}
	catch (...)
	{
		tx->rollback();
		throw;
	}
	if (!(co_await gme::CommitTransaction(std::move(tx))))
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "issue commit failed");
	co_await settleAttempt(attemptId);
	LOG_INFO << "TransferCodeIssue: new code for user " << session->userId;

	TransferCodeIssueResp resp{};
	resp.signal_key.key = kSignalKey;
	resp.issued.transfer_code = std::move(code);
	resp.issued.password = li.password;
	resp.issued.code_expire_date = expiresAt;
	co_return reply(resp);
}

// ---------------------------------------------------------------- Check

drogon::Task<HandleResult> checkCode(const std::string& json)
{
	const auto req = parseRequest<TransferCodeCheckReq>(json, "TransferCodeCheck");
	if (!req)
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "unreadable request");
	const auto& li = req->login_info;

	const auto session = co_await resolveSession(li.gumi_live_userid, li.gumi_live_token);
	const auto requester = requesterOf(session, li.device_id);
	const auto verdict = co_await judgeAttempt(requester, "check", li, false);
	if (!verdict.ok())
		co_return HandleResult::refuseToRetry(verdict.error, "requester " + requester);
	if (session && verdict.userId == session->userId)
		co_return HandleResult::refuseToRetry(kOwnCodeMessage);

	const auto target = co_await theDb()->execSqlCoro(
		"SELECT id, username, level, model_change_count FROM user_info WHERE id=$1;", verdict.userId);
	if (target.empty())
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "code for a missing user " + verdict.userId);

	TransferCodeCheckResp resp{};
	resp.signal_key.key = kSignalKey;
	resp.target.user_id = target[0]["id"].as<std::string>();
	resp.target.handle_name = target[0]["username"].as<std::string>();
	resp.target.level = target[0]["level"].as<int32_t>();
	resp.target.model_change_count = target[0]["model_change_count"].as<int32_t>();
	resp.target.contact_id = "";
	co_return reply(resp);
}

// ---------------------------------------------------------------- Transfer

/*!
* The advertising id a Transfer carries, and its hash when it can identify a
* device (not empty, not the all-zero limited-tracking id).
*/
struct ClaimKey
{
	std::string vidHash;

	bool usable() const { return !vidHash.empty(); }
};

const std::string& claimIdOf(const TransferLoginInfoReq& li)
{
	return li.advertising_id.empty() ? li.device_id : li.advertising_id;
}

ClaimKey claimKeyOf(const TransferLoginInfoReq& li)
{
	const auto& adid = claimIdOf(li);
	if (!usableClientId(adid))
		return {};
	return { .vidHash = account::Sha256Hex("vid:" + adid) };
}

/*!
* Whether this exact Transfer already succeeded: the client may resend it
* after a lost reply, and by then the code is gone.  Matched on the sending
* session (path A) or the claimed advertising id (path B).
*/
drogon::Task<bool> alreadyTransferred(const TransferLoginInfoReq& li, const ClaimKey& claim)
{
	const auto tokenHash = li.gumi_live_token.empty() ? std::string{} : account::Sha256Hex(li.gumi_live_token);
	const auto rows = co_await theDb()->execSqlCoro(
		"SELECT 1 FROM account_transfers WHERE created_at>$1 AND to_user_id=$2"
		" AND ((from_gumi_user_id=$3 AND from_token_hash=$4) OR claim_vid_hash=$5) LIMIT 1;",
		nowSeconds() - kReplayWindowSeconds, li.user_id,
		li.gumi_live_userid, tokenHash, claim.vidHash);
	// '' never matches a stored id, token hash or vid hash, so an absent
	// session or advertising id simply cannot match its branch.
	co_return !rows.empty();
}

/*!
* Path A: the requester has a device-bound session, so its device key moves
* to the target's gumi user.  Order matters for the unique index on
* device_key.  The old device's token dies with it (SessionError -> Close on
* its next request; on relaunch it guest-logs-in to a new gumi user).
*/
drogon::Task<> moveDeviceKey(
	const db::Database tx, const TransferSession& session, const std::string& targetGumiId,
	const std::string& targetUserId, int64_t now)
{
	co_await tx->execSqlCoro(
		"UPDATE gumi_live_users SET device_key=NULL, token_hash=NULL, unbound_at=$1 WHERE id=$2;",
		now, session.gumiUserId);
	co_await tx->execSqlCoro(
		"UPDATE gumi_live_users SET device_key=$1, token_hash=NULL, unbound_at=0, last_login_at=$2 WHERE id=$3;",
		session.deviceKey, now, targetGumiId);
	co_await tx->execSqlCoro(
		"INSERT INTO account_transfers(from_gumi_user_id, from_user_id, from_token_hash, claim_vid_hash,"
		" to_gumi_user_id, to_user_id, via, created_at) VALUES($1,$2,$3,NULL,$4,$5,'session',$6);",
		session.gumiUserId, session.userId, session.tokenHash, targetGumiId, targetUserId, now);
}

/*!
* Path B: no device-bound session (a fresh install that has not tapped the
* title).  Leave a claim that the device's next guest login redeems by its
* advertising id (AccountController::HandleGuest), and end the target's
* current session.
*/
drogon::Task<> leaveDeviceClaim(
	const db::Database tx, const std::optional<TransferSession>& session, const ClaimKey& claim,
	const std::string& targetGumiId, const std::string& targetUserId, int64_t now)
{
	const auto ttl = static_cast<int64_t>(theServer()->cache().serverConfig().transferClaimTtlSeconds);
	co_await tx->execSqlCoro(
		"INSERT OR REPLACE INTO transfer_device_claims(vid_hash, to_gumi_user_id, created_at, expires_at)"
		" VALUES($1,$2,$3,$4);", claim.vidHash, targetGumiId, now, now + ttl);
	co_await tx->execSqlCoro("UPDATE gumi_live_users SET token_hash=NULL WHERE id=$1;", targetGumiId);

	// NULL, not '', when there was no session: the audit row then says so.
	std::optional<std::string> fromGumiId;
	std::string fromUserId;
	std::optional<std::string> fromTokenHash;
	if (session)
	{
		fromGumiId = session->gumiUserId;
		fromUserId = session->userId;
		fromTokenHash = session->tokenHash;
	}
	co_await tx->execSqlCoro(
		"INSERT INTO account_transfers(from_gumi_user_id, from_user_id, from_token_hash, claim_vid_hash,"
		" to_gumi_user_id, to_user_id, via, created_at) VALUES($1,$2,$3,$4,$5,$6,'claim',$7);",
		fromGumiId, fromUserId, fromTokenHash, claim.vidHash, targetGumiId, targetUserId, now);
}

enum class CommitOutcome
{
	Done,
	// The code was consumed (or replaced) after verifyCode read it: another
	// Transfer got there first.  Nothing changed.
	CodeGone,
	Failed,
};

/*!
* Moves the account in one transaction.  The code is consumed FIRST, by the
* exact hash verifyCode accepted, and only a request that deletes the row
* goes on: two concurrent Transfers of one code cannot both commit, and a
* code re-issued in between is not consumed by the old one.
*/
drogon::Task<CommitOutcome> commitTransfer(
	const std::optional<TransferSession>& session, const ClaimKey& claim,
	const std::string& targetGumiId, const std::string& targetUserId, const std::string& codeHash)
{
	const auto now = nowSeconds();
	auto tx = co_await gme::newTransaction(theDb());
	try
	{
		const auto consumed = co_await tx->execSqlCoro(
			"DELETE FROM account_transfer_codes WHERE user_id=$1 AND code_hash=$2;", targetUserId, codeHash);
		if (consumed.affectedRows() != 1)
		{
			tx->rollback();
			co_return CommitOutcome::CodeGone;
		}
		if (session && session->deviceKey)
			co_await moveDeviceKey(tx, *session, targetGumiId, targetUserId, now);
		else
			co_await leaveDeviceClaim(tx, session, claim, targetGumiId, targetUserId, now);
		co_await tx->execSqlCoro(
			"UPDATE user_info SET model_change_count=model_change_count+1 WHERE id=$1;", targetUserId);
	}
	catch (...)
	{
		tx->rollback();
		throw;
	}
	co_return (co_await gme::CommitTransaction(std::move(tx))) ? CommitOutcome::Done : CommitOutcome::Failed;
}

drogon::Task<HandleResult> transferAccount(const std::string& json)
{
	const auto req = parseRequest<TransferReq>(json, "Transfer");
	if (!req)
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "unreadable request");
	const auto& li = req->login_info;
	const auto claim = claimKeyOf(li);

	TransferResp resp{};
	resp.signal_key.key = kSignalKey;
	if (co_await alreadyTransferred(li, claim))
	{
		LOG_INFO << "Transfer: repeated request for user " << li.user_id << " answered from the audit trail";
		co_return reply(resp);
	}

	const auto session = co_await resolveSession(li.gumi_live_userid, li.gumi_live_token);
	const auto requester = requesterOf(session, claimIdOf(li));
	const auto verdict = co_await judgeAttempt(requester, "transfer", li, true);
	if (!verdict.ok())
		co_return HandleResult::refuseToRetry(verdict.error, "requester " + requester);
	if (li.user_id != verdict.userId)
		LOG_WARN << "Transfer: client named user " << li.user_id << " but the code belongs to "
			<< verdict.userId << "; the code wins";
	if (session && verdict.userId == session->userId)
		co_return HandleResult::refuseToRetry(kOwnCodeMessage);

	const auto target = co_await theDb()->execSqlCoro(
		"SELECT gumi_user_id FROM user_info WHERE id=$1;", verdict.userId);
	const auto targetGumiId = target.empty() ? std::string{} : target[0]["gumi_user_id"].as<std::string>();
	if (targetGumiId.empty() || (session && targetGumiId == session->gumiUserId))
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "no gumi user for " + verdict.userId);
	if ((!session || !session->deviceKey) && !claim.usable())
		co_return HandleResult::refuseToRetry(kNoSessionMessage, "no session and no usable advertising id");

	if (session && session->deviceKey && !session->userId.empty())
		LOG_WARN << "Transfer: gumi user " << session->gumiUserId << " leaves its account "
			<< session->userId << " unbound; only an admin can restore it";
	switch (co_await commitTransfer(session, claim, targetGumiId, verdict.userId, verdict.codeHash))
	{
	case CommitOutcome::Done:
		break;
	case CommitOutcome::CodeGone:
		// A concurrent duplicate of this very request committed: answer as a
		// replay.  Anyone else lost the race for the code.
		if (co_await alreadyTransferred(li, claim))
			co_return reply(resp);
		co_return HandleResult::refuseToRetry(kBadCodeMessage, "code consumed by a concurrent transfer");
	case CommitOutcome::Failed:
		co_return HandleResult::refuseToRetry(kServerErrorMessage, "transfer commit failed");
	}
	LOG_INFO << "Transfer: user " << verdict.userId << " moved "
		<< (session && session->deviceKey ? "to the requesting device" : "to a device claim");
	co_return reply(resp);
}

/*!
* Runs a transfer handler with every failure turned into a retry refusal --
* see the file comment for why nothing may escape.
*/
drogon::Task<HandleResult> guarded(
	const char* name, std::function<drogon::Task<HandleResult>(const std::string&)> body, const std::string& json)
{
	try
	{
		co_return co_await body(json);
	}
	catch (const drogon::orm::DrogonDbException& ex)
	{
		LOG_ERROR << name << ": database error: " << ex.base().what();
		co_return HandleResult::refuseToRetry(kServerErrorMessage, ex.base().what());
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << name << ": " << ex.what();
		co_return HandleResult::refuseToRetry(kServerErrorMessage, ex.what());
	}
}

} // namespace

HANDLEF(TransferCodeIssue)
{
	(void)session;
	co_return co_await guarded("TransferCodeIssue", issueCode, json);
}

HANDLEF(TransferCodeCheck)
{
	(void)session;
	co_return co_await guarded("TransferCodeCheck", checkCode, json);
}

HANDLEF(Transfer)
{
	(void)session;
	co_return co_await guarded("Transfer", transferAccount, json);
}
