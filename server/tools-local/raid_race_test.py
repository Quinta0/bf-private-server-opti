"""Concurrent RaidQuickStart: every caller must get its own solo room.

Regression for the room-id race (separate last_insert_rowid() await let
another request's INSERT slip in, handing players strangers' rooms).
"""
import sys, argparse, threading
sys.path.insert(0, '/t')
import raid_entry_test as R

ap = argparse.ArgumentParser()
ap.add_argument('--url'); ap.add_argument('--n', type=int, default=12); ap.add_argument('--rounds', type=int, default=3)
a = ap.parse_args()
players = [R.Player(a.url, f'Race{i}') for i in range(a.n)]
for p in players:
    p.call(R.RAID_USER_INFO); p.call(R.RAID_AGREEMENT)
bad = 0; total = 0
for rnd in range(a.rounds):
    results = {}
    barrier = threading.Barrier(len(players))
    def go(p):
        barrier.wait()
        results[p.user_id] = p.call(R.RAID_QUICK_START, **{'8iBuZa3n': [{'j28VNcUW': '2000'}]})
    ts = [threading.Thread(target=go, args=(p,)) for p in players]
    [t.start() for t in ts]; [t.join() for t in ts]
    for uid, r in results.items():
        total += 1
        rooms = r.get('8iBuZa3n', []); mem = r.get('uwH62EZy', [])
        master = rooms[0].get('8ZK3w0Np') if rooms else None
        mids = [m.get('h7eY3sAK') for m in mem]
        login = R.user_info(r).get('8VYd6xSX')
        room = rooms[0].get('8VYd6xSX') if rooms else None
        if master != uid or mids != [uid] or login != room:
            bad += 1
            print(f'ROUND {rnd} user {uid}: room={room} master={master} members={mids} login_room={login}')
print(f'{bad}/{total} QuickStart replies named a room the caller does not own')
sys.exit(1 if bad else 0)
