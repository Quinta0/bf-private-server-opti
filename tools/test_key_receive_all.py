"""Wire test for "Receive All" dungeon keys (server branch fix/key-receive-all,
ticket #33): DungeonKeyReceipt with key id 0 and receipt type 2 must claim
every key that is claimable today.

Against a running LOCAL server and its save database:
  1. GetDistributeDungeonKeyInfo lists the keys and which are claimable today.
  2. Receive All raises each claimable key's possession and clears its flag; keys
     that were not claimable stay as they were.
  3. A second Receive All changes nothing.
  4. A single-key Receive (type 1) still works the old way: nothing left to claim.

Reuses the fair-summon harness (tools/test_fair_summon.py): a throwaway guest
account, save edits made inside the server's container.

Usage: python3 -I tools/test_key_receive_all.py --src wt/dblock --url http://127.0.0.1:9979
Run it against a test server, never a save with real players.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

KEY_INFO = ('1mr9UsYz', 'r0ZA3pn5')
KEY_RECEIPT = ('WCJE0xe2', 'V4pfQo5C')
KEYS, KEY_ID, HELD, CLAIMABLE = 'eFU7Qtb0', '16KMNJLb', 'wgV86x1q', 'g85qMNxf'
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
    player = Player(args.url, db)
    player.set(tutorial_status=CHAPTER_DONE, tutorial_end_flag=1)

    def keys(reply: dict) -> dict:
        return {int(r[KEY_ID]): (int(r[HELD]), int(r[CLAIMABLE])) for r in reply.get(KEYS, [])}

    def receipt(kind: int, key_id: int) -> dict:
        reply = gme_call(args.url, *KEY_RECEIPT, {'IKqx1Cn9': player.login,
                                                  '9S5M2yI3': [{'i1WQkh4G': str(kind), '16KMNJLb': str(key_id)}]})
        if wire_error(reply) is not None:
            raise RuntimeError(f'DungeonKeyReceipt refused: {wire_error(reply)}')
        return reply

    before = keys(gme_call(args.url, *KEY_INFO, {'IKqx1Cn9': player.login}))
    claimable = {k for k, (_, flag) in before.items() if flag}
    check('1: the key list has keys, some claimable today', before and claimable, before)

    after = keys(receipt(2, 0))
    gained = {k for k in before if after.get(k, (0, 0))[0] > before[k][0]}
    check('2: Receive All raised every claimable key', gained == claimable, {'before': before, 'after': after})
    check('2: ...cleared their flags, and left the others alone',
          all(after[k][1] == 0 for k in claimable)
          and all(after[k] == before[k] for k in before if k not in claimable), after)
    stored = {int(k): int(n) for k, n in db.rows(
        'SELECT dungeon_key_id, possession FROM user_dungeon_keys WHERE user_id = ?', player.user_id)}
    check('2: ...and stored the new counts', all(stored.get(k) == after[k][0] for k in after), stored)

    again = keys(receipt(2, 0))
    check('3: a second Receive All changes nothing', again == after, again)
    single = keys(receipt(1, next(iter(claimable))))
    check('4: a single Receive afterwards has nothing left to claim', single == after, single)

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
