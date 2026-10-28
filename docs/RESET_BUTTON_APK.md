# Reset Account button (APK)

Menu > Credits becomes **Reset Account**. It is a shortcut into the in-game reset that is already
live (ticket #53, server `docs/features/ACCOUNT_RESET.md`): the server stays as it is.

## What a player sees

1. Menu > **Reset Account**: the game asks the server for a reset code (it sends Friend Search
   `000000` for the player). Nothing is reset yet.
2. A popup names the account (name, level, ID), says everything is deleted and gives a 6-digit code
   that works for 10 minutes. **OK** opens Social > Add Friend > Friend Search.
3. Only searching that code there confirms: "Reset confirmed. Close the game now." The operator's
   reset job runs the reset (backup first), and the next start begins at the tutorial.

A stray tap only shows a code. Backing out, waiting 10 minutes or typing anything else resets
nothing; a wrong code says so.

## What players must install

- The new APK, installed **over** the current one (same package, same signing key, same version):
  no uninstall, no data loss, the account stays logged in.
- Nobody has to: older APKs keep their Credits button (still the empty page) and can reset the old
  way, by searching `000000` in Friend Search. The server cannot tell the two apart.

## Build

```bash
python3 -I tools/patch_apk.py --apk "$HOME/Downloads/Brave Frontier 2.19.6.0.apk" \
    --out build/bfrevive-reset-button.apk --host api.example.com --https --root-ca signing/bf-root-ca.pem
```

These are the flags of the APK in #apk (`build/bfrevived.apk`). Against it, the new build changes
three spots in `libgame.so` and adds two images, nothing else:

| Where | Change |
|---|---|
| `0x1751498` (11 instructions) | `MenuTopScene::touchBegan`, tag 10009 (Credits): `UserState::setFriendID("000000")`, then `changeSceneWithSceneID(614)`, which is what the Friend Search button does. Scene 614 sends FriendSearch and returns to the search box (613) after the popup. |
| `0x2353443`, `0x2353458` | label names `menu_credit_btn{1,2}.png` → `menu_reset_btn_{1,2}.png` |
| `assets/menu_reset_btn_{1,2}.png` | the new label (normal / pressed), drawn by `tools/make_reset_label.py` |

The client loads a downloaded image first and the APK's bundled one otherwise, so the CDN does not
change. The patch is in every build `tools/patch_apk.py` makes (both modes).

## Tested (2026-10-19, emulator-5558)

Plain-HTTP build (`--host 127.0.0.1:9964 --tall`, same patch sites), server `feat/ingame-reset`
6a89570 (what production runs) on a copy of a test save, FrTestA Lv 217:

- Old APK: Menu shows Credits; the tap opens the empty Credits page and sends nothing.
- New APK: Menu shows Reset Account in the same style as its neighbours; one tap sends FriendSearch
  `000000`, the popup names FrTestA with a code, nothing is queued; OK lands in Friend Search;
  searching the code gives "Reset confirmed" and one queued row; one pass of the reset job
  resets the account (`done`, snapshot taken, no `user_info` row left).
