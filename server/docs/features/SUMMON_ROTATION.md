# Summon banner rotation

The summon rail is a fixed core of banners that are always open, plus three
**featured** slots whose banners change every week, Monday 08:00 UTC (the
Merit shop's week), the same for every player.

- Core: everything on `deploy/mst/gacha_category_mst.json` that is not in the
  pool (All Star 11960, Honor, Rare, Veteran, the Divine summons, Omni, Dawn).
- Pool and calendar: `deploy/archive/summon_rotation.json`. Each week takes
  the next three gates of `pool`, wrapping; the week starting `first_week`
  shows the first three. **The order is the calendar**: append new gates at the
  end, or every future week moves.
- Code: `gimuserver/gme/common/SummonRotation.{hpp,cpp}`, week math in
  `GameWeek.hpp`. No stored state: the week's gates are a pure function of the
  week number, like the 6-star selector.

## How it reaches the client

GachaList (sent on every visit to the rail) and UserInfo (every login) build
the rail per request: `applyWeeklyRail` strips pool gates from the static tiles
(so the collab, Halloween and new-banner tiles stay in the MST file for the
generators and `gate_info.py --check`, but never ship) and inserts one tile per
slot (ids 21-23, fixed for good) right after the Rare tile. Display orders are
renumbered 1..n so no two tiles tie.

Every tile keeps start 0 / end 2147483647. The client does filter tiles by
date, but on the phone's clock (`getGachaCategoryList` @0x1ea6978,
`time(NULL)`), and maps taps by index into the re-filtered list
(`touchEnded` @0x1ea70dc), so the server swaps the gates instead.

Rail tiles cannot show text. The end date is in the gate's caption: pool gates
carry `{featured_until}` under the title (`scripts/gate_info.py`), which the
server fills with "Featured until Mon 27 Oct, 08:00 UTC".

## Pulls

`isOnSummonRail` asks the rotation for pool gates: open in their featured week,
and last week's for `grace_seconds` (15 min) after the turn, for a player who sat
on the gate pager across 08:00. Anything else is refused with "This banner has
rotated out" and the player is sent Home (refuseToHome); the next visit to the
rail shows the new week.

## Discord

`GET /bf/gme/summonRotation.php` returns this week and the next as JSON. The bot
(`discord-bot/src/rotation.js`) polls it every 5 minutes and posts each new
week once in #banner-rotation (`ROTATION_CHANNEL`, layout key `banner_rotation`), a read-only channel under NEWS.

## Test

`scripts/test_summon_rotation_wire.py` runs the server under libfaketime from a
Monday 07:56 across 08:00 and a Tuesday 08:00: both feeds, the captions, pulls
in and out of the week, the grace, and the JSON.
