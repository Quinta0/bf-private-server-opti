// Own-unit helper patches for libgame.so (Brave Frontier Global v2.19.6, arm64).
//
// The server adds the player's favourited units to the helper list as cards
// whose user id is "MYUNIT<user_unit_id>" and whose own user_unit_id is a
// stand-in (993000 + slot), see server gme/common/Relations.hpp.  The client
// keeps those cards from the last FriendGet, so they go stale when the unit
// levels, and auto-repeat re-rolls them like any friend.  This code:
//
//   - refreshes every own card from the live unit when the helper picker opens
//     (hook in ReinforcementSelectScene2::downloadFiles), and the one card a
//     lookup returns (exits of both ReinforcementInfoList::getObjectWithUserID
//     overloads: MissionCheckScene, MissionStart, the battle's PlayerParty
//     build, every auto-repeat run);
//   - keeps an own card as the helper for the whole auto-repeat run instead of
//     re-rolling it (MissionResultRepeatCheckScene::resetFriendUnit);
//   - leaves own cards out of auto-repeat's friend rolls and out of its count
//     of friends left, so a run with a real friend keeps rotating friends
//     (refreshing them when they run out) instead of landing on an own card
//     that the rule above would then keep.
//
// It lives in the zero block 0x186b4d8..0x186c4cc: linker padding after the
// Cortex-A53 erratum 843419 veneer at 0x186b4cc..0x186b4d7, which nothing
// branches into or points at.  tools/patch_apk.py carries the assembled bytes
// (OWN_HELPER_CAVE_HEX); tools/test_patch_apk.py rebuilds them from this file
// with "as -arch arm64".  File offsets equal VAs in the executable segment.
// Calls go through the game's PLT stubs, as the game's own code does.

.set CAVE, 0x186b4e0

// PLT stubs (re/full.s)
.set MissionRepeatConfigInfo_shared,              0xc7bb60
.set MissionRepeatConfigInfo_getReinforcementFlg, 0xc895e0
.set UserState_shared,                            0xc298c0
.set ReinforcementInfoList_shared,                0xc6b1d0
.set ReinforcementInfoList_getCount,              0xc74060
.set ReinforcementInfoList_getObject,             0xc498c0  // (int)
.set ReinforcementInfoList_getObjectWithUserID,   0xc171f0  // (std::string) by value; only reads it
.set MissionResultRepeatCheckScene_getRandReinforcement, 0xc74a60
.set UserUnitInfoList_sharedOriginal,             0xc14330  // the account's units in every mode
.set UserUnitInfoList_getObjectWithUserUnitID,    0xc5d310  // (std::string) by value; may cut it at '.'
.set UserUnitInfoBase_copy,                       0xc39b80  // (this = to, x1 = from), not virtual
.set string_assign,                               0xc42460  // basic_string::assign(const char*, size_t)
.set operator_delete,                             0xc52840
.set FriendInfoList_existTypeOK,                  0xc64030  // (std::string) by value

// Field offsets.  ReinforcementInfo and UserUnitInfo both start with
// UserUnitInfoBase (0x250 bytes).
.set USERSTATE_REINFORCEMENT_USER_ID, 0x2f0  // std::string, UserState::getReinforcementUserID
.set UNIT_USER_UNIT_ID, 0x18                 // std::string, UserUnitInfoBase::getUserUnitID
.set UNIT_PLAYER_UNIT,  0x38                 // PlayerUnit* cache, released by ~UserUnitInfo
.set UNIT_SUMMONER,     0x248                // byte, UserUnitInfoBase::isSummoner
.set UNIT_OMNI_LEVEL,   0x24c                // int, not part of copy()
.set REIN_USER_ID,      0x2e0                // std::string, ReinforcementInfo::getUserID

.macro farcall target
	bl	cave_start + (\target - CAVE)
.endm
.macro farjump target
	b	cave_start + (\target - CAVE)
.endm

.text
.p2align 2
cave_start:

// keep_own_helper: replaces "bl getRandReinforcement" at 0x18c8ca4 in
// MissionResultRepeatCheckScene::resetFriendUnit, which copies the returned
// card's user id, deck no and friend point into UserState for the next run.
// With "Summoner Helpers" on and an own card as the current helper, hand back
// that same card instead of a random friend.  Otherwise (setting off, a real
// friend, or the card gone after a FriendGet refresh) run the stock pick.
keep_own_helper:
	stp	x29, x30, [sp, #-0x20]!
	mov	x29, sp
	stp	x19, x20, [sp, #0x10]
	mov	x19, x0
	farcall	MissionRepeatConfigInfo_shared
	farcall	MissionRepeatConfigInfo_getReinforcementFlg
	tbz	w0, #0, 1f
	farcall	UserState_shared
	add	x20, x0, #USERSTATE_REINFORCEMENT_USER_ID
	mov	x0, x20
	bl	own_helper_chars
	cbz	x0, 1f
	farcall	ReinforcementInfoList_shared
	mov	x1, x20
	farcall	ReinforcementInfoList_getObjectWithUserID
	cbz	x0, 1f
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x20
	ret
1:	mov	x0, x19
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x20
	farjump	MissionResultRepeatCheckScene_getRandReinforcement

// rotation_friend_ok: replaces "bl FriendInfoList::existTypeOK" (x0 =
// FriendInfoList, x1 = the row's user id) at the two auto-repeat friend tests:
//   0x18c9710  getRandReinforcement's candidate loop, so the random pick for a
//              real-friend run stays among friends;
//   0x18c9f5c  MissionResultScene::initConnect's count of unused friends, so
//              FriendGet refreshes the list once the friends run out (with own
//              cards counted it never would, and the pick fell back to the
//              whole list, own cards included).
// Own cards count as no friend there.  Other existTypeOK callers (friend
// points, battle, the picker) are untouched.
rotation_friend_ok:
	stp	x29, x30, [sp, #-0x20]!
	mov	x29, sp
	stp	x19, x20, [sp, #0x10]
	mov	x19, x0
	mov	x20, x1
	mov	x0, x1
	bl	own_helper_chars
	cbnz	x0, 1f
	mov	x0, x19
	mov	x1, x20
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x20
	farjump	FriendInfoList_existTypeOK
1:	mov	w0, #0
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x20
	ret

// refresh_picker_list: replaces "bl ReinforcementInfoList::shared" at
// 0x18ac208, the normal helper picker's first touch of the list
// (ReinforcementSelectScene2::downloadFiles, before it fetches unit art and
// before initialize builds the rows).  Refreshes every own card, then returns
// the list like the call it replaces.
refresh_picker_list:
	stp	x29, x30, [sp, #-0x30]!
	mov	x29, sp
	stp	x19, x20, [sp, #0x10]
	str	x21, [sp, #0x20]
	farcall	ReinforcementInfoList_shared
	mov	x19, x0
	farcall	ReinforcementInfoList_getCount
	mov	w20, w0
	mov	w21, #0
1:	cmp	w21, w20
	b.ge	2f
	mov	x0, x19
	mov	w1, w21
	farcall	ReinforcementInfoList_getObject
	bl	refresh_own_card
	add	w21, w21, #1
	b	1b
2:	mov	x0, x19
	ldr	x21, [sp, #0x20]
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x30
	ret

// refresh_lookup_x22: replaces "mov x0, x22", the single exit of
// ReinforcementInfoList::getObjectWithUserID(std::string, int) at 0x12741b0
// (MissionStartRequest::createBody, PlayerParty::entry and ::entryChange,
// MissionStartScene and SandbagStartConnectScene downloads).  x30 is
// reloaded by the next instruction, so the bl is free.
refresh_lookup_x22:
	stp	x29, x30, [sp, #-0x10]!
	mov	x29, sp
	mov	x0, x22
	bl	refresh_own_card
	mov	x0, x22
	ldp	x29, x30, [sp], #0x10
	ret

// refresh_lookup_x21: the same for "mov x0, x21" at 0x1274044, the exit of
// ReinforcementInfoList::getObjectWithUserID(std::string) (BattleManager,
// MissionCheckScene, DLCManager and keep_own_helper above).
refresh_lookup_x21:
	stp	x29, x30, [sp, #-0x10]!
	mov	x29, sp
	mov	x0, x21
	bl	refresh_own_card
	mov	x0, x21
	ldp	x29, x30, [sp], #0x10
	ret

// refresh_own_card(x0 = ReinforcementInfo* or NULL): if it is an own card and
// the unit is still in the account, copy the unit's current stats, skills,
// spheres and FE data onto it with UserUnitInfoBase::copy.  The card keeps
// its stand-in user_unit_id (copy() overwrites it), its own PlayerUnit cache
// pointer (copy() would alias the live unit's without a retain, and
// ~UserUnitInfo releases it) and its isSummoner byte (copy() clears it).
// Everything ReinforcementInfo adds (user id, name, deck no, friend point,
// questRepeatUsed) sits above 0x250 and copy() never touches it.
// Preserves x19-x28 (AAPCS64).  Frame: 0x30 saved user_unit_id, 0x48 key.
refresh_own_card:
	cbz	x0, 9f
	stp	x29, x30, [sp, #-0x60]!
	mov	x29, sp
	stp	x19, x20, [sp, #0x10]
	stp	x21, x22, [sp, #0x20]
	mov	x19, x0
	add	x0, x19, #REIN_USER_ID
	bl	own_helper_chars
	cbz	x0, 8f
	stp	xzr, xzr, [sp, #0x48]          // key = "" (short, no heap)
	str	xzr, [sp, #0x58]
	sub	x2, x1, #6
	add	x1, x0, #6
	add	x0, sp, #0x48
	farcall	string_assign                  // key = the real user_unit_id after "MYUNIT"
	farcall	UserUnitInfoList_sharedOriginal
	add	x1, sp, #0x48
	farcall	UserUnitInfoList_getObjectWithUserUnitID
	mov	x20, x0
	ldrb	w8, [sp, #0x48]                // free the key if it went to the heap
	tbz	w8, #0, 1f
	ldr	x0, [sp, #0x58]
	farcall	operator_delete
1:	cbz	x20, 8f                        // sold or fused: keep the server's stats
	cmp	x20, x19
	b.eq	8f
	ldp	x8, x9, [x19, #UNIT_USER_UNIT_ID]
	ldr	x10, [x19, #UNIT_USER_UNIT_ID + 0x10]
	stp	x8, x9, [sp, #0x30]
	str	x10, [sp, #0x40]
	stp	xzr, xzr, [x19, #UNIT_USER_UNIT_ID]   // empty it so copy() cannot free or reuse it
	str	xzr, [x19, #UNIT_USER_UNIT_ID + 0x10]
	ldr	x21, [x19, #UNIT_PLAYER_UNIT]
	ldrb	w22, [x19, #UNIT_SUMMONER]
	mov	x0, x19
	mov	x1, x20
	farcall	UserUnitInfoBase_copy
	ldr	w8, [x20, #UNIT_OMNI_LEVEL]
	str	w8, [x19, #UNIT_OMNI_LEVEL]
	ldrb	w8, [x19, #UNIT_USER_UNIT_ID]  // drop the real id copy() wrote
	tbz	w8, #0, 2f
	ldr	x0, [x19, #UNIT_USER_UNIT_ID + 0x10]
	farcall	operator_delete
2:	ldp	x8, x9, [sp, #0x30]
	ldr	x10, [sp, #0x40]
	stp	x8, x9, [x19, #UNIT_USER_UNIT_ID]
	str	x10, [x19, #UNIT_USER_UNIT_ID + 0x10]
	str	x21, [x19, #UNIT_PLAYER_UNIT]
	strb	w22, [x19, #UNIT_SUMMONER]
8:	ldp	x21, x22, [sp, #0x20]
	ldp	x19, x20, [sp, #0x10]
	ldp	x29, x30, [sp], #0x60
9:	ret

// own_helper_chars(x0 = const std::string*): if the string is an own-card id
// ("MYUNIT" plus at least one more character, like the server's
// isOwnHelperId) returns x0 = its characters and x1 = its length, else x0 = 0.
// Leaf; uses x9 and x10 only.  libc++ layout: byte 0 bit 0 set = long string
// (size at +8, data at +0x10), else size = byte 0 >> 1 and data at +1.
own_helper_chars:
	ldrb	w9, [x0]
	tbnz	w9, #0, 1f
	lsr	x1, x9, #1
	add	x0, x0, #1
	b	2f
1:	ldr	x1, [x0, #8]
	ldr	x0, [x0, #0x10]
2:	cmp	x1, #6
	b.ls	3f
	ldr	w9, [x0]
	movz	w10, #0x594d                   // "MYUN"
	movk	w10, #0x4e55, lsl #16
	cmp	w9, w10
	b.ne	3f
	ldrh	w9, [x0, #4]
	movz	w10, #0x5449                   // "IT"
	cmp	w9, w10
	b.ne	3f
	ret
3:	mov	x0, #0
	ret
