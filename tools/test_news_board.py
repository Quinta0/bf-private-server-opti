"""Wire test for the news page behind Home's INFO button (server branch feat/news-board,
gme/common/NewsPosts.cpp; the posts file is authored by hand, see
server/docs/features/NEWS_BOARD.md).

Checks, against a running server, by rewriting <src>/deploy/balance/news/posts.json
(the server's balance_root) and logging a throwaway guest in:
  A. No posts.json: Initialize and UserInfo carry id 0 (no badge) and a
     "no news" page -- never the shipped gumi url, which patched INFO would open.
  B. Visible posts: NoticeInfo id is 10000 + the minutes since 2026-01-01 of
     the latest moment a visible post appeared (max of start and published),
     and its url is a data:text/html page with the visible posts in order
     (priority, newest start, highest id), a post's own html used as is, a
     post without html escaped by the server, future, expired, id-less and
     badly-timed (UTC offset) posts left out, and an unclosed comment at the
     end (the client appends ?friendId=..&os=..).
  C. The file is re-read when it changes, without a restart; a later
     `published` raises the id even when the start is old.
  D. A file that does not parse keeps the last good copy.
  E. Removing the file brings the "no news" page back.

Usage: python3 -I tools/test_news_board.py --src wt/news [--url http://127.0.0.1:9967]
Run it against a local test server only (it refuses any other host): it replaces
that server's posts.json while it runs and puts the old one back at the end.
"""
import argparse
import json
import sys
import time
import urllib.parse
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import INITIALIZE, USER_INFO, Database, Player, container_on_port, gme_call  # noqa: E402

NEWS_BASE = 10000
NEWS_EPOCH = datetime(2026, 1, 1, tzinfo=timezone.utc)
PREFIX = 'data:text/html;charset=utf-8,'


def badge(when: str) -> str:
    """The NoticeInfo id for a newest appearance at `when` (UTC)."""
    t = datetime.strptime(when, '%Y-%m-%d %H:%M:%S').replace(tzinfo=timezone.utc)
    return str(NEWS_BASE + int((t - NEWS_EPOCH).total_seconds()) // 60)


def notice(reply: dict) -> dict:
    info = reply.get('Pj6zDW3m')
    return (info[0] if isinstance(info, list) else info) or {}


def page_of(url: str) -> str:
    assert url.startswith(PREFIX), url[:60]
    return urllib.parse.unquote(url[len(PREFIX):])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9967')
    ap.add_argument('--src', type=Path, required=True, help='server checkout the server runs from')
    ap.add_argument('--container', help="Docker container (default: the one publishing the --url port)")
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    if urllib.parse.urlsplit(args.url).hostname not in ('127.0.0.1', 'localhost', '::1'):
        raise SystemExit(f'{args.url} is not a local test server; refusing to rewrite its news')
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    posts_file = args.src / 'deploy' / 'balance' / 'news' / 'posts.json'
    saved = posts_file.read_bytes() if posts_file.exists() else None
    container = args.container or container_on_port(args.url)
    db = Database(args.src / 'deploy' / 'gme.sqlite', container, args.container_db)
    player = Player(args.url, db)

    def write(data) -> None:
        posts_file.parent.mkdir(parents=True, exist_ok=True)
        text = data if isinstance(data, str) else json.dumps(data)
        posts_file.write_text(text, encoding='utf-8')
        # The server notices a change by size or mtime; make sure the mtime moves.
        time.sleep(1.1)

    def initialize() -> dict:
        return notice(gme_call(args.url, *INITIALIZE, {'IKqx1Cn9': player.login}))

    def user_info() -> dict:
        return notice(player.user_info())

    try:
        # A. nothing published
        posts_file.unlink(missing_ok=True)
        time.sleep(1.1)
        init = initialize()
        check('A. no posts.json: id 0 and the "no news" page',
              init.get('xJNom6i0') == '0' and '<p class="empty">No news right now.</p>' in page_of(init.get('jsRoN50z', '')),
              init.get('xJNom6i0'))
        check('A. UserInfo carries the same', user_info() == init)

        # B. a mix of visible, future and expired posts
        write({'page': {'title': 'Test page', 'css': 'body{color:#abc}', 'empty': 'none'},
               'posts': [
                   {'id': 7, 'title': 'Older', 'body': 'b', 'html': '<article><h2>Older</h2></article>',
                    'start': '2026-01-01 00:00:00', 'end': '', 'priority': 0},
                   {'id': 9, 'title': 'Pinned', 'body': 'b', 'html': '<article><h2>Pinned</h2></article>',
                    'start': '2025-06-01 00:00:00', 'end': '', 'priority': 1},
                   {'id': 8, 'title': 'Hand <written>', 'body': 'line 1\nline 2 & more',
                    'start': '2026-02-01', 'end': '', 'priority': 0},
                   {'id': 20, 'title': 'Future', 'body': 'b', 'html': '<article><h2>Future</h2></article>',
                    'start': '2099-01-01 00:00:00', 'end': '', 'priority': 0},
                   {'id': 21, 'title': 'Expired', 'body': 'b', 'html': '<article><h2>Expired</h2></article>',
                    'start': '2025-01-01 00:00:00', 'end': '2025-02-01 00:00:00', 'priority': 5},
                   {'id': 0, 'title': 'No id', 'body': 'b', 'start': '2025-01-01', 'end': '', 'priority': 0},
                   {'id': 22, 'title': 'Offset', 'body': 'b', 'start': '2026-01-01T00:00:00+09:00', 'end': ''},
               ]})
        init = initialize()
        check('B. NoticeInfo id follows the newest visible start', init.get('xJNom6i0') == badge('2026-02-01 00:00:00'),
              init.get('xJNom6i0'))
        page = page_of(init.get('jsRoN50z', ''))
        order = [t for t in ('Pinned', 'Hand &lt;written&gt;', 'Older') if t in page]
        check('B. visible posts in order: priority, then newest start',
              order == ['Pinned', 'Hand &lt;written&gt;', 'Older'] and page.index('Pinned') < page.index('Hand &lt;') < page.index('Older'),
              order)
        check('B. future, expired, id-less and UTC-offset posts are left out',
              not any(t in page for t in ('Future', 'Expired', 'No id', 'Offset')))
        check('B. a post without html is escaped, newlines kept', 'line 1<br>line 2 &amp; more' in page)
        check('B. the page block is used', '<title>Test page</title>' in page and 'body{color:#abc}' in page)
        check('B. the page ends in an unclosed comment', page.endswith('</body></html><!--'), page[-40:])
        raw = init.get('jsRoN50z', '')[len(PREFIX):]
        check('B. no raw ? or # in the url (the client appends ?friendId=..)', '?' not in raw and '#' not in raw)
        check('B. UserInfo carries the same NoticeInfo', user_info() == init)

        # C. re-read without a restart
        write({'posts': [{'id': 10, 'title': 'Newer', 'body': 'x', 'start': '2026-03-01 00:00:00', 'end': '',
                          'published': '2026-04-02 10:30:00'}]})
        init = initialize()
        check('C. a rewritten file is picked up without a restart; published counts',
              init.get('xJNom6i0') == badge('2026-04-02 10:30:00') and 'Newer' in page_of(init.get('jsRoN50z', '')),
              init.get('xJNom6i0'))

        # D. a broken file keeps the last good copy
        write('{"posts": [ this is not json')
        init = initialize()
        check('D. a file that does not parse keeps the last good copy', init.get('xJNom6i0') == badge('2026-04-02 10:30:00'),
              init.get('xJNom6i0'))

        # E. withdrawn
        posts_file.unlink()
        time.sleep(1.1)
        check('E. removing posts.json brings the "no news" page back', initialize().get('xJNom6i0') == '0')
    finally:
        if saved is None:
            posts_file.unlink(missing_ok=True)
        else:
            posts_file.write_bytes(saved)

    print(f"\n{'ALL PASS' if not failures else f'{failures} FAILURE(S)'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
