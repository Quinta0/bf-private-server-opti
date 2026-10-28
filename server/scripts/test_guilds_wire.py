"""Wire test for guilds between real players (docs/features/GUILDS_SOCIAL_2026-10-17.md).

Creates fresh accounts on a running test server and walks the whole flow:
  A. Landing without a guild: GuildInfo sends guild id 0 (the client's own "no
     guild"), the founding fee, and the recommended guilds (csI7dh30).
  B. A founds a guild; B finds it in the recommended list and by search (name
     and id); a nonsense search is empty.
  C. B applies (GuildJoin type 1): nothing changes until A decides.  B's
     message box shows the pending application (action 91); A's Requests
     screen (GuildJoinedList) shows it as a type-1 row with guild id 0 and an
     invite id; A accepts (GuildInviteManage) and B is a Member.
  D. B's GuildInfo shows the guild, the real Guild Master name, and a roster
     where A is rank 1 and B rank 4, both with names and units.
  E. Invites: a Member cannot invite; A invites C; C's message box shows the
     invite (action 90); C accepts and the reply carries the guild.  A invites
     D, D declines in the message box.  D applies, A refuses it.
  F. Ranks: A makes C an Officer and B the Vice; a second Vice is refused;
     an Officer cannot dismiss anyone; A dismisses C, whose next GuildInfo is
     guild 0 again.
  G. Settings and board: A saves a description and crest (GuildUpdate); B
     cannot.  A posts; the post reply carries the board, newest first; B's
     GuildInfo shows the unread badge until B opens the board.
  H. Ranking: E founds a second guild; GuildRanking lists every guild and
     repeats the viewer's own guild as the LAST row; GuildRankingDetail answers
     for the TAPPED guild, with each member's name in s35idar9.
  I. Friend cards carry the guild id under sD73jd20 (FriendSearch), and
     GuildMemberGuildInfo answers with that player's guild including Nt38aDqi.
  K. Computer-run friends: only the Guild Master can add one from the Hall's
     candidates, and a real member sees them under their own name (drawn
     from the owner's roster, not the viewer's).
  J. Leaving: the Guild Master leaves with a Vice present and the Vice becomes
     Guild Master (the old master's computer-run friends leave with them); the last real member's GuildUpdate disband removes the guild.

No refusal may close the session: every reply must be a success.

Usage: python3 -I scripts/test_guilds_wire.py [--url http://127.0.0.1:9968]
           [--container NAME] [--db /src/deploy/gme.sqlite]
The database is read from inside the server's container (docker exec), like
tools/test_fair_summon.py, because SQLite locks are not shared across a Docker
Desktop bind mount.  Creates throwaway accounts: never run it against a save
with real players.
"""
import argparse
import base64
import gzip
import json
import secrets
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
GUILD_INFO = ('138ba8d4', '23gD81ia')
GUILD_CREATE = ('g298Da10', 'G23Bd01d')
GUILD_SEARCH = ('R38ba9M3', '0D18dQn4')
GUILD_JOIN = ('bfa2D1bp', '9b3abdk1')
GUILD_JOINED_LIST = ('3890ab5j', '820b38z5')
GUILD_INVITE_MANAGE = ('1D8bba8D', 'bUd2bd0e')
GUILD_MEMBER_UPDATE = ('ad81b8at', '2b1bDo2m')
GUILD_MEMBER_GUILD_INFO = ('38bSeq81', 'h8TmR1bi')
GUILD_UPDATE = ('92bDoqBi', 'w3Bne038')
GUILD_BOARD_INFO = ('J93ki3Bw', 'm83D19ib')
GUILD_BOARD_POST = ('X89bDai1', 'Bi9Ralbq')
GUILD_RANKING = ('2b9D01b4', '23Djab0e')
GUILD_RANKING_DETAIL = ('7ekSBz2y', 'tWF58aK0')
INBOX = ('rYSfaC4P', '0R9ZPaSf')
GUILD_RECOMMENDED_MEMBER = ('ja5Enusw', '8upheqaC')
FRIEND_SEARCH = ('umNt8M7i', 'qa3sHuDgKxV5nI1F')

RUN = secrets.token_hex(2)
failures = 0
checks = 0


def check(label: str, cond: bool, detail='') -> None:
    global failures, checks
    checks += 1
    print(('PASS  ' if cond else 'FAIL  ') + label + ('' if cond else f'  -- {detail}'))
    failures += 0 if cond else 1


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


def aes_ecb(key: str, data: bytes, decrypt: bool) -> bytes:
    hexkey = key.encode().ljust(16, b'\0')[:16].hex()
    cmd = ['openssl', 'enc', '-aes-128-ecb', '-K', hexkey, '-nopad'] + (['-d'] if decrypt else [])
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def gme_call(base: str, group: str, key: str, body: dict) -> dict:
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    envelope = json.dumps({
        'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'guildtest'},
        'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(aes_ecb(key, raw + bytes([pad]) * pad, False)).decode()},
    }).encode()
    status, data = fetch(f'{base}/bf/gme/action.php', envelope)
    if status != 200:
        raise RuntimeError(f'HTTP {status}: {data[:200]!r}')
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        return {'_unencrypted': outer}
    plain = aes_ecb(key, base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']), True)
    return json.loads(plain[:-plain[-1]])


def wire_error(reply: dict) -> str | None:
    if '_unencrypted' not in reply:
        return None
    error = reply['_unencrypted'].get('b5PH6mZa') or {}
    return error.get('ZC0msu2L') or json.dumps(reply['_unencrypted'])[:200]


_IN_CONTAINER_SQL = r'''
import json, sqlite3, sys
req = json.load(sys.stdin)
con = sqlite3.connect(req["path"], timeout=30)
rows = con.execute(req["sql"], req["params"]).fetchall()
con.commit()
print(json.dumps(rows))
'''


class Database:
    def __init__(self, container: str, path: str):
        self.container = container
        self.path = path

    def rows(self, sql: str, *params) -> list:
        done = subprocess.run(
            ['docker', 'exec', '-i', self.container, 'python3', '-c', _IN_CONTAINER_SQL],
            input=json.dumps({'path': self.path, 'sql': sql, 'params': list(params)}),
            capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f'SQL in {self.container} failed: {done.stderr.strip()[-300:]}')
        return json.loads(done.stdout)

    def one(self, sql: str, *params):
        rows = self.rows(sql, *params)
        return rows[0] if rows else None


def container_on_port(url: str) -> str | None:
    port = urllib.parse.urlsplit(url).port
    out = subprocess.run(['docker', 'ps', '--filter', f'publish={port}', '--format', '{{.Names}}'],
                         capture_output=True, text=True, timeout=30).stdout.split()
    return out[0] if len(out) == 1 else None


class Player:
    def __init__(self, base: str, db: Database, label: str):
        self.base = base
        self.label = label
        query = urllib.parse.urlencode({'identifiers': json.dumps({'android_id': secrets.token_hex(8)}),
                                        'dp': 'Android', 'dn': 'guild-test', 'v': '16'})
        status, body = fetch(f'{base}/accounts/guest/login/?{query}')
        if status != 200:
            raise RuntimeError(f'guest login HTTP {status}')
        login = json.loads(body)
        node = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token'], 'h7eY3sAK': ''}
        self.name = f'G{label}{RUN}'
        created = gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(node, B5JQyV8j=self.name)],
                                                '9u45RGcV': [{'Kn51uR4Y': '1'}]})
        if wire_error(created) is not None:
            raise RuntimeError(f'CreateUser refused: {wire_error(created)}')
        init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [node]})
        user_id = ((init.get('IKqx1Cn9') or [{}])[0]).get('h7eY3sAK')
        if not user_id:
            raise RuntimeError(f'Initialize gave no user id: {wire_error(init) or init}')
        self.login = [dict(node, h7eY3sAK=user_id)]
        self.user_id = user_id
        row = db.one('SELECT friend_id FROM user_info WHERE id = ?', user_id)
        self.friend_id = row[0] if row else ''
        # A fresh account cannot afford the founding fee (50,000 Zel).
        db.rows('UPDATE user_info SET zel = 1000000 WHERE id = ?', user_id)

    def call(self, request: tuple[str, str], body: dict | None = None, what: str = '') -> dict:
        reply = gme_call(self.base, *request, dict(body or {}, IKqx1Cn9=self.login,
                                                    **{'6FrKacq7': [{'Kn51uR4Y': 'guildtest'}]}))
        err = wire_error(reply)
        check(f'{self.label}: {what or request[0]} answers without an error', err is None, err)
        return reply

    # --- guild helpers
    def guild_info(self) -> dict:
        return self.call(GUILD_INFO, what='GuildInfo')

    def guild_id(self) -> int:
        rows = self.guild_info().get('IkdSufj5') or []
        return int(rows[0]['sD73jd20']) if rows else -1

    def create(self, name: str) -> dict:
        return self.call(GUILD_CREATE, {'IkdSufj5': [{'sD73jd20': '0', 's35idar9': name,
                                                      'qp37xTDh': 'test guild', 'dDKCN293': '2'}]},
                         'GuildCreate')

    def search(self, text: str) -> list:
        return self.call(GUILD_SEARCH, {'kj1d80ai': [{'bj729kiq': text}]}, f'GuildSearch "{text}"').get('kj1d80ai', [])

    def join(self, guild_id: int, user_id: str, kind: int) -> dict:
        return self.call(GUILD_JOIN, {'aj38Jk10': [{'sD73jd20': str(guild_id), 'h7eY3sAK': user_id,
                                                    'xvkLFco0': str(kind)}]},
                         'GuildJoin ' + ('apply' if kind == 1 else 'invite'))

    def requests(self) -> list:
        return self.call(GUILD_JOINED_LIST, what='GuildJoinedList').get('aj38Jk10', [])

    def manage(self, invite_id: int, accept: bool) -> dict:
        return self.call(GUILD_INVITE_MANAGE, {'aj38Jk10': [{'7ad38bad': str(invite_id),
                                                             'ad8D1kqP': '1' if accept else '0'}]},
                         'GuildInviteManage ' + ('accept' if accept else 'decline'))

    def inbox(self) -> list:
        return self.call(INBOX, {'YBsNO3cI': [{'78Zema70': '0', 'Hhgi79M1': '2'}]},
                         'InboxMessageManage fetch').get('D9dkXSPY', [])

    def inbox_action(self, message_id: int, action: int) -> dict:
        return self.call(INBOX, {'YBsNO3cI': [{'78Zema70': str(message_id), 'Hhgi79M1': str(action)}]},
                         f'InboxMessageManage action {action}')

    def member_update(self, guild_id: int, member: str, rank: int) -> dict:
        return self.call(GUILD_MEMBER_UPDATE, {'IkdSufj5': [{'sD73jd20': str(guild_id)}],
                                               'csIuech30': [{'h7eY3sAK': member, 'gr48vsdJ': str(rank)}]},
                         f'GuildMemberUpdate {rank}')

    def update(self, guild_id: int, description: str, art: int, action: int) -> dict:
        return self.call(GUILD_UPDATE, {'IkdSufj5': [{'sD73jd20': str(guild_id), 'qp37xTDh': description,
                                                      'dDKCN293': str(art), 'ad8bdAj1': str(action)}]},
                         f'GuildUpdate action {action}')


def roster(reply: dict) -> dict:
    return {m['h7eY3sAK']: m for m in reply.get('csIuech30', [])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9968')
    ap.add_argument('--container', default=None)
    ap.add_argument('--db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    container = args.container or container_on_port(args.url)
    if not container:
        print('No container publishes that port; pass --container', file=sys.stderr)
        return 2
    db = Database(container, args.db)

    a, b, c, d, e = (Player(args.url, db, x) for x in 'ABCDE')
    print(f'accounts: A={a.user_id} B={b.user_id} C={c.user_id} D={d.user_id} E={e.user_id}')

    # A. Landing without a guild
    info = a.guild_info()
    rows = info.get('IkdSufj5') or []
    check('A: guildless GuildInfo carries guild id 0', len(rows) == 1 and rows[0].get('sD73jd20') == '0', rows)
    check('A: guildless GuildInfo carries the founding fee', bool(info.get('jKeiqDbl')), info.keys())

    # B. Founding, recommended, search
    name = f'Guild{RUN}'
    created = a.create(name)
    gid = int((created.get('IkdSufj5') or [{}])[0].get('sD73jd20', 0))
    check('A: GuildCreate returns the new guild', gid > 0, created.get('IkdSufj5'))
    check('A: founder is rank 1 on the roster', roster(created).get(a.user_id, {}).get('gr48vsdJ') == '1', roster(created))
    if gid <= 0:
        print('Cannot go on without a guild')
        return 1

    binfo = b.guild_info()
    rec_ids = [int(g['sD73jd20']) for g in binfo.get('csI7dh30', [])]
    check('B: recommended list (csI7dh30) offers the new guild', gid in rec_ids, rec_ids)
    rec = next((g for g in binfo.get('csI7dh30', []) if int(g['sD73jd20']) == gid), {})
    check('B: recommended row has level >= 1 and the master name',
          int(rec.get('osidufj5', 0)) >= 1 and rec.get('aBcniqj8') == a.name, rec)
    found = b.search(name[-6:].lower())
    check('B: search by part of the name finds it', gid in [int(g['sD73jd20']) for g in found], found)
    found = b.search(str(gid))
    check('B: search by guild id finds it', gid in [int(g['sD73jd20']) for g in found], found)
    check('B: search for nonsense is empty', b.search('zz' + secrets.token_hex(6)) == [])

    # C. Apply -> pending -> accept
    b.join(gid, b.user_id, 1)
    check('B: applying does not make B a member', b.guild_id() == 0)
    msgs = b.inbox()
    own = [m for m in msgs if m.get('oohpPLCt') == '91' and int(m.get('sD73jd20', 0)) == gid]
    check('B: message box shows the pending application (action 91)', len(own) == 1, msgs)
    reqs = a.requests()
    app = [r for r in reqs if r.get('h7eY3sAK') == b.user_id]
    check('A: Requests screen lists B as a type-1 row', len(app) == 1 and app[0].get('xvkLFco0') == '1', reqs)
    check('A: application row has guild id 0 (Accept/Refuse buttons)', app and app[0].get('sD73jd20') == '0', app)
    check('A: application row carries a name and a unit', app and app[0].get('B5JQyV8j') == b.name
          and int(app[0].get('pn16CNah', '0')) > 0, app)
    invite_id = int(app[0]['7ad38bad']) if app else 0
    accepted = a.manage(invite_id, True)
    check('A: accepting answers {} (the client updates itself)', accepted == {} or 'csIuech30' not in accepted, accepted)
    check('B: is now a member', b.guild_id() == gid)
    check('A: Requests screen no longer lists B', not [r for r in a.requests() if r.get('h7eY3sAK') == b.user_id])
    check('B: the application message is gone', not [m for m in b.inbox() if m.get('oohpPLCt') == '91'])

    # D. B's view of the guild
    binfo = b.guild_info()
    block = (binfo.get('IkdSufj5') or [{}])[0]
    check('B: guild block names A as Guild Master', block.get('aBcniqj8') == a.name, block)
    check('B: guild block counts two members', block.get('SivJ9sL9') == '2', block)
    r = roster(binfo)
    check('B: roster shows A as rank 1 with name', r.get(a.user_id, {}).get('gr48vsdJ') == '1'
          and r.get(a.user_id, {}).get('B5JQyV8j') == a.name, r.get(a.user_id))
    check('B: roster shows B as rank 4 with name and unit', r.get(b.user_id, {}).get('gr48vsdJ') == '4'
          and r.get(b.user_id, {}).get('B5JQyV8j') == b.name and int(r.get(b.user_id, {}).get('pn16CNah', 0)) > 0,
          r.get(b.user_id))

    # E. Invites
    b.join(gid, d.user_id, 2)
    check('B (Member): cannot invite', not db.rows('SELECT 1 FROM user_guild_requests WHERE user_id = ? AND kind = 2', d.user_id))
    a.join(gid, c.user_id, 2)
    msgs = c.inbox()
    inv = [m for m in msgs if m.get('oohpPLCt') == '90' and int(m.get('sD73jd20', 0)) == gid]
    check('C: message box shows the invite (action 90)', len(inv) == 1, msgs)
    check('C: invite message names the guild and the inviter', inv and name in inv[0].get('bQvVOkkR', '')
          and a.name in inv[0].get('bQvVOkkR', ''), inv)
    check('A: Requests screen lists the invite as a type-2 row',
          [x for x in a.requests() if x.get('h7eY3sAK') == c.user_id and x.get('xvkLFco0') == '2'])
    joined = c.manage(int(inv[0]['7ad38bad']) if inv else 0, True)
    jrows = joined.get('IkdSufj5') or []
    check('C: accepting the invite returns the guild just joined', jrows and int(jrows[0]['sD73jd20']) == gid, joined.keys())
    check('C: that reply\'s roster includes C', c.user_id in roster(joined))

    a.join(gid, d.user_id, 2)
    inv = [m for m in d.inbox() if m.get('oohpPLCt') == '90']
    check('D: has the invite', len(inv) == 1)
    d.inbox_action(int(inv[0]['78Zema70']) if inv else 0, 0)
    check('D: declining removes the invite', not [m for m in d.inbox() if m.get('oohpPLCt') == '90'])
    check('D: is not a member after declining', d.guild_id() == 0)
    d.join(gid, d.user_id, 1)
    app = [x for x in a.requests() if x.get('h7eY3sAK') == d.user_id]
    a.manage(int(app[0]['7ad38bad']) if app else 0, False)
    check('A: refusing D\'s application removes it', not [x for x in a.requests() if x.get('h7eY3sAK') == d.user_id])
    check('D: is not a member after the refusal', d.guild_id() == 0)

    # F. Ranks
    def rank_of(p: Player) -> str | None:
        return roster(a.guild_info()).get(p.user_id, {}).get('gr48vsdJ')
    a.member_update(gid, c.user_id, 3)
    check('A: C promoted to Officer', rank_of(c) == '3')
    a.member_update(gid, b.user_id, 3)
    a.member_update(gid, b.user_id, 2)
    check('A: B promoted to Vice', rank_of(b) == '2')
    a.member_update(gid, c.user_id, 2)
    check('A: a second Vice is refused', rank_of(c) == '3')
    c.member_update(gid, b.user_id, 0)
    check('C (Officer): cannot dismiss the Vice', rank_of(b) == '2')
    reply = a.member_update(gid, c.user_id, 0)
    check('A: dismissal reply carries no roster', 'csIuech30' not in reply, reply.keys())
    check('A: C dismissed', rank_of(c) is None)
    cinfo = c.guild_info()
    check('C: next GuildInfo is guild 0 again', (cinfo.get('IkdSufj5') or [{}])[0].get('sD73jd20') == '0', cinfo.get('IkdSufj5'))

    # G. Settings and board
    a.update(gid, 'new description', 3, 0)
    row = db.one('SELECT description, guild_art_id FROM user_guilds WHERE guild_id = ?', gid)
    check('A: GuildUpdate saves description and crest', row == ['new description', 3], row)
    b.update(gid, 'vice edit', 4, 0)
    row = db.one('SELECT description, guild_art_id FROM user_guilds WHERE guild_id = ?', gid)
    check('B (Vice): cannot edit the settings', row == ['new description', 3], row)
    a.call(GUILD_BOARD_INFO, {'adf38ba1': [{}]}, 'GuildBoardInfo')
    posted = a.call(GUILD_BOARD_POST, {'YBsNO3cI': [{'ZC0msu2L': 'first post'}]}, 'GuildBoardPost')
    a.call(GUILD_BOARD_POST, {'YBsNO3cI': [{'ZC0msu2L': 'second post'}]}, 'GuildBoardPost')
    posted = a.call(GUILD_BOARD_INFO, {'adf38ba1': [{}]}, 'GuildBoardInfo')
    posts = posted.get('adf38ba1', [])
    check('A: board lists both posts, newest first', [p_['ZC0msu2L'] for p_ in posts] == ['second post', 'first post'], posts)
    check('A: posts carry the author name, user id and a unit', posts and posts[0].get('B5JQyV8j') == a.name
          and posts[0].get('h7eY3sAK') == a.user_id and posts[0].get('pn16CNah') not in ('', None), posts[:1])
    flag = (b.guild_info().get('IkdSufj5') or [{}])[0].get('b6D19bg9')
    check('B: GuildInfo shows the unread board badge', flag in ('1', 1, True), flag)
    b.call(GUILD_BOARD_INFO, {'adf38ba1': [{}]}, 'GuildBoardInfo')
    flag = (b.guild_info().get('IkdSufj5') or [{}])[0].get('b6D19bg9')
    check('B: opening the board clears the badge', flag in ('0', 0, False), flag)

    # H. Ranking
    e_created = e.create(f'  Second{RUN}  ')
    gid2 = int((e_created.get('IkdSufj5') or [{}])[0].get('sD73jd20', 0))
    check('E: founds a second guild', gid2 > 0 and gid2 != gid, gid2)
    check('E: the founding name is trimmed', db.one('SELECT name FROM user_guilds WHERE guild_id = ?', gid2)
          == [f'Second{RUN}'], gid2)
    ranking = b.call(GUILD_RANKING, {'g7hx43Pq': [{'C6W4Vpow': '2'}]}, 'GuildRanking').get('g7hx43Pq', [])
    ids = [int(x['sD73jd20']) for x in ranking]
    check('B: ranking lists both guilds', gid in ids[:-1] and gid2 in ids[:-1], ids)
    check('B: ranking repeats B\'s own guild as the last row', ids and ids[-1] == gid, ids)
    check('B: ranks are numbered 1..N', [int(x['P_RANK']) for x in ranking[:-1]] == list(range(1, len(ranking))), ranking)
    detail = b.call(GUILD_RANKING_DETAIL, {'R6YwbeCB': [{'dk39bDa1': '1', 'sD73jd20': str(gid2)}]},
                    'GuildRankingDetail').get('R6YwbeCB', [])
    check('B: ranking detail answers for the tapped guild', [x['h7eY3sAK'] for x in detail] == [e.user_id], detail)
    check('B: detail rows carry the member name in s35idar9', detail and detail[0].get('s35idar9') == e.name, detail)

    # I. Friend cards and a friend's guild
    found = d.call(FRIEND_SEARCH, {'BPz1e5tU': [{'98WfKiyA': b.friend_id}]}, 'FriendSearch').get('tojMy68W', [])
    card = next((x for x in found if x.get('h7eY3sAK') == b.user_id), {})
    check('D: a friend card carries the guild id under sD73jd20', card.get('sD73jd20') == str(gid), card.get('sD73jd20'))
    viewed = d.call(GUILD_MEMBER_GUILD_INFO, {'csIuech30': [{'h7eY3sAK': b.user_id}]}, 'GuildMemberGuildInfo')
    vrow = (viewed.get('8bD18LPe') or [{}])[0]
    check('D: GuildMemberGuildInfo answers with B\'s guild', int(vrow.get('sD73jd20', 0)) == gid, viewed)
    check('D: that row carries max members (Nt38aDqi)', int(vrow.get('Nt38aDqi', 0)) >= 10, vrow)

    # K. Computer-run friends
    npc_id = None
    cands = a.call(GUILD_RECOMMENDED_MEMBER, what='GuildRecomendedMember').get('fRaBu6et', [])
    check('A: the Hall offers computer-run candidates', len(cands) > 0, cands[:1])
    if cands:
        npc = cands[0]
        npc_id = npc['h7eY3sAK']
        b.join(gid, npc['h7eY3sAK'], 2)
        check('B (Vice): cannot add a computer-run friend', npc['h7eY3sAK'] not in roster(a.guild_info()))
        a.join(gid, npc['h7eY3sAK'], 2)
        seen = roster(b.guild_info()).get(npc['h7eY3sAK'], {})
        check('A: computer-run friend joins at once', bool(seen), npc['h7eY3sAK'])
        check('B: sees the computer-run member under their own name', seen.get('B5JQyV8j') == npc.get('B5JQyV8j')
              and int(seen.get('pn16CNah', 0)) > 0, (seen.get('B5JQyV8j'), npc.get('B5JQyV8j')))

    # J. Leaving and disbanding
    a.member_update(gid, a.user_id, 0)
    check('A: leaving as Guild Master leaves A without a guild', a.guild_id() == 0)
    binfo = b.guild_info()
    check('B: the Vice is the new Guild Master', roster(binfo).get(b.user_id, {}).get('gr48vsdJ') == '1'
          and (binfo.get('IkdSufj5') or [{}])[0].get('aBcniqj8') == b.name, roster(binfo).get(b.user_id))
    check('B: the old master\'s computer-run friend left with them', npc_id is not None
          and npc_id not in roster(binfo), list(roster(binfo)))
    b.update(gid, '', 0, -1)
    check('B: disband removes the guild', db.one('SELECT 1 FROM user_guilds WHERE guild_id = ?', gid) is None)
    check('B: and every membership', db.one('SELECT COUNT(*) FROM user_guild_members WHERE guild_id = ?', gid) == [0])
    check('B: next GuildInfo is guild 0', b.guild_id() == 0)
    e.update(gid2, '', 0, -1)

    print(f'\n{checks - failures}/{checks} checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
