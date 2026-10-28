#!/usr/bin/env python3
"""Wire test: a Frontier Gate run pays 1 Merit Point per 100 points, at most 1,000 a run.

    .venv/bin/python -I <wt>/scripts/test_frontier_gate_merit_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-fgate PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_frontier_gate_merit_wire.py --src <wt> --url http://127.0.0.1:9983

Relentless Practice runs scored in the tens of millions and paid score / 100 uncapped, so 2-3 runs
filled the 999,999 balance (#balancing, 2026-10-22).  It plays two runs and retires each:

  1. a 50,000,000-point run pays exactly 1,000 (result screen and balance),
  2. a 25,000-point run still pays 250 (the rate below the cap is unchanged).

Uses the request shapes and --prepare of test_frontier_gate_run_wire.py.  Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_frontier_gate_run_wire as fg  # noqa: E402

GATE = 1


def merit_total(reply: dict) -> int | None:
    """The balance the End reply re-states (UserAchievementInfo idfCDG70, outside the result block)."""
    for key, value in reply.items():
        rows = value if isinstance(value, list) else [value]
        for row in rows:
            if key != 'mu0kXAlV' and isinstance(row, dict) and 'idfCDG70' in row:
                return int(row['idfCDG70'])
    return None


def run_and_retire(url: str, acc: dict, deck: list, units: list, score: int) -> tuple[int, int | None]:
    """One floor reporting `score`, then Retire.  Returns (merit on the result screen, balance after)."""
    run = fg.num(fg.start(url, acc, GATE, deck, units, fg.SUPPORT))
    fg.battle(url, acc, run, floors=1, score=score)
    end = fg.call(url, acc, fg.END, {'Mg8K8Y1a': [{'2FQdEbG3': run.get('2FQdEbG3', '0'), 'j3g5P4cq': '3'}]})
    result = (end.get('mu0kXAlV') or [{}])[0]
    return int(result.get('idfCDG70', -1)), merit_total(end)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        fg.prepare(args.src)
        return 0
    acc = json.loads((args.src / 'out/qa/fgate/account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    deck, units = fg.party(acc['user'])

    # A zero-point run pays nothing and re-states the starting balance.
    paid0, before = run_and_retire(url, acc, deck, units, score=0)
    fg.check(paid0 == 0 and before is not None, f'a 0-point run pays 0 (balance {before})')
    before = before or 0

    paid, after = run_and_retire(url, acc, deck, units, score=50_000_000)
    fg.check(paid == 1000, f'a 50,000,000-point run shows 1,000 Merit on the result ({paid})')
    fg.check(after == min(before + 1000, 999_999), f'and adds 1,000 to the balance ({before} -> {after})')

    paid, later = run_and_retire(url, acc, deck, units, score=25_000)
    fg.check(paid == 250, f'a 25,000-point run still pays 250 ({paid})')
    fg.check(later == min((after or 0) + 250, 999_999), f'and adds 250 ({after} -> {later})')

    print(f'{len(fg.failures)} failure(s)')
    return 1 if fg.failures else 0


if __name__ == '__main__':
    sys.exit(main())
