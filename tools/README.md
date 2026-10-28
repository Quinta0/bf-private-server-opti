# Tools

Scripts for patching the client, managing the asset CDN, building images and testing a running
server. Each script documents its own options in its header or docstring; read it before running
anything that writes.

## Conventions

- Python scripts are run from the repo root with `python3 -I tools/<script>.py` (isolated mode).
  Most need `pycryptodome`; the image tools need Pillow.
- Wire tests talk to a **running** server the way the client does and usually take
  `--url http://127.0.0.1:<port>` and `--src <server checkout>` (the checkout whose `deploy/` holds
  that server's save and tables). Many create, edit or delete players. Run them only against a
  disposable server and a copy of a save, never against a save with real players.
- None of these tools ship game data. Paths to the client, the game archive or a save always point
  at copies you obtained separately (see the [root README](../README.md)).

## Client (APK)

| Script | Purpose |
|---|---|
| [`patch_apk.py`](patch_apk.py) | Patch the v2.19.6 APK to talk to your own server: discovery host (`--host`), plain HTTP or pinned private root (`--https --root-ca`), optional tall-screen mode (`--tall`), own-unit helper patches; checks every original byte first and re-signs. |
| [`test_patch_apk.py`](test_patch_apk.py) | Offline check of `patch_apk.py` against your own `libgame.so`: the cave assembles to the embedded bytes and every hook and call target lands where it should (needs Xcode command line tools). |
| [`own_helper_cave.s`](own_helper_cave.s) | ARM64 source of the own-unit helper code that `patch_apk.py` places in `libgame.so` padding. |
| [`make_reset_label.py`](make_reset_label.py) | Draw the "Reset Account" Menu button label that replaces Credits, matched to the game's button style (`--check` compares against the original). |
| [`device_run.sh`](device_run.sh) | Install the patched APK on a device, `adb reverse` the server port, optionally clear app data (`--fresh`) and launch; expects `adb` at `~/Library/Android/sdk/platform-tools`. |
| [`mint_leaf_cert.sh`](mint_leaf_cert.sh) | Mint a TLS server certificate for one host, signed by your private root in `signing/` (gitignored), as the HTTPS client build requires. |

## CDN and assets

| Script | Purpose |
|---|---|
| [`upload_assets.py`](upload_assets.py) | Upload your game archive (`content/` + `mst/`) to R2 through the temporary `bf-asset-uploader` Worker, resumably, then upload the by-name index. |
| [`build_cdn_static.py`](build_cdn_static.py) | Build `cdn-worker/static/` (Workers Static Assets) from verified archive files; `--deploy` uploads a new `bf-cdn` version, byte-checks it at 0% traffic, then promotes it. |
| [`cdn_compare.py`](cdn_compare.py) | Fetch the same paths from two CDN base URLs and compare status, body SHA-256 and placeholder marker. |
| [`cdn_misses.py`](cdn_misses.py) | Count CDN misses (404s, placeholders, 503s) per path from saved `wrangler tail --format json` output. |
| [`cdn_upload_missing.sh`](cdn_upload_missing.sh) | Upload a verified manifest of substitute assets plus `_meta/aliases.json` and the name index to R2; dry run unless `--apply`, never overwrites archive keys. |

See [cdn-worker/README.md](../cdn-worker/README.md) for the Workers these talk to.

## Build and deploy

| Script | Purpose |
|---|---|
| [`build_prod_image.sh`](build_prod_image.sh) | Build the linux/amd64 production image from a clean commit and load it on your host over SSH as `bf-server:next` (`VPS=<user>@<host>`, `SRC`, `TAG`, `JOBS`, `UPLOAD=0`). See [prod/README.md](../prod/README.md). |

## Server checks and wire tests

| Script | Checks |
|---|---|
| [`smoke_test.py`](smoke_test.py) | Content and table downloads, the by-name content fallback and an encrypted request round trip. Start here. |
| [`test_accounts.py`](test_accounts.py) | Per-device guest accounts and session tokens: stale, wrong or missing tokens are rejected. |
| [`test_transfer.py`](test_transfer.py) | Account transfer codes: issue, check, transfer, replay, expiry and rate limiting, against replies and the save. |
| [`test_fair_summon.py`](test_fair_summon.py) | Only offered gates can be pulled, at their original prices; test gates are gone. Also the shared harness for several tests below. |
| [`test_house_gates.py`](test_house_gates.py) | The house summon gates: listing, price per pull and pools. |
| [`test_free_energy.py`](test_free_energy.py) | Free energy refills: price announced as 0, refill to max, over-max refused. |
| [`test_starter_selector.py`](test_starter_selector.py) | The Summoner Rank 50 selector reaches the player within a session, once. |
| [`test_item_bar.py`](test_item_bar.py) | MissionEnd re-sends the battle-item bar after spending the battle's uses. |
| [`test_key_receive_all.py`](test_key_receive_all.py) | "Receive All" for dungeon keys claims every key claimable today. |
| [`test_random_drop_types.py`](test_random_drop_types.py) | Quest unit drops roll random types like a summon. |
| [`test_frontier_hunter.py`](test_frontier_hunter.py) | Frontier Hunter scoring, rewards, caps and ranking. |
| [`test_news_board.py`](test_news_board.py) | The INFO news page and its NEW badge, driven by `balance_root/news/posts.json`. |
| [`test_vortex_tiles.py`](test_vortex_tiles.py) | Every Vortex tile the server permits leads to a playable mission. |
| [`test_captured_missions.py`](test_captured_missions.py) | Missions added from captured battles start, serve their waves and finish. |

## Related documents

- [Root README](../README.md)
- [server/README.MD](../server/README.MD): server-side wire tests in `server/scripts/`
- [docker/README.md](../docker/README.md): running a local server to test against
- [docs/GETTING_STARTED.md](../docs/GETTING_STARTED.md)
- [CONTRIBUTING.md](../CONTRIBUTING.md)
