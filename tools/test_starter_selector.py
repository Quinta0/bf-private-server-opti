"""Wire test for the Summoner Rank 50 selector (server branch fix/starter-selector,
ticket #23): it must reach a player within a session, not only at the rare
UserInfo the client sends.

Against a running LOCAL server and its save database:
  1. Level 55, never paid: Initialize (every app start) adds one type-8005 selector
     present and sets starter_selector_granted.  A second Initialize adds nothing.
  2. Level 50, never paid: the 30-minute UpdateInfo pays it.
  3. Level 49: neither request pays anything.

Reuses the fair-summon harness (tools/test_fair_summon.py): throwaway guest accounts,
save edits made inside the server's container.

Usage: python3 -I tools/test_starter_selector.py --src wt/dblock --url http://127.0.0.1:9979
Run it against a test server, never a save with real players.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
UPDATE_INFO = ('RUV94Dqz', 'hy0P9xjsGJ6MAgb2')
SELECTOR_PRESENT = 8005
CHAPTER_DONE = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9979')
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--container-db', default='/src/deploy/gme.sqlite')
    args = ap.parse_args()
    failures = 0

    def check(label: str, ok: bool, detail='') -> None:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + str(detail) if detail != '' else ''}")

    db = Database(args.src / 'deploy' / 'gme.sqlite', container_on_port(args.url), args.container_db)

    def fresh(level: int) -> Player:
        player = Player(args.url, db)
        player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1, level=level, starter_selector_granted=0)
        return player

    def state(player: Player) -> tuple[int, int]:
        """(selector presents, starter_selector_granted)"""
        presents = db.one('SELECT COUNT(*) FROM user_presents WHERE user_id = ? AND present_type = ?',
                          player.user_id, SELECTOR_PRESENT)[0]
        flag = db.one('SELECT starter_selector_granted FROM user_info WHERE id = ?', player.user_id)[0]
        return presents, flag

    def call(player: Player, request) -> str | None:
        return wire_error(gme_call(args.url, *request, {'IKqx1Cn9': player.login}))

    p55 = fresh(55)
    err = call(p55, INITIALIZE)
    check('1: Initialize pays a level-55 player one selector', err is None and state(p55) == (1, 1),
          {'wire': err, 'presents, flag': state(p55)})
    call(p55, INITIALIZE)
    call(p55, UPDATE_INFO)
    check('1: ...and never a second one', state(p55) == (1, 1), state(p55))

    p50 = fresh(50)
    err = call(p50, UPDATE_INFO)
    check('2: the 30-minute UpdateInfo pays a level-50 player', err is None and state(p50) == (1, 1),
          {'wire': err, 'presents, flag': state(p50)})

    p49 = fresh(49)
    errs = (call(p49, INITIALIZE), call(p49, UPDATE_INFO))
    check('3: level 49 gets nothing from either request', errs == (None, None) and state(p49) == (0, 0),
          {'wire': errs, 'presents, flag': state(p49)})

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
