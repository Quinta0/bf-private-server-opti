"""Wire test for friends between real players (docs/features/FRIENDS_SPEC.md §8).

Creates three throwaway accounts (three fake devices), then walks: public
friend IDs, search, request/refuse, request/accept, favorite, the helper
picker, delete, mutual auto-accept and random search.

Usage: python -I test_friends.py --url http://127.0.0.1:9961
Needs /tools (smoke_test.py, test_accounts.py) on the path; see the run line in
the track notes.  Run against a TEST save: it creates accounts.
"""
import argparse
import secrets
import sys

sys.path.insert(0, '/tools')
from smoke_test import INITIALIZE, gme_call  # noqa: E402
from test_accounts import guest_login  # noqa: E402

CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')
FRIEND_GET = ('2o4axPIC', 'EoYuZ2nbImhCU1c0')
APPLY = ('WUNi08YL', 'gbpdAEZuv8IP40UF')
DELETE = ('SfMN9w4p', 'yWXI80UKQNBZFozw')
GET_AGREE = ('aVqJrR08', 'H4zJCK2jPeaskVdf')
AGREE = ('NYKBH4V6', '8PFVDiUIZyES2vzR')
REFUSE = ('N5SQd7oA', 'fk8YaVy9puJbnLWv')
SEARCH = ('umNt8M7i', 'qa3sHuDgKxV5nI1F')
RANDOM = ('mE5gUQOp', 'oK4VaDGF')
FAVORITE = ('ps6Vgx4R', 'XcE1CK6L')


class Player:
    def __init__(self, base: str, name: str):
        self.base = base
        login = guest_login(base, secrets.token_hex(8))
        self.login = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token']}
        # Not self.call: that would overwrite the login tag and drop the name.
        reply = gme_call(base, *CREATE_USER, {'IKqx1Cn9': [dict(self.login, B5JQyV8j=name)],
                                              '9u45RGcV': [{'Kn51uR4Y': '1'}]})
        info = self.call(INITIALIZE, {'IKqx1Cn9': [self.login]})
        user = (info.get('IKqx1Cn9') or reply.get('IKqx1Cn9') or [{}])[0]
        self.user_id = user.get('h7eY3sAK', '')
        self.friend_id = user.get('98WfKiyA', '')
        self.name = name
        # The client names itself in every later login tag; identity needs it.
        self.login['h7eY3sAK'] = self.user_id

    def call(self, action, body=None) -> dict:
        return gme_call(self.base, *action, dict(body or {}, IKqx1Cn9=[self.login]))

    def target(self, action, other: 'Player', **extra) -> dict:
        return self.call(action, {'BPz1e5tU': [dict({'h7eY3sAK': other.user_id}, **extra)]})


def rows(reply: dict) -> list:
    return reply.get('tojMy68W', []) or []


def type_of(reply: dict, user_id: str):
    hit = [r for r in rows(reply) if r.get('h7eY3sAK') == user_id]
    return hit[0].get('96Nxs2WQ') if hit else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9961')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail: str = '') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail else ''}")

    a, b, c = (Player(args.url, n) for n in ('FrTestA', 'FrTestB', 'FrTestC'))

    # 1. Public IDs
    check('A has a user id', bool(a.user_id), a.user_id)
    check('A friend ID is 9 digits', a.friend_id.isdigit() and len(a.friend_id) == 9, a.friend_id)
    again = a.call(INITIALIZE, {}).get('IKqx1Cn9', [{}])[0].get('98WfKiyA')
    check('A friend ID is stable', again == a.friend_id, again)
    check('B has a different ID', b.friend_id and b.friend_id != a.friend_id, b.friend_id)

    # 2. Search
    r = a.call(SEARCH, {'BPz1e5tU': [{'98WfKiyA': b.friend_id}]})
    check('search B -> type 4 row', type_of(r, b.user_id) == '4', type_of(r, b.user_id))
    check('search keeps NPC roster', any(x.get('96Nxs2WQ') == '1' for x in rows(r)), len(rows(r)))
    r = a.call(SEARCH, {'BPz1e5tU': [{'98WfKiyA': a.friend_id}]})
    check('search own ID -> own row', type_of(r, a.user_id) == '4')
    r = a.call(SEARCH, {'BPz1e5tU': [{'98WfKiyA': '000000001'}]})
    check('search unknown -> no type 4, no error',
          not any(x.get('96Nxs2WQ') == '4' for x in rows(r)) and bool(rows(r)))

    # 3. Apply, then refuse
    r = a.target(APPLY, b)
    check('A apply -> B is type 2 for A', type_of(r, b.user_id) == '2', type_of(r, b.user_id))
    r = b.call(GET_AGREE)
    check('B GetAgree -> A is type 3', type_of(r, a.user_id) == '3', type_of(r, a.user_id))
    row = [x for x in rows(r) if x.get('h7eY3sAK') == a.user_id]
    check('pending row has req_time', bool(row and row[0].get('3InKeya4')), row and row[0].get('3InKeya4'))
    r = b.call(FRIEND_GET)
    check('B FriendGet carries the type 3 row (badge)', type_of(r, a.user_id) == '3')
    r = b.call(REFUSE, {'BPz1e5tU': [{'h7eY3sAK': a.user_id, '7f3WL6Mx': ''}]})
    check('B refuse -> row gone for B', type_of(r, a.user_id) is None)
    check('refuse -> row gone for A', type_of(a.call(GET_AGREE), b.user_id) is None)

    # 4. Apply, then accept
    a.target(APPLY, b)
    r = b.target(AGREE, a)
    check('B agree -> A is friend for B', type_of(r, a.user_id) == '1', type_of(r, a.user_id))
    check('A sees B as friend', type_of(a.call(GET_AGREE), b.user_id) == '1')
    r = b.call(FRIEND_GET)
    helpers = [x for x in r.get('xZH6EIQ7', []) if x.get('h7eY3sAK') == a.user_id]
    check('B picker lists A', bool(helpers), len(r.get('xZH6EIQ7', [])))
    if helpers:
        h = helpers[0]
        check('A helper card is a friend card with a real unit',
              h.get('q9Arz8Jv') == '1' and h.get('pn16CNah') not in (None, '0'), {k: h.get(k) for k in ('q9Arz8Jv', 'pn16CNah')})
        check('picker lists A first', r['xZH6EIQ7'][0].get('h7eY3sAK') == a.user_id)

    # 5. Favorite
    r = b.target(FAVORITE, a, **{'5JbjC3Pp': '1'})
    fav_b = [x.get('5JbjC3Pp') for x in rows(r) if x.get('h7eY3sAK') == a.user_id]
    fav_a = [x.get('5JbjC3Pp') for x in rows(a.call(GET_AGREE)) if x.get('h7eY3sAK') == b.user_id]
    check('favorite is per side', fav_b == ['1'] and fav_a == ['0'], (fav_b, fav_a))

    # 6. Delete
    r = b.target(DELETE, a)
    check('B delete -> A gone from reply', type_of(r, a.user_id) is None and bool(rows(r)))
    check('delete is two-sided', type_of(a.call(GET_AGREE), b.user_id) is None)

    # 7. Mutual apply -> auto accept
    a.target(APPLY, c)
    r = c.target(APPLY, a)
    check('mutual apply -> friends at once', type_of(r, a.user_id) == '1', type_of(r, a.user_id))

    # 8. Random search: B shows (no relation), C does not (friend)
    r = a.call(RANDOM, {'3fTYzqT6': [{'3Ug2QynF': '0', 'lzpDckWv': '100'}], 'StQIyohe': [{'jkldTrhL': '0'}]})
    cands = [x.get('h7eY3sAK') for x in rows(r) if x.get('96Nxs2WQ') == '4']
    check('random search offers B', b.user_id in cands, len(cands))
    check('random search excludes friend C and self', c.user_id not in cands and a.user_id not in cands)

    # 9. Apply never refuses, even to nonsense
    r = a.call(APPLY, {'BPz1e5tU': [{'h7eY3sAK': 'NoSuchUser'}]})
    check('apply to unknown id still succeeds with a list', bool(rows(r)))

    # 10. NPCs are seeded once: deleting the last one does not bring five back
    d = Player(args.url, 'FrTestD')
    npcs = [x.get('h7eY3sAK') for x in rows(d.call(FRIEND_GET)) if x.get('96Nxs2WQ') == '1']
    check('new account gets the NPC roster', len(npcs) == 5, npcs)
    counts = []
    for npc in npcs:
        r = d.call(DELETE, {'BPz1e5tU': [{'h7eY3sAK': npc}]})
        counts.append(sum(1 for x in rows(r) if x.get('96Nxs2WQ') == '1'))
    check('deleting every NPC counts down to 0, no re-seed', counts == [4, 3, 2, 1, 0], counts)
    left = [x.get('h7eY3sAK') for x in rows(d.call(FRIEND_GET)) if x.get('96Nxs2WQ') == '1']
    check('later FriendGet does not re-seed', left == [], left)
    a.target(APPLY, d)
    r = d.target(AGREE, a)
    types = [x.get('96Nxs2WQ') for x in rows(r) if x.get('96Nxs2WQ') == '1']
    check('real friend fills the freed slot alone', types == ['1'] and type_of(r, a.user_id) == '1', types)

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
