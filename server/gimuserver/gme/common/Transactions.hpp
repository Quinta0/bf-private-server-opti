#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <coroutine>
#include <functional>
#include <memory>
#include <utility>

namespace gme
{

/*!
* The first statement of every game transaction: an UPDATE that matches no row.
* It takes SQLite's write lock without writing a page (migration_status exists
* from the very first start: MigrationManager creates it before anything else).
*/
inline constexpr const char* kTakeWriteLock = "UPDATE migration_status SET hash = hash WHERE 0;";

/*!
* Opens a transaction that holds SQLite's write lock from its first statement,
* as BEGIN IMMEDIATE does.  Every transaction in the server starts here, never
* with drogon's own newTransactionCoro()/newTransaction(), which
* scripts/check_transactions.py refuses.
*
*     auto transaction = co_await gme::newTransaction(theDb());
*
* Drogon begins with a deferred "begin": the first read takes a snapshot and the
* first write must then upgrade to the write lock.  SQLite never waits for that
* upgrade (busy_timeout does not apply), so when another process holds the write
* lock, or has committed since the snapshot, the write fails at once with
* "database is locked" (SQLITE_BUSY / SQLITE_BUSY_SNAPSHOT) and the player loses
* the result.  bf-daily-gift writes every 2 minutes and the admin tooling's resets
* and presents write too: about one MissionEnd in 8 minutes was lost that way
* (ticket #72).  A transaction's first statement is the one place where
* SQLite does wait for another writer (busy_timeout, 10 s, main.cpp), and a
* snapshot taken under the write lock cannot go stale.
*
* The server has one SQLite connection (config connection_numbr 1), so its own
* transactions never overlapped anyway; holding the write lock for the whole
* transaction only makes those outside writers wait for a gap between two of
* them (BEGIN IMMEDIATE with a 10 s busy timeout, the admin tooling).
* At prod's load the SQLite thread is idle most of the time, so they get in
* within milliseconds; only a thread busy without pause could keep them out.
* With a second connection the lock statement would wait on the other one
* while holding drogon's per-client lock: keep one.
* A transaction handed in as `database` is joined, as drogon joins it; the lock
* statement is then a no-op inside it.
*/
inline drogon::Task<std::shared_ptr<drogon::orm::Transaction>> newTransaction(
	drogon::orm::DbClientPtr database)
{
	auto transaction = co_await database->newTransactionCoro();
	if (database->type() == drogon::orm::ClientType::Sqlite3)
		co_await transaction->execSqlCoro(kTakeWriteLock);
	co_return transaction;
}

/*!
* newTransaction() for blocking code (startup, the debug console): drogon's
* newTransaction() with the same first statement.  Never on the SQLite
* connection's own thread, which would then wait for itself.
*/
inline std::shared_ptr<drogon::orm::Transaction> newTransactionSync(
	const drogon::orm::DbClientPtr& database,
	const std::function<void(bool)>& commitCallback = {})
{
	auto transaction = database->newTransaction(commitCallback);
	if (database->type() == drogon::orm::ClientType::Sqlite3)
		transaction->execSqlSync(kTakeWriteLock);
	return transaction;
}

/*!
* Commits a transaction and waits for the result.
*
* Drogon commits a transaction when its last reference goes away and reports
* the outcome only through the commit callback, so a handler that returns
* success as soon as it has prepared its reply can tell the client about a
* charge the database never kept (FeSkillGet's first test run caught a reader
* arriving between the reply and the asynchronous commit).  Awaiting this
* hands over the handler's reference and resumes with the commit's result:
*
*     if (!(co_await gme::CommitTransaction(std::move(transaction))))
*         co_return HandleResult::refuseToHome(...);
*
* The caller must hold the only reference -- anything else keeping the
* transaction alive keeps it open, and the await never resumes.
*/
struct CommitTransaction : drogon::CallbackAwaiter<bool>
{
	explicit CommitTransaction(std::shared_ptr<drogon::orm::Transaction> transaction)
		: tx(std::move(transaction))
	{
	}

	std::shared_ptr<drogon::orm::Transaction> tx;

	void await_suspend(std::coroutine_handle<> handle)
	{
		auto transaction = std::move(tx);
		transaction->setCommitCallback([this, handle](bool ok) {
			setValue(ok);
			handle.resume();
		});
		transaction.reset();
	}
};

} // namespace gme
