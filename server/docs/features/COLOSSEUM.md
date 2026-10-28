# Colosseum (ticket #133)

Implemented in `gimuserver/gme/handlers/Colosseum.cpp` and `gme/common/Colosseum.{hpp,cpp}`,
schemas in `packet-generator/assets/net/colosseum.kdl`, tables from migration
`23102026_CreateColosseumTables`, wire test `scripts/test_colosseum_wire.py`. ArenaInfo
(`Arena.cpp`) answers with the Colosseum hub reply (`gme::colosseum::hubBody`), and
ChallengeArenaResetInfo appends it to its own: on device the hub screen (1801, reached from the
Arena screen's Colosseum shortcut) sends only that request before its banner opens Class Select,
whose list stayed empty until it carried the class rows. No response was ever captured from the live
game: every shape is rebuilt from the v2.19.6 client's `createBody` and `readParam`
(strcmp -> setter pairs, `re/work/arena/annot.py`). Evidence labels as in ARENA_SPEC.md.

## How the client gets there (OBSERVED)

The Arena hub (1801 ChallengeArenaSelectScene) shows the Colosseum banner when
`ColosseumCommonInfo::isColosseumOpen`: `FUNC_COLOSSEUM` (features.json `colosseum_enable`, 1)
and (UserReleaseInfo 8 released or `max_arena_rank_id > 3`). So a player opens it at Arena
rank 4. No APK patch is involved, and feature gate 16 (the client's Colosseum id) is not locked.
The first tap plays the `colosseumOP` scenario (ColosseumEventScene), which DungeonEventUpdate
records, then shows 1806 Class Select. Later taps go to 1805 Top.

Scenes: 1805 Top, 1806 Class Select, 1807 Match, 1808 Party Select, 1809 Formation, 1810 Support
("Giving"), 1811 Rank, 1813 Party Set, 1820/1821 Battle Start/Restart, 1830 ArenaBattleScene,
1831 Battle End, 1832/1833 Result, 1834 Result Judge, 1835 Reward, 1836 Friend Request,
1837 Past Season, 1840 Shop Select, 1845 Shop.

Requests: Top sends ArenaInfo and then ColosseumDeckEdit on every visit, and DeckEdit again
before Match. Match sends BattleStart. ArenaBattleScene runs two fights: the player's Attack
squad against the opponent's Defence squad, then the player's Defence squad against the
opponent's Attack squad. Battle End sends ColosseumBattleEnd with one row per fight (a give-up
sends both as losses), then GetPlayerInfo. Rank sends FriendList, and the Reward button and the
shop send GetRewardInfo and RewardReceipt. Tickets are bought through ShopUse type 8.

## Reply rules the client imposes (OBSERVED)

* HgYvs3am (ColosseumClassInfo) must list all four classes. Top, Class Select, Match and
  Reward look their class up with `getObjectID` and use the result unchecked.
* FfBebG0R (ColosseumUserInfo) is a singleton: exactly one row. `JQ23rIvk` is shown once as
  a notice by Top, which then clears it (used for the season-end summary).
* 7oXGyuVH carries the player's OWN units as P_* JSON. `PlayerParty::entryColosseumPlayer`
  builds the player's side from it, not from the unit box. Empty parties make Match refuse with
  ARENA_COLOSSEUM_PLAYER_NULL.
* BZ95Eg3M must carry a battle_type 3 summary row besides the two fight rows. Result, Result
  Judge and Reward fetch it with `getObjectWithBattleType(3)` unchecked.
* Squad strings are npc_mst JSON `[{P_USER_UNIT_ID, P_MEMBER_TYPE, P_DISPORDER}]`
  (NpcPartyParseList). DeckEdit also sends the class's other selected units on the attack
  squad at positions 5+. They are stored and echoed back, but never fielded.
* Never send the master rows (wmpLokaR, aCmTmaOx, kaQtidz4) or class rows outside the hub and
  BattleEnd replies. Every Colosseum readParam calls removeAllObjects first, and
  ColosseumTopScene::initialize keeps pointers into ColosseumClassMstList (+0x3b0, set only
  there) and ColosseumClassInfoList (+0x3b8, refreshed in onSceneVisible). With English
  class rows in the Top screen's own ArenaInfo reply, the cost check read freed memory and
  refused a Hero Class squad ("exceeds the set cost limit"). The DeckEdit reply carries no
  HgYvs3am for the same reason. The CDN tables already show English formation and support
  names on device. In the client, ColosseumClassMst `XuJL4pc5` is the description and
  `YDxl64JI` the highest Cost one unit may have (999/54/50/45).
* Reward receive status 1 means it can be exchanged. Exchange-all (`setIsPackage`) sends exactly
  the status-1 rows. With no 1x6T2Trz category rows, the Reward button opens the list directly.

## Server rules (ours, nothing was dumped)

* Opponents: another player's saved squads for the same class, near the attacker's season CBP
  (within max(500, 25%)), then anyone, with the last opponent tried last. If nobody qualifies,
  a generated squad: ten different 7*/Omni units within the class's Cost cap at max level, with
  random formations and supports, named "Iron Gladiator" and similar (ids `COLOBOT<n>`).
  Snapshots are taken at BattleStart. BattleRestart returns the stored match without spending.
* CBP per match = tickets x (40 per fight won + 10 per fight lost) + 20 x tickets for winning
  both. Both fights won is a win, none won is a loss, and one each goes to the side with more HP
  left over the two fights (INFERRED from ResultJudgeScene's crown). A resend pays nothing (the
  reply is cached on the battle row). A new BattleStart abandons a match left open: its tickets
  stay spent and it pays nothing.
* Tickets: topped up to 10 at the first Colosseum request of each game day (08:00 UTC, the
  client's midnight), never added on top. ShopUse 8 sells 5 per gem, up to 99 held. 1..99
  tickets can go on one match (the Top screen's slider).
* Seasons are calendar months on the game clock (`YYYYMM`). The first Colosseum request after
  a season ends closes each class that fought: the final position is archived
  (user_colosseum_season, shown as Past Rankings), the season score resets, and gems go to the
  Gift Box (#1 50, #2-3 30, #4-10 20, everyone else who fought 10).
* Ranking (FriendList): the class's season CBP. On device, info type 1 is the everyone list (top
  50) and type 0 the Show Friends list (the reverse of ArenaFriendList's guess). The caller is in
  both, because ColosseumRankScene::getMyInfo copies its header from them. `ZW19nYrU` "0"
  (Overall Ranking) ranks all seasons' CBP in the class, and a past season ranks the archived
  standings (list 1).
* The CBP exchange: four stages (Bronze, Silver, Gold Vault, then a Champion's Market that
  never runs out). Rewards go to the Gift Box, and only the vault the player is on can be
  exchanged from. The shop's "Go to Next Stage" (after the client's own
  ARENA_COLOSSEUM_SHOP_NEXT_CHECK confirm) asks for the next stage, and the server moves the
  player on (user_colosseum.reward_stage): what they did not buy stays behind and cannot be
  reopened. Clearing a vault moves on by itself. Before this, the server answered Stage 1 again
  and the button looked broken. The stage list (U6MWwoW4) starts at the player's stage, because
  the client's next stage is ColosseumRewardStageMstList::getNextStageID, the FIRST row whose
  id differs from the shop's stage: with all four sent, stage 2 asked for stage 1 and nobody got
  past it (#140, seen on the emulator). The table is `kRewards` in Colosseum.cpp.
* Units fight with their SP picks (`P_FE_INFO` = user_units.fe_skill_info), own Extra Skill
  and Elgif skill (#136). SP "cost reduction" options lower BB activation cost, not unit Cost:
  the class caps are checked client-side against raw UnitMst Cost (isDcekCheck), and the server
  applies no cost changes of its own.
* Defenders collect `battle_cnt_accept` / `win_cnt_accept` and never lose CBP.

## Not done

* The friend-request prompt after a match (`Sx5H0Ikn` is always 0), extra rules
  (`Z7PXoxc2`/`IvuccUuD` 0), campaign bonuses (echoed only), the Colosseum archive group
  7UVKrgle, and trophies. The P_EXT_* caveat from ARENA_SPEC.md applies.
