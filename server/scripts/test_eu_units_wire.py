"""Wire test: Trinity Goddess Htilit (scripts/gen_eu_units.py) on a running server.

  A. Initialize carries her extra skill, and announces the client tables of
     scripts/build_eu_tables.py at their versions (the balance root must hold them).
  B. The Realm of the Gods Bazaar lists her at 2000 tokens with a stock of 1;
     buying her spends 2000 and puts her in the unit box with her own stats
     and skills; a second purchase is refused.
  C. The server hands out her art and animation files (findContentByName), as
     the CDN does on prod.

B needs Realm of the Gods featured: run the server with a test copy of
deploy/archive/event_rotation.json whose first_week is the current week
(only the copy is changed). The prod image has no python, so SQL goes
through a sidecar on the save's volume:

  python3 -I scripts/test_eu_units_wire.py --url http://127.0.0.1:PORT \\
      --container SIDECAR --container-db /data/gme.sqlite --art DIR
Never point it at a save with real players.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_daily_rollover_wire import check, results  # noqa: E402
from test_event_rotation_wire import EXCHANGE_BUY, EXCHANGE_INFO, OFFER_ID, OFFERS, REMAINING, TOKEN_ID, TOKENS  # noqa: E402
from test_fair_summon import INITIALIZE, Database, Player, fetch, gme_call, wire_error  # noqa: E402
from build_eu_tables import TABLES  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
UNIT, TOKEN, OFFER, PRICE = 750257, 48, '48-6-750257', 2000
ARCHIVE = next(r for r in json.loads((ROOT / 'deploy/archive/unit.json').read_text(encoding='utf-8'))
               if r['id'] == UNIT)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', required=True)
    ap.add_argument('--container', required=True, help='sidecar with python3 and the save mounted')
    ap.add_argument('--container-db', default='/data/gme.sqlite')
    ap.add_argument('--art', type=Path, required=True, help='folder holding content/unit/...')
    args = ap.parse_args()
    db = Database(Path('/nonexistent'), args.container, args.container_db)
    player = Player(args.url, db)
    call = lambda api, body: gme_call(args.url, *api, dict(body, IKqx1Cn9=player.login))  # noqa: E731

    # A
    init = gme_call(args.url, *INITIALIZE, {'IKqx1Cn9': player.login})
    extra = [r for rows in init.values() if isinstance(rows, list) for r in rows
             if isinstance(r, dict) and r.get('cP83zNsv') == str(UNIT)]
    check('A: Initialize carries her extra skill', len(extra) == 1 and extra[0]['hjAy9St3'] == '46,1@55,1@66,1',
          extra)
    announced = {r.get('moWQ30GH'): r.get('d2RFtP8T') for rows in init.values() if isinstance(rows, list)
                 for r in rows if isinstance(r, dict) and 'd2RFtP8T' in r}
    want = {t: str(spec[-1]) for t, spec in TABLES.items()}
    check('A: Initialize announces every EU table version', all(announced.get(t) == v for t, v in want.items()),
          {t: announced.get(t) for t in want})

    # B
    db.rows('INSERT INTO user_event_tokens (user_id, token_id, count) VALUES (?, ?, ?)'
            ' ON CONFLICT(user_id, token_id) DO UPDATE SET count = excluded.count', player.user_id, str(TOKEN), 2500)
    info = call(EXCHANGE_INFO, {TOKENS: [{TOKEN_ID: str(TOKEN)}]})
    offers = {r[OFFER_ID]: r for r in info.get(OFFERS) or []}
    check('B: the Realm of the Gods Bazaar lists her with a stock of 1',
          OFFER in offers and int(offers[OFFER][REMAINING]) == 1, wire_error(info) or sorted(offers)[:5])
    buy = lambda: call(EXCHANGE_BUY, {OFFERS: [{OFFER_ID: OFFER, 'H6k1LIxC': '1', TOKEN_ID: str(TOKEN)}]})  # noqa: E731
    first = buy()
    left = db.one('SELECT count FROM user_event_tokens WHERE user_id = ? AND token_id = ?', player.user_id, str(TOKEN))
    check('B: buying her spends 2000 tokens', wire_error(first) is None and left and int(left[0]) == 2500 - PRICE,
          (wire_error(first), left))
    row = db.one('SELECT base_hp, base_atk, base_def, base_rec, bb_id, sbb_id FROM user_units'
                 ' WHERE user_id = ? AND unit_id = ?', player.user_id, UNIT)
    s = ARCHIVE['stats'][0]
    check('B: she is in the unit box with her stats and skills',
          row is not None and tuple(row) == (s['hp'], s['atk'], s['def'], s['rec'], '750257', '1750257'), row)
    second = buy()
    count = db.one('SELECT COUNT(*) FROM user_units WHERE user_id = ? AND unit_id = ?', player.user_id, UNIT)[0]
    check('B: a second purchase is refused', wire_error(second) is not None and count == 1,
          (wire_error(second), count))

    # C
    files = sorted(p for p in (args.art / 'content').rglob('*') if p.is_file())
    served = []
    for p in files:
        status, body = fetch(f'{args.url}/{p.relative_to(args.art).as_posix()}')
        served.append(status == 200 and hashlib.sha256(body).digest() == hashlib.sha256(p.read_bytes()).digest())
    check(f'C: all {len(files)} art and animation files are served byte for byte', len(files) == 8 and all(served),
          served)

    failed = [n for n, ok in results if not ok]
    print(f'{len(results) - len(failed)}/{len(results)} passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
