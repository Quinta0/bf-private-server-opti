#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The owner's news posts ("what's new"), shown behind Home's INFO button.
//
// What the client does (libgame RE, 2026-10-17; docs/features/NEWS_BOARD.md):
// the native notice list (NoticeListScene, NoticeUpdate 68pTQAJv) is compiled
// in but switched off -- FUNC_NOTICE_LIST is never set -- so the server's
// NoticeMst rows are never even parsed.  What the client does use is
// NoticeInfo (Pj6zDW3m, parsed out of ANY reply):
//   - Home shows the NEW badge on INFO while the last id opened (SaveData,
//     UserState::getLastNoticeID) < NoticeInfo.id (HomeScene2::updateEvent);
//   - opening INFO (and the raid world's News button) shows a WebView and
//     stores NoticeInfo.id as the last one seen, which clears the badge.
// RaidWorldScene::newsWeb opens NoticeInfo.url as given; Home's INFO opens it
// only with the --news APK patch (tools/patch_apk.py), because stock it opens
// https://<api host>/<url_mst cH0Kdchp>, whose private-CA certificate the
// WebView refuses.
//
// So a publish becomes a NoticeInfo whose id rises whenever a post appears --
// newly published, or reaching its start time -- and whose url is a
// self-contained data: page with every visible post.  The id is kNewsIdBase +
// the minutes since kNewsEpoch of the latest such moment (a post appears at
// max(start, published)); it stays above the 3876 of system/notice_info.json,
// which players who already tapped INFO have stored, and editing a live post
// does not move it.  The WebView only trusts public certificates and refuses
// plain http, so nothing is fetched: the page is the URL.
//
// The posts live in <balance_root>/news/posts.json, written by an operator or an
// external tool, re-read whenever the file changes:
//
//   {"page": {"title": "What's new", "css": "...", "empty": "..."},
//    "posts": [{"id": 3, "title": "...", "body": "...", "html": "<article>...",
//               "start": "2026-10-17 12:00:00", "end": "", "priority": 0,
//               "published": "2026-10-17 12:05:00"}]}
//
// Times are UTC "YYYY-MM-DD HH:MM:SS"; an empty end means "until withdrawn" and
// an empty published means "at start".
// `html` is the post as its author pre-rendered it; without it the server
// escapes title and body itself.  A file that does not parse keeps the last
// good copy (and says so in the log) rather than emptying the page.

struct NoticeInfo;   // packets/all.hpp: Pj6zDW3m

namespace gme
{

/// NoticeInfo id = kNewsIdBase + minutes since kNewsEpoch (2026-01-01 00:00 UTC)
/// of the moment the newest visible post appeared.
constexpr int32_t kNewsIdBase = 10000;
constexpr int64_t kNewsEpoch = 1767225600;

/// At most this many posts on the page (the newest / highest priority ones):
/// every login re-encodes the page, and the WebView refuses URLs past 2 MiB.
constexpr size_t kMaxPagePosts = 30;

struct NewsPost
{
	int32_t id = 0;
	std::string title;
	std::string body;
	std::string html;               // pre-rendered by the author, may be empty
	std::string start;              // UTC, "YYYY-MM-DD HH:MM:SS"
	std::string end;                // UTC, or empty
	int32_t priority = 0;           // higher first; ties: newer start first
	std::string published;          // UTC, first publish, or empty
};

struct NewsPage
{
	std::string title = "What's new";
	std::string css;
	std::string empty = "No news right now.";
};

struct NewsNotice
{
	int32_t id = 0;                 // NoticeInfo xJNom6i0
	std::string url;                // NoticeInfo jsRoN50z
};

/*!
* The posts visible at `nowUnix`, highest priority first, then newest first.
* Reads posts.json again only when its size or mtime changed.
* Empty when balance_root is off or nothing is published.
*/
std::vector<NewsPost> visibleNewsPosts(uint64_t nowUnix);

/*!
* The NoticeInfo to send at `nowUnix`: the page of visible posts, or (id 0, so no
* badge) a "no news" page when none is visible.  nullopt only when balance_root is
* off, and callers then keep the shipped system/notice_info.json.
*/
std::optional<NewsNotice> newsNotice(uint64_t nowUnix);

/*!
* Overwrites `info` with newsNotice(now); leaves it alone when balance_root is off.
*/
void applyNewsNotice(::NoticeInfo& info);

/*!
* "YYYY-MM-DD HH:MM:SS" (UTC) to Unix seconds, -1 when malformed.  A bare
* "YYYY-MM-DD" is midnight; "T" may replace the space and a trailing "Z" is ignored.
*/
int64_t parseNewsTime(const std::string& text);

/// The page as a data: URL.  Exposed for tests.
std::string newsPageUrl(const NewsPage& page, const std::vector<NewsPost>& posts);

} // namespace gme
