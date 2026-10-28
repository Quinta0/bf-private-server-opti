#!/usr/bin/env python3
"""Wire test: a device keeps its account when its ANDROID_ID changes (23102026_GumiDeviceHint, ticket #69).
    python3 -I <wt>/scripts/test_guest_device_hint_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-devicehint PORT=9986 DETACH=1 docker/run.sh
    python3 -I <wt>/scripts/test_guest_device_hint_wire.py --src <wt> --url http://127.0.0.1:9986 \\
        --container bf-devicehint
--prepare puts three accounts past the tutorial on known ANDROID_IDs (out/qa/devicehint/accounts.json).
BlueStacks hands the game a new ANDROID_ID after a crash or a long session while the advertising id
(vid) and altvid stay; the guest login must then keep the account, and move it back when the old id
returns.  It must still make a new login when altvid differs, when the advertising id is all zeros,
when two accounts share the pair (a cloned emulator), and when the row's device key changed after the
hint was stored (a transfer or an admin tooling move).  Writes during the run go through docker exec (WAL
on a bind mount: host writes can split from the server's).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import sqlite3
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        failures.append(what)


def device_key(android_id: str) -> str:
    return hashlib.sha256(('android_id:' + android_id).encode()).hexdigest()


def new_android_id() -> str:
    return secrets.token_hex(8)


def new_uuid() -> str:
    h = secrets.token_hex(16)
    return f'{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}'


def prepare(src: Path) -> None:
    """Three logins, each with a copy of the save's first player past the tutorial (only the
    user_info row: the guest login checks nothing else)."""
    with sqlite3.connect(src / 'deploy/gme.sqlite', timeout=30) as db:
        model = db.execute('SELECT id FROM user_info WHERE tutorial_end_flag = 1 ORDER BY id LIMIT 1').fetchone()
        if not model:
            raise SystemExit('the save needs an account past the tutorial')
        columns = [c[1] for c in db.execute('PRAGMA table_info(user_info)')]
        fresh = {'id': '?', 'gumi_user_id': '?', 'friend_id': '?'}
        picks = ', '.join(fresh.get(c, c) for c in columns)
        accounts = []
        for _ in range(3):
            login, user, aid = secrets.token_hex(4), secrets.token_hex(4), new_android_id()
            db.execute('INSERT INTO gumi_live_users(id, device_key, created_at, last_login_at) VALUES (?, ?, 1, 1)',
                       (login, device_key(aid)))
            values = {'id': user, 'gumi_user_id': login, 'friend_id': str(secrets.randbelow(10**9))}
            db.execute(f'INSERT INTO user_info({", ".join(columns)}) SELECT {picks} FROM user_info WHERE id = ?',
                       [values[c] for c in columns if c in fresh] + [model[0]])
            accounts.append({'login': login, 'android_id': aid})
    out = src / 'out/qa/devicehint'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'accounts.json').write_text(json.dumps(accounts))
    print('prepared logins ' + ', '.join(a['login'] for a in accounts))


def login(url: str, android_id: str, vid: str, altvid: str) -> str:
    query = urllib.parse.urlencode({
        'dn': 'test-device', 'dp': 'Android', 'vid': vid, 'v': '11', 'ak': '0000000000000000', 'device_id': '',
        'altvid': altvid, 'identifiers': json.dumps({'android_id': android_id}), 'daid': vid})
    with urllib.request.urlopen(f'{url}/accounts/guest/login/?{query}', timeout=30) as r:
        answer = json.loads(r.read())
    if not answer.get('token'):
        raise SystemExit(f'guest login failed: {answer}')
    return str(answer.get('game_user_id') or answer.get('user_id'))


def write_save(container: str, sql: str, *args: str) -> None:
    code = ('import sqlite3, sys; c = sqlite3.connect("/src/deploy/gme.sqlite", timeout=30); '
            'c.execute(sys.argv[1], sys.argv[2:]); c.commit()')
    subprocess.run(['docker', 'exec', container, 'python3', '-I', '-c', code, sql, *args], check=True)


def run(src: Path, url: str, container: str) -> None:
    a, b1, b2 = json.loads((src / 'out/qa/devicehint/accounts.json').read_text())
    vid, alt = new_uuid(), new_uuid()

    check(login(url, a['android_id'], vid, alt) == a['login'], 'a known ANDROID_ID logs into its account')
    second = new_android_id()
    check(login(url, second, vid, alt) == a['login'], 'a new ANDROID_ID with the same vid+altvid keeps the account')
    check(login(url, second, vid, alt) == a['login'], 'the new ANDROID_ID is now the account\'s key')
    check(login(url, a['android_id'], vid, alt) == a['login'], 'the old ANDROID_ID coming back keeps the account too')

    other = login(url, new_android_id(), vid, new_uuid())
    check(other not in (a['login'], b1['login'], b2['login']), 'a different altvid (a reinstall) gets a new login')
    zeros = login(url, new_android_id(), '00000000-0000-0000-0000-000000000000', alt)
    check(zeros not in (a['login'], other), 'an all-zero advertising id gets a new login')

    shared_vid, shared_alt = new_uuid(), new_uuid()
    check(login(url, b1['android_id'], shared_vid, shared_alt) == b1['login'], 'clone 1 logs into its account')
    check(login(url, b2['android_id'], shared_vid, shared_alt) == b2['login'], 'clone 2 logs into its account')
    third = login(url, new_android_id(), shared_vid, shared_alt)
    check(third not in (b1['login'], b2['login']), 'a pair shared by two accounts gets a new login')

    # A transfer or an admin tooling move gives the row another phone's key; the old phone's hint must
    # not pull the account back before the new phone has logged in.
    phone = new_android_id()
    write_save(container, 'UPDATE gumi_live_users SET device_key=? WHERE id=?', device_key(phone), a['login'])
    stale = login(url, new_android_id(), vid, alt)
    check(stale != a['login'], 'a hint stored with an older device key is not trusted')
    phone_vid, phone_alt = new_uuid(), new_uuid()
    check(login(url, phone, phone_vid, phone_alt) == a['login'], 'the new phone logs into the moved account')
    check(login(url, new_android_id(), phone_vid, phone_alt) == a['login'],
          'the new phone keeps the account when its ANDROID_ID changes')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--prepare', action='store_true')
    ap.add_argument('--url')
    ap.add_argument('--container')
    args = ap.parse_args()
    if args.prepare:
        prepare(args.src)
        return 0
    if not args.url or not args.container:
        ap.error('--url and --container are needed to run the test')
    run(args.src, args.url.rstrip('/'), args.container)
    print(f'{len(failures)} failure(s)' if failures else 'all checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
