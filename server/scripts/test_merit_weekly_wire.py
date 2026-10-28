"""Wire test: Merit Shop evolution materials restock weekly (ticket #75)
and Merit Points are counted over a lifetime (ticket #76).

#75: an evolution-material offer (a kind-2 unit such as Fire Totem, or an
item_type-4 item such as Ignis Shard) is limited to 99 per week, the week
turning over on Monday at 08:00 UTC (gme/common/MeritShop.hpp).  Its row in
user_achievement_trades carries the week it counts toward and is reset by the
purchase itself; the shop shows the seconds to the restock in VDKB0Y5h.  Every
other offer keeps its lifetime limit and -1 there.

#76: user_info.achieve_point_total holds every point earned, uncapped; the
"N Merit Points Accumulated" subjects (band 7000) read it instead of the
balance, which stops at 999,999.  The migration seeds it with the balance plus
what the shop rows record as bought.

The test starts the servers itself, in Docker, with libfaketime clocks, on one
save in a named Docker volume:

  A. Monday 07:40 UTC, the server from before this change: two players buy
     (Fire Totem x5, Burst Frog x3, Ignis Shard x2) and hold balances.
  B. Monday 07:57, this server on the same save: the seed, the countdown, a
     buy to 99 and a refusal at 100, the lifetime limit untouched, earning past
     the cap through Deliver; then across 08:00: the Totem sells again, the
     Frog does not.
  C. Tuesday 07:59, restarted: the game day turns at 08:00 but the week does
     not, so the Totem stays sold out.

Usage (from the worktree root; get libfaketime as in test_daily_rollover_wire.py):
  python3 -I scripts/test_merit_weekly_wire.py \\
      --new-bin out/build-prof/standalone_frontend/gimuserverw \\
      --old-bin ../db-locked/out/build-prof/standalone_frontend/gimuserverw \\
      --faketime "$DIR/usr/lib/aarch64-linux-gnu/faketime"
Never point it at a save with real players: it makes its own under out/.
"""
import argparse
import datetime as dt
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_rollover_wire import (LIB, UPDATE_INFO, Server, at, check, fresh_save,  # noqa: E402
                                      results, stamp)
from test_fair_summon import Player, gme_call, wire_error  # noqa: E402

TRADE = ('m9LiF6P2', '0IWC9LVq')
ACHIEVEMENT_INFO = ('YPBU7MD8', 'AKjzyZ81')
DELIVER = ('vsaXI4M0', '2Lj5hIEG')

ROOT = Path(__file__).resolve().parents[1]
OFFERS = {r['Mdgsh04u']: r for r in json.loads(
    (ROOT / 'deploy/mst/achievement_trade_mst.json').read_text(encoding='utf-8'))['82CcMZhp']}
TOTEM = '81400392'      # Fire Totem, unit 10133 (kind 2): weekly
FROG = '80180392'       # Burst Frog, unit 10312 (kind 1): lifetime limit
SHARD = '91000059'      # Ignis Shard, item 800901 (item_type 4): weekly, keeps its 400
MILLION = '7140'        # "Gather a total of 1000000 Merit Points."

WEEK = 7 * 86400
WEEK_START = 4 * 86400 + 8 * 3600   # Monday 1970-01-05 08:00 UTC


def week_at(t: int) -> int:
    return (t - WEEK_START) // WEEK


def to_next_week(t: int) -> int:
    return WEEK_START + (week_at(t) + 1) * WEEK - t


def price(oid: str) -> int:
    return int(OFFERS[oid]['3EWLm0sA'])


class Shopper:
    def __init__(self, server: Server, player: Player | None = None):
        self.server = server
        self.player = player or Player(server.base, server.db)
        self.player.db, self.player.base = server.db, server.base
        self.uid = self.player.user_id

    def call(self, ids, body=None) -> dict:
        return gme_call(self.server.base, *ids, dict({'IKqx1Cn9': self.player.login}, **(body or {})))

    def now(self) -> int:
        return int(self.call(UPDATE_INFO)['AhSmZF07']['QxWZoA04'])

    def buy(self, oid: str, n: int) -> dict:
        return self.call(TRADE, {'rX74GNsm': [{'Mdgsh04u': oid, 'H6k1LIxC': str(n)}]})

    def info(self) -> tuple[dict, dict]:
        reply = self.call(ACHIEVEMENT_INFO)
        if wire_error(reply):
            raise RuntimeError(f'GetAchievementInfo refused: {wire_error(reply)}')
        trades = {r['Mdgsh04u']: (int(r['H6k1LIxC']), int(r['VDKB0Y5h'])) for r in reply.get('9j3ALx8I') or []}
        subjects = {r['M7SXoc31']: r for r in reply.get('YTRJLG65') or []}
        return trades, subjects

    def merit(self) -> tuple[int, int]:
        row = self.server.db.one('SELECT achieve_point, achieve_point_total FROM user_info WHERE id = ?', self.uid)
        return int(row[0]), int(row[1])

    def row(self, oid: str):
        row = self.server.db.one('SELECT count, period FROM user_achievement_trades'
                                 ' WHERE user_id = ? AND trade_id = ?', self.uid, oid)
        return None if row is None else tuple(row)

    def wait_until(self, t: int) -> None:
        deadline = time.monotonic() + max(0, t - self.now()) + 60
        while self.now() < t:
            if time.monotonic() > deadline:
                raise RuntimeError(f'the server clock never reached {stamp(t)}')
            time.sleep(1)


def bought(label: str, reply: dict, oid: str, count: int) -> None:
    rows = {r['Mdgsh04u']: int(r['H6k1LIxC']) for r in reply.get('9j3ALx8I') or []}
    check(f'{label}: accepted, the shop shows {count} bought',
          not wire_error(reply) and rows.get(oid) == count, wire_error(reply) or rows.get(oid))


def refused(label: str, shopper: Shopper, oid: str, n: int) -> None:
    before = (shopper.merit(), shopper.row(oid))
    reply = shopper.buy(oid, n)
    check(f'{label}: refused', bool(wire_error(reply)), reply)
    check(f'{label}: nothing debited or counted', (shopper.merit(), shopper.row(oid)) == before,
          ((shopper.merit(), shopper.row(oid)), before))


def countdown(label: str, shopper: Shopper, oid: str, count: int) -> None:
    trades, _ = shopper.info()
    now = shopper.now()
    got_count, state = trades.get(oid, (None, None))
    check(f'{label}: {oid} shows {count} bought', got_count == count, got_count)
    check(f'{label}: {oid} counts down to Monday 08:00 UTC ({to_next_week(now)} s)',
          state is not None and abs(state - to_next_week(now)) <= 3, (state, to_next_week(now)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--new-bin', type=Path, default=ROOT / 'out/build-prof/standalone_frontend/gimuserverw')
    ap.add_argument('--old-bin', type=Path, required=True, help='a server built before this change')
    ap.add_argument('--faketime', type=Path, required=True, help=f'folder holding {LIB}')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--boot-timeout', type=int, default=240)
    ap.add_argument('--monday', default='2026-11-02')
    args = ap.parse_args()
    args.root, args.volumes, args.workdir = ROOT, [], ROOT / 'out' / 'merit-weekly-test'
    args.new_bin, args.old_bin, args.faketime = (p.resolve() for p in (args.new_bin, args.old_bin, args.faketime))
    if not args.port:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    monday = int(dt.datetime.fromisoformat(args.monday).replace(tzinfo=dt.timezone.utc).timestamp()) // 86400
    restock = at(monday, '08:00:00')

    # ---- the data ----------------------------------------------------------
    check('data: Monday 08:00 UTC is a week boundary', to_next_week(restock) == WEEK and
          dt.datetime.fromtimestamp(restock, dt.timezone.utc).weekday() == 0)
    check('data: Fire Totem and Ignis Shard are 99 and 400, Burst Frog 10',
          (OFFERS[TOTEM]['S8rdp9zk'], OFFERS[SHARD]['S8rdp9zk'], OFFERS[FROG]['S8rdp9zk']) == ('99', '400', '10'))

    save = fresh_save(args, 'merit')

    # ---- A: the server before this change buys -----------------------------
    with Server(args, save, args.old_bin, at(monday, '07:40:00'), 'old') as old:
        a, b = Shopper(old), Shopper(old)
        a.player.set(achieve_point=500000)
        b.player.set(achieve_point=999999)
        for oid, n in ((TOTEM, 5), (FROG, 3), (SHARD, 2)):
            check(f'A: the old server sells {oid} x{n}', not wire_error(a.buy(oid, n)))
        spent = 5 * price(TOTEM) + 3 * price(FROG) + 2 * price(SHARD)
        balance_a = old.db.one('SELECT achieve_point FROM user_info WHERE id = ?', a.uid)[0]
        check(f'A: the balance paid {spent}', balance_a == 500000 - spent, balance_a)

    # ---- B: this server on the same save, before and after Monday 08:00 -----
    with Server(args, save, args.new_bin, at(monday, '07:57:00'), 'new') as new:
        a, b = Shopper(new, a.player), Shopper(new, b.player)
        check('B: seed = balance + shop spending (500,000)', a.merit() == (500000 - spent, 500000), a.merit())
        check('B: seed at the cap = 999,999', b.merit() == (999999, 999999), b.merit())
        check('B: old shop rows carry period 0', a.row(TOTEM) == (5, 0) and a.row(FROG) == (3, 0),
              (a.row(TOTEM), a.row(FROG)))

        # The Totem bought before the restock rule is spent: 99 more this week.
        countdown('B before 08:00', a, TOTEM, 0)
        countdown('B before 08:00', a, SHARD, 0)
        trades, _ = a.info()
        check('B: Burst Frog keeps its lifetime count and no countdown (-1)', trades.get(FROG) == (3, -1),
              trades.get(FROG))
        weekly = sum(1 for count, state in trades.values() if state != -1)
        check('B: 56 offers count down', weekly == 56, weekly)

        bought('B: Fire Totem x90', a.buy(TOTEM, 90), TOTEM, 90)
        bought('B: Fire Totem x9 (99)', a.buy(TOTEM, 9), TOTEM, 99)
        check('B: the row is in this week', a.row(TOTEM) == (99, week_at(a.now())), (a.row(TOTEM), week_at(a.now())))
        refused('B: Fire Totem #100', a, TOTEM, 1)
        bought('B: Burst Frog x7 (10)', a.buy(FROG, 7), FROG, 10)
        refused('B: Burst Frog #11', a, FROG, 1)
        check('B: spending leaves the lifetime total alone', a.merit()[1] == 500000, a.merit())

        # Earning past the cap: Deliver Zel at a full balance.
        b.player.set(zel=900000000)
        reply = b.call(DELIVER, {'1Q2pGAXE': [{'KT71m8Ae': '3', '3rhygS9K': '4', 'M7SXoc31': ''}],
                                 'dE79UwNE': [{'Rs7bCE3t': '100000000', 'idfCDG70': '0',
                                               'edy7fq3L': '', 'n6E8iMf3': ''}]})
        paid = b.server.db.one('SELECT COALESCE(SUM(points), 0) FROM user_achievement_deliver WHERE user_id = ?',
                               b.uid)[0]
        check('B: Deliver Zel at 999,999 pays points', not wire_error(reply) and paid > 0, (wire_error(reply), paid))
        check('B: the balance stays at 999,999; the total passes it',
              b.merit() == (999999, 999999 + paid), (b.merit(), paid))
        _, subjects = b.info()
        row = subjects.get(MILLION) or {}
        check(f'B: "1,000,000 Merit Points" reads the total ({999999 + paid})',
              int(row.get('pG2n1A28', -1)) >= min(1000000, 999999 + paid), row)

        check('B: the checks above ran before 08:00', a.now() < restock, stamp(a.now()))
        a.wait_until(restock + 2)
        countdown('B after 08:00', a, TOTEM, 0)
        bought('B: Fire Totem sells again after Monday 08:00', a.buy(TOTEM, 1), TOTEM, 1)
        check('B: the row moved to the new week', a.row(TOTEM) == (1, week_at(restock)), a.row(TOTEM))
        refused('B: Fire Totem x99 on top of 1 (100)', a, TOTEM, 99)
        bought('B: Fire Totem x98 (99)', a.buy(TOTEM, 98), TOTEM, 99)
        refused('B: Burst Frog still sold out (lifetime)', a, FROG, 1)

    # ---- C: the next game day is not a new week ----------------------------
    tuesday = at(monday + 1, '08:00:00')
    with Server(args, save, args.new_bin, tuesday - 50, 'tue') as tue:
        a = Shopper(tue, a.player)
        a.wait_until(tuesday + 2)
        countdown('C after Tuesday 08:00', a, TOTEM, 99)
        refused('C: Fire Totem stays sold out until Monday', a, TOTEM, 1)

    failed = [name for name, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} passed')
    if not failed:
        for volume in args.volumes:
            subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
