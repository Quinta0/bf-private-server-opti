"""Wire test for account transfer codes (gimuserver/gme/handlers/Transfer.cpp).

Drives TransferCodeIssue / TransferCodeCheck / Transfer the way the client
sends them, then checks both the replies and the save database:

  issue     reply IKqx1Cn9 has exactly LZ2PfNm4/4WSu1irc/iyJH5k6p; the DB holds
            only hashes; bad password and missing session are refused
  check     target account returned under u3JU7LzQ (no IKqx1Cn9); wrong
            password is a Retry refusal (cmd 2) and counts a failure; own code
            is refused; per-user failures are rate-limited; expired code
  path A    the requesting device's key moves to the target; both tokens die
            (old ones get Close); the device's next guest login resolves to the
            target; a replay is answered without changing anything; expiry
            grace after a passed Check; the old device gets a new account and
            a tolerant Initialize
  path B    a locked code stays locked for Transfer unless Check passed
            in the last 15 minutes; a code locked AFTER a passed Check still
            transfers with the right password; no-session Transfer leaves a
            vid claim; a replay succeeds; guest login with that vid binds the
            device and consumes the claim; an all-zero advertising id is
            refused
  hardening a claim aimed at a device whose account has progress is dropped
            (the victim keeps its account); two concurrent Transfers of one
            code cannot both win; a concurrent burst from one requester gets
            at most 5 judged failures; a burst beyond the KDF queue is refused
            with M_RATE; a malformed code is refused without a derivation;
            with --log-dir, no code, transfer password or session token of
            this run appears in the HTTP dump logs

Usage (inside the toolchain image, deploy dir mounted read-write at /deploy):
  python -I tools/test_transfer.py --url http://host.docker.internal:9960 \
      --db /deploy/gme.sqlite --log-dir /deploy/log

Creates throwaway gumi users and accounts.  Back up the save first.  Every
device in a run has its own ids, so no-session requests land in their own
rate-limit buckets and runs can be repeated back to back.
"""
import argparse
import base64
import hashlib
import json
import secrets
import sqlite3
import sys
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
from Crypto.Cipher import AES  # noqa: E402
from smoke_test import INITIALIZE, fetch, gme_call  # noqa: E402

CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
ISSUE = ('LK9e0WyX', 'B6siV3AT')
CHECK = ('Fn1UzTo3', 'A6QiFT7D')
TRANSFER = ('I9DyF2aU', '2hp90fZ1')

M_BAD = 'The Transfer ID or password is incorrect.'
M_RATE = 'Too many attempts. Please wait 15 minutes and try again.'
M_LOCK = ('This Transfer ID is locked after too many failed attempts. '
          'Issue a new code on the original device.')
M_SELF = 'This Transfer ID belongs to the account you are already using.'
M_EXP = 'This Transfer ID has expired. Issue a new code on the original device.'
M_SESS = 'Please restart the game, tap the title screen once, then try again.'
M_PW = 'Password must be 4-12 letters or numbers.'
CMD_RETRY, CMD_CLOSE = 2, 4
SIGNAL = [{'Kn51uR4Y': 'test'}]
VERSIONS = [{'nE3p0YtG': 'F_TEST', 'Kn51uR4Y': '1'}]

# Every session token and transfer code this run produced; none may show up
# in the server's dump logs (--log-dir).
SECRETS: set[str] = set()


def sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


class Device:
    """One emulated install: stable ids plus its current gumi session."""

    def __init__(self, base: str, label: str):
        self.base = base
        self.label = label
        self.android_id = secrets.token_hex(8)
        self.vid = f'{secrets.token_hex(4)}-{secrets.token_hex(2)}-{secrets.token_hex(2)}-' \
                   f'{secrets.token_hex(2)}-{secrets.token_hex(6)}'
        self.push_token = 'push-' + secrets.token_hex(12)
        self.gumi_id = ''
        self.token = ''
        self.user_id = ''  # what the client's keychain would hold

    @property
    def device_key(self) -> str:
        return sha('android_id:' + self.android_id)

    def guest_login(self) -> str:
        query = urllib.parse.urlencode({
            'identifiers': json.dumps({'android_id': self.android_id}),
            'vid': self.vid, 'dp': 'Android', 'dn': 'test-device', 'v': '16',
        })
        status, body = fetch(f'{self.base}/accounts/guest/login/?{query}')
        reply = json.loads(body)
        if status != 200 or reply.get('status') != 'successful':
            raise RuntimeError(f'{self.label}: guest login failed: HTTP {status} {reply}')
        self.gumi_id, self.token = reply['game_user_id'], reply['token']
        SECRETS.add(self.token)
        return self.gumi_id

    def login_node(self, **extra) -> dict:
        node = {'iN7buP2h': self.gumi_id, 'iN7buP1i': self.token, 'h7eY3sAK': self.user_id}
        node.update(extra)
        return node

    def initialize(self, gumi_id=None, token=None) -> dict:
        node = self.login_node()
        if gumi_id is not None:
            node['iN7buP2h'] = gumi_id
        if token is not None:
            node['iN7buP1i'] = token
        return gme_call(self.base, *INITIALIZE, {'IKqx1Cn9': [node]})

    def create_user(self, name: str) -> str:
        self.user_id = ''
        node = self.login_node(B5JQyV8j=name)
        reply = gme_call(self.base, *CREATE_USER, {'IKqx1Cn9': [node], '9u45RGcV': [{'Kn51uR4Y': '1'}]})
        if '_unencrypted' in reply:
            raise RuntimeError(f'{self.label}: CreateUser refused: {reply}')
        init = self.initialize()
        self.user_id = init['IKqx1Cn9'][0]['h7eY3sAK']
        return self.user_id

    def issue(self, password: str) -> dict:
        node = {'h7eY3sAK': self.user_id, 'nrg19RGe': '0', 'iN7buP0j': 'test-device',
                '4WSu1irc': password, 'iN7buP2h': self.gumi_id, 'iN7buP1i': self.token,
                'K29dp2Q': '', '236dItKo': 'x', 'Ma5GnU0H': self.push_token, 'DFY3k6qp': '2'}
        reply = gme_call(self.base, *ISSUE, {'6FrKacq7': SIGNAL, 'KeC10fuL': VERSIONS, 'IKqx1Cn9': [node]})
        code = (reply.get('IKqx1Cn9') or [{}])[0].get('LZ2PfNm4')
        if code:
            SECRETS.add(code)
        return reply

    def check(self, code: str, password: str, session: bool = True) -> dict:
        node = {'h7eY3sAK': self.user_id, 'LZ2PfNm4': code, '4WSu1irc': password, 'nrg19RGe': '0',
                'iN7buP2h': self.gumi_id if session else '', 'iN7buP1i': self.token if session else '',
                'K29dp2Q': '', 'Ma5GnU0H': self.push_token, 'DFY3k6qp': '2'}
        # A real body: unknown keys, including a duplicated 236dItKo.
        raw = json.dumps({'6FrKacq7': SIGNAL, 'KeC10fuL': VERSIONS, 'IKqx1Cn9': [node]})
        raw = raw.replace('"K29dp2Q": ""', '"K29dp2Q": "", "236dItKo": "a", "236dItKo": "b"')
        return _raw_call(self.base, CHECK, raw)

    def transfer(self, target_id: str, code: str, password: str, session: bool = True,
                 adid: str | None = None) -> dict:
        adid = self.vid if adid is None else adid
        node = {'h7eY3sAK': target_id, 'iN7buP0j': 'test-device',
                'iN7buP2h': self.gumi_id if session else '', 'iN7buP1i': self.token if session else '',
                'nrg19RGe': '0', 'dzhBs92A': self.push_token, 'LZ2PfNm4': code, '4WSu1irc': password,
                'Ma5GnU0H': adid, 'DFY3k6qp': '2', '6K7LToCD': adid}
        body = {'6FrKacq7': SIGNAL, 'KeC10fuL': VERSIONS, 'IKqx1Cn9': [node],
                'u3JU7LzQ': [{'h7eY3sAK': self.user_id}]}
        return gme_call(self.base, *TRANSFER, body)


def _raw_call(base: str, request: tuple[str, str], raw_json: str) -> dict:
    """gme_call with a pre-serialised body, so duplicate keys survive."""
    group, key = request
    cipher = AES.new(key.encode().ljust(16, b'\0')[:16], AES.MODE_ECB)
    raw = raw_json.encode()
    pad = 16 - len(raw) % 16
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'test'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    status, data = fetch(f'{base}/bf/gme/action.php', json.dumps(envelope).encode())
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


def refusal(reply: dict) -> tuple[int, str] | None:
    """(cmd, message) of an error reply, or None for a success."""
    error = reply.get('_unencrypted', {}).get('b5PH6mZa')
    if not error:
        return None
    return int(error.get('iPD12YCr', -1)), error.get('ZC0msu2L', '')


class Db:
    def __init__(self, path: str):
        self.path = path

    def rows(self, sql: str, *args) -> list[tuple]:
        for attempt in range(10):
            try:
                with sqlite3.connect(self.path, timeout=10) as conn:
                    return conn.execute(sql, args).fetchall()
            except sqlite3.OperationalError:
                if attempt == 9:
                    raise
                time.sleep(0.5)
        return []

    def one(self, sql: str, *args):
        rows = self.rows(sql, *args)
        return rows[0] if rows else None

    def execute(self, sql: str, *args) -> None:
        with sqlite3.connect(self.path, timeout=10) as conn:
            conn.execute(sql, args)


class Runner:
    def __init__(self):
        self.failures = 0

    def check(self, label: str, ok: bool, detail: str = '') -> bool:
        self.failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail and not ok else ''}")
        return ok

    def refused(self, label: str, reply: dict, message: str, cmd: int = CMD_RETRY) -> bool:
        got = refusal(reply)
        return self.check(label, got == (cmd, message), got if got else reply)

    def succeeded(self, label: str, reply: dict) -> bool:
        return self.check(label, refusal(reply) is None and '_unencrypted' not in reply, reply)


def test_issue(t: Runner, db: Db, a: Device, password: str) -> str:
    reply = a.issue(password)
    issued = (reply.get('IKqx1Cn9') or [{}])[0]
    t.check('issue: IKqx1Cn9 keys are exactly LZ2PfNm4/4WSu1irc/iyJH5k6p',
            set(issued) == {'LZ2PfNm4', '4WSu1irc', 'iyJH5k6p'}, reply)
    t.check('issue: reply has only 6FrKacq7 and IKqx1Cn9', set(reply) == {'6FrKacq7', 'IKqx1Cn9'}, sorted(reply))
    code = issued.get('LZ2PfNm4', '')
    t.check('issue: code is 10 digits, first not 0', len(code) == 10 and code.isdigit() and code[0] != '0', code)
    t.check('issue: password echoed', issued.get('4WSu1irc') == password)
    expiry = int(issued.get('iyJH5k6p', '0'))
    t.check('issue: expiry about 30 days out', abs(expiry - (time.time() + 30 * 86400)) < 120, expiry)

    row = db.one('SELECT code_hash, password_hash FROM account_transfer_codes WHERE user_id=?', a.user_id)
    t.check('issue: DB stores sha256("code:"+code)', row is not None and row[0] == sha('code:' + code), row)
    t.check('issue: DB stores a PBKDF2 password hash',
            row is not None and row[1].startswith('pbkdf2-sha256$600000$') and len(row[1].split('$')) == 4, row)
    dump = repr(db.rows('SELECT * FROM account_transfer_codes')) + repr(db.rows('SELECT * FROM account_transfers'))
    t.check('issue: neither code nor password in plaintext in the DB',
            code not in dump and f"'{password}'" not in dump)

    t.refused('issue: short password refused (M_PW)', a.issue('ab'), M_PW)
    t.refused('issue: non-alphanumeric password refused (M_PW)', a.issue('ab-c1'), M_PW)
    saved = a.token
    a.token = secrets.token_hex(16)
    t.refused('issue: wrong token refused (M_SESS)', a.issue(password), M_SESS)
    a.token = saved
    return code


def test_check(t: Runner, db: Db, a: Device, b: Device, code: str, password: str, name: str) -> None:
    before = db.one('SELECT failed_attempts FROM account_transfer_codes WHERE user_id=?', a.user_id)[0]
    t.refused('check: wrong password refused with Retry (M_BAD)', b.check(code, 'wrong1'), M_BAD)
    after = db.one('SELECT failed_attempts FROM account_transfer_codes WHERE user_id=?', a.user_id)[0]
    t.check('check: wrong password counted', after == before + 1, (before, after))
    started = time.time()
    t.refused('check: malformed code refused (M_BAD)', b.check('12-4567890', password), M_BAD)
    malformed = time.time() - started
    started = time.time()
    t.refused('check: unknown code refused (M_BAD)', b.check(random_code(), password), M_BAD)
    unknown = time.time() - started
    t.check('check: malformed code costs no derivation (much faster than an unknown one)',
            malformed < unknown / 2, f'malformed {malformed:.2f}s, unknown {unknown:.2f}s')
    t.refused('check: own code refused (M_SELF)', a.check(code, password), M_SELF)

    reply = b.check(' ' + code + ' ', password)
    target = (reply.get('u3JU7LzQ') or [{}])[0]
    t.check('check: target returned under u3JU7LzQ',
            target.get('h7eY3sAK') == a.user_id and target.get('B5JQyV8j') == name
            and target.get('D9wXQI2V', '').isdigit(), reply)
    t.check('check: no IKqx1Cn9, code or password in the reply',
            'IKqx1Cn9' not in reply and 'LZ2PfNm4' not in json.dumps(reply)
            and '4WSu1irc' not in json.dumps(reply), reply)
    t.check('check: success resets failures',
            db.one('SELECT failed_attempts FROM account_transfer_codes WHERE user_id=?', a.user_id)[0] == 0)


def random_code() -> str:
    return '9' + str(secrets.randbelow(10 ** 9)).zfill(9)


def lock_code(base: str, code: str) -> None:
    """Ten wrong passwords from two gumi users (five each, the per-user budget)."""
    for i in range(2):
        locker = Device(base, f'locker{i}')
        locker.guest_login()
        for _ in range(5):
            locker.check(code, 'guess99')


def test_rate_limit(t: Runner, base: str) -> None:
    probe = Device(base, 'rate-probe')
    probe.guest_login()
    for _ in range(5):
        probe.check(random_code(), 'nopass1')
    t.refused('check: 6th failure from one gumi user rate-limited (M_RATE)',
              probe.check('1234567890', 'nopass1'), M_RATE)

    burst = Device(base, 'burst-probe')
    burst.guest_login()
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(lambda _: refusal(burst.check(random_code(), 'nopass1')), range(8)))
    judged = results.count((CMD_RETRY, M_BAD))
    t.check('check: a concurrent burst from one gumi user gets at most 5 judged failures',
            judged <= 5 and results.count((CMD_RETRY, M_RATE)) == 8 - judged, results)


def test_kdf_queue(t: Runner, base: str) -> None:
    probes = [Device(base, f'queue{i}') for i in range(20)]  # no session, each its own bucket
    with ThreadPoolExecutor(len(probes)) as pool:
        results = list(pool.map(lambda p: refusal(p.check(random_code(), 'nopass1', session=False)), probes))
    t.check('kdf queue: every reply is M_BAD or M_RATE',
            all(r in ((CMD_RETRY, M_BAD), (CMD_RETRY, M_RATE)) for r in results), results)
    t.check('kdf queue: a burst beyond the queue is refused with M_RATE',
            (CMD_RETRY, M_RATE) in results, results)


def test_path_a(t: Runner, db: Db, a: Device, b: Device, code: str, password: str) -> None:
    old_a_gumi, old_a_token = a.gumi_id, a.token
    old_b_gumi, old_b_token = b.gumi_id, b.token
    b_old_user = b.user_id

    # Expiry: Check refuses an expired code, Transfer still honours it because
    # Check passed within the last 15 minutes (the client has already swapped).
    db.execute('UPDATE account_transfer_codes SET expires_at=? WHERE user_id=?', int(time.time()) - 100, a.user_id)
    t.refused('check: expired code refused (M_EXP)', b.check(code, password), M_EXP)

    b.user_id = a.user_id  # Confirm OK swaps the target id into the keychain
    transfers_before = db.one('SELECT COUNT(*) FROM account_transfers')[0]
    reply = b.transfer(a.user_id, code, password)
    b.user_id = b_old_user  # u3JU7LzQ above carried the old id
    t.check('transfer A: success, reply is only 6FrKacq7', refusal(reply) is None and set(reply) == {'6FrKacq7'},
            reply)

    rows = {r[0]: r[1:] for r in db.rows(
        'SELECT id, device_key, token_hash, unbound_at FROM gumi_live_users WHERE id IN (?,?)',
        old_a_gumi, old_b_gumi)}
    t.check("transfer A: B's device key moved to A's gumi user", rows[old_a_gumi][0] == b.device_key, rows)
    t.check("transfer A: B's old gumi user unbound", rows[old_b_gumi][0] is None and rows[old_b_gumi][2] > 0, rows)
    t.check('transfer A: both token hashes cleared', rows[old_a_gumi][1] is None and rows[old_b_gumi][1] is None)
    t.check('transfer A: code consumed',
            db.one('SELECT COUNT(*) FROM account_transfer_codes WHERE user_id=?', a.user_id)[0] == 0)
    audit = db.one("SELECT via, from_gumi_user_id, to_gumi_user_id, to_user_id FROM account_transfers"
                   " ORDER BY id DESC LIMIT 1")
    t.check('transfer A: audit row written', audit == ('session', old_b_gumi, old_a_gumi, a.user_id), audit)
    t.check('transfer A: model_change_count bumped',
            db.one('SELECT model_change_count FROM user_info WHERE id=?', a.user_id)[0] == 1)

    snapshot = repr(db.rows('SELECT * FROM gumi_live_users WHERE id IN (?,?)', old_a_gumi, old_b_gumi))
    b.user_id = a.user_id
    t.succeeded('transfer A: identical replay answered with success', b.transfer(a.user_id, code, password))
    b.user_id = b_old_user
    t.check('transfer A: replay changed nothing',
            snapshot == repr(db.rows('SELECT * FROM gumi_live_users WHERE id IN (?,?)', old_a_gumi, old_b_gumi))
            and db.one('SELECT COUNT(*) FROM account_transfers')[0] == transfers_before + 1)

    t.refused("transfer A: old device's token now gets Close",
              a.initialize(old_a_gumi, old_a_token), 'Your session has expired. Please restart the game.', CMD_CLOSE)
    t.refused("transfer A: B's previous token now gets Close",
              b.initialize(old_b_gumi, old_b_token), 'Your session has expired. Please restart the game.', CMD_CLOSE)

    b.user_id = a.user_id  # what the keychain holds after the done screen
    t.check('transfer A: device B guest login resolves to the target', b.guest_login() == old_a_gumi, b.gumi_id)
    init = b.initialize()
    login = (init.get('IKqx1Cn9') or [{}])[0]
    t.check('transfer A: Initialize on B returns the target account', login.get('h7eY3sAK') == a.user_id, init)

    new_gumi = a.guest_login()  # old device relaunches; keychain still has the target id
    t.check('transfer A: old device gets a new gumi user', new_gumi not in (old_a_gumi, old_b_gumi), new_gumi)
    init = a.initialize()
    login = (init.get('IKqx1Cn9') or [{}])[0]
    t.check('transfer A: mismatched Initialize tolerated, new-user flow',
            refusal(init) is None and login.get('h7eY3sAK') == '' and login.get('LZ2PfNm4') == ''
            and login.get('4WSu1irc') == '', init if refusal(init) else login)


def test_path_b(t: Runner, db: Db, owner: Device, target_user: str, password: str, base: str) -> None:
    owner.user_id = target_user
    code = owner.issue(password)['IKqx1Cn9'][0]['LZ2PfNm4']
    target_gumi = owner.gumi_id

    newcomer = Device(base, 'C')  # fresh install: no guest login yet
    lock_code(base, code)
    t.refused('check: code locked after 10 wrong passwords (M_LOCK)', newcomer.check(code, password, session=False),
              M_LOCK)
    newcomer.user_id = target_user
    t.refused('transfer B: locked code without a recent Check refused even with the right password (M_LOCK)',
              newcomer.transfer(target_user, code, password, session=False), M_LOCK)
    t.check('transfer B: refused transfer left the code in place',
            db.one('SELECT COUNT(*) FROM account_transfer_codes WHERE user_id=?', target_user)[0] == 1)

    # The client's own sequence: Check passes, someone else then locks the
    # code, and the Transfer that follows Confirm OK must still go through.
    code = owner.issue(password)['IKqx1Cn9'][0]['LZ2PfNm4']  # re-issue clears the lock
    newcomer.user_id = ''
    t.succeeded('check: fresh code passes Check without a session', newcomer.check(code, password, session=False))
    lock_code(base, code)
    newcomer.user_id = target_user
    reply = newcomer.transfer(target_user, code, password, session=False)
    t.succeeded('transfer B: code locked after a passed Check still transfers with the right password', reply)
    claim = db.one('SELECT to_gumi_user_id, expires_at FROM transfer_device_claims WHERE vid_hash=?',
                   sha('vid:' + newcomer.vid))
    t.check('transfer B: vid claim written for the target gumi user',
            claim is not None and claim[0] == target_gumi and claim[1] > time.time(), claim)
    t.check("transfer B: target's session ended",
            db.one('SELECT token_hash FROM gumi_live_users WHERE id=?', target_gumi)[0] is None)
    t.succeeded('transfer B: replay answered from the claim',
                newcomer.transfer(target_user, code, password, session=False))

    t.check('transfer B: guest login with the vid binds the target', newcomer.guest_login() == target_gumi,
            newcomer.gumi_id)
    t.check('transfer B: claim consumed',
            db.one('SELECT COUNT(*) FROM transfer_device_claims WHERE vid_hash=?', sha('vid:' + newcomer.vid))[0] == 0)
    login = (newcomer.initialize().get('IKqx1Cn9') or [{}])[0]
    t.check('transfer B: Initialize returns the target account', login.get('h7eY3sAK') == target_user, login)

    code2 = newcomer.issue(password)['IKqx1Cn9'][0]['LZ2PfNm4']
    zero = Device(base, 'D')
    zero.user_id = target_user
    t.refused('transfer B: all-zero advertising id refused (M_SESS)',
              zero.transfer(target_user, code2, password, session=False,
                            adid='00000000-0000-0000-0000-000000000000'), M_SESS)


def test_claim_needs_empty_device(t: Runner, db: Db, base: str, password: str) -> None:
    victim = Device(base, 'victim')
    victim.guest_login()
    victim_user = victim.create_user('Victim' + secrets.token_hex(2))
    victim_gumi = victim.gumi_id

    attacker = Device(base, 'attacker')
    attacker.guest_login()
    attacker.create_user('Thrw' + secrets.token_hex(2))
    code = attacker.issue(password)['IKqx1Cn9'][0]['LZ2PfNm4']
    stranger = Device(base, 'stranger')  # no session; aims the claim at the victim's advertising id
    stranger.user_id = attacker.user_id
    t.succeeded('claim attack: session-less Transfer naming the victim advertising id is accepted',
                stranger.transfer(attacker.user_id, code, password, session=False, adid=victim.vid))

    t.check('claim attack: victim guest login keeps its own gumi user', victim.guest_login() == victim_gumi,
            victim.gumi_id)
    t.check('claim attack: the claim was dropped',
            db.one('SELECT COUNT(*) FROM transfer_device_claims WHERE vid_hash=?', sha('vid:' + victim.vid))[0] == 0)
    row = db.one('SELECT device_key, unbound_at FROM gumi_live_users WHERE id=?', victim_gumi)
    t.check('claim attack: victim gumi user still bound to the victim device', row == (victim.device_key, 0), row)
    login = (victim.initialize().get('IKqx1Cn9') or [{}])[0]
    t.check('claim attack: victim Initialize returns its own account', login.get('h7eY3sAK') == victim_user, login)


def test_single_use(t: Runner, db: Db, base: str, password: str) -> None:
    owner = Device(base, 'once')
    owner.guest_login()
    owner.create_user('Once' + secrets.token_hex(2))
    code = owner.issue(password)['IKqx1Cn9'][0]['LZ2PfNm4']
    racers = [Device(base, f'racer{i}') for i in range(2)]
    for racer in racers:
        racer.user_id = owner.user_id
    with ThreadPoolExecutor(len(racers)) as pool:
        replies = list(pool.map(lambda r: r.transfer(owner.user_id, code, password, session=False), racers))
    outcomes = [refusal(r) for r in replies]
    t.check('single use: exactly one of two concurrent Transfers wins',
            outcomes.count(None) == 1 and (CMD_RETRY, M_BAD) in outcomes, outcomes)
    claims = db.one('SELECT COUNT(*) FROM transfer_device_claims WHERE to_gumi_user_id=?', owner.gumi_id)[0]
    t.check('single use: one device claim for the account', claims == 1, claims)
    audits = db.one('SELECT COUNT(*) FROM account_transfers WHERE to_user_id=?', owner.user_id)[0]
    t.check('single use: one audit row for the account', audits == 1, audits)


def test_dump_logs(t: Runner, log_dir: str, started: float, password: str) -> None:
    files = [f for f in Path(log_dir).glob('http_log_*') if f.stat().st_mtime >= started - 1]
    leaks = []
    for f in files:
        text = f.read_text(errors='replace')
        leaks += [f'{f.name}: secret' for s in SECRETS if s in text]
        if f'"4WSu1irc":"{password}"' in text:
            leaks.append(f'{f.name}: password')
    t.check(f'logs: no code, password or token in {len(files)} dump files of this run',
            bool(files) and not leaks, leaks[:5] or 'no dump files written')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9960')
    ap.add_argument('--db', required=True, help='path to the server save DB (gme.sqlite)')
    ap.add_argument('--log-dir', help='server dump log dir (deploy/log), to check that no secret was logged')
    args = ap.parse_args()
    run_started = time.time()
    t, db = Runner(), Db(args.db)

    a, b = Device(args.url, 'A'), Device(args.url, 'B')
    a.guest_login()
    b.guest_login()
    name = 'Xfer' + secrets.token_hex(3)
    a.create_user(name)
    b.create_user('Other' + secrets.token_hex(3))
    t.check('setup: two accounts', bool(a.user_id) and bool(b.user_id) and a.user_id != b.user_id,
            (a.user_id, b.user_id))

    password = 'abc' + str(secrets.randbelow(900) + 100)
    started = time.time()
    code = test_issue(t, db, a, password)
    print(f'      (issue took {time.time() - started:.1f}s including PBKDF2)')
    test_check(t, db, a, b, code, password, name)
    test_rate_limit(t, args.url)
    target_user = a.user_id
    test_path_a(t, db, a, b, code, password)
    test_path_b(t, db, b, target_user, password, args.url)
    test_claim_needs_empty_device(t, db, args.url, password)
    test_single_use(t, db, args.url, password)
    test_kdf_queue(t, args.url)
    if args.log_dir:
        test_dump_logs(t, args.log_dir, run_started, password)

    print(f'{t.failures} failure(s)')
    return 1 if t.failures else 0


if __name__ == '__main__':
    sys.exit(main())
