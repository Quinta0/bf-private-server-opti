"""Wire test: weekly summon banner rotation (gme/common/SummonRotation.hpp).

The rail is a fixed core plus featured slots whose gates change every game
week, Monday 08:00 UTC, as a pure function of the week number over the pool in
deploy/archive/summon_rotation.json.  A pool gate is open only in its featured
weeks (plus a short grace after the turn for a rail loaded before it).

The test starts the server itself, in Docker, with a libfaketime clock, on one
scratch save in a named Docker volume:

  data. The pool's calendar, mirrored here: three gates a week, no repeats
        until the pool has cycled, consecutive weeks disjoint.
  A. Monday 07:56 UTC: GachaList and UserInfo both send core + week W in the
     slot tiles, right after the Rare tile; captions say until when; a week-W
     gate pulls, a gate featured in neither W nor W-1 is refused as rotated
     out; the JSON for the Discord bot names W and W+1.  Then across 08:00:
     both feeds send W+1, a W+1 gate pulls, a W-only gate still pulls inside
     the grace.
  B. Tuesday 07:58, restarted: the grace is over, so the W-only gate is
     refused; across Tuesday 08:00 (a new game day, not a new week) the rail
     stays W+1.

Usage (from the worktree root; get libfaketime as in test_daily_rollover_wire.py):
  python3 -I scripts/test_summon_rotation_wire.py \\
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
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_rollover_wire import (LIB, UPDATE_INFO, Server, at, check, fresh_save,  # noqa: E402
                                      results, stamp)
from test_fair_summon import Player, fetch, gme_call, wire_error  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WEEK = 7 * 86400
WEEK_START = 4 * 86400 + 8 * 3600   # Monday 1970-01-05 08:00 UTC
TOKEN = '{featured_until}'
CAPTION = '3sdHQb69'
ROTATED_OUT = 'rotated out'

ROTATION = json.loads((ROOT / 'deploy/archive/summon_rotation.json').read_text(encoding='utf-8'))
ARCHIVE = {g['id']: g for g in json.loads((ROOT / 'deploy/archive/gacha.json').read_text(encoding='utf-8'))}
STATIC = json.loads((ROOT / 'deploy/mst/gacha_category_mst.json').read_text(encoding='utf-8'))['IBs49NiH']
# The server drops pool gates the archive cannot sell; so does the mirror.
POOL = [p['gate'] for p in ROTATION['pool'] if p['gate'] in ARCHIVE]
SLOTS = ROTATION['slot_tiles']
GRACE = ROTATION['grace_seconds']
FIRST_WEEK = None   # set in main


def week_at(t: int) -> int:
    return (t - WEEK_START) // WEEK


def week_start(week: int) -> int:
    return WEEK_START + week * WEEK


def featured(week: int) -> list[int]:
    k = (week - FIRST_WEEK) * len(SLOTS)
    return [POOL[(k + i) % len(POOL)] for i in range(len(SLOTS))]


def core_tiles() -> dict[int, str]:
    """Static tile id -> gate list with pool gates stripped; the Rare tile (3)
    is hidden once the tutorial is over, and empty tiles are dropped."""
    out = {}
    for row in STATIC:
        gates = [g for g in row['3rCmq58M'].split(',') if int(g) not in POOL]
        if gates and int(row['vx9uyQVQ']) != 3:
            out[int(row['vx9uyQVQ'])] = ','.join(gates)
    return out


def until_text(week: int) -> str:
    t = dt.datetime.fromtimestamp(week_start(week + 1), dt.timezone.utc)
    return f"Featured until {t.strftime('%a')} {t.day} {t.strftime('%b')}, 08:00 UTC"


class Summoner:
    def __init__(self, server: Server, player: Player | None = None):
        self.server = server
        self.player = player or Player(server.base, server.db)
        self.player.db, self.player.base = server.db, server.base

    def now(self) -> int:
        reply = gme_call(self.server.base, *UPDATE_INFO, {'IKqx1Cn9': self.player.login})
        return int(reply['AhSmZF07']['QxWZoA04'])

    def wait_until(self, t: int) -> None:
        deadline = time.monotonic() + max(0, t - self.now()) + 60
        while self.now() < t:
            if time.monotonic() > deadline:
                raise RuntimeError(f'the server clock never reached {stamp(t)}')
            time.sleep(1)

    def gems(self) -> int:
        return int(self.server.db.one('SELECT gems FROM user_info WHERE id = ?', self.player.user_id)[0])


def rail(label: str, s: Summoner, week: int) -> None:
    """Both feeds send core + this week's gates in the slot tiles."""
    want = featured(week)
    for feed, reply in (('GachaList', s.player.gacha_list()), ('UserInfo', s.player.user_info())):
        tiles = [r for r in reply.get('IBs49NiH') or [] if int(r['vx9uyQVQ']) < 100]
        by_id = {int(r['vx9uyQVQ']): r for r in tiles}
        slots = [by_id.get(t, {}).get('3rCmq58M') for t in SLOTS]
        check(f'{label} {feed}: slots {SLOTS} carry week {week} = {want}',
              slots == [str(g) for g in want], slots)
        core = {i: r['3rCmq58M'] for i, r in by_id.items() if i not in SLOTS}
        check(f'{label} {feed}: the core is the static rail without pool gates', core == core_tiles(),
              (core, core_tiles()))
        orders = [int(r['2r4EoNt4']) for r in tiles]
        check(f'{label} {feed}: no two tiles share a display order', len(set(orders)) == len(orders), orders)
        ranked = [int(r['vx9uyQVQ']) for r in sorted(tiles, key=lambda r: int(r['2r4EoNt4']))]
        check(f'{label} {feed}: the slots follow All Star (1) and Honor (2)', ranked[:2 + len(SLOTS)] == [1, 2] + SLOTS,
              ranked)
        everywhere = {int(g) for r in reply.get('IBs49NiH') or [] for g in r['3rCmq58M'].split(',')}
        check(f'{label} {feed}: no off-week pool gate on any tile',
              everywhere & set(POOL) == set(want), sorted(everywhere & set(POOL)))
        captions = {int(g['7Ffmi96v']): g.get(CAPTION, '') for g in reply.get('1IR86sAv') or []}
        check(f'{label} {feed}: no caption carries the raw token',
              not any(TOKEN in c for c in captions.values()))
        tokened = [g for g in want if TOKEN in (ARCHIVE[g].get('caption_msg') or '')]
        check(f'{label} {feed}: featured captions say "{until_text(week)}" ({len(tokened)} with the token)',
              all(until_text(week) in captions.get(g, '') for g in tokened),
              {g: captions.get(g, '')[:120] for g in tokened})


def pulls(label: str, s: Summoner, gate: int) -> None:
    before = s.gems()
    pull = s.player.pull(gate)
    check(f'{label}: gate {gate} pulls', not wire_error(pull.reply) and s.gems() < before,
          (wire_error(pull.reply), before, s.gems()))


def rotated_out(label: str, s: Summoner, gate: int) -> None:
    before = s.gems()
    pull = s.player.pull(gate)
    error = wire_error(pull.reply) or ''
    check(f'{label}: gate {gate} refused as rotated out, nothing spent',
          ROTATED_OUT in error and s.gems() == before, (error, before, s.gems()))


def bot_json(label: str, s: Summoner, week: int) -> None:
    status, body = fetch(f'{s.server.base}/bf/gme/summonRotation.php')
    data = json.loads(body) if status == 200 else {}
    ok = (data.get('week') == week and data.get('starts') == week_start(week)
          and data.get('ends') == week_start(week + 1)
          and [f['gate'] for f in data.get('featured', [])] == featured(week)
          and [f['gate'] for f in data.get('next', [])] == featured(week + 1)
          and all(f.get('title') for f in data.get('featured', [])))
    check(f'{label}: summonRotation.php names week {week} and the next', ok, (status, body[:300]))


def main() -> int:
    global FIRST_WEEK
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bin', type=Path, default=ROOT / 'out/build-linux-arm64/standalone_frontend/gimuserverw')
    ap.add_argument('--faketime', type=Path, required=True, help=f'folder holding {LIB}')
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive/assets')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--boot-timeout', type=int, default=240)
    ap.add_argument('--monday', default='2026-11-02')
    args = ap.parse_args()
    args.root, args.volumes, args.workdir = ROOT, [], ROOT / 'out' / 'summon-rotation-test'
    args.bin, args.faketime = args.bin.resolve(), args.faketime.resolve()
    if not args.port:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    first = dt.datetime.fromisoformat(ROTATION['first_week']).replace(tzinfo=dt.timezone.utc)
    FIRST_WEEK = week_at(int(first.timestamp()) + 8 * 3600)
    monday = int(dt.datetime.fromisoformat(args.monday).replace(tzinfo=dt.timezone.utc).timestamp()) // 86400
    turn = at(monday, '08:00:00')
    w = week_at(turn) - 1

    # ---- the calendar --------------------------------------------------------
    check(f'data: {args.monday} 08:00 UTC starts a week', week_at(turn) == w + 1 and week_at(turn - 1) == w)
    check(f'data: first_week {ROTATION["first_week"]} is a Monday', first.weekday() == 0)
    check(f'data: {len(POOL)} pool gates for {len(SLOTS)} slots', len(POOL) >= 2 * len(SLOTS), POOL)
    cycle = [g for k in range(len(POOL)) for g in featured(FIRST_WEEK + k)]
    check('data: every gate once per cycle, nothing twice in a row',
          all(cycle.count(g) == len(SLOTS) for g in POOL)
          and all(not set(featured(x)) & set(featured(x + 1)) for x in range(w - 20, w + 20)))
    stale = next(g for g in POOL if g not in featured(w) and g not in featured(w - 1) and g not in featured(w + 1))
    w_only = next(g for g in featured(w) if g not in featured(w + 1))
    print(f'  week {w}: {featured(w)}  week {w + 1}: {featured(w + 1)}  never-open gate {stale}', flush=True)

    save = fresh_save(args, 'rotation')

    # ---- A: Monday, across 08:00 ---------------------------------------------
    with Server(args, save, args.bin, at(monday, '07:56:00'), 'rot') as server:
        s = Summoner(server)
        s.player.set(tutorial_status=12, tutorial_end_flag=1, gems=1000)
        rail('A before 08:00', s, w)
        pulls('A before 08:00', s, featured(w)[0])
        rotated_out('A before 08:00', s, stale)
        bot_json('A before 08:00', s, w)
        check('A: the checks above ran before 08:00', s.now() < turn, stamp(s.now()))

        s.wait_until(turn + 2)
        rail('A after 08:00', s, w + 1)
        pulls('A after 08:00', s, featured(w + 1)[0])
        pulls(f'A after 08:00, inside the {GRACE} s grace', s, w_only)
        bot_json('A after 08:00', s, w + 1)

    # ---- B: the next game day is not a new week ------------------------------
    tuesday = at(monday + 1, '08:00:00')
    with Server(args, save, args.bin, tuesday - 120, 'tue') as server:
        s = Summoner(server, s.player)
        rotated_out('B after the grace', s, w_only)
        s.wait_until(tuesday + 2)
        rail('B after Tuesday 08:00', s, w + 1)

    failed = [name for name, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} passed')
    if not failed:
        for volume in args.volumes:
            subprocess.run(['docker', 'volume', 'rm', '-f', volume], capture_output=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
