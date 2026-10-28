"""A scripted second player for on-device friend checks.

Creates a fresh account (or reuses one by device id) and acts on the device
player's account:
  --apply USER_ID    send that user a friend request
  --agree USER_ID    accept that user's request
Prints the peer's user id and public friend ID so the device can search it.

Usage: python -I device_peer.py --url http://host.docker.internal:9961 --device-id peer1 --apply <uid>
"""
import argparse
import sys

sys.path.insert(0, '/t')
sys.path.insert(0, '/tools')
from test_friends import AGREE, APPLY, GET_AGREE, Player, rows  # noqa: E402
import test_friends  # noqa: E402
from test_accounts import guest_login  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:9961')
    ap.add_argument('--name', default='PeerOne')
    ap.add_argument('--device-id', default='peer-device-0001')
    ap.add_argument('--apply')
    ap.add_argument('--agree')
    args = ap.parse_args()

    # A fixed device id gives the same account back on every run.
    test_friends.guest_login = lambda base, _ignored: guest_login(base, args.device_id)
    peer = Player(args.url, args.name)
    print('peer user_id', peer.user_id, 'friend_id', peer.friend_id)

    class Other:
        user_id = args.apply or args.agree
    if args.apply:
        r = peer.target(APPLY, Other)
        print('apply ->', [(x.get('B5JQyV8j'), x.get('96Nxs2WQ')) for x in rows(r)][-5:])
    if args.agree:
        r = peer.target(AGREE, Other)
        print('agree ->', [(x.get('B5JQyV8j'), x.get('96Nxs2WQ')) for x in rows(r)][-5:])
    r = peer.call(GET_AGREE)
    print('peer list:', [(x.get('B5JQyV8j'), x.get('96Nxs2WQ')) for x in rows(r)])
    return 0


if __name__ == '__main__':
    sys.exit(main())
