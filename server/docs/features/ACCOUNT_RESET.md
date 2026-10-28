# In-game account reset (ticket #53)

Players reset their own account inside the game, without asking in Discord.

## Why the Add Friend box

Global v2.19.6 draws no reset or transfer button (Settings' Transfer tags 30053/30054 were never
drawn, and their strings are not in libgame.so). Settings sends no request at all. The Add Friend
search box is the one place where a player types free digits and the server answers with text:

- FriendIdSearchScene sends whatever was typed as **FriendSearch** (decimal box, max 128 chars;
  the server reads 1-12 digits).
- `HandleResult::refuseToRetry(text)` arrives as notice -4000. FriendIdSearchFriendRequestScene
  handles it (FRIENDS_SPEC §1.4): the text shows in a popup, OK goes back to the search box.
- Real friend IDs are 9 digits from 100000000, so shorter numbers are free.

## Flow (gme/common/AccountReset.{hpp,cpp}, hooked in FriendSocial.cpp)

1. Search **000000**. Popup: "Reset <name> (Lv N, ID <friend id>)? Everything on this account is
   deleted for good ... To confirm, search <code> within 10 minutes." The code is 6 digits from the
   OS CSPRNG, kept in memory per profile (a restart only means asking again). The name loses `<` and
   `>`: the client's text renderer reads them as markup.
2. Search the code. A row goes into `account_reset_queue` (migration 19102026_AccountResetQueue)
   and the popup says "Reset confirmed. Close the game now ...". A wrong 6-digit number while a code
   is pending says so; a run-out code says so; 000000 while a request is queued says it is on its way.
3. An operator-side job (not part of this repository) polls the queue and runs the reset:
   off-site backup, snapshot, erase, audit lines. The erase ends
   the session, so the client's next request is refused and the game closes; the next start runs the
   tutorial. Rows stay as the record (state done / gone / failed).

Whatever tool erases accounts must know the queue table as a global (not player-owned) table.

## Checked

- Emulator (emulator-5558, 2026-10-19): the popup shows the full text with the code; OK returns to
  the search box; the code confirms; after the pass the next request is refused and the game
  closes; relaunching starts the tutorial.
