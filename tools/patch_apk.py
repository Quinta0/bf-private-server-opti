"""Patch the Brave Frontier Global v2.19.6 APK to talk to our own server.

Two modes. Every site is checked against the original bytes first; nothing is
written on a mismatch.

Default (plain HTTP, local testing) changes lib/arm64-v8a/libgame.so as follows:

  1. Boot service-discovery host "api-sl.bfww.gumi.sg" -> --host (e.g. 127.0.0.1:9960).
     SERVICE_PHP_URL() builds it with strlen, and it is the literal's only user,
     so a shorter NUL-padded value is safe.
  2. The shared "https://" literal used by CommonUtils::getPhpUrl and friends -> "http://".
  3. The inline "https://" SSO prefix in GumiLiveManager::init (initial hosts) -> "http://".
  4. The same inline prefix in ServiceRequestManager::reinitEndPoints (hosts from /dls) -> "http://".

Together these send all game, login and asset traffic over plain HTTP to the
host the server's /dls reply names, so no TLS patch or server certificate is
needed. lib/armeabi-v7a is dropped so an arm64 device can never load an
unpatched library. The old signature is removed and the APK is re-signed.

Always (both modes): Home's INFO and Menu's News open the URL the server sends in
NoticeInfo (Pj6zDW3m jsRoN50z) instead of https://<game host>/<url_mst
cH0Kdchp>. That page is on the game host, whose private-CA certificate the
WebView refuses (black page); the server now sends a self-contained data: page
with the owner's news there (server gme/common/NewsPosts.hpp). The NEW badge
needs no patch: it already compares NoticeInfo's id with the last one opened.

Always (both modes): Menu's Credits button (its WebView never loaded) becomes
"Reset Account". A tap does what searching 000000 in Social > Add Friend >
Friend Search does: it sends FriendSearch 000000, the server's popup names the
account and gives a 6-digit code, and OK lands in the Friend Search box, where
only searching that code queues the reset (server gme/common/AccountReset.cpp,
docs/features/ACCOUNT_RESET.md). The label is two new bundled images,
assets/menu_reset_btn_{1,2}.png (tools/make_reset_label.py); the client loads a
downloaded file first and the APK's bundled one otherwise, so nothing on the CDN
changes and older APKs keep their Credits button.

--https (internet servers): keep the client's own "https://" prefixes and
instead replace the single trusted root certificate (the Go Daddy root PEM at
0x2318F2F, loaded with strlen by BFCertificateProvider) with --root-ca, plus
point getPurchasePhpUrl at "https://" so shop requests are encrypted too. The
server must present a certificate for --host signed by that root.

Always (both modes): the server lists the player's favourited units as helper
cards (user id "MYUNIT<user_unit_id>", server gme/common/Relations.hpp). Six
hooks into new code in linker padding keep those cards current and keep them
through auto-repeat: the helper picker, MissionStart and every battle copy the
live unit's stats onto the card; auto-repeat keeps an own card as the helper
for the whole run instead of re-rolling a random friend, and a real-friend run
re-rolls among friends only. Source, hook sites and reasoning:
tools/own_helper_cave.s; tools/test_patch_apk.py checks the bytes.

--tall (either mode): fill tall phones instead of letterboxing them.
AppDelegate::applicationDidFinishLaunching picks the cocos2d design resolution
from h/w of the GL frame (Android, API > 18). Every ratio above 1.76 gets the
iPhone-5 size 320x568 (ShowAll, or ExactFit for exactly 16:9), so a 20:9 phone
shows the 568-high game with black bars above and below. The patch makes that
branch use 320 x (320 * h/w) with ShowAll instead: the design aspect then
equals the screen's, so there are no bars and no stretching. Most layout is
parametric: GameLayer::getOffsetY() is -(winHeight - 480) / 2, so headers and
menu bars move to the real screen edges and touch stays aligned. Caveats seen
on a 1080x2424 emulator: full-screen art is still 480/568 high, so black bands
remain behind the middle of some scenes; about 70 sites (incl.
Utils::filterFilesForiPhone5 and the battle managers) test offsetY == -44, the
exact iPhone-5 value, so with --tall the client takes the plain (non-_iph5)
asset variants (a one-off extra download of a few hundred files) and battle
layout runs its non-iPhone-5 path, which has not been checked yet. Ratios up
to 1.76 are unchanged. Opt-in and experimental.

Usage:
  python3 -I tools/patch_apk.py --apk IN.apk --out OUT.apk [--host 127.0.0.1:9960]
  python3 -I tools/patch_apk.py --apk IN.apk --out OUT.apk --host api.example.net \\
      --https --root-ca signing/bf-root-ca.pem [--tall]
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_TOOLS = Path.home() / 'Library/Android/sdk/build-tools/36.1.0'
KEYSTORE_PROPS = ROOT / 'signing/keystore.properties'

LIB = 'lib/arm64-v8a/libgame.so'
LIB_SHA256 = '076c8ed12e84d122938255c1530a4d4b1853d876b5928dd0c2389538fc4cad95'
DROP_PREFIXES = ('lib/armeabi-v7a/',)
DROP_SIGNATURE = ('META-INF/CERT.RSA', 'META-INF/CERT.SF', 'META-INF/MANIFEST.MF')

DISCOVERY_HOST_OFFSET = 0x232711F
DISCOVERY_HOST_ORIGINAL = b'api-sl.bfww.gumi.sg\0'

# RES_URL() @0x1162ed8 ignores the /dls resource host while the MST repair
# counter (Options -> data update, or a corrupt-MST check at login) is set, and
# uses this literal instead: gumi's S3 bucket, now gone (404 NoSuchBucket), so
# every /mst file failed with DLC_PROBLEM_ENCOUNTERED. Built with strlen at
# 0xcc2e74; that static initializer is the literal's only reference.
REDOWNLOAD_HOST_OFFSET = 0x23270E6
REDOWNLOAD_HOST_ORIGINAL = b'bravefrontier-dlc-130.s3-website-us-west-1.amazonaws.com\0'

# (file offset, original bytes, new bytes, purpose). File offsets equal VAs.
FIXED_PATCHES = [
    (0x2369CE3, b'https://\0', b'http://\0\0', 'getPhpUrl & co: "https://" literal -> "http://"'),
    # GumiLiveManager::init: x9 = "https://" (SSO, size byte 0x10) -> "http://" (size byte 0xe)
    (0xF5FBEC, bytes.fromhex('694ec7f2'), bytes.fromhex('49e7c5f2'), 'GumiLiveManager::init movk x9,#0x2f3a,lsl#32'),
    (0xF5FBF0, bytes.fromhex('08028052'), bytes.fromhex('c8018052'), 'GumiLiveManager::init mov w8,#0xe'),
    (0xF5FBF4, bytes.fromhex('e9e5e5f2'), bytes.fromhex('e905e0f2'), 'GumiLiveManager::init movk x9,#0x2f,lsl#48'),
    # ServiceRequestManager::reinitEndPoints: same prefix in x8
    (0x1C7A5C8, bytes.fromhex('08028052'), bytes.fromhex('c8018052'), 'reinitEndPoints mov w8,#0xe'),
    (0x1C7A5DC, bytes.fromhex('684ec7f2'), bytes.fromhex('48e7c5f2'), 'reinitEndPoints movk x8,#0x2f3a,lsl#32'),
    (0x1C7A5E0, bytes.fromhex('e8e5e5f2'), bytes.fromhex('e805e0f2'), 'reinitEndPoints movk x8,#0x2f,lsl#48'),
]


# --https: the pinned root PEM slot (1445-byte PEM + NUL; nothing else points
# inside it), guards around it, and getPurchasePhpUrl -> the "https://" literal.
PEM_OFFSET = 0x2318F2F
PEM_SLOT_LEN = 0x5A6  # 1445-byte PEM + NUL
PEM_SLOT_SHA256 = 'cbfb7dcf86bc532d0f503479109faa03fcc68d6b98ef121252551d365b5b4605'
HTTPS_GUARDS = [
    (0xF48CB4, bytes.fromhex('809e009000bc3c9101008012'), 'BIO_new_mem_buf(0x2318f2f, -1): strlen-terminated'),
    (0x2318F17, b'21BFCertificateProvider\0', 'type name before the PEM slot'),
    (0x23194D5, b'9PWWebView\0', 'type name after the PEM slot'),
    (0x2369CE3, b'https://\0', '"https://" literal used by getPurchasePhpUrl'),
]
PURCHASE_HTTPS_PATCHES = [
    (0x1CE93D0, bytes.fromhex('e13100d0'), bytes.fromhex('01340090'), 'getPurchasePhpUrl adrp x1 -> page of "https://"'),
    (0x1CE93D4, bytes.fromhex('21c00291'), bytes.fromhex('218c3391'), 'getPurchasePhpUrl add x1,#0xce3 ("https://")'),
]

# Home's INFO button: HomeScene2::dispNoticePage (and its copy in AnotherHomeScene,
# the home of an opened gate), and Menu's News button. FUNC_NOTICE_LIST is never set in this build, so both
# take the web path, which builds getPhpUrl() + UrlMst::getNoticeUrl(), opens it
# with pushWebViewScene(url, 320, 464, "caption_notice.png") and stores
# NoticeInfo::getNoticeID() as the last notice seen (that clears the badge).
# RaidWorldScene::newsWeb already opens NoticeInfo::getNoticeUrl() as is; these six
# instructions make INFO do the same:
#   bl NoticeInfo::shared; add x8,sp,#0x18; bl NoticeInfo::getNoticeUrl   (url -> [sp+0x18])
#   stp xzr,xzr,[sp]; str xzr,[sp,#0x10]; mov x20,sp     (empty string for the append that follows)
NEWS_PATCHES = [
    (0x16F464C,
     bytes.fromhex('e863009150aad597e7e2d497e8030091f403009178afd597'),
     bytes.fromhex('ad16d697e8630091af90d597ff7f00a9ff0b00f9f4030091'),
     'HomeScene2::dispNoticePage: INFO opens NoticeInfo url'),
    (0x16E612C,
     bytes.fromhex('e863009198e3d5972f1cd597e8030091f4030091c0e8d597'),
     bytes.fromhex('f54fd697e8630091f7c9d597ff7f00a9ff0b00f9f4030091'),
     'AnotherHomeScene::dispNoticePage: INFO opens NoticeInfo url'),
    # Menu's red News button (MenuTopScene::touchBegan, tag 10000): the same build into
    # [sp+0x38] with the temporary at [sp+0x20].
    (0x17511F0,
     bytes.fromhex('e8e300916737d497fe6fd397e8830091f48300918f3cd497'),
     bytes.fromhex('c4a3d497e8e30091c61dd497ff7f02a9ff1b00f9f4830091'),
     'MenuTopScene::touchBegan: Menu News opens NoticeInfo url'),
]

# Menu's Credits button (MenuTopScene::touchBegan, tag 10009) -> "Reset Account". After its
# playOkSe the original builds getPhpUrl() + UrlMst::getCreditUrl() for a WebView; instead:
#   bl UserState::shared; x8 = libc++ short string "000000" (size byte 12)
#   stp x8,xzr,[sp,#0x38]; str xzr,[sp,#0x48]; add x1,sp,#0x38; bl UserState::setFriendID
#   mov w1,#614; b 0x1751168   (changeSceneWithSceneID(614): FriendIdSearchFriendRequestScene)
# That is exactly what FriendIdSearchScene's search button does. setFriendID copies the string
# with assign(), so the short temporary needs no destructor. Scene 614 sends FriendSearch with
# UserState::getFriendID() and returns to scene 613 (the Friend Search box) after the popup.
# The rest of the old branch (up to 0x1751520) is left in place, unreachable.
# setLayoutControl builds the label names inline: 16 bytes from these literals + ".png".
RESET_BUTTON_PATCHES = [
    (0x1751498,
     bytes.fromhex('e8e30091bd36d497546fd397e8830091f4830091914fd397e8834039eaa742a98b0240b2e0e300910cfd41d3'),
     bytes.fromhex('0a61d397880186d20806a6f20806c6f20806e0f2e8ff03a9ff2700f9e1e30091927dd397c14c80522affff17'),
     'MenuTopScene::touchBegan: Credits sends FriendSearch 000000'),
    (0x2353443, b'menu_credit_btn1', b'menu_reset_btn_1', 'Credits label -> menu_reset_btn_1.png'),
    (0x2353458, b'menu_credit_btn2', b'menu_reset_btn_2', 'Credits pressed label -> menu_reset_btn_2.png'),
]

# After a battle, go back to the list the battle was picked from (ticket #74).
# MissionResultFriendRequestScene::changeNextScene (0x18c301c) and its twin in
# MissionResultScene::updateEvent route the player out of the result screens.
# The stubs below live in GachaEffectDebugScene::initialize (0x164a770), dead
# code: nothing constructs that class (its ctor has no callers). They were
# assembled with clang -target aarch64-linux-android and their branch targets
# checked with objdump.
#
# Vortex (area type 1): both build DungeonSPSelectScene2(area, true), and
# DungeonSPSelectScene2::initialize (0x1844cb0) sends a one-dungeon area to
# scene 115, the Vortex top, when that flag is set (MissionSelectScene2's Back
# passes true so it does not bounce into the level list again). A many-dungeon
# area showed its dungeon list. Both sites now call sp_scene (0x164a8e4), which
# opens the played dungeon's level list the way a tap on the dungeon does
# (DungeonSPSelectScene2::changeNextScene 0x1844fc8: MissionSelectScene2,
# setParams(dungeon, 1), pushed with vtable +0x320). If the last mission or its
# dungeon is missing it builds DungeonSPSelectScene2(area, false) instead, which
# is what a tap on the area does. Back from the level list is unchanged.
#
# Story and Ishgria quests: both go to scene 61 (DungeonSelectScene2, the area
# map), or 62 (AnotherDungeonSelectScene) in Ishgria. Those scenes open a
# dungeon's quest list only after setAutoSelectSceneParams, which the client
# calls only after story scripts (MissionEventScene::updateEvent 0x17f3bb4).
# Both changeSceneWithSceneID calls (0x18c4c18, 0x18cb71c) now go to the stub at
# 0x164a770. For scene 61/62 it does what the script path does with the last
# mission's dungeon: new DungeonSelectScene2 / AnotherDungeonSelectScene,
# setAutoSelectSceneParams(dungeon), then changeScene through vtable +0x318,
# the same call changeSceneWithSceneID makes. It keeps the plain scene change
# when the result opened a new area (MissionRewardInfo clear_area_id is not
# "0", so the map plays its NEW AREA animation), when the mission, dungeon or
# area is missing, or when the area type is not story (0, scene 61) or
# Ishgria (2, scene 62). Any other scene id passes straight through.
RETURN_PATCHES = [
    (0x164A770,
     bytes.fromhex(
                   'ff0306d1ea8300fde923116dfc6f12a9fa6713a9f85f14a9f65715a9f44f16a9'
                   'fd7b17a9fdc3059148d03bd5e80700f9081540f9f30300aaa80318f8cdbbd897'
                   '1827d8971f040071ab2a0054e8c3019169e20c91e91700f9e9430191080140b2'
                   'eac30091e81300f9280140b2f4031f2ae80f00f9480140b2e80b00f906000014'
                   '94060011bbbbd8970627d8979f02006b6a280054b7bbd897e103142a7139d897'
                   'f90300aad78fd897e8e30291f1dbd897026800f0e0e3029123028052e1031faa'
                   '42b019913b8ad7970000c03d080840f9016800d022008052a003963ca80317f8'
                   '1f7c00a91f0800f9a08302d121e00791bcb6d89717004039180840f91f7c00a9'
                   '1f0800f9a803563868000036a00357f8f01fd897e8e3423968000036e06740f9'
                   'ec1fd897a88302d1e00319aafd92d897a8035638ffff07a9ff3b00f9c8000037'
                   'a003d63ca80357f8e01f803de84300f919000014ba8356f85f4300b1e2360054'
                   'bb0357f85f5f00f1c2000054fc1340f9487b1f53e8c301395a0100b50d000014'
                   '4843009116ed7c92e00316aaad0cd897c80240b2fc0300aae86b07a9e04300f9'
                   'e0031caae1031baae2031aaad19ed8979f6b3a38e1c30191e00313aa751bd897'
                   'e8c34139fa03002a68000036e04340f9c01fd8973a0200377ca241f968a641f9'
                   '9f0308eb400100549f7f00a99f0b00f9a803563868120037a80357f8a003d63c'
                   '880b00f98003803dbc000014e01740f9a18302d15f84d797a803563868000036'
                   'a00357f8ab1fd897a88302d1'),
     bytes.fromhex(
                   '3ff40071600000543ff80071810a0054fd7bb9a9fd030091f35301a9f55b02a9'
                   'f30300aaf403012a9293d797e80301919890d897e98340794a0086523f010a6b'
                   'f5179f1ae003019146000094f5070034407cd797e80301919ef8d797dd41d797'
                   'e1030191f3dfd797f50300aae00301913c000094b50600b4e00315aae8630191'
                   'e0d7d8975f55d897e1630191cd03d997800500b4e80301916a00d797611fd897'
                   'e10301914b47d897f50300aae00301912c000094750400b4e00315aa652dd797'
                   '89f6005129791f531f00096ba10300549ff6007121010054008e8052d90cd897'
                   'f50300aa57e2d897e00315aae1630191b423d7970800001400898052d10cd897'
                   'f50300aa4fb0d797e00315aae163019134cbd797e063019112000094680240f9'
                   '088d41f9e00313aae10315aa00013fd6f55b42a9f35341a9fd7bc7a8c0035fd6'
                   'e063019107000094e00313aae103142af55b42a9f35341a9fd7bc7a83d4ed717'
                   '0900403969000036000840f9d91fd817c0035fd6fd7bbca9fd030091f35301a9'
                   'f30300aaf37bd797e883009151f8d7979041d797e1830091a6dfd797f40300aa'
                   'e0830091efffff97b40200b4e00314aae883009193d7d8971255d897e1830091'
                   '8003d997f40300aae0830091e5ffff97740100b4007180529a0cd897f30300aa'
                   '74bbd897e00313aae10314aa2200805200f3d897e00313aa0a00001400788052'
                   '900cd897f40300aa0a8ad897e00314aae10313aae2031f2ab627d897e00314aa'
                   'f35341a9fd7bc4a8c0035fd6'),
     'result-screen stubs in dead GachaEffectDebugScene::initialize'),
    (0x18C420C,
     bytes.fromhex(
                   '007880526826ce97f60300aae2a3ce9722008052e00316aae10315aa37008052'
                   '8d41ce97'),
     bytes.fromhex(
                   'e00315aab519f697f60300aa370080521f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d5'),
     'changeNextScene: Vortex result opens the played dungeon level list (sp_scene)'),
    (0x18CEC48,
     bytes.fromhex(
                   '00788052d9fbcd97f50300aa5379ce9722008052e00315aae10314aaff16ce97'),
     bytes.fromhex(
                   'e00314aa26eff597f50300aa1f2003d51f2003d51f2003d51f2003d51f2003d5'),
     'MissionResultScene::updateEvent: same for its Vortex exit'),
    (0x18C4C18, bytes.fromhex('6a65cd97'), bytes.fromhex('d616f697'),
     'changeNextScene: changeSceneWithSceneID -> quest-list stub'),
    (0x18CB71C, bytes.fromhex('a94acd97'), bytes.fromhex('15fcf597'),
     'MissionResultScene::updateEvent: changeSceneWithSceneID -> quest-list stub'),
]

def arm64_b(at: int, target: int) -> bytes:
    offset = target - at
    assert offset % 4 == 0 and -(1 << 27) <= offset < (1 << 27), (hex(at), hex(target))
    return (0x14000000 | ((offset >> 2) & 0x3FFFFFF)).to_bytes(4, 'little')


# Vortex story scenes: after a cutscene the player lands on the Vortex top, not the
# event's dungeon list (Glitch in the Heart, Oct 2026).  MissionEventScene::updateEvent
# (0x17f34bc) picks the exit from the script's return point (PARAM id=41; 993 of the
# 1,228 bundled scripts end in 2, none in 6).  Return points 1 and 2, and the default
# case of a Vortex (area type 1) mission, all end at 0x17f3998, which is
# changeSceneWithSceneID(115), the Vortex top.  Return point 6 is the same function's
# "DungeonSPSelectScene2(area of the mission's dungeon, false)" block at 0x17f390c, the
# area's own dungeon list, which is what a tap on the area opens.  The first instruction
# of the 115 fallback now branches into a two-instruction cave that rebuilds the one
# register that block reads (x28 = &mission id string = this + 0x350; the type-10 loop
# before the fallback reuses w28) and jumps to the 6-block.  Every path that used to
# end on the Vortex top now opens the played area's dungeon list; the level-list branch
# for type-10 areas with a playable mission is untouched.  The cave is one of the 17
# erratum-padding blocks (0x16bc128, see memory libgame-code-caves).
VORTEX_SCENE_CAVE = 0x16BC128
VORTEX_SCENE_FALLBACK = 0x17F3998
VORTEX_SCENE_AREA_LIST = 0x17F390C
VORTEX_SCENE_PATCHES = [
    (VORTEX_SCENE_CAVE, bytes(8),
     (0x910D4000 | (19 << 5) | 28).to_bytes(4, 'little') + arm64_b(VORTEX_SCENE_CAVE + 4, VORTEX_SCENE_AREA_LIST),
     'Vortex scene exit cave: x28 = this + 0x350, then the area dungeon list block'),
    (VORTEX_SCENE_FALLBACK, bytes.fromhex('610e8052'), arm64_b(VORTEX_SCENE_FALLBACK, VORTEX_SCENE_CAVE),
     'MissionEventScene::updateEvent: a Vortex scene opens the area dungeon list, not the Vortex top'),
]

# The two nag pop-ups (ticket #25), both decided only on the phone.
# PTRateThisAppPopup::showPopupIfRequired runs after every result and asks for a
# store rating from the third one on; it now returns at once. changeNextScene
# offers "connect Facebook for a gem" at 0x18c4be4 after a full dungeon clear
# for non-Facebook logins; the cbz becomes a nop, so it takes the normal next-
# scene path, as it does when skipFlg is set.
NAG_PATCHES = [
    (0xE1415C, bytes.fromhex('ff8302d1'), bytes.fromhex('c0035fd6'),
     'PTRateThisAppPopup::showPopupIfRequired: return at entry'),
    (0x18C4BE4, bytes.fromhex('08050034'), bytes.fromhex('1f2003d5'),
     'changeNextScene: never offer Facebook-for-a-gem'),
]

# Up to 10 fusion materials instead of 5 (ticket #28). The cap is
# UnitMixElemSelectScene+0x430, which the constructor (0x1c05160) loads with
# {0x430, 0x434} = {5, 0} from this 8-byte literal (its only xref).
# checkMixAllSelectFlg already raises it to 99 for a maxed Omni base, so the
# material list, count badges, getMixGold/getMixExp and UnitMixRequest all walk
# the whole list. The fusion and animation screens draw only their 5 layout
# slots; materials 6-10 are fused and counted but not drawn. The server's
# UnitMix takes any number of materials.
FUSION_PATCHES = [
    (0x236BC10, bytes.fromhex('05000000'), bytes.fromhex('0a000000'),
     'UnitMixElemSelectScene: material cap 5 -> 10'),
]

# The fusion screen draws all 10 materials (V5, ticket #28). Its
# slots are the elem_unit0..4 rows of layout_unit_mix_main.csv, a DLC file that
# installs already hold, and a missing row crashes setElemUnit (the LayoutCache
# is used unchecked). So the scene loads a new name, bundled in the APK with
# ten elem_unit rows in a ring around the base. Three loops go to 10. The
# tap tests were unrolled for tags 200-204; touchEnded (tap: open the material
# list) and touchLong (long-press: unit detail) become loops over 10 in place:
#   touchEnded: frame 0x20 -> 0x30, counter at [sp,#0x20]; for tag in 200..209:
#     if isTouchObject(tag) -> the existing open-list path at 0x1c13ef8
#   touchLong: counter at the free [sp,#8]; for i in 0..9: x21 = getElemUnit(i);
#     if x21 and isTouchObject(200+i) -> the existing detail path at 0x1c13de4
# The fusion animation
# (UnitMixPlayScene) keeps 5: it stores them in a 5-entry array at +0x4a0.
FUSION_SLOT_PATCHES = [
    (0x1C0E148, bytes.fromhex('9f1600f1'), bytes.fromhex('9f2a00f1'),
     'UnitMixMainScene::setElemUnit: cmp x20,#10 (draw 10 slots)'),
    (0x1C112B0, bytes.fromhex('1f1700f1'), bytes.fromhex('1f2b00f1'),
     'UnitMixMainScene::setBbStop: cmp x24,#10'),
    (0x1C16024, bytes.fromhex('1f1700f1'), bytes.fromhex('1f2b00f1'),
     'UnitMixMainScene::setBbUpVisible: cmp x24,#10'),
    (0x236BDD2, b'layout_unit_mix_main.csv\0', b'layout_unit_mix_mn10.csv\0',
     'UnitMixMainScene::initialize: load the bundled 10-slot layout'),
    (0x1C13E68, bytes.fromhex('f44fbea9'),
     bytes.fromhex('f44fbda9'),
     'UnitMixMainScene::touchEnded: 0x30-byte frame (a slot for the loop counter)'),
    (0x1C13E8C, bytes.fromhex('f44fc2a8'),
     bytes.fromhex('f44fc3a8'),
     'UnitMixMainScene::touchEnded: matching epilogue'),
    (0x1C13F20, bytes.fromhex('f44fc2a8'),
     bytes.fromhex('f44fc3a8'),
     'UnitMixMainScene::touchEnded: matching epilogue (tail call)'),
    (0x1C13E94, bytes.fromhex(
                   '01198052e00313aae20314aa68aac197a002003721198052e00313aae20314aa'
                   '63aac1970002003741198052e00313aae20314aa5eaac1976001003761198052'
                   'e00313aae20314aa59aac197c000003781198052e00313aae20314aa54aac197'
                   'a0fc0736'),
     bytes.fromhex(
                   '08198052e82300b9e103082ae00313aae20314aa66aac19760020037e82340b9'
                   '080500111f490371e1feff54f2ffff171f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d5'),
     'UnitMixMainScene::touchEnded: tap test loops over tags 200-209'),
    (0x1C13CD8, bytes.fromhex(
                   'fa56c09775e0c19708a441a9290108cb3f7d7df200010054150140f9d50000b4'
                   '01198052e00313aae20314aacfaac197e0060037ed56c09768e0c19708a441a9'
                   '290108cb3f797cf200010054150540f9d50000b421198052e00313aae20314aa'
                   'c2aac19740050037e056c0975be0c19708a441a9290108cb297d7d923f4500f1'
                   '03010054150940f9d50000b441198052e00313aae20314aab4aac19780030037'
                   'd256c0974de0c19708a441a9290108cb3f757bf200010054150d40f9d50000b4'
                   '61198052e00313aae20314aaa7aac197e0010037c556c09740e0c19708a441a9'
                   '290108cb297d7d923f8500f103f5ff54151140f9d5f4ffb481198052e00313aa'
                   'e20314aa99aac19720f40736'),
     bytes.fromhex(
                   'e8031f2ae80b00b9e103082ae00313aa4f000094f50300aae00000b4e80b40b9'
                   '01210311e00313aae20314aacfaac197e0060037e80b40b9080500111f290071'
                   '21feff54d2ffff171f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d51f2003d5'
                   '1f2003d51f2003d51f2003d5'),
     'UnitMixMainScene::touchLong: long-press loops over slots 0-9'),
]
# Settings > Downloadable Content: Full Download and Update are switched off.
# OptionTopScene::confirmAnswerYes (0x178a114) dispatches on the dialog id
# through a byte table at 0x2354d31 (index id - 30004, target 0x178a15c + 4*b).
# Entry 55 (30059, Full Download) starts the whole download; entry 57 (30061,
# Update) deletes the key chain, calls clearApplicationData and zeroes every MST
# version, after which MST repair loops on "Application will close now". Both
# entries become 0x74, the default case (0x178a32c, the epilogue): Yes only
# closes the dialog. On-demand downloads during play are untouched.
NODL_PATCHES = [
    (0x2354D68, b'\x0b', b'\x74', 'OptionTopScene: Full Download confirm does nothing'),
    (0x2354D6A, b'\x1c', b'\x74', 'OptionTopScene: Update (data wipe) confirm does nothing'),
]
FUSION_SLOT_ASSETS = {
    'assets/layout_unit_mix_mn10.csv': ROOT / 'tools/apk-assets/layout_unit_mix_mn10.csv',
}
# Bundled next to the APK's other flat assets/*.png, stored like them.
EXTRA_ASSETS = {
    'assets/menu_reset_btn_1.png': ROOT / 'tools/apk-assets/menu_reset_btn_1.png',
    'assets/menu_reset_btn_2.png': ROOT / 'tools/apk-assets/menu_reset_btn_2.png',
}

# --tall: AppDelegate::applicationDidFinishLaunching at 0x102fa38, reached only
# when int(100 * h/w) > 176. The original cmp/b.eq chain (177 -> 320x568
# ExactFit, 185/200 -> 320x568 ShowAll, else -> generic) becomes
#   bl sharedOpenGLView; ldr x8,[x0]; ldr x8,[x8,#0x58]   (setDesignResolutionSize)
#   mov w9,#0x43a00000; fmov s0,w9; fmul s1,s0,s8          (320, 320*h/w; d8 survives the bl)
#   mov w1,#2; b 0x102faf8                                 (ShowAll; blr x8, setContentScaleFactor(2))
# 0x102fa58.. stays intact: 0x102ff18 still branches to 0x102fa5c.
TALL_PATCHES = [
    (0x102FA38,
     bytes.fromhex('1fc50271800200541fe50271600000541f210371e1030054b0a4ef97080040f9'),
     bytes.fromhex('b6a4ef97080040f9082d40f90974a8522001271e0108281e4100805229000014'),
     'AppDelegate design size for h/w > 1.76 -> 320 x 320*h/w, ShowAll'),
]


# Own-unit helper cards: tools/own_helper_cave.s, assembled at OWN_HELPER_CAVE.
# The cave is the zero block 0x186b4d8..0x186c4cc (erratum 843419 padding; the
# veneer at 0x186b4cc..0x186b4d7 before it stays live, the next function starts
# at OWN_HELPER_CAVE_END).
OWN_HELPER_CAVE = 0x186B4E0
OWN_HELPER_CAVE_END = 0x186C4CC
OWN_HELPER_CAVE_HEX = (
    'fd7bbea9fd030091f35301a9f30300aa9c41d0973b78d097a0010036f1f8ce97'
    '14c00b91e00314aa77000094000100b430ffcf97e10314aa36afce97800000b4'
    'f35341a9fd7bc2a8c0035fd6e00313aaf35341a9fd7bc2a84a25d017fd7bbea9'
    'fd030091f35301a9f30300aaf40301aae00301aa64000094c00000b5e00313aa'
    'e10314aaf35341a9fd7bc2a8b1e2cf1700008052f35341a9fd7bc2a8c0035fd6'
    'fd7bbda9fd030091f35301a9f51300f910ffcf97f30300aab222d097f403002a'
    '15008052bf02146bea000054e00313aae103152ac378cf9716000094b5060011'
    'f9ffff17e00313aaf51340f9f35341a9fd7bc3a8c0035fd6fd7bbfa9fd030091'
    'e00316aa0b000094e00316aafd7bc1a8c0035fd6fd7bbfa9fd030091e00315aa'
    '04000094e00315aafd7bc1a8c0035fd6800600b4fd7bbaa9fd030091f35301a9'
    'f55b02a9f30300aa60820b912e000094200500b4ffff04a9ff2f00f9221800d1'
    '01180091e0230191865bcf9739a3ce97e12301912fc7cf97f40300aae8234139'
    '68000036e02f40f9769ccf97540300b49f0213eb0003005468a641a96a1640f9'
    'e82703a9ea2300f97ffe01a97f1600f9751e40f976224939e00313aae10314aa'
    '3839cf97884e42b9684e02b96862403968000036601640f9629ccf97e82743a9'
    'ea2340f968a601a96a1600f9751e00f976220939f55b42a9f35341a9fd7bc6a8'
    'c0035fd6090040398900003721fd41d30004009103000014010440f9000840f9'
    '3f1800f169010054090040b9aa298b52aacaa9723f010a6bc100005409084079'
    '2a898a523f010a6b41000054c0035fd6000080d2c0035fd6'
)
OWN_HELPER_ENTRY = {
    'keep_own_helper': 0x0,
    'rotation_friend_ok': 0x5C,
    'refresh_picker_list': 0xA0,
    'refresh_lookup_x22': 0xF8,
    'refresh_lookup_x21': 0x114,
}


def arm64_bl(at: int, target: int) -> bytes:
    offset = target - at
    assert offset % 4 == 0 and -(1 << 27) <= offset < (1 << 27), (hex(at), hex(target))
    return (0x94000000 | ((offset >> 2) & 0x3FFFFFF)).to_bytes(4, 'little')


def own_helper_patches() -> list[tuple[int, bytes, bytes, str]]:
    cave = bytes.fromhex(OWN_HELPER_CAVE_HEX)
    assert OWN_HELPER_CAVE + len(cave) <= OWN_HELPER_CAVE_END, 'own-helper code overruns its cave'

    def call(at: int, entry: str) -> bytes:
        return arm64_bl(at, OWN_HELPER_CAVE + OWN_HELPER_ENTRY[entry])

    return [
        (OWN_HELPER_CAVE, bytes(len(cave)), cave, 'own-helper code (tools/own_helper_cave.s)'),
        (0x18C8CA4, bytes.fromhex('6fafce97'), call(0x18C8CA4, 'keep_own_helper'),
         'resetFriendUnit: auto-repeat keeps an own-card helper'),
        (0x18C9710, bytes.fromhex('486ace97'), call(0x18C9710, 'rotation_friend_ok'),
         'getRandReinforcement: friend auto-repeat rolls skip own cards'),
        (0x18C9F5C, bytes.fromhex('3568ce97'), call(0x18C9F5C, 'rotation_friend_ok'),
         'MissionResultScene::initConnect: unused-friend count skips own cards'),
        (0x18AC208, bytes.fromhex('f2fbce97'), call(0x18AC208, 'refresh_picker_list'),
         'ReinforcementSelectScene2::downloadFiles: own cards refreshed when the picker opens'),
        (0x12741B0, bytes.fromhex('e00316aa'), call(0x12741B0, 'refresh_lookup_x22'),
         'getObjectWithUserID(string, int): own card refreshed on lookup'),
        (0x1274044, bytes.fromhex('e00315aa'), call(0x1274044, 'refresh_lookup_x21'),
         'getObjectWithUserID(string): own card refreshed on lookup'),
    ]


def load_root_pem(path: Path) -> bytes:
    """The root CA PEM to pin; refuses anything that is not one CA certificate."""
    pem = path.read_bytes().replace(b'\r\n', b'\n')
    if not pem.endswith(b'\n'):
        pem += b'\n'
    if (pem.count(b'-----BEGIN CERTIFICATE-----') != 1
            or not pem.startswith(b'-----BEGIN CERTIFICATE-----\n')
            or not pem.endswith(b'-----END CERTIFICATE-----\n')):
        raise SystemExit(f'{path} must hold exactly one PEM certificate')
    if b'\0' in pem or not pem.isascii() or len(pem) >= PEM_SLOT_LEN:
        raise SystemExit(f'{path}: PEM must be ASCII and at most {PEM_SLOT_LEN - 1} bytes (got {len(pem)})')
    text = subprocess.run(['openssl', 'x509', '-noout', '-text'], input=pem, capture_output=True)
    if text.returncode != 0 or b'CA:TRUE' not in text.stdout or b'Certificate Sign' not in text.stdout:
        raise SystemExit(f'{path} is not a CA certificate with keyCertSign')
    return pem


def https_patches(data: bytes, pem: bytes) -> list[tuple[int, bytes, bytes, str]]:
    for offset, expected, purpose in HTTPS_GUARDS:
        if data[offset:offset + len(expected)] != expected:
            raise SystemExit(f'guard mismatch at {offset:#x} ({purpose})')
    slot = data[PEM_OFFSET:PEM_OFFSET + PEM_SLOT_LEN]
    if hashlib.sha256(slot).hexdigest() != PEM_SLOT_SHA256:
        raise SystemExit('pinned-root slot does not hold the original Go Daddy PEM')
    return [(PEM_OFFSET, slot, pem.ljust(PEM_SLOT_LEN, b'\0'), 'pinned root CA'), *PURCHASE_HTTPS_PATCHES]


def _host_slot(flag: str, host: str, offset: int, original: bytes, purpose: str) -> tuple[int, bytes, bytes, str]:
    encoded = host.encode('ascii')
    if not encoded or len(encoded) >= len(original):
        raise SystemExit(f'{flag} must be 1..{len(original) - 1} ASCII chars, got {host!r}')
    if any(c in host for c in '/ ') or '://' in host:
        raise SystemExit(f'{flag} is host[:port] only, without scheme or path')
    return offset, original, encoded.ljust(len(original), b'\0'), f'{purpose} -> {host}'


def host_patch(host: str) -> tuple[int, bytes, bytes, str]:
    return _host_slot('--host', host, DISCOVERY_HOST_OFFSET, DISCOVERY_HOST_ORIGINAL, '/dls host')


def res_host_patch(host: str) -> tuple[int, bytes, bytes, str]:
    return _host_slot('--res-host', host, REDOWNLOAD_HOST_OFFSET, REDOWNLOAD_HOST_ORIGINAL, 'MST re-download host')


def default_res_host(host: str) -> str:
    """The CDN for the public server; a local server serves /mst itself."""
    return 'cdn.example.com' if host == 'api.example.com' else host


def patch_lib(data: bytes, host: str, root_pem: bytes | None = None, tall: bool = False,
              res_host: str | None = None) -> bytes:
    digest = hashlib.sha256(data).hexdigest()
    if digest != LIB_SHA256:
        raise SystemExit(f'{LIB} sha256 {digest} is not the known v2.19.6 build; refusing to patch')
    extra = https_patches(data, root_pem) if root_pem else FIXED_PATCHES
    if tall:
        extra = [*extra, *TALL_PATCHES]
    extra = [*extra, *NEWS_PATCHES, *RESET_BUTTON_PATCHES, *RETURN_PATCHES, *VORTEX_SCENE_PATCHES, *NAG_PATCHES,
             *FUSION_PATCHES, *FUSION_SLOT_PATCHES, *NODL_PATCHES, *own_helper_patches()]
    out = bytearray(data)
    res = res_host_patch(res_host or default_res_host(host))
    for offset, original, new, purpose in [host_patch(host), res, *extra]:
        assert len(original) == len(new), purpose
        found = bytes(out[offset:offset + len(original)])
        if found != original:
            raise SystemExit(f'byte mismatch at {offset:#x} ({purpose}): {found.hex()} != {original.hex()}')
        out[offset:offset + len(new)] = new
        shown = f'{len(new)} bytes' if len(new) > 32 else f'{original.hex()} -> {new.hex()}'
        print(f'  patched {offset:#09x}  {shown}  {purpose}')
    if root_pem and out.index(b'\0', PEM_OFFSET) - PEM_OFFSET != len(root_pem):
        raise SystemExit('pinned root is not NUL-terminated where expected')
    return bytes(out)


def rebuild_zip(src: Path, dst: Path, patched_lib: bytes, extra_assets: dict) -> None:
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, 'w') as zout:
        for info in zin.infolist():
            name = info.filename
            if name in DROP_SIGNATURE or name.startswith(DROP_PREFIXES):
                continue
            data = patched_lib if name == LIB else zin.read(name)
            new_info = zipfile.ZipInfo(name, date_time=info.date_time)
            new_info.compress_type = info.compress_type
            new_info.external_attr = info.external_attr
            zout.writestr(new_info, data)
        clash = set(extra_assets) & set(zin.namelist())
        if clash:
            raise SystemExit(f'{src} already holds {sorted(clash)}; refusing to overwrite')
        for name, path in extra_assets.items():
            new_info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            new_info.compress_type = zipfile.ZIP_STORED
            new_info.external_attr = 0o600 << 16
            zout.writestr(new_info, path.read_bytes())


def read_props(path: Path) -> dict:
    if not path.exists():
        raise SystemExit(f'missing {path}; create a keystore first (see README)')
    pairs = (line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)
    return {k.strip(): v.strip() for k, v in pairs}


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f'{Path(cmd[0]).name} failed:\n{result.stdout}{result.stderr}')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--host', default='127.0.0.1:9960', help='host[:port] the client sends /dls to')
    ap.add_argument('--https', action='store_true', help='keep https and pin --root-ca instead of downgrading to http')
    ap.add_argument('--root-ca', type=Path, help='root CA PEM to pin (required with --https)')
    ap.add_argument('--tall', action='store_true', help='fill tall (> 16:9) screens instead of letterboxing')
    ap.add_argument('--res-host', help='host[:port] for the in-game MST re-download '
                    '(default: cdn.example.com for api.example.com, else --host)')
    args = ap.parse_args()
    args.res_host = args.res_host or default_res_host(args.host)
    host_patch(args.host)  # validate before doing any work
    res_host_patch(args.res_host)
    if args.https and not args.root_ca:
        raise SystemExit('--https needs --root-ca')
    root_pem = load_root_pem(args.root_ca) if args.https else None

    props = read_props(KEYSTORE_PROPS)
    with zipfile.ZipFile(args.apk) as z:
        lib = z.read(LIB)
    print(f'Patching {LIB} for host {args.host} ({"https, pinned root" if root_pem else "plain http"}):')
    patched = patch_lib(lib, args.host, root_pem, args.tall, args.res_host)

    with tempfile.TemporaryDirectory() as tmp:
        unaligned = Path(tmp) / 'unaligned.apk'
        aligned = Path(tmp) / 'aligned.apk'
        rebuild_zip(args.apk, unaligned, patched, {**EXTRA_ASSETS, **FUSION_SLOT_ASSETS})
        run([str(BUILD_TOOLS / 'zipalign'), '-f', '-p', '4', str(unaligned), str(aligned)])
        args.out.parent.mkdir(parents=True, exist_ok=True)
        signed = Path(tmp) / 'signed.apk'
        # Password goes through the environment, never the command line.
        result = subprocess.run(
            [str(BUILD_TOOLS / 'apksigner'), 'sign',
             '--ks', str(ROOT / props['storeFile']), '--ks-key-alias', props['alias'],
             '--ks-pass', 'env:BF_KS_PASS', '--key-pass', 'env:BF_KS_PASS',
             '--out', str(signed), str(aligned)],
            capture_output=True, text=True, env={'BF_KS_PASS': props['password'], 'PATH': '/usr/bin:/bin'})
        if result.returncode != 0:
            raise SystemExit(f'apksigner failed:\n{result.stdout}{result.stderr}')
        run([str(BUILD_TOOLS / 'apksigner'), 'verify', str(signed)])
        shutil.copyfile(signed, args.out)

    print(f'Wrote {args.out} ({args.out.stat().st_size / 1e6:.1f} MB), signed and verified')
    return 0


if __name__ == '__main__':
    sys.exit(main())
