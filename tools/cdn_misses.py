#!/usr/bin/env python3
"""Count bf-cdn misses (404s and placeholders) per path from saved `wrangler tail` output.

Capture (read-only, stop with Ctrl-C; a cut-off last record is ignored):

    cd cdn-worker
    npx wrangler tail bf-cdn --format json > ~/bf-tail.json

`--format json` writes one pretty-printed JSON object per request, back to back
(not JSON Lines); this script reads that as is. Add `--search bf_miss` to the
tail command to keep only miss events. The same lines are in the Cloudflare
Workers Logs view (filter on the field bf_miss).

The worker logs one object per miss:
    {"bf_miss": "<R2 key, no query>", "result": "placeholder" | "404" | "503", "ua": "<first 64 chars>"}
"503" means the miss could not be confirmed (an R2 error, or _meta/aliases.json or
_meta/name-index.json missing or unreadable), so no placeholder was served.
Events from worker versions before that log line existed are counted from the
response status instead (a 404 on /content/ or /mst/), as result "404 (status)".
wrangler tail samples under heavy traffic, so the counts are a lower bound.

Usage:
    python3 tools/cdn_misses.py ~/bf-tail.json [more.json ...]
    python3 tools/cdn_misses.py - < ~/bf-tail.json
    options: --result placeholder|404|503|"404 (status)"   --top N   --json
"""
import argparse
import json
import sys
from collections import Counter, defaultdict
from urllib.parse import unquote, urlsplit


def events(text):
    """Yield each top-level JSON object of concatenated (pretty) JSON."""
    decoder = json.JSONDecoder()
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] in ' \r\n\t':
            i += 1
        if i >= n:
            return
        try:
            obj, i = decoder.raw_decode(text, i)
        except json.JSONDecodeError:
            nxt = text.find('\n{', i + 1)  # skip a stray line, resync on the next record
            if nxt < 0:
                return  # partial trailing record (tail stopped mid-write)
            i = nxt + 1
            continue
        if isinstance(obj, dict):
            yield obj


def miss_records(event):
    """bf_miss objects logged during one request (as dicts)."""
    for log in event.get('logs') or []:
        for part in log.get('message') or []:
            if isinstance(part, str) and part.lstrip().startswith('{'):
                try:
                    part = json.loads(part)
                except json.JSONDecodeError:
                    continue
            if isinstance(part, dict) and isinstance(part.get('bf_miss'), str):
                yield part


def status_miss(event):
    """(key, '404 (status)') for an unlogged 404 on /content/ or /mst/, else None."""
    ev = event.get('event') or {}
    if ((ev.get('response') or {}).get('status')) != 404:
        return None
    url = (ev.get('request') or {}).get('url') or ''
    key = unquote(urlsplit(url).path).lstrip('/')
    return (key, '404 (status)') if key.startswith(('content/', 'mst/')) else None


def collect(texts):
    counts = Counter()  # (key, result) -> requests
    clients = defaultdict(set)  # (key, result) -> client IPs (only their number is printed)
    total = 0
    for text in texts:
        for event in events(text):
            total += 1
            headers = ((event.get('event') or {}).get('request') or {}).get('headers') or {}
            ip = headers.get('cf-connecting-ip')
            found = [(m['bf_miss'], str(m.get('result', '?'))) for m in miss_records(event)]
            if not found:
                fallback = status_miss(event)
                found = [fallback] if fallback else []
            for item in found:
                counts[item] += 1
                if ip:
                    clients[item].add(ip)
    return total, counts, {k: len(v) for k, v in clients.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='+', help="saved 'wrangler tail --format json' output, or - for stdin")
    ap.add_argument('--result', help='only this result (placeholder, 404, 503, "404 (status)")')
    ap.add_argument('--top', type=int, default=0, help='show only the N most requested paths')
    ap.add_argument('--json', action='store_true', help='print JSON rows instead of a table')
    args = ap.parse_args(argv)

    texts = []
    for name in args.files:
        if name == '-':
            texts.append(sys.stdin.read())
        else:
            with open(name, encoding='utf-8', errors='replace') as f:
                texts.append(f.read())

    total, counts, clients = collect(texts)
    rows = [
        {'path': key, 'result': result, 'requests': n, 'clients': clients.get((key, result), 0)}
        for (key, result), n in counts.items()
        if args.result is None or result == args.result
    ]
    rows.sort(key=lambda r: (-r['requests'], r['path'], r['result']))
    if args.top:
        rows = rows[:args.top]

    if args.json:
        print(json.dumps({'events': total, 'misses': rows}, indent=1))
        return 0
    by_result = Counter()
    for (_, result), n in counts.items():
        by_result[result] += n
    print(f'events read: {total}; miss requests: {sum(counts.values())} '
          + ' '.join(f'[{r}: {n}]' for r, n in sorted(by_result.items())))
    print(f'{"requests":>8} {"clients":>7}  {"result":<12}  path')
    for r in rows:
        print(f'{r["requests"]:>8} {r["clients"]:>7}  {r["result"]:<12}  {r["path"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
