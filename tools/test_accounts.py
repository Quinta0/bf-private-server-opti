"""Wire test for per-device guest accounts and session tokens.

Checks: two devices get two accounts; the same device gets its account back
with a new token; GME requests with a stale, wrong or missing token are
rejected, and the current token is accepted.

Usage: python -I tools/test_accounts.py [--url http://127.0.0.1:9960]
Requires pycryptodome. Creates throwaway gumi users with random device ids.

Run it against a test server, not a save with real players: on a save that
still has a single pre-migration account, the first fake device would adopt it.
"""
import argparse
import json
import secrets
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from smoke_test import INITIALIZE, fetch, gme_call  # noqa: E402


def guest_login(base: str, android_id: str) -> dict:
    query = urllib.parse.urlencode({
        'identifiers': json.dumps({'android_id': android_id}),
        'dp': 'Android', 'dn': 'test-device', 'v': '16',
    })
    status, body = fetch(f'{base}/accounts/guest/login/?{query}')
    if status != 200:
        raise RuntimeError(f'guest login HTTP {status}')
    return json.loads(body)


def initialize_ok(base: str, gumi_id: str, token: str | None) -> bool:
    login = {'iN7buP2h': gumi_id}
    if token is not None:
        login['iN7buP1i'] = token
    reply = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [login]})
    return bool(reply) and '_unencrypted' not in reply


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9960')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail: str = '') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + detail if detail else ''}")

    device_a, device_b = secrets.token_hex(8), secrets.token_hex(8)
    a1 = guest_login(args.url, device_a)
    b1 = guest_login(args.url, device_b)
    a2 = guest_login(args.url, device_a)

    check('login succeeds', all(r.get('status') == 'successful' for r in (a1, b1, a2)))
    check('two devices get two accounts', a1['game_user_id'] != b1['game_user_id'],
          f"{a1['game_user_id']} vs {b1['game_user_id']}")
    check('same device gets its account back', a1['game_user_id'] == a2['game_user_id'])
    check('each login mints a new token', a1['token'] != a2['token'] and len(a2['token']) == 32)
    check('no test_token anywhere', 'test_token' not in (a1['token'], b1['token'], a2['token']))

    gid = a2['game_user_id']
    check('current token accepted', initialize_ok(args.url, gid, a2['token']))
    check('stale token rejected', not initialize_ok(args.url, gid, a1['token']))
    check('wrong token rejected', not initialize_ok(args.url, gid, secrets.token_hex(16)))
    check('missing token rejected', not initialize_ok(args.url, gid, None))
    check("another device's token rejected", not initialize_ok(args.url, gid, b1['token']))
    check('unknown gumi user rejected', not initialize_ok(args.url, 'NoSuchId', a2['token']))

    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
