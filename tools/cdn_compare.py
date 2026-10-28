#!/usr/bin/env python3
"""Download the same CDN paths from two base URLs and compare the answers byte for byte.

Used to check a new bf-cdn version (its preview URL) against the live CDN before
it takes traffic: status, body SHA-256 and the placeholder marker must match.

Usage:
  python3 -I tools/cdn_compare.py A_BASE B_BASE paths.txt ['Header: value sent to B only']
paths.txt: one path per line (/content/... or /mst/...), query optional.
To test a version at 0% on the live hostname, use the same base twice and send B
  'Cloudflare-Workers-Version-Overrides: bf-cdn="<version id>"'.
"""
import hashlib
import sys
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

UA = 'Apache-HttpClient/UNAVAILABLE (java 1.5)'  # what the game sends


def fetch(url: str, extra: dict) -> tuple:
    req = urllib.request.Request(url, headers={'User-Agent': UA, **extra})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                body = resp.read()
                return resp.status, hashlib.sha256(body).hexdigest(), len(body), resp.headers.get('x-bf-placeholder')
        except urllib.error.HTTPError as err:
            return err.code, None, 0, None
        except OSError as err:
            if attempt == 2:
                return f'error {err}', None, 0, None


def compare(a: str, b: str, b_headers: dict, path: str) -> tuple:
    path = path if '?' in path else path + '?v=132'
    return path, fetch(a + path, {}), fetch(b + path, b_headers)


def main() -> int:
    a, b, list_file = sys.argv[1].rstrip('/'), sys.argv[2].rstrip('/'), sys.argv[3]
    b_headers = dict([sys.argv[4].split(': ', 1)]) if len(sys.argv) > 4 else {}
    paths = [p.strip() for p in open(list_file) if p.strip()]
    same = diff = 0
    with ThreadPoolExecutor(8) as pool:
        for path, ra, rb in pool.map(lambda p: compare(a, b, b_headers, p), paths):
            if ra == rb:
                same += 1
            else:
                diff += 1
                print(f'DIFF {path}\n  a={ra}\n  b={rb}')
    print(f'{same} identical, {diff} different, of {len(paths)}')
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
