# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Until 1.0.0, minor versions may contain breaking changes to configuration,
database migrations or the overlay file formats.

## [Unreleased]

### Added

Initial public release of the source code, planned as **0.1.0**. The repository
contains code only; game data and art must be obtained separately.

#### Game server (`server/`)

- C++ game server for the Brave Frontier v2.19.6 client, forked from
  Seltraeh/server (itself a fork of decompfrontier/server): Drogon HTTP
  server, SQLite save database with migrations, request/response schemas
  generated at build time by the vendored Rust `packet-generator`
  (`server/packet-generator`).
- Linux/GCC portability fixes on top of the upstream Windows/MSVC build, and a
  Docker build toolchain (`docker/Dockerfile.build`, `docker/build.sh`,
  `docker/run.sh`).
- Android support: `/dls` boot discovery that names configurable public and
  resource hosts, extra account and maintenance routes, and lenient request
  parsing for the Android login block.
- Real per-player accounts instead of a single shared save, with account
  transfer and an in-game account reset flow.
- Summoning that follows the original Global rules, a weekly featured-banner
  rotation (which also carries the Collab gates), event gates and unit
  selector tickets.
- Multiplayer and social systems between real players: friends and helpers,
  guilds (invites, ranks, member management, join requests), Classic Arena and
  Colosseum.
- Frontier Hunter scoring, Hunter Rank and claimable rewards; Daily
  Challenges paying Merit Points; Summoners' Research Lab trials, including
  The Creation God.
- Classic Raid handlers: entry screens, solo runs, multiplayer rooms and lobby,
  camp rest, raid items, crafting and the raid shop (see
  `server/docs/features/RAIDS_*.md` for what is verified on a device).
- An in-game news page and persistent feature-visit tracking.
- Optional boot-applied overlay and config files read from a `balance_root`
  directory, so balance changes, hand-built battles and news posts ship without
  a new server image (`server/docs/features/BALANCE_OVERLAYS.md`).
- A portable Windows release package (`server/RELEASE.md`).

#### Cloudflare Workers (`cdn-worker/`)

- CDN Worker that serves the game's download archive.
- Write-only backup sink for SQLite snapshots, including R2 multipart upload
  for large files.
- Add-only sink for MST table parts, restricted by token and by
  source address.
- A temporary bulk uploader for the initial asset load.
- `node --test` suite: `cd cdn-worker && npm ci && npm test`.

#### Website (`site/`)

- Static landing page with strict security headers, a `www` redirect Worker,
  and a unit finder page backed by a Worker that proxies the game server's
  unit data.

#### Tools and research (`tools/`, `re/`, `docs/`)

- `tools/patch_apk.py`: patches a user-supplied v2.19.6 APK to talk to your own
  server. It verifies every original byte before writing, and the
  `tools/test_patch_apk.py` check rebuilds and verifies the own-unit helper
  code cave (`tools/own_helper_cave.s`).
- Device and emulator helpers, a smoke test, and wire-level test scripts for
  individual game features.
- CDN maintenance scripts.
- Reverse-engineering notes for the client's boot flow, HTTPS transport and
  request tables (`re/notes/`), and a project roadmap (`docs/ROADMAP.md`).

#### Deployment (`prod/`)

- Production Dockerfile and Docker Compose configuration with example
  configuration files. All hosts and addresses are placeholders.

[Unreleased]: https://github.com/joppe2001/bf-private-server/commits/main
