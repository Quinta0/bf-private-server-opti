"""Wire test for Classic Raid slice 1 (docs/features/RAIDS_SPEC.md 5.4).

Two fake devices A and B walk the entry flow the client sends when Raid is
tapped: RaidUserInfo -> RaidAgreement -> RaidWorloInfo -> RaidRoomFriendGet ->
queued RoonItemEdit/UpDateCash -> RaidQuickStart -> RaidRoomInfo ->
RaidMissionStart (opens the run; round 2) and the exit/dissolve paths.

Usage: python -I raid_entry_test.py --url http://127.0.0.1:9963
Needs pycryptodome and the shared helpers in tools/ (mounted at /tools).
Creates throwaway accounts: back up deploy/gme.sqlite first.
"""
import argparse
import secrets
import sys
from pathlib import Path

# /tools inside the toolchain container; ../../../tools from the worktree on the host.
_here = Path(__file__).resolve()
for candidate in ['/tools'] + [str(p / 'tools') for p in _here.parents]:
    if Path(candidate, 'smoke_test.py').exists():
        sys.path.insert(0, candidate)
        break
from smoke_test import INITIALIZE, gme_call  # noqa: E402
from test_accounts import guest_login  # noqa: E402

CREATE_USER = ('uV6yH5MX', '4agnATy2DrJsWzQk')  # as in tools/test_transfer.py
RAID_USER_INFO = ('jJZ01VQo', 'SsQ26dVY')
RAID_AGREEMENT = ('Cu94xU8j', 'Eh5a3yr9')
RAID_WORLD_INFO = ('H7GRp01L', '5JxFp0tD')
RAID_FRIEND_GET = ('Xe2UB3A8', '9WN8oYBx')
RAID_QUICK_START = ('CWeTY8BQ', 'HsVofOJJ')
RAID_ITEM_EDIT = ('bv9xt3m6', '9mKud8kc')
RAID_UPDATE_CASH = ('XTban70i', 'H5d6B2pi')
RAID_EXIT = ('jqi71nWu', 'PD4ke3Ii')
RAID_DISSOLVE = ('H8N1R0TE', 'q7dbw29J')
RAID_ROOM_INFO = ('0VxE6kTW', 'cUrw7K6W')
RAID_MISSION_START = ('2qDW9NBU', 'kY5y1zBj')
RAID_MISSION_EXIT = ('a12v0RcA', '14GMJFIN')

HELPER = {'h7eY3sAK': '987654', 'B5JQyV8j': 'HelperBob', '2Fh3J7ng': '120',
          '96Nxs2WQ': '1', 'pn16CNah': '10017', '4A6LzBxr': '80', '2pAyFjmZ': '0'}


class Player:
    def __init__(self, base: str, label: str):
        self.base, self.label = base, label
        login = guest_login(base, secrets.token_hex(8))
        self.login = {'iN7buP2h': login['game_user_id'], 'iN7buP1i': login['token']}
        # CreateUser (the name-entry step) makes the user_info row, then
        # Initialize hands back the game user id every later request carries.
        node = dict(self.login, h7eY3sAK='', B5JQyV8j=f'Raider{label}')
        made = gme_call(base, *CREATE_USER, {'IKqx1Cn9': [node], '9u45RGcV': [{'Kn51uR4Y': '1'}]})
        if '_unencrypted' in made:
            raise RuntimeError(f'{label}: CreateUser refused: {made}')
        init = gme_call(base, *INITIALIZE, {'IKqx1Cn9': [self.login]})
        if '_unencrypted' in init:
            raise RuntimeError(f'{label}: Initialize refused: {init}')
        self.user_id = init['IKqx1Cn9'][0]['h7eY3sAK']
        self.login['h7eY3sAK'] = self.user_id

    def call(self, request: tuple[str, str], **groups) -> dict:
        body = {'IKqx1Cn9': [self.login]}
        body.update(groups)
        return gme_call(self.base, *request, body)


def user_info(reply: dict) -> dict:
    rows = reply.get('1ry6BKoe') or [{}]
    return rows[0] if isinstance(rows, list) else rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9963')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail: str = '') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail else ''}")

    a, b = Player(args.url, 'A'), Player(args.url, 'B')

    # 1. First contact: terms not accepted, no room, full aube.
    r = a.call(RAID_USER_INFO)
    u = user_info(r)
    check('1 RaidUserInfo first contact', u.get('dziN1Ha9') == '0' and u.get('7x3pPB2C') == '1'
          and u.get('8VYd6xSX') == '' and u.get('C45mRxnZ') == '3' and u.get('S92Hcor3') == '3'
          and u.get('iFA0fd6Y') == '1', u)

    # 2. Agreement persists; permit list is rc-1 only.
    r = a.call(RAID_AGREEMENT)
    check('2a RaidAgreement sets agreement', user_info(r).get('dziN1Ha9') == '1', user_info(r))
    r = a.call(RAID_USER_INFO)
    permits = r.get('2WqoMm4d', [])
    check('2b agreement persisted', user_info(r).get('dziN1Ha9') == '1')
    check('2c permit list = 19 rc-1 missions incl. 2000',
          len(permits) == 19 and any(p['j28VNcUW'] == '2000' and p['qY49LBjw'] == '1800' for p in permits),
          f'{len(permits)} rows')
    check('2d clear list present (empty)', r.get('8LF2ohCY') == [], r.get('8LF2ohCY'))

    # 3. World list.
    r = a.call(RAID_WORLD_INFO)
    worlds = r.get('taWh0Pr7', [])
    check('3 RaidWorloInfo one open world "1"', len(worlds) == 1 and worlds[0].get('43hMuY2I') == '1'
          and worlds[0].get('iFA0fd6Y') == '1' and len(r.get('2WqoMm4d', [])) == 19, worlds)

    # 4. Helper picker (may be empty for a fresh account).
    r = a.call(RAID_FRIEND_GET, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': ''}]})
    check('4 RaidRoomFriendGet answers a list', isinstance(r.get('ft4zY6Uv'), list), list(r)[:5])

    # 5. Queued pre-start requests must succeed.
    r1 = a.call(RAID_ITEM_EDIT, Fihk73ZR=[{'kixHbe54': '20000', 'XuJL4pc5': '1', 'wgV86x1q': '2'}], GvV1w4yM=[])
    # The real shape (RaidUpDateCashRequest::createBody @0x13C8198): X6WBc9kJ is a
    # constant 1 in 79PaHFBh, sQDIZ8Pj 1 only for the master.
    r2 = a.call(RAID_UPDATE_CASH, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': ''}],
                                    '79PaHFBh': [{'X6WBc9kJ': '1', 'sQDIZ8Pj': '1'}]})
    check('5 RoonItemEdit + UpDateCash acknowledged', '_unencrypted' not in r1 and '_unencrypted' not in r2,
          (r1, r2))

    # 6. Quick start with a helper.
    r = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2000'}], 'ZX7gzQ6s': [HELPER]})
    rooms, members = r.get('8iBuZa3n', []), r.get('uwH62EZy', [])
    room = rooms[0] if rooms else {}
    a_uid = members[0].get('h7eY3sAK') if members else None
    check('6a QuickStart room row', len(rooms) == 1 and room.get('43hMuY2I') == '1'
          and room.get('GWfpjED4') == '1' and room.get('rPIya39u') == '1'
          and room.get('j28VNcUW') == '2000' and room.get('8ZK3w0Np') == a_uid, room)
    check('6b QuickStart member row is A, ready, not kicked', len(members) == 1
          and members[0].get('uX87tAZB') == '1' and members[0].get('D2ZKtGR9') == '0', members)
    check('6c helper echoed and on the room row', r.get('ZX7gzQ6s') == [HELPER]
          and room.get('gkYiqB40') == '987654' and room.get('1ctR6GHC') == '10017', room)
    check('6d login ids now name the room', user_info(r).get('8VYd6xSX') == room.get('8VYd6xSX')
          and user_info(r).get('43hMuY2I') == '1', user_info(r))
    room_id = room.get('8VYd6xSX', '')

    # 7. B is untouched.
    ub = user_info(b.call(RAID_USER_INFO))
    check('7 B isolated (no login room, own agreement 0)', ub.get('8VYd6xSX') == '' and ub.get('dziN1Ha9') == '0', ub)

    # 8. Heartbeat, then MissionStart refuses and closes the room.
    r = a.call(RAID_ROOM_INFO, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id}],
                                  'pw38qdui': [{'NLKce95n': '0'}], 'T49LxzKr': [{'wbo6qS5P': '1', 'c13QLNRf': '0'}]})
    check('8a RaidRoomInfo returns the room', [x.get('8VYd6xSX') for x in r.get('8iBuZa3n', [])] == [room_id], r)
    # Round 2 (MULTIPLAYER 4.1 rule 1): a non-member's heartbeat is a Retry
    # refusal; an empty list would leave a lobby polling a stale room forever.
    r = b.call(RAID_ROOM_INFO, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id}]})
    check('8b B polling A\'s room (not a member) is refused', '_unencrypted' in r, r)
    # Round 2: MissionStart opens the run (raid_field_test.py covers it); a
    # retire then closes the quick room.
    r = a.call(RAID_MISSION_START, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id, '8ZK3w0Np': a_uid}]})
    check('8c MissionStart opens the run', (r.get('8iBuZa3n') or [{}])[0].get('3tWuc7de') == '1', r)
    rm = next((m.get('FV6MU5nQ') for m in r.get('uwH62EZy', []) if m.get('h7eY3sAK') == a_uid), '')
    a.call(RAID_MISSION_EXIT, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id}],
                                 'g97TWISb': [{'ja39HMLq': rm}], 'GWfpjED4': [{'GWfpjED4': '1'}]})
    u = user_info(a.call(RAID_USER_INFO))
    check('8d login ids cleared after MissionExit', u.get('8VYd6xSX') == '' and u.get('43hMuY2I') == '', u)
    r = a.call(RAID_ROOM_INFO, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id}]})
    check('8e room gone (refused, not an empty list)', '_unencrypted' in r, r)

    # 9. RC gate: an RC-7 mission is refused and creates nothing.
    rc7 = '87100'
    r = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': rc7}]})
    u = user_info(a.call(RAID_USER_INFO))
    check('9 QuickStart above RC refused, no room', '_unencrypted' in r and u.get('8VYd6xSX') == '', (r, u))
    r = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': 'nope'}]})
    check('9b QuickStart unknown mission refused', '_unencrypted' in r, r)

    # 10. QuickStart without a helper, then RaidExit; then dissolve on another room.
    r = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2001'}]})
    room_id = (r.get('8iBuZa3n') or [{}])[0].get('8VYd6xSX', '')
    check('10a QuickStart without helper (no echo)', room_id != '' and 'ZX7gzQ6s' not in r
          and (r.get('8iBuZa3n') or [{}])[0].get('gkYiqB40') == '', r.get('8iBuZa3n'))
    r = a.call(RAID_EXIT, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': room_id}], 'uwH62EZy': [{'h7eY3sAK': a_uid}]})
    check('10b RaidExit clears login ids', user_info(r).get('8VYd6xSX') == '', user_info(r))
    r = b.call(RAID_AGREEMENT)
    r = b.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2002'}], 'ZX7gzQ6s': []})
    b_room = (r.get('8iBuZa3n') or [{}])[0].get('8VYd6xSX', '')
    check('10c B QuickStart with empty helper group', b_room not in ('', room_id) and 'ZX7gzQ6s' not in r, r.get('8iBuZa3n'))
    r = b.call(RAID_DISSOLVE, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': b_room}]})
    check('10d RaidRoomDissolve clears B', user_info(r).get('8VYd6xSX') == '', user_info(r))
    # A second QuickStart while still in a room replaces it rather than stacking.
    r1 = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2000'}]})
    r2 = a.call(RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2001'}]})
    first = (r1.get('8iBuZa3n') or [{}])[0].get('8VYd6xSX')
    r = a.call(RAID_ROOM_INFO, **{'8iBuZa3n': [{'43hMuY2I': '1', '8VYd6xSX': first}]})
    check('10e re-QuickStart closes the previous room', '_unencrypted' in r
          and user_info(r2).get('8VYd6xSX') != first, r)
    a.call(RAID_DISSOLVE)   # no body: falls back to the login room

    print(f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
