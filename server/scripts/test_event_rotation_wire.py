"""Wire test: the weekly featured event (gme/common/EventRotation.hpp).

Each game week, Monday 08:00 UTC, one event of deploy/archive/event_rotation.json
is featured: only its missions pay its token and only its Bazaar shows and
sells.  No event is featured before first_week.  The server runs in Docker with
a libfaketime clock on one scratch save, restarted at each point in time:

  A. The Monday of first_week, 07:58 -> 08:00: nothing is featured before, so
     a clear pays nothing and the Bazaar refuses; after 08:00 pool[0] is: its
     token counts down to the week's end and names its dungeons, "each" pays
     every clear, "run" and "daily" once, its Bazaar lists full stock, a
     purchase spends tokens, an Elgif goes to the present box with its skill,
     and the permanent tokens keep timeleft -1.
  B. Tuesday 07:59 -> 08:00: a new game day pays "daily" again, not "run".
  C. The next Monday 08:00: pool[1] is featured.  Inside the grace the old
     event still pays and sells; after it, it does neither, its token is
     hidden (timeleft 0) with the balance kept, and pool[1]'s missions pay.
  D. pool[0]'s next run (len(pool) weeks later): full stock again, and "run"
     pays again.

Usage (from the worktree root; get libfaketime as in test_daily_rollover_wire.py):
  python3 -I scripts/test_event_rotation_wire.py \\
      --bin out/build-linux-arm64/standalone_frontend/gimuserverw \\
      --faketime "$DIR/usr/lib/aarch64-linux-gnu/faketime"
Never point it at a save with real players: it makes its own under out/.
"""
import argparse
import datetime as dt
import json
import socket
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_rollover_wire import LIB, UPDATE_INFO, Server, at, check, fresh_save, results, stamp  # noqa: E402
from test_fair_summon import Player, gme_call, wire_error  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WEEK = 7 * 86400
WEEK_START = 4 * 86400 + 8 * 3600   # Monday 1970-01-05 08:00 UTC
TOKEN_INFO = ('f49als4D', '94lDsgh4')
EXCHANGE_INFO = ('49zxdfl3', 'v2DfDSFl')
EXCHANGE_BUY = ('SLf48fs0', 'Odiel30s')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
TOKENS, TOKEN_ID, AMOUNT, TIMELEFT, DUNGEONS = 'l234vdKs', 'Slkc395l', 'lDk4hv20', 'fE2d6ivS', '0Dk4fc81'
OFFERS, OFFER_ID, REMAINING = 'c2Sls4DD', 'Kd3DL39d', 'S8rdp9zk'
CLOSED = 'closed for the week'

ROTATION = json.loads((ROOT / 'deploy/archive/event_rotation.json').read_text(encoding='utf-8'))
POOL = ROTATION['pool']
GRACE = ROTATION['grace_seconds']
MISSIONS = json.loads((ROOT / 'deploy/mst/mission_mst.json').read_text(encoding='utf-8'))['oXeC1Ak9']
DUNGEON_OF = {int(m['j28VNcUW']): int(m['MHx05sXt']) for m in MISSIONS}


def payout(event: dict, mode: str) -> dict:
    return next(p for p in event['payouts'] if p['mode'] == mode)


class Tester:
    def __init__(self, server: Server, player: Player | None = None):
        self.server = server
        self.player = player or Player(server.base, server.db)
        self.player.db, self.player.base = server.db, server.base

    def call(self, api: tuple, body: dict) -> dict:
        return gme_call(self.server.base, *api, dict(body, IKqx1Cn9=self.player.login))

    def now(self) -> int:
        return int(self.call(UPDATE_INFO, {})['AhSmZF07']['QxWZoA04'])

    def wait_until(self, t: int) -> None:
        while self.now() < t:
            subprocess.run(['sleep', '1'])

    def tokens(self, reply: dict | None = None) -> dict:
        rows = (reply if reply is not None else self.call(TOKEN_INFO, {})).get(TOKENS) or []
        return {int(r[TOKEN_ID]): r for r in rows}

    def balance(self, token: int) -> int:
        row = self.server.db.one('SELECT count FROM user_event_tokens WHERE user_id = ? AND token_id = ?',
                                 self.player.user_id, str(token))
        return int(row[0]) if row else 0

    def give(self, token: int, count: int) -> None:
        self.server.db.rows('INSERT INTO user_event_tokens (user_id, token_id, count) VALUES (?, ?, ?)'
                            ' ON CONFLICT(user_id, token_id) DO UPDATE SET count = excluded.count',
                            self.player.user_id, str(token), count)

    def clear(self, mission: int) -> dict:
        start = self.call(MISSION_START, {
            '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}], 'JzS3uxsZ': [{'0b4efi1W': '1'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0',
                          'Z0Y4RoD7': '0', 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}]})
        if wire_error(start) or 'Kz7qfSs5' not in start:
            raise RuntimeError(f'MissionStart {mission} refused: {wire_error(start) or json.dumps(start)[:300]}')
        battle = {k: '0' for k in ('Najhr8m6', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                                   'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9', 'HTVh8a65')}
        battle.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
        end = self.call(MISSION_END, {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}], 'rXvA1E5y': [battle],
                                      'Kz7qfSs5': [{'k9cxD7Ba': start['Kz7qfSs5'][0]['k9cxD7Ba'], 'j3g5P4cq': '2'}]})
        if wire_error(end):
            raise RuntimeError(f'MissionEnd {mission} refused: {wire_error(end)}')
        return end

    def pays(self, label: str, event: dict, mission: int, want: int) -> None:
        token = event['token']
        before = self.balance(token)
        end = self.clear(mission)
        got = self.balance(token) - before
        sent = self.tokens(end).get(token, {}).get(AMOUNT) if want else None
        check(f'{label}: clearing {mission} pays {want} of token {token}'
              + (' and the reply carries the balance' if want else ''),
              got == want and (not want or sent == str(before + want)), (got, sent))

    def offers(self, token: int) -> dict:
        reply = self.call(EXCHANGE_INFO, {TOKENS: [{TOKEN_ID: str(token)}]})
        return {r[OFFER_ID]: int(r[REMAINING]) for r in reply.get(OFFERS) or []}

    def buy(self, token: int, offer: str) -> dict:
        return self.call(EXCHANGE_BUY, {'c2Sls4DD': [{OFFER_ID: offer, 'H6k1LIxC': '1', TOKEN_ID: str(token)}]})

    def presents(self) -> list:
        return self.server.db.rows('SELECT target_id, unit_extra_skill FROM user_presents WHERE user_id = ?',
                                   self.player.user_id)


def featured_token(label: str, t: Tester, event: dict, at_time: int) -> None:
    rows = t.tokens()
    row = rows.get(event['token'], {})
    left = int(row.get(TIMELEFT, 0))
    week_end = WEEK_START + ((at_time - WEEK_START) // WEEK + 1) * WEEK
    want_dungeons = sorted({DUNGEON_OF[p['mission']] for p in event['payouts']})
    check(f'{label}: token {event["token"]} counts down to the week end and names its dungeons',
          0 <= (week_end - at_time) - left < 60
          and sorted(int(d) for d in row.get(DUNGEONS, '').split(',') if d) == want_dungeons,
          (left, week_end - at_time, row.get(DUNGEONS)))
    others = {e['token'] for e in POOL} - {event['token']}
    check(f'{label}: no other pool token is shown', all(int(rows[k][TIMELEFT]) == 0 for k in others if k in rows),
          {k: rows[k][TIMELEFT] for k in others if k in rows})
    check(f'{label}: the permanent tokens keep timeleft -1',
          all(int(r[TIMELEFT]) == -1 for k, r in rows.items() if k not in {e['token'] for e in POOL}))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bin', type=Path, default=ROOT / 'out/build-linux-arm64/standalone_frontend/gimuserverw')
    ap.add_argument('--faketime', type=Path, required=True, help=f'folder holding {LIB}')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--boot-timeout', type=int, default=240)
    args = ap.parse_args()
    args.root, args.volumes, args.workdir = ROOT, [], ROOT / 'out' / 'event-rotation-test'
    args.bin, args.faketime = args.bin.resolve(), args.faketime.resolve()
    if not args.port:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]

    first = dt.datetime.fromisoformat(ROTATION['first_week']).replace(tzinfo=dt.timezone.utc)
    monday = int(first.timestamp()) // 86400
    turn = at(monday, '08:00:00')
    e0, e1 = POOL[0], POOL[1]
    each, run, daily = payout(e0, 'each'), payout(e0, 'run'), payout(e0, 'daily')
    cheap = min((o for o in e0['offers'] if 'extra_skill' not in o and o['reward_type'] == 6 and o['limit'] > 1),
                key=lambda o: o['price'])
    elgif = next(o for o in e0['offers'] if 'extra_skill' in o)
    check(f'data: first_week {ROTATION["first_week"]} is a Monday', first.weekday() == 0)
    check('data: every pool token is distinct', len({e['token'] for e in POOL}) == len(POOL))
    save = fresh_save(args, 'rotation')

    # ---- A: first_week's Monday, across 08:00 --------------------------------
    with Server(args, save, args.bin, turn - 120, 'evt') as server:
        t = Tester(server)
        t.player.set(tutorial_status=12, tutorial_end_flag=1, energy=9999)
        t.pays('A before first_week', e0, each['mission'], 0)
        bought = t.buy(e0['token'], cheap['id'])
        check('A before first_week: the Bazaar refuses', CLOSED in (wire_error(bought) or ''), wire_error(bought))
        check('A: the checks above ran before 08:00', t.now() < turn, stamp(t.now()))

        t.wait_until(turn + 2)
        featured_token('A featured', t, e0, t.now())
        t.pays('A "each" first', e0, each['mission'], each['amount'])
        t.pays('A "each" again', e0, each['mission'], each['amount'])
        t.pays('A "run" first', e0, run['mission'], run['amount'])
        t.pays('A "run" again', e0, run['mission'], 0)
        t.pays('A "daily" first', e0, daily['mission'], daily['amount'])
        t.pays('A "daily" again', e0, daily['mission'], 0)

        stock = t.offers(e0['token'])
        check(f'A: the Bazaar lists all {len(e0["offers"])} offers at full stock',
              stock == {o['id']: o['limit'] for o in e0['offers']}, len(stock))
        before, units = t.balance(e0['token']), t.player.unit_count()
        bought = t.buy(e0['token'], cheap['id'])
        check(f'A: buying {cheap["name"]} spends {cheap["price"]} and adds the unit',
              not wire_error(bought) and t.balance(e0['token']) == before - cheap['price']
              and t.player.unit_count() == units + 1, (wire_error(bought), t.balance(e0['token'])))
        check('A: its stock drops by one', t.offers(e0['token'])[cheap['id']] == cheap['limit'] - 1)
        t.give(e0['token'], elgif['price'])
        presents = len(t.presents())
        bought = t.buy(e0['token'], elgif['id'])
        check(f'A: {elgif["name"]} goes to the present box with skill {elgif["extra_skill"]}',
              not wire_error(bought) and len(t.presents()) == presents + 1
              and (str(elgif['target_id']), elgif['extra_skill']) in [(str(a), int(b or 0)) for a, b in t.presents()],
              (wire_error(bought), t.presents()[-2:]))
        player = t.player

    # ---- B: Tuesday 08:00 is a new game day, not a new run --------------------
    with Server(args, save, args.bin, turn + 86400 - 60, 'tue') as server:
        t = Tester(server, player)
        t.wait_until(turn + 86400 + 2)
        t.pays('B new day "daily"', e0, daily['mission'], daily['amount'])
        t.pays('B new day "run"', e0, run['mission'], 0)

    # ---- C: the next Monday, pool[1] ------------------------------------------
    nxt = turn + WEEK
    with Server(args, save, args.bin, nxt - 60, 'next') as server:
        t = Tester(server, player)
        t.wait_until(nxt + 2)
        featured_token('C next week', t, e1, t.now())
        t.pays(f'C inside the {GRACE} s grace, the old event', e0, each['mission'], each['amount'])
        bought = t.buy(e0['token'], cheap['id'])
        check('C inside the grace: the old Bazaar still sells', not wire_error(bought), wire_error(bought))
    with Server(args, save, args.bin, nxt + GRACE + 30, 'late') as server:
        t = Tester(server, player)
        kept = t.balance(e0['token'])
        t.pays('C after the grace, the old event', e0, each['mission'], 0)
        bought = t.buy(e0['token'], cheap['id'])
        check('C after the grace: the old Bazaar refuses', CLOSED in (wire_error(bought) or ''), wire_error(bought))
        row = t.tokens().get(e0['token'], {})
        check('C: the old token is hidden (timeleft 0) with its balance kept',
              row.get(TIMELEFT) == '0' and row.get(AMOUNT) == str(kept), row)
        t.pays('C pool[1] "each"', e1, payout(e1, 'each')['mission'], payout(e1, 'each')['amount'])

    # ---- D: pool[0] comes back -------------------------------------------------
    back = turn + len(POOL) * WEEK
    with Server(args, save, args.bin, back + 30, 'back') as server:
        t = Tester(server, player)
        check('D next run: full stock again', t.offers(e0['token']).get(cheap['id']) == cheap['limit'])
        t.pays('D next run "run"', e0, run['mission'], run['amount'])

    failed = [name for name, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} passed')
    if not failed:
        for volume in args.volumes:
            subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
