"""Smoke-test a running Brave Frontier server emulator.

Checks that static content and MST downloads are served, that the by-name
content fallback works, and that an encrypted GME action round-trips.

Usage: .venv/bin/python tools/smoke_test.py [--url http://127.0.0.1:9960] [--assets PATH]
Requires pycryptodome.
"""
import argparse
import base64
import gzip
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

DEFAULT_ASSETS = Path.home() / 'bf-archive/assets'
# Initialize: request group id and its AES key, as registered in
# gimuserver/gme/handlers/GmeControllerHandlers.cpp.
INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')


def fetch(url: str, data: bytes | None = None) -> tuple[int, bytes]:
    req = urllib.request.Request(url, data, {'Content-Type': 'application/json'} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            body = r.read()
            if r.headers.get('Content-Encoding') == 'gzip':
                body = gzip.decompress(body)
            return r.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def gme_call(base: str, group: str, key: str, body: dict) -> dict:
    cipher = AES.new(key.encode().ljust(16, b'\0')[:16], AES.MODE_ECB)
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'smoke'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    status, data = fetch(f'{base}/bf/gme/action.php', json.dumps(envelope).encode())
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9960')
    ap.add_argument('--assets', type=Path, default=DEFAULT_ASSETS)
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail: str = '') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + detail if detail else ''}")

    content = args.assets / 'content'
    sample = next((content / 'area').iterdir())
    status, body = fetch(f'{args.url}/content/area/{sample.name}')
    check('static content served byte-for-byte', status == 200 and body == sample.read_bytes(), f'HTTP {status}')

    mst = next(p for p in sorted((args.assets / 'mst').iterdir()) if p.suffix == '.dat')
    status, body = fetch(f'{args.url}/mst/{mst.name}')
    check('MST download served byte-for-byte', status == 200 and body == mst.read_bytes(), f'{mst.name}, HTTP {status}')

    png = next((content / 'sam').rglob('*.png'))
    status, _ = fetch(f'{args.url}/content/_dlcbundle/{png.name}')
    check('by-name fallback (_dlcbundle -> sam/)', status == 200, f'{png.name}, HTTP {status}')

    # Guest login hands out the gumi user id that Initialize identifies the player by.
    status, body = fetch(f'{args.url}/accounts/guest/login/')
    try:
        login = json.loads(body)
    except ValueError:
        login = {}
    gumi_id = login.get('game_user_id')
    check('guest login', status == 200 and bool(gumi_id), f'HTTP {status}, reply keys {sorted(login)}')

    try:
        reply = gme_call(args.url, *INITIALIZE, {'IKqx1Cn9': [{'iN7buP2h': gumi_id or ''}]})
        keys = sorted(reply)[:8]
        check('encrypted Initialize round-trip', bool(reply) and '_unencrypted' not in reply,
              f'{len(reply)} top-level keys, e.g. {keys}')
    except Exception as e:  # report, don't hide, the failure
        check('encrypted Initialize round-trip', False, repr(e))

    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
