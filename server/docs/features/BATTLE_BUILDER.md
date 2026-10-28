# Hand-built battles (overlay files)

An operator can replace the waves of a mission with a hand-authored file: one file per
mission under `balance_root`, and the
game server serves it within about 2 seconds, without a rebuild or a restart. A
file that would break MissionStart or MissionEnd is refused on its own; every
other mission is untouched.

## Layout

```
<balance_root>/server/missions/<mission id>.json
```

- **One `MissionRecord` object per file**, exactly the shape of one element of
  `deploy/archive/mission.json` (`gimuserver/archive/archive.hpp`, generated from
  `packet-generator/assets/archive/mission.kdl`). No other keys, and no key left out:
  optional ones (`visuals`, `skills`, `ailment_resists`, `act_rate`,
  `authored_capture`, `random_mimics`, `mimic_chests`, `script_monsters`) may be absent.
- **Exact names only.** The name must be `std::to_string(id) + ".json"`: no sign, no
  leading zero, and the record's `id` must match it. Dotfiles (for example atomic-write
  temp files `.<name>.XXXX`) are skipped silently. Any other
  name (`0100700.json`, `100700.json.bak`) is ignored with one WARN line. On a
  Windows build, so is a name the system code page cannot hold (an emoji, say): it
  never stops the other files from loading.
- **Folder.** Locally `balance_root` is `./balance` (`deploy/config.docker.json`),
  so the folder is `deploy/balance/server/missions` in this checkout. In prod it is
  `/opt/bf/prod/balance/server/missions`, mounted read-only at `/app/balance`. The
  container runs as uid 10001, so the folders must be 0755 and the files 0644. A folder the server cannot read is logged once at
  ERROR, and what was served before stays served.
- **Unpublish** = delete the file. Write = write a temp file, then `os.replace`.

## Precedence

- A file **replaces the whole archive record** with its id. No field-by-field merge.
- A file for an id the archive lacks **adds** it. The id must be in `mission_mst`.
- **Never replaceable:** 1 and 2 (the tutorial's scripted captures), 10 (the
  template that every mission without a record borrows) and 6000000 (the Battle
  Simulator sandbag).
- `lookup()` serves the file's record, then merges the shared `mimic.json` pool into
  it, exactly as for an archive record. `archived()` is true for both.
- The capture floor applies to the file too: a wave monster without
  `authored_capture: true` and with a capturable unit is raised to 40%. Write
  `authored_capture: true` on every monster whose capture chance is meant as written
  (after making the floor explicit).

## Pickup

`MissionArchiver::setup` loads the folder at the end of boot, after the archive,
AI and mimic caches, so a file is checked against everything MissionStart will use.
Every file is loaded before the first request is served. It then starts a 2-second
timer on Drogon's main loop (`gimuserver/archive/MissionOverlay.cpp`). Requests
never do file IO: `lookup()`/`archived()` read an immutable snapshot that a refresh
swaps in whole.

- **Each tick** lists the folder and stats every exactly-named file. It does not use
  the folder's own mtime as a "nothing changed" shortcut. Docker Desktop's file
  sharing keeps serving a stale folder mtime (measured: still stale 10 s after an
  `os.replace` on the Mac), while per-file stats refresh within about a second. So a
  publish shows up within about 2-3 s locally as well as in prod. The cost is one
  readdir plus one stat per file every 2 s, on the main loop and never on a request
  thread.
- **A file is re-read only when its bytes changed:** new inode, size, mtime or ctime,
  taken from `fstat` on the descriptor that was read. Rejected files are remembered
  too, so the same bad bytes are not re-parsed or re-logged every 2 s. A hand edit in
  place is picked up the same way.
- **At most 200 changed files per tick**; a bulk restore spreads over a few ticks.
  At boot, passes repeat until everything is read.
- **A failed listing never counts as "everything was removed".** A file whose stat
  fails after listing is skipped for that round.
- **Removing a file forgets its last good version**: the archive record (if any) is
  served again.
- MissionStart and MissionEnd each look the record up. A publish between the two
  pays the new version's `zel`/`karma`/`exp`; keep those equal to the MST values and
  nothing changes in practice.

## What is checked before a file is served

In this order. The first failure rejects the file:

1. **The file.** Opened once, then `fstat`. It must be a regular file (a FIFO cannot
   block the loop: the open is non-blocking) of at most 4 MiB.
2. **Parse.** Glaze with unknown keys, missing keys and trailing bytes all as errors.
3. **Identity.** The id matches the name and is not reserved, and the id is in
   `mission_mst`. `energy_cost` equals that row's `energy_use` (`A8DEK5ob`): the quest
   screen and the client's start check quote the MST, while MissionStart charges the
   file's cost, so a higher cost fails the start with a server error for a player who
   has the MST amount but not the file's (and a Frontier Hunter mission has spent its
   orb by then). When the archive record has
   `script_monsters`, the file must carry `script_monsters` with the same set of ids:
   the client's mission script swaps monsters in by id alone, and a missing one
   crashes mid-battle.
4. **Structure**, for wave, mimic and scripted monsters alike:
   - 1-30 waves, 1-6 enemies each, `first_attack_rate` at most 100;
   - monster id not 0, `position` is `x:y`;
   - `1 <= hp <= 2,147,483,647`, `act_min <= act_max`;
   - `unit_drop_chance` and `treasure_chest_chance` at most 100, `unit_drop_type` at most 6;
   - treasure: at most 64 rows, `target_type` 1-4, `weight` 1-1000 (the weights are
     summed in u32, and a wrapped sum throws inside MissionStart only when the chest
     drops), and a type-4 `target_id` is a plain decimal item id found in item_mst;
   - treasure `amount`: 1-1,000,000 for Zel, Karma and crystals (`target_type` 1-3:
     the client adds them to battle totals it keeps in int32 and reports back), and
     1-9,999 for items (`target_type` 4: the client spawns one pickup per unit and
     MissionEnd adds the count to an int32 stack). The archive's largest are 6,000
     Zel, 5,000 Karma, 100 crystals and 1 item;
   - `zel`/`karma`/`exp` at most 100,000,000; `zel_max_drop`/`karma_max_drop` at
     most 1,000,000 and the drop counts at most 30 (MissionEnd adds the Zel and Karma
     the client reports from these in u32).
5. **Capture floor** (as above).
6. **Captures.** Every monster with `unit_drop_id != 0` and a chance above 0 has
   level >= 1, and its unit is in `unit.json` and in unit_mst with `max_lv >= 1`.
   Otherwise MissionEnd would throw after the energy was spent.
7. **Dry run.** The mimic policy is merged in as `lookup()` would, then the seven
   `populatePacket` calls MissionStart makes run into throwaway objects. This covers
   AI ids (in `ai.json`, with actions), unit ids or `visuals`, cgs files, skill ids,
   scripted monsters and mimic chests. A failing block logs its own ERROR line
   (`Unable to populate ... mission N`), followed right away by the rejection line.

What the server does **not** check, because it only plays wrong (check it
yourself before writing a file): the wave count against MST `battle_count`, `zel`/`karma`/`exp` against the
MST, exactly one boss wave (the last), unique positions, distinct stat
blocks sharing a monster id, sprite files the client lacks, ailment values, and
AI skill indexes against the skill list.

## Log lines (INFO; prod logs at INFO)

```
balance: 2 mission record(s) from ./balance/server/missions (1 replace archive records, 1 new), 0 rejected
balance: mission 100700 from server/missions/100700.json (replaces the archive record)
balance: mission 104000 from server/missions/104000.json (new mission)
balance: mission 100700 rejected (wave 1 enemy 1 (monster 40850): treasure weight 0 (1-1000)); serving the archive record
balance: mission 100700 rejected (does not parse: 1:20941: unknown_key near: ...); keeping the previous version
balance: mission 100700 rejected (dry run: ais); serving the archive record
balance: mission 100700 rejected (energy_cost 11 but mission_mst energy_use is 10); serving the archive record
balance: mission 100700 rejected (wave 1 enemy 1 (monster 40850): treasure amount 10000 (1-9999 for target_type 4)); serving the archive record
balance: mission 100700 file removed; serving the archive record
balance: ignoring server/missions/0100700.json (only files named <mission id>.json are read)   [WARN]
balance: cannot list ./balance/server/missions: Permission denied; still serving the 2 hand-built mission(s) loaded before   [ERROR]
```

The first line is the boot summary. It is always printed when `balance_root` is set,
including `0 mission record(s)` when the folder does not exist yet.

## Limits

- **Map tiles for NEW ids.** PermitPlace builds a `static const kPermitBase` on the
  first UserInfo after boot (`gme/common/PermitPlace.cpp`). It freezes `archived()`
  for the Trials, the Training Grounds and the always-open Vortex areas, dungeons and
  missions. So a NEW id in one of those needs a restart before its tile appears. A
  replaced id is unaffected (it was archived already). Story missions, the Research
  Lab, today's Vortex rotation and gated Vortex ladders are checked per request, so
  a new id there shows up on the next UserInfo.
- **Removing a NEW id's file** leaves those frozen tiles in place until a restart.
  Entering one then serves template mission 10 relabelled (no energy charged), and
  MissionEnd pays mission 10's `zel`/`karma`/`exp` (`Mission.cpp`, MissionStart
  fallback and MissionEnd lookup).
- **Raids** build their route battles from the non-boss waves of story records
  through `lookup()` (`gme/common/RaidBattles.cpp`), so a hand-built story mission
  also changes those raid fights. Their HP is scaled up to 7x and then clamped to
  2,147,483,647, so a wave near the HP cap plays as a capped enemy in a raid
  instead of wrapping.
- **Mimics.** Any record with a chest (`treasure_chest_chance > 0` and treasure rows)
  and no `random_mimics` of its own gets the shared 10% random mimic from
  `mimic.json`. Write `"random_mimics": {"chance": 0, "monsters": []}` to opt out.
- **No new AI, skill or unit records.** A file can only reference what `ai.json`,
  `skill_mst.json` and `unit.json` already hold.
- **Missions without MST rows** (the 20 Vortex ids with no dungeon or area rows, for
  example) cannot be made reachable by a file alone.

## Testing

`scripts/test_balance_missions_wire.py` runs against a running Docker server for a
checkout. It needs a TEST save (it rewrites the top account's session token and
energy):

```
SRC=$PWD/wt/battles NAME=bf-battles PORT=9978 DETACH=1 docker/run.sh
python3 -I wt/battles/scripts/test_balance_missions_wire.py \
    --src $PWD/wt/battles --url http://127.0.0.1:9978 --container bf-battles
```

It covers: a served sentinel record plus a MissionEnd; an unknown key keeping the
previous version; removal; a misnamed file and a dotfile; an unknown AI; chest
weights 0 and 2000; an `energy_cost` that differs from the MST; chest amounts of
1,000,001 Zel and 10,000 items (refused) and 9,999 items (served); and, with
`--container`, a restart showing that a good file is
served from boot and a broken one only costs its own mission. Negative steps are
fenced by a second mission's file instead of fixed sleeps. The test removes every
file it wrote.
