#!/usr/bin/env python3
"""Build cdn-worker/static/: the bf-cdn Worker's static assets (Workers Static Assets).

Requests that match a file in static/ are answered by Cloudflare without running
the Worker script, and static asset requests are not billed. Everything else
(files uploaded to R2 later, aliases, name-index fallbacks, placeholders, /diag)
still reaches the Worker, which reads R2 as before. Before this, every one of
~21M daily asset requests was a billed Worker invocation.

Two sources feed static/:
  - the local archive (logs/upload-done.txt keys), as APFS clones (no extra disk);
  - extras: R2-only files (published table parts, later art) downloaded from the
    live CDN into cdn-worker/static-extra/ and listed in logs/cdn-static-extra.txt.
    --add-from-tail finds them by sampling the requests that still run the Worker.
A key goes into static/ only when the live CDN serves the exact R2 object for it
(200, no placeholder, day-long cache) and its ETag (the R2 MD5) equals the MD5 of
the file, so static/ never changes a byte that clients get today.

Publishing does not change: a new R2 file is served by the Worker at once, and
moves to static/ on the next sync. A file in static/ wins over R2, so replacing
an existing key needs a sync (with --recheck) after the R2 upload.

Usage:
  python3 -I tools/build_cdn_static.py --assets "~/bf-archive/assets" \
      [--add-from-tail 300] [--add-keys keys.txt] [--deploy]
--deploy uploads a new bf-cdn version (only new files upload), byte-checks it at
0% traffic through the version-override header, then moves 100% to it.
"""
import argparse
import ctypes
import hashlib
import http.client
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKER_DIR = ROOT / 'cdn-worker'
KEYS_FILE = ROOT / 'logs/upload-done.txt'
EXTRA_KEYS_FILE = ROOT / 'logs/cdn-static-extra.txt'
EXTRA_DIR = WORKER_DIR / 'static-extra'
OUT_DIR = WORKER_DIR / 'static'
CHECKED_FILE = ROOT / 'logs/cdn-static-checked.json'
MAX_FILES = 100_000  # Workers Paid: static asset files per version
MAX_BYTES = 25 * 1024 * 1024
# Same as CACHE_EXACT in cdn-worker/src/index.js.
HEADERS_FILE = '/*\n  Cache-Control: public, max-age=86400, no-transform\n'
SERVABLE = re.compile(r'^(content|mst)/[^\\\0]+$')

_local = threading.local()
_libc = ctypes.CDLL('libc.dylib', use_errno=True) if sys.platform == 'darwin' else None


def request(host: str, method: str, key: str) -> dict:
    for _ in range(4):
        try:
            conn = getattr(_local, 'conn', None) or http.client.HTTPConnection(host, timeout=120)
            _local.conn = conn
            conn.request(method, '/' + urllib.parse.quote(key) + '?v=static-check')
            resp = conn.getresponse()
            body = resp.read()
            exact = (resp.status == 200 and resp.getheader('x-bf-placeholder') is None
                     and 'max-age=86400' in (resp.getheader('cache-control') or ''))
            return {'status': resp.status, 'etag': (resp.getheader('etag') or '').strip('"'),
                    'exact': exact, 'body': body}
        except (OSError, http.client.HTTPException):
            _local.conn = None
    return {'status': 0, 'exact': False}


def md5(path: Path) -> str:
    h = hashlib.md5()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def clone(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if _libc is None or _libc.clonefile(str(src).encode(), str(dst).encode(), 0) != 0:
        shutil.copyfile(src, dst)


def check(host: str, path: Path, key: str) -> str:
    if not path.is_file():
        return 'no local file'
    if path.stat().st_size > MAX_BYTES:
        return 'over 25 MiB'
    live = request(host, 'HEAD', key)
    if not live['exact']:
        return 'live answer is not the exact R2 file' if live['status'] == 200 else f"live status {live['status']}"
    return 'ok' if live['etag'] == md5(path) else 'differs from R2'


# ---------------------------------------------------------------- extras

def sample_tail(seconds: int) -> set:
    """Keys of the requests that ran the Worker during a `wrangler tail` sample."""
    with tempfile.TemporaryFile('w+') as out:
        proc = subprocess.Popen(['npx', 'wrangler', 'tail', 'bf-cdn', '--format', 'json'],
                                cwd=WORKER_DIR, stdout=out, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait()
        out.seek(0)
        text = out.read()
    keys, decoder, i = set(), json.JSONDecoder(), 0
    while i < len(text):
        while i < len(text) and text[i] in ' \r\n\t':
            i += 1
        try:
            event, i = decoder.raw_decode(text, i)
        except ValueError:
            break  # a cut-off last record
        url = ((event.get('event') or {}).get('request') or {}).get('url', '')
        key = urllib.parse.unquote(urllib.parse.urlsplit(url).path).lstrip('/')
        if SERVABLE.match(key) and 'REDACTED' not in key:
            keys.add(key)
    return keys


def add_extras(host: str, keys: set, known: set) -> list:
    """Download each new key from the live CDN; keep it only if it is the exact R2 file."""
    def fetch(key):
        live = request(host, 'GET', key)
        ok = (live['exact'] and len(live['body']) <= MAX_BYTES
              and hashlib.md5(live['body']).hexdigest() == live['etag'])
        return key, live['body'] if ok else None

    added = []
    with ThreadPoolExecutor(8) as pool:
        for key, body in pool.map(fetch, sorted(keys - known)):
            if body is None:
                continue
            dst = EXTRA_DIR / key
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(body)
            added.append(key)
    if added:
        with EXTRA_KEYS_FILE.open('a') as f:
            f.write(''.join(k + '\n' for k in added))
    return added


# ---------------------------------------------------------------- build

def sync_out_dir(sources: dict) -> int:
    """Make static/ hold exactly the given key -> source file set (plus _headers).
    Returns how many files were added or removed."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    present = {str(p.relative_to(OUT_DIR)) for p in OUT_DIR.rglob('*') if p.is_file()}
    present.discard('_headers')
    for key in present - sources.keys():
        (OUT_DIR / key).unlink()
    for key in sources.keys() - present:
        clone(sources[key], OUT_DIR / key)
    (OUT_DIR / '_headers').write_text(HEADERS_FILE)
    return len(present ^ sources.keys())


def wrangler(*args: str) -> str:
    proc = subprocess.run(['npx', 'wrangler', *args], cwd=WORKER_DIR, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f'wrangler {" ".join(args)} failed:\n{proc.stdout[-2000:]}{proc.stderr[-2000:]}')
    return proc.stdout


def deploy(new_keys: list, all_keys: list) -> None:
    status = wrangler('deployments', 'status')
    current = re.findall(r'\(100%\)\s+([0-9a-f-]{36})', status)
    if len(current) != 1:
        sys.exit(f'deploy skipped: the live deployment is not one version at 100%:\n{status}')
    old = current[0]
    out = wrangler('versions', 'upload', '--message', f'static assets sync (+{len(new_keys)} files)')
    new = re.search(r'Worker Version ID:\s*([0-9a-f-]{36})', out).group(1)
    wrangler('versions', 'deploy', f'{old}@100%', f'{new}@0%', '--message', 'static sync check at 0%', '--yes')
    sample = sorted(set(new_keys) | set(random.sample(all_keys, min(200, len(all_keys)))))
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(''.join('/' + k + '\n' for k in sample))
    check_run = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/cdn_compare.py'),
                                'http://cdn.example.com', 'http://cdn.example.com', f.name,
                                f'Cloudflare-Workers-Version-Overrides: bf-cdn="{new}"'],
                               capture_output=True, text=True)
    os.unlink(f.name)
    print(check_run.stdout[-1500:])
    if check_run.returncode != 0:
        wrangler('versions', 'deploy', f'{old}@100%', '--message', 'static sync failed its byte check', '--yes')
        sys.exit(f'byte check failed; live stays on {old}')
    wrangler('versions', 'deploy', f'{new}@100%', '--message', f'static assets sync (rollback: {old})', '--yes')
    print(f'live: {new} at 100% (rollback: npx wrangler versions deploy {old}@100% --yes)')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--assets', required=True, help='local archive with content/ and mst/')
    ap.add_argument('--cdn', default='cdn.example.com')
    ap.add_argument('--workers', type=int, default=32)
    ap.add_argument('--recheck', action='store_true', help='ignore saved results and HEAD every key again')
    ap.add_argument('--add-from-tail', type=int, metavar='SECONDS',
                    help='sample Worker requests this long and add the exact R2 files they asked for')
    ap.add_argument('--add-keys', type=Path, help='file with R2 keys to add, one per line')
    ap.add_argument('--deploy', action='store_true', help='upload, byte-check at 0%%, then go 100%%')
    args = ap.parse_args()
    assets = Path(args.assets).expanduser()

    def read_keys(path: Path) -> set:
        return {k.strip() for k in path.read_text().splitlines() if k.strip()} if path.exists() else set()

    archive_keys = read_keys(KEYS_FILE)
    extra_keys = read_keys(EXTRA_KEYS_FILE) - archive_keys
    wanted = set()
    if args.add_from_tail:
        wanted |= sample_tail(args.add_from_tail)
    if args.add_keys:
        wanted |= read_keys(args.add_keys)
    added = add_extras(args.cdn, wanted, archive_keys | extra_keys) if wanted else []
    if wanted:
        print(f'{len(wanted)} requested keys, {len(added)} new R2-only files added to {EXTRA_DIR.name}/')
    extra_keys |= set(added)

    sources = {k: assets / k for k in archive_keys}
    sources.update({k: EXTRA_DIR / k for k in extra_keys})
    checked = {} if args.recheck or not CHECKED_FILE.exists() else json.loads(CHECKED_FILE.read_text())
    checked.update({k: 'ok' for k in added})  # verified against the R2 MD5 on download
    todo = sorted(k for k in sources if checked.get(k) != 'ok')
    print(f'{len(sources)} keys, {len(todo)} to check against http://{args.cdn}', flush=True)
    with ThreadPoolExecutor(args.workers) as pool:
        for n, (key, result) in enumerate(zip(todo, pool.map(lambda k: check(args.cdn, sources[k], k), todo)), 1):
            checked[key] = result
            if n % 5000 == 0:
                print(f'  {n}/{len(todo)}', flush=True)
    CHECKED_FILE.write_text(json.dumps(checked, indent=0, sort_keys=True))

    good = sorted(k for k in sources if checked.get(k) == 'ok')
    if len(good) >= MAX_FILES:
        sys.exit(f'{len(good)} files: over the {MAX_FILES} static asset limit')
    changed = sync_out_dir({k: sources[k] for k in good})
    skipped = {}
    for key in sources:
        if checked.get(key) != 'ok':
            skipped.setdefault(checked.get(key, 'unchecked'), []).append(key)
    print(f'static/: {len(good)} files (+_headers); left to the Worker: '
          + (', '.join(f'{len(v)} {r}' for r, v in sorted(skipped.items())) or 'none'))
    for reason, items in sorted(skipped.items()):
        print(f'  {reason}: ' + ', '.join(sorted(items)[:5]) + (' ...' if len(items) > 5 else ''))

    print(f'{changed} files changed in static/')
    if args.deploy and changed:
        deploy(added, good)
    return 0


if __name__ == '__main__':
    sys.exit(main())
