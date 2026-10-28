# Guilds between real players

Dated 2026-10-17. Server-tested with `scripts/test_guilds_wire.py` (133
checks). Client-confirmed on the emulator (BF_Tall_Test, a copy of a real
save): a profile found by friend ID shows View Guild Info, the guild page
renders (name, rank 1st, master, 1/10 members, date, description), and Request
to Join shows "Guild Join Request Sent" and returns to the profile; the
application then appears on the guild's Requests list and accepting it adds
the player. Not yet seen on a client: the recommended list, the guild town as
a joined member, the Requests screen, the message box, the board and the
ranking rows. On that tall-screen AVD the Home carousel cannot be swiped to the
Guild page (the known tall-screen swipe bug), and the Social menu's "Guild
Inbox" button opens the serial-code screen. Builds on [GUILD_INVITE_2026-09-26.md](GUILD_INVITE_2026-09-26.md),
which covers the Guild Hall, the computer-run friends and the rank vocabulary.

## Why nobody could join anyone

Every guild was one player plus computer-run friends because the server only
implemented the founder's side. Root causes, all confirmed in libgame.so:

- **No recommended list.** A player with no guild lands on
  GuildRecommendedListScene (102002), which sends no request and draws
  `csI7dh30`. Only the GuildInfo reply can carry it, and it never did.
- **Search was a stub.** GuildSearch (`R38ba9M3`) answered `{}`, which the
  client shows as "not found".
- **Applying went nowhere.** GuildJoin type 1 was logged and dropped.
- **The Requests screen asked the wrong handler.** GuildJoinedList
  (`3890ab5j`) is sent only by GuildRequestsListScene, for open applications
  and invites (`aj38Jk10`). It was mapped to the GuildInfo handler.
- **Accept/decline was a stub.** GuildInviteManage (`1D8bba8D`) answered `{}`.
- **Invites to real players did nothing.** GuildJoin type 2 only accepted
  ids on the inviter's computer-run roster.
- **Friend cards hid guilds.** `friends.kdl` put FriendInfo's guild id under
  `mgNdrCEe`; the client reads it from `sD73jd20` (FriendInfoResponse
  @0x13d983c). Every friend looked guildless, so the profile's View Guild
  button never appeared, and its request (GuildMemberGuildInfo `38bSeq81`)
  was not even registered, which would have closed the session.
- **The roster assumed the viewer was the founder.** Every viewer was drawn as
  Guild Master, and members were drawn from the viewer's own friend list.
- **Ranking showed only your guild.** Both ranking scenes draw the last row in
  the fixed "your guild" frame (GuildRaidRankingResultScene::setRankingLayout
  @0x1E1C574, GuildRankingScene::setScrollArea @0x1E41E54). With one row the
  list was empty. Ranking detail ignored the tapped guild id.

## The flows as built

**Finding a guild.** GuildInfo for a player with no guild sends guild id 0
(the client's own "no guild"; it also resets a player who was dismissed while
their game was open), the founding fee, and up to 30 recommended guilds with
room, busiest first. Search matches names case-insensitively, or a guild id
typed as digits. A friend's profile shows View Guild when their card carries a
guild id; GuildMemberGuildInfo answers with that guild (including `Nt38aDqi`,
max members), and the detail screen offers Apply.

**Applying.** GuildJoin type 1 stores an application. The client shows
"request sent" and stays outside the guild. The applicant's message box shows
it as action 91 with Cancel. The guild's Requests screen lists it as a type-1
row whose `sD73jd20` is the applicant's own guild, 0 (that is what gives the
row Accept/Refuse). Accepting or refusing sends GuildInviteManage with the
row's invite id; the reply is `{}` because the client raises the member count
and drops the row itself.

**Inviting.** Any friend profile's Invite button sends GuildJoin type 2. A real
player gets the invite in their message box (InboxMessageManage fetch, action
90, Accept/Decline) and the guild's Requests screen lists it as a type-2 row
(Cancel). Accept is GuildInviteManage; its reply carries the guild just joined,
because the client pops back to the inbox without asking. Decline and Cancel
go through InboxMessageManage actions 0 and 91. Computer-run friends still
join at once, but only the Guild Master can add them (their cards come from the
owner's roster).

**Ids.** Applications and invites share `user_guild_requests.request_id`. It is
the row's invite id (`7ad38bad`), the message id (`78Zema70`) and the message's
invite id, so every path names a request the same way.

**Ranks (AUTHORED, matching the client's buttons).** Guild Master 1, Vice 2,
Officer 3, Member 4. Ranks 1-3 recruit (invite and decide applications; the
client enables Invite only for them). The Guild Master manages everyone, a Vice
manages Officers and Members. One Vice at a time. A Guild Master who leaves
hands the guild to the highest-ranked, longest-serving real member (the client
only lets them leave while a Vice exists); the last real member leaving
dissolves it. GuildMemberUpdate replies stay `{}` (see the 2026-09-27 crash).

**Settings.** GuildUpdate carries description, crest and an action (0 save,
-1 disband). There is no rename on the client. Guild Master only.

**Board.** GuildBoardInfo lists the newest 100 posts, newest first, and marks
them seen. GuildBoardPost must reply with the refreshed board, because the
scene only redraws. GuildInfo's `b6D19bg9` shows the unread badge when someone
else posted since the member last opened the board. Posts are capped at 400
bytes, 10 per minute per player.

**Ranking.** Every guild ordered by prestige, experience, members and age, then
the viewer's own guild again as the last row with its real place. A player
with no guild gets a "-" row there instead. P_RANK in GuildInfo and the search
rows is the same place. Ranking detail answers for the tapped guild; each
member's name also goes in `s35idar9`, which shares B5JQyV8j's setter and wins.

## Limits

- One guild per real player, checked inside the transaction that adds them.
- 10 open applications per player, 30 open invites per guild, 14-day expiry.
- Neither the Requests list nor the message box can be emptied over the wire
  (a zero-row array never reaches readParam), so the client can show a row
  the server has already dropped. Acting on it gets a benign reply.

## Still missing

- Guild raids, guild quests and Guild Token earning (see the session report).
- Guild level-up: the client never asks for it; the level follows
  `user_guilds.experience`, which nothing raises yet.
- The Research Lab (GuildUpgrade / GuildUpgradeCont / GuildResearchInfo).
- GuildPreviousRanking (records) answers `{}`; there is no history.
- No "you were accepted / dismissed" notices (T_GUILD_MESSAGES could carry them).
