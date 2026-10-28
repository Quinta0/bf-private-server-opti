"""Wire test for free energy refills (server branch feat/free-energy).

Checks, against a running LOCAL server and its save database:
  1. Initialize announces the refill price (DefineMst C8KkHGa7) as 0.
  2. ShopUse type 1 and type 10 below max fill energy to the level's max,
     clear the refill timer and charge nothing, with 0 gems and with gems.
  3. Over max and exactly at max are still refused (nothing changes).

Reuses the fair-summon harness (tools/test_fair_summon.py): a throwaway guest
account, save edits made inside the server's container.

Usage: python3 -I tools/test_free_energy.py --src wt/freeenergy [--url http://127.0.0.1:9977]
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fair_summon import Database, Player, container_on_port, gme_call, wire_error  # noqa: E402

INITIALIZE = ('MfZyu1q9', 'EmcshnQoDr20TZz1')
SHOP_USE = ('xe8tiSf4', 'qthMXTQSkz3KfH9R')
CHAPTER_DONE = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9977')
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

    def state():
        level, energy, full_ts, gems = db.one(
            'SELECT level, energy, energy_full_ts, gems FROM user_info WHERE id = ?', player.user_id)
        return {'level': level, 'energy': energy, 'energy_full_ts': full_ts, 'gems': gems}

    def shop_use(use_type: int, cost: int) -> dict:
        return gme_call(args.url, *SHOP_USE, {'IKqx1Cn9': player.login, '32ibWjFG': [
            {'60IsqxDt': str(use_type), '03UGMHxF': str(cost), '5gXxT7LZ': '0'}]})

    init = gme_call(args.url, *INITIALIZE, {'IKqx1Cn9': player.login})
    defines = init.get('VkoZ5t3K') or [{}]
    price = (defines[0] if isinstance(defines, list) else defines).get('C8KkHGa7')
    check('1: Initialize announces the energy refill price as 0', str(price) == '0', price)

    # The level's max energy is whatever the first refill fills to.
    player.set(energy=0, energy_full_ts=int(time.time()) + 3600, gems=0)
    reply = shop_use(1, 0)
    after = state()
    max_energy = after['energy']
    check('2: type 1 with 0 gems fills energy and charges nothing',
          wire_error(reply) is None and max_energy > 0 and after['gems'] == 0 and after['energy_full_ts'] == 0,
          {**after, 'wire': wire_error(reply)})

    player.set(energy=max(0, max_energy - 5), energy_full_ts=int(time.time()) + 3600, gems=25)
    reply = shop_use(10, 0)
    after = state()
    check('2: type 10 (repeat quest prompt) with gems fills energy and keeps every gem',
          wire_error(reply) is None and after['energy'] == max_energy and after['gems'] == 25
          and after['energy_full_ts'] == 0, after)

    player.set(energy=max(0, max_energy - 5), energy_full_ts=int(time.time()) + 3600, gems=7)
    reply = shop_use(1, 1)  # an old client still claiming 1 gem pays nothing
    after = state()
    check('2: a client claiming the old price of 1 gem is charged 0',
          wire_error(reply) is None and after['energy'] == max_energy and after['gems'] == 7, after)

    player.set(energy=max_energy + 8, energy_full_ts=0, gems=3)
    shop_use(1, 0)
    after = state()
    check('3: over max is refused, nothing changes', after['energy'] == max_energy + 8 and after['gems'] == 3, after)
    player.set(energy=max_energy, energy_full_ts=0, gems=3)
    shop_use(1, 0)
    after = state()
    check('3: exactly at max is refused, nothing changes', after['energy'] == max_energy and after['gems'] == 3, after)

    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
