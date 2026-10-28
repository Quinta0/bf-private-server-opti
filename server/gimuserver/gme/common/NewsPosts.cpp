#include "App.hpp"

#include <gimuserver/gme/common/GameClock.hpp>
#include <gimuserver/gme/common/NewsPosts.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

namespace
{

struct NewsFile
{
	gme::NewsPage page;
	std::vector<gme::NewsPost> posts;
};

// The parsed file and what it looked like on disk when it was read.
struct NewsCache
{
	std::mutex lock;
	bool seen = false;               // a file was there last time we looked
	uintmax_t size = 0;
	std::filesystem::file_time_type mtime{};
	gme::NewsPage page;
	std::vector<gme::NewsPost> posts;
};

NewsCache& cache()
{
	static NewsCache value;
	return value;
}

std::filesystem::path postsPath()
{
	const auto& root = theServer()->cache().serverConfig().balanceRoot;
	if (root.empty())
		return {};
	return std::filesystem::path(root) / "news" / "posts.json";
}

// Re-reads posts.json when it appeared, vanished or changed.  Caller holds the lock.
void refresh(NewsCache& c, const std::filesystem::path& path)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const bool present = !path.empty() && fs::is_regular_file(path, ec) && !ec;
	if (!present)
	{
		if (c.seen)
			LOG_INFO << "News: " << path.string() << " is gone; the board is empty";
		c.seen = false;
		c.page = {};
		c.posts.clear();
		return;
	}

	const auto size = fs::file_size(path, ec);
	if (ec)
		return;
	const auto mtime = fs::last_write_time(path, ec);
	if (ec)
		return;
	if (c.seen && size == c.size && mtime == c.mtime)
		return;

	std::string text;
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return;
		text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	NewsFile file{};
	if (const auto err = glz::read<glz::opts{ .error_on_unknown_keys = false }>(file, text); err)
	{
		// Keep the last good copy, but don't re-parse the same bad bytes every request.
		LOG_ERROR << "News: " << path.string() << " does not parse, keeping the previous "
			<< c.posts.size() << " post(s): " << glz::format_error(err, text);
		c.seen = true;
		c.size = size;
		c.mtime = mtime;
		return;
	}

	std::vector<gme::NewsPost> kept;
	for (auto& post : file.posts)
	{
		if (post.id <= 0 || post.title.empty() || gme::parseNewsTime(post.start) < 0
			|| (!post.end.empty() && gme::parseNewsTime(post.end) < 0)
			|| (!post.published.empty() && gme::parseNewsTime(post.published) < 0))
		{
			LOG_WARN << "News: skipping post " << post.id << " (needs an id > 0, a title and valid times)";
			continue;
		}
		kept.push_back(std::move(post));
	}

	c.seen = true;
	c.size = size;
	c.mtime = mtime;
	c.page = std::move(file.page);
	c.posts = std::move(kept);
	LOG_INFO << "News: loaded " << c.posts.size() << " post(s) from " << path.string();
}

} // namespace

namespace gme
{

int64_t parseNewsTime(const std::string& text)
{
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
	std::string t = text;
	std::replace(t.begin(), t.end(), 'T', ' ');
	if (!t.empty() && t.back() == 'Z')
		t.pop_back();
	// The whole string must match: "+09:00" or any other trailing text is refused,
	// not silently read as UTC.
	const char* c = t.c_str();
	const int len = static_cast<int>(t.size());
	int used = -1;
	bool whole = std::sscanf(c, "%4d-%2d-%2d %2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &used) == 6 && used == len;
	if (!whole)
	{
		h = mi = s = 0;
		used = -1;
		whole = std::sscanf(c, "%4d-%2d-%2d %2d:%2d%n", &y, &mo, &d, &h, &mi, &used) == 5 && used == len;
	}
	if (!whole)
	{
		h = mi = 0;
		used = -1;
		whole = std::sscanf(c, "%4d-%2d-%2d%n", &y, &mo, &d, &used) == 3 && used == len;
	}
	if (!whole)
		return -1;
	if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59
		|| s < 0 || s > 59)
		return -1;

	using namespace std::chrono;
	const year_month_day ymd{ year{ y }, month{ static_cast<unsigned>(mo) }, day{ static_cast<unsigned>(d) } };
	if (!ymd.ok())
		return -1;
	return sys_days{ ymd }.time_since_epoch().count() * 86400LL + h * 3600LL + mi * 60LL + s;
}

namespace
{

// Snapshot of the file (refreshed if it changed) and the posts visible at `now`, sorted.
std::pair<NewsPage, std::vector<NewsPost>> snapshot(const uint64_t nowUnix)
{
	auto& c = cache();
	NewsPage page;
	std::vector<NewsPost> posts;
	{
		std::lock_guard guard(c.lock);
		refresh(c, postsPath());
		page = c.page;
		posts = c.posts;
	}

	const auto now = static_cast<int64_t>(nowUnix);
	std::erase_if(posts, [now](const NewsPost& p) {
		return parseNewsTime(p.start) > now || (!p.end.empty() && parseNewsTime(p.end) <= now);
	});
	std::stable_sort(posts.begin(), posts.end(), [](const NewsPost& a, const NewsPost& b) {
		if (a.priority != b.priority)
			return a.priority > b.priority;
		const auto sa = parseNewsTime(a.start), sb = parseNewsTime(b.start);
		if (sa != sb)
			return sa > sb;
		return a.id > b.id;
	});
	return { std::move(page), std::move(posts) };
}

std::string escapeHtml(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (const char ch : text)
	{
		switch (ch)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		default: out += ch;
		}
	}
	return out;
}

// A post without admin tooling HTML (posts.json written by hand): escaped, newlines kept.
std::string plainPostHtml(const NewsPost& post)
{
	std::string body = escapeHtml(post.body);
	std::string withBreaks;
	for (const char ch : body)
	{
		if (ch == '\n')
			withBreaks += "<br>";
		else if (ch != '\r')
			withBreaks += ch;
	}
	return "<article><h2>" + escapeHtml(post.title) + "</h2><p class=\"date\">"
		+ escapeHtml(post.start.substr(0, 10)) + "</p><p>" + withBreaks + "</p></article>";
}

// RFC 3986 unreserved plus the sub-delims a data: URL path may hold; '%', '#', '?' and
// everything non-ASCII are escaped.
std::string percentEncode(const std::string& text)
{
	static constexpr char kHex[] = "0123456789ABCDEF";
	static constexpr std::string_view kKeep = "-._~!$&'()*+,;=:@/";   // no NUL: find(0) is npos
	std::string out;
	out.reserve(text.size() * 2);
	for (const unsigned char ch : text)
	{
		if (std::isalnum(ch) || kKeep.find(static_cast<char>(ch)) != std::string_view::npos)
		{
			out += static_cast<char>(ch);
		}
		else
		{
			out += '%';
			out += kHex[ch >> 4];
			out += kHex[ch & 0xF];
		}
	}
	return out;
}

constexpr const char* kFallbackCss =
	"html{background:#0b1114}body{margin:0;padding:14px 16px 28px;color:#e8e2d0;"
	"font:15px/1.55 sans-serif}h2{margin:0 0 2px;font-size:18px;color:#f5cf6b}"
	".date{margin:0 0 10px;font-size:12px;color:#9aa7ab}article{margin:0 0 22px}";

} // namespace

std::vector<NewsPost> visibleNewsPosts(const uint64_t nowUnix)
{
	return snapshot(nowUnix).second;
}

std::string newsPageUrl(const NewsPage& page, const std::vector<NewsPost>& posts)
{
	std::string html = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>"
		+ escapeHtml(page.title) + "</title><style>" + (page.css.empty() ? kFallbackCss : page.css)
		+ "</style></head><body>";
	if (posts.empty())
		html += "<p class=\"empty\">" + escapeHtml(page.empty) + "</p>";
	for (const auto& post : posts)
		html += post.html.empty() ? plainPostHtml(post) : post.html;
	// The client appends "?friendId=<id>&os=<n>" to whatever it opens
	// (LayerWebView::setView); an unclosed comment keeps that off the page.
	html += "</body></html><!--";
	return "data:text/html;charset=utf-8," + percentEncode(html);
}

std::optional<NewsNotice> newsNotice(const uint64_t nowUnix)
{
	if (theServer()->cache().serverConfig().balanceRoot.empty())
		return std::nullopt;                // news off: keep system/notice_info.json

	auto [page, posts] = snapshot(nowUnix);
	if (posts.empty())
	{
		// Patched clients open this url from INFO, so never fall back to the
		// shipped (dead, plain-http) gumi page.  Id 0: no NEW badge for nothing.
		return NewsNotice{ 0, newsPageUrl(page, posts) };
	}
	if (posts.size() > kMaxPagePosts)
	{
		LOG_WARN << "News: " << posts.size() << " posts are visible; the page shows the first "
			<< kMaxPagePosts << " (give old ones an end time)";
		posts.resize(kMaxPagePosts);
	}

	// The badge rises when a post appears: when it is published, or when its
	// start comes, whichever is later.  Editing a live post moves neither.
	int64_t appeared = 0;
	for (const auto& post : posts)
	{
		const auto published = post.published.empty() ? 0 : parseNewsTime(post.published);
		appeared = std::max({ appeared, parseNewsTime(post.start), published });
	}
	const auto minutes = std::clamp<int64_t>((appeared - kNewsEpoch) / 60, 0, INT32_MAX - kNewsIdBase);
	return NewsNotice{ kNewsIdBase + static_cast<int32_t>(minutes), newsPageUrl(page, posts) };
}

void applyNewsNotice(::NoticeInfo& info)
{
	if (auto news = newsNotice(GameClock::nowSeconds()))
	{
		info.id = news->id;
		info.url = std::move(news->url);
	}
}

} // namespace gme
