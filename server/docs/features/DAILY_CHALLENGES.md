# Daily Challenges (ticket #18)

Summoner's Hall → Exchange → Merit Shop → Back → **Missions → Challenges** was empty.
It now shows the day's challenges, counts real battles for the ones a player
started, pays their Merit Points once, and turns over every day.

## What players get

The original game's weekly schedule, unchanged (it ran from 2018-12 to 2022-01):

| Day (resets 08:00 UTC = 17:00 JST) | Challenges | Merit Points |
|---|---|---|
| Mon | 5 Arena battles, 20 status ailments, 5 Earth enemies in Arena, 30 Earth enemies, 10 Arena BB kills | 400 each |
| Tue | 5 Arena battles, 5 Fire enemies in Arena, 30 Fire enemies, 50 criticals, 2 Raid clears | 400 each |
| Wed | 5 quest clears, 1000 BC in one quest, 5 Arena wins, 5 Thunder enemies in Arena, 30 Thunder enemies | 400 each |
| Thu | 5 quest clears, 10 Arena BB kills, 5 Water enemies in Arena, 30 Water enemies, 100 sparks in one quest | 400 each |
| Fri | 20 Arena overkills, 20 multi-kills, 200 overkills, 200,000 damage to stronger elements, 50 criticals | 400 each |
| Sat + Sun (one 48 h window) | ten "(Event)" challenges | 1,000 to 4,000 each |

That is 2,000 Merit Points a weekday and 20,000 a weekend if everything is done.

- A challenge counts only after **Start** (the list's "Accept All", or Start on its
  page), at most **5 held at once**. Claiming one frees its slot, so all ten weekend
  challenges can be done. Give Up returns it to Start and drops its progress.
- A challenge finished but not claimed before the reset **stays claimable**.
  One started and not finished is shown once as "expired" after the reset.

## How it works (server)

- `gme/common/DailyChallenges.{hpp,cpp}`: schedule, counting, list rows, accept, claim.
- **The ids are the original ones** (89008881..89008915, the week that ended
  2021-06-08..14). The client only draws ids that are in its own downloaded
  F_ACHIEVEMENT_SUBJECT_MST v503; the H9ATfJ38 table the server sends is written
  to a file the client never reads back. So names, targets and points on screen
  are the client's own. `scripts/gen_achievement_mst.py` keeps those 35 rows in
  `deploy/mst/achievement_subject_mst.json` so the server knows them too.
- **The condition is the row's `disp_order`** (8500 quest clears, 6606 enemies by
  element, ...), not its id or `cond_type`.
- **Progress comes from the battles**, counted only for started challenges in the
  open window: MissionEnd's battle log `rXvA1E5y` (won or lost; only a clear is a
  "quest cleared"), ArenaBattleEnd `yTEvm85f`, and a won RaidMissionEnd. Key
  meanings were traced in the client: `Fw2dr06x` enemies by element, `EcDR3f6M`
  overkills, `xFH5i4o8` multi-kills, `J3EBd90b` ailments, `2z8kTtBy` criticals,
  `32edsbP0` damage to a stronger element (the challenge's own example: an Earth
  unit hitting a Fire unit), `U8uZLA34` sparks, `hoG2ieT5` BC; Arena `XRoD9Hg7`
  BB kills, `Wnc6A31E` overkills, `s92RgSwp` flawless, `sQ2aMk8B` downed enemy units.
- Table `user_daily_challenges` (migration `18102026_CreateUserDailyChallenges`),
  one row per started challenge and window; a player-owned table (an account
  reset or delete must remove its rows).
- The Challenges badge (`2:0:N` in `h23iRjGN.dUujjBBK`) counts finished,
  unclaimed challenges. BadgeInfo, RewardReceive and Deliver send it, and so do
  the battle ends that count challenges (MissionEnd, ArenaBattleEnd,
  RaidMissionEnd), so the red count on Missions > Challenges lights as soon as a
  battle finishes one. The client only asks for BadgeInfo at login and on
  Home/Randall town entry, but it applies `h23iRjGN` from any reply
  (GameResponseParser hands every body key to the global getResponseObject).
  The Missions screen (RandallAchievementTopScene, rebuilt on every entry) reads
  the live AchievementBadgeInfo singleton. Each send is the complete set,
  because parseBadgeData replaces it. The copies of the Arena and Raid replies
  kept for resends leave the badge out. A resend gets the badge computed at that
  moment instead (`gme::withCurrentBadge`), so an old count can't re-light a
  challenge that was claimed since.

## Tests

- `python3 -I scripts/test_daily_challenges_wire.py --url http://127.0.0.1:<port>`
  (36 checks; needs the main repo's `tools/test_fair_summon.py`). It follows
  whatever window the server is in.
- `scripts/test_challenge_badge_wire.py` (23 checks, same harness; `--seed-raid` runs the
  weekday path): each battle end carries the complete badge, which equals BadgeInfo.
  Finishing a quest, an Arena and a raid challenge reads 2:0:1, 2:0:2, 2:0:3.
  Resends and retries carry the current badge, but the stored copies hold none.
  After Receive All, a replayed MissionEnd, a resent ArenaBattleEnd and a retried
  RaidMissionEnd all read dark.
- Emulator, 2026-10-18 (a temporary clone of BF_Tall_Test, FrTestA): Missions showed
  no count on Challenges. A Vortex quest (Royal Court Lv.1) then finished two
  started challenges. Back through EXCHANGE > Merit Shop > Back, Challenges showed
  a red 2. The server's request log has no BadgeInfo between that MissionEnd and
  the screen. Only the MissionEnd reply carried `h23iRjGN` (`...,2:0:2,...`).
- Emulator, 2026-10-18 (weekend window): the list showed the ten (Event)
  challenges with "Remaining: 21hr"; Accept All started 5; a real Vortex quest
  moved "300 Sparks" and "300 Enemies"; the finished one was claimed once for
  2,000 Merit Points, and a second Receive All found nothing.

## Not done

- Lifetime achievements finished outside a battle (summons, town upgrades,
  friends) still wait for the next BadgeInfo. The Merit Shop and the Challenges
  list both send GetAchievementInfo (client 0x1A65E70 / 0x1A58AF4). Putting the
  badge in that reply too would refresh every category whenever the player opens
  the Merit hall.
- The monthly "Frontier Rift" challenge (disp_order 8900) and the SP challenges
  (category 5) are not offered.
- Daily Tasks (Brave Points) still roll over at 00:00 UTC while the client's own
  countdown runs to 08:00 UTC.
