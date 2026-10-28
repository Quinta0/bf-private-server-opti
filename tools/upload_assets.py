"""Upload the game asset archive (content/ + mst/) to the R2 bucket.

Sends each file to the temporary bf-asset-uploader Worker over keep-alive
HTTPS connections, in parallel. Finished keys are recorded so an interrupted
run resumes where it stopped. Afterwards it uploads _meta/name-index.json,
the by-name fallback the CDN Worker uses when the client asks for a file under
a different folder than the archive stores it in.

Usage:
  python3 -I tools/upload_assets.py --assets "~/bf-archive/assets" \
      --endpoint https://bf-asset-uploader.<account>.workers.dev
The upload secret is read from cdn-worker/.upload-token.
"""
import argparse
import http.client
import json
import mimetypes
import sys
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKEN_FILE = ROOT / 'cdn-worker/.upload-token'
DONE_FILE = ROOT / 'logs/upload-done.txt'
SKIP_NAMES = {'.DS_Store', 'Thumbs.db'}
EXTRA_TYPES = {'.sam': 'application/octet-stream', '.cgg': 'text/csv', '.cgs': 'text/csv',
               '.plist': 'application/xml', '.dat': 'application/octet-stream'}

_local = threading.local()


def connection(host: str) -> http.client.HTTPSConnection:
    conn = getattr(_local, 'conn', None)
    if conn is None:
        conn = http.client.HTTPSConnection(host, timeout=120)
        _local.conn = conn
    return conn


def put(host: str, token: str, key: str, data: bytes, content_type: str) -> None:
    path = '/' + urllib.parse.quote(key)
    headers = {'x-upload-token': token, 'content-type': content_type}
    for attempt in range(5):
        try:
            conn = connection(host)
            conn.request('PUT', path, body=data, headers=headers)
            resp = conn.getresponse()
            body = resp.read()
            if resp.status == 200:
                return
            if resp.status in (400, 403):
                raise RuntimeError(f'{key}: HTTP {resp.status} {body[:100]!r}')
        except (OSError, http.client.HTTPException):
            _local.conn = None  # reconnect
        time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f'{key}: failed after retries')


def content_type(path: Path) -> str:
    ext = path.suffix.lower()
    return EXTRA_TYPES.get(ext) or mimetypes.guess_type(path.name)[0] or 'application/octet-stream'


def build_name_index(keys: list[str]) -> dict[str, str]:
    """basename -> preferred key. Prefer regular folders over themed/event
    copies (top folders starting with '_', except _dlcbundle), then the
    shortest path, so a by-name fallback picks the canonical file."""
    def rank(key: str) -> tuple:
        parts = key.split('/')
        top = parts[1] if len(parts) > 2 else ''
        themed = top.startswith('_') and top != '_dlcbundle'
        return (themed, len(key), key)

    index: dict[str, str] = {}
    for key in sorted(keys, key=rank):
        index.setdefault(key.rsplit('/', 1)[-1], key)
    return index


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', type=Path, required=True)
    ap.add_argument('--endpoint', required=True)
    ap.add_argument('--workers', type=int, default=32)
    args = ap.parse_args()

    assets = args.assets.expanduser()
    host = urllib.parse.urlparse(args.endpoint).netloc
    token = TOKEN_FILE.read_text().strip()

    files = [p for top in ('content', 'mst') for p in (assets / top).rglob('*')
             if p.is_file() and p.name not in SKIP_NAMES]
    keys = {p: p.relative_to(assets).as_posix() for p in files}

    DONE_FILE.parent.mkdir(exist_ok=True)
    done = set(DONE_FILE.read_text().split('\n')) if DONE_FILE.exists() else set()
    todo = [p for p in files if keys[p] not in done]
    total_bytes = sum(p.stat().st_size for p in todo)
    print(f'{len(files)} files, {len(todo)} to upload ({total_bytes / 1e9:.2f} GB)', flush=True)

    lock = threading.Lock()
    sent = 0
    sent_bytes = 0
    failures = []
    start = time.time()
    with DONE_FILE.open('a') as done_log, ThreadPoolExecutor(args.workers) as pool:
        def send(path: Path) -> None:
            put(host, token, keys[path], path.read_bytes(), content_type(path))  # read lazily: 3 GB total

        futures = {pool.submit(send, p): p for p in todo}
        for fut in as_completed(futures):
            p = futures[fut]
            try:
                fut.result()
            except Exception as exc:  # keep going; report at the end
                failures.append(str(exc))
                continue
            with lock:
                done_log.write(keys[p] + '\n')
                sent += 1
                sent_bytes += p.stat().st_size
                if sent % 2000 == 0 or sent == len(todo):
                    done_log.flush()
                    rate = sent_bytes / 1e6 / max(time.time() - start, 1)
                    print(f'{sent}/{len(todo)} files, {sent_bytes / 1e9:.2f} GB, {rate:.1f} MB/s', flush=True)

    index = build_name_index(list(keys.values()))
    put(host, token, '_meta/name-index.json', json.dumps(index, separators=(',', ':')).encode(), 'application/json')
    print(f'name index: {len(index)} names; failures: {len(failures)}', flush=True)
    for f in failures[:20]:
        print('  FAIL', f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
