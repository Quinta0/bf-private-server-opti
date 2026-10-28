# News page (Home → INFO)

The operator writes "what's new" posts into `<balance_root>/news/posts.json`. Players get a
NEW badge on Home's INFO button and, behind it, one page with every live post.
Publishing needs no deploy and no restart.

## What the client actually does (libgame RE, 2026-10-17)

The Global v2.19.6 client has two notice systems. Only one of them is switched on.

**The native list (off).** NoticeListScene, its rows (NoticeMst, groups
`oOmzJJSd`/`bcdqwXES`/`mqq6eMFZ`/`yPOwZWDU`), NoticeUpdate (`68pTQAJv`) and
NoticeReadUpdate (`cuKwx5rF`) all sit behind the global byte `FUNC_NOTICE_LIST`
(.bss 0x2997127, GOT 0x287d5d8). Nothing in libgame.so writes it, and the server's
FeatureCheck does not cover it. While it is 0:
- `GameResponseParser::getResponseObject` @0x1393e84 skips the four NoticeMst groups and `CihCiL05`;
- Home never sends NoticeUpdate, and no server log has ever seen one.

Notice.cpp and UpdateInfoLight.cpp keep their handlers registered (an unknown
GroupId closes the session) but have nothing to send. The full NoticeMst field
map is in the appendix, in case the flag is ever patched on.

**NoticeInfo (on).** Group `Pj6zDW3m` is parsed out of any reply
(`NoticeInfoResponse::readParam` @0x13e686c): `xJNom6i0` → noticeID, `jsRoN50z` →
noticeUrl. The server sends it in Initialize and UserInfo.
- **Badge.** Home shows NEW on INFO while the last id the player opened is below
  NoticeInfo's id. That id is `SaveData` "notice_save_key", plus `UserState::lastNoticeID`
  for the current session. The live check is inlined in `HomeScene2::updateEvent`
  @0x16f17f4 (`createNewBadge("new_icon_key")`); `HomeScene2::isNewNotice` has no
  callers.
- **Opening.** `HomeScene2::dispNoticePage` @0x16f45e4 (and its copies in
  AnotherHomeScene and MenuTopScene's red News button, tag 10000) opens a WebView,
  then stores NoticeInfo's id as seen, which clears NEW.
- **Stock URL.** The WebView opens `getPhpUrl() + url_mst cH0Kdchp`, i.e.
  `https://api.example.com//bf/web/notice/indexCtrl.php`. The WebView uses Android's
  trust store, not the root CA pinned for the game's own HTTPS, so it shows a black
  page.
- **Raid world.** `RaidWorldScene::newsWeb` opens NoticeInfo's url as is.
- **Appended query.** Every one of these goes through
  `pushWebViewScene(url, 320, 464, "caption_notice.png")`, and `LayerWebView::setView`
  appends `?friendId=<friend id>&os=<n>`. BFWebView then calls
  `loadUrl(url, {X-Device-Platform: Android})`. The app targets SDK 30, so plain http
  is refused too.

## What we do

1. **Server (this repo).** `gme/common/NewsPosts.cpp`:
   - Reads `<balance_root>/news/posts.json`, again only when its size or mtime changes.
   - Keeps the posts visible now: start ≤ now < end.
   - Sorts them by priority, then newest start, then highest id, and caps the page at 30.
   - Overwrites NoticeInfo in Initialize and UserInfo:
     - **id** = 10000 + the minutes since 2026-01-01 UTC of the latest moment a visible
       post appeared. A post appears at max(start, published), where `published` is its
       first publish on that target. So the badge rises for a new post, for a scheduled
       post when its start comes, and for a re-published one, but not for an edit. The
       id stays above the shipped 3876, which players who already tapped INFO have
       stored.
     - **url** = `data:text/html;charset=utf-8,<percent-encoded page>`. The page ends in
       an unclosed `<!--`, so the appended `?friendId=..&os=..` is never shown. Nothing
       is fetched and there is no web host or certificate involved. A page on our own
       domain could also be forged by anyone with a link; a data: URL cannot.

   With nothing visible it sends id 0 (no badge) and a "No news right now." page. The
   shipped gumi URL is never used while balance_root is on; patched INFO would open it.
   A posts.json that does not parse keeps the last good copy and logs an ERROR. Times
   must be plain UTC: an offset such as `+09:00` gets the post skipped with a WARN.

2. **APK (root repo, `tools/patch_apk.py`, always on).** Six instructions at each of
   HomeScene2::dispNoticePage, AnotherHomeScene::dispNoticePage and
   MenuTopScene::touchBegan replace `getPhpUrl() + UrlMst::getNoticeUrl()` with
   `NoticeInfo::getNoticeUrl()`. Players need the new APK for INFO and Menu → News to
   open the page. With an older APK, the NEW badge still appears, the raid world's
   News opens the page, and INFO still shows the black page.

3. **The posts file (authored by you).** Write posts.json under balance_root, which the
   production container mounts read-only at `/app/balance`. A post may carry a
   pre-rendered `<article>` fragment in `html`, and the page CSS rides along in `page`.
   Replace the file atomically (write a temporary file, then rename it) so the server
   never reads half a file.

posts.json:

```json
{"page": {"title": "What's new", "css": "...", "empty": "No news right now."},
 "posts": [{"id": 3, "title": "Update: 17 October", "body": "plain text",
            "html": "<article>...</article>", "start": "2026-10-17 12:00:00",
            "end": "", "priority": 0, "published": "2026-10-17 12:05:00"}]}
```

Times are UTC. `html` and `published` are optional: without `html` the server escapes
title and body and keeps the newlines, and without `published` a post appears at its
start.

## Checked

- **Wire test.** `tools/test_news_board.py` in the root repo, run against a local
  server: shipped NoticeInfo with no file; visible / future / expired posts; order;
  escaping; the trailing comment; reload without restart; broken file; withdrawal.
- **Emulator** (local APK with the patch, port 9967):
  - INFO shows NEW; the page renders and scrolls; Back closes it and NEW is gone.
  - A post published while the server ran showed NEW again after re-login, listed
    above the older one.
  - After a relaunch NEW stayed gone.
  - Menu → News opens the same page.

## Not done

- **UpdateInfo.** Home's 30-minute poll does not carry NoticeInfo, so a post published
  mid-session shows NEW at the next login. Adding `Pj6zDW3m` to UpdateInfoResp (a kdl
  field) would show it on the next poll.
- **Pre-login pages.** The title screen's News button and TitleInfoScene still open
  the stock URL. They run before login, so NoticeInfo is not there yet.
- **Credits.** Menu → Credits (url_mst credit url) has the same certificate problem.

## Appendix: NoticeMst (native list, unused while FUNC_NOTICE_LIST is 0)

`NoticeMstResponse::readParam` @0x13e6f10; group → list: `oOmzJJSd` 0 (Latest),
`bcdqwXES` 1 (Past), `mqq6eMFZ` 2 (Events: ongoing), `yPOwZWDU` 3 (Events: upcoming).

| key | field | type |
|---|---|---|
| xJNom6i0 | noticeID (keep 1..999, unique across lists: row tags are 1000/5000/6000/10000 + id) | int |
| Hl7S4ldK | noticeType (badge news_label{5,3,1,2,4}.png for 1..5) | int |
| aigSzwVh | platform (unused by the list) | int |
| dTd8sy7S | caption (row title, drawn by the tag-parsing text renderer: `<` is markup) | str |
| yu18xScw | priority (unused) | int |
| XBcWN6pL | dispDate (row shows "更新日：" + this) | str |
| qA7M9EjP / SzV0Nps7 | start / end date (unused) | str |
| DmqOwu39 / bTNgT3uB | init start / end (unused) | str |
| 2cjlwMWH / RRsveqOc | new start / end (unused) | str |
| axkRcMGN / ysWxNHQf | open start / end (unused) | str |
| rOCK3Jrv | openDescription (replaces the date line on the Events page) | str |
| jSXDubGg | pageType (unused) | int |
| cH0Kdchp | url opened on tap (as is, plus ?friendId=..&os=..) | str |
| 0S1jS5fM | switchIndex; 1 = read (hides NEW) | int |

- **Ordering and filtering.** The client neither sorts nor filters; rows appear in
  array order.
- **Read updates.** NoticeReadUpdate sends `OnWEVMLa` / `T134tP4K`: the comma-joined
  ids opened since the last NoticeUpdate. It goes out on leaving the Latest or Events
  page, not on the tap itself.
- **Fetching rows.** NoticeUpdate (`1eun7Ly7` / `wDgU3rAh` = page kind 1..3) clears
  all four lists. Its reply must carry at least one of the four groups, or the page
  waits forever.
- **Labels.** The tab and section labels are Japanese literals.
