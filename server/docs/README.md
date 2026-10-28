# Server documentation

Developer documentation for the game server in `server/`. For the whole project, start at the
[repository README](../../README.md) and the [docs index](../../docs/README.md).

## Current

| Document | What it covers |
|---|---|
| [DEVELOPMENT.md](DEVELOPMENT.md) | Building from a fresh checkout (Windows, Linux presets), running and debugging, troubleshooting |
| [BF_OFFLINE_SERVER_HANDBOOK.md](BF_OFFLINE_SERVER_HANDBOOK.md) | Gameplay development handbook: evidence labels, conventions, how to work on handlers |
| [GAMEPLAY_WORK_QUEUE.md](GAMEPLAY_WORK_QUEUE.md) | Implementation queue for gameplay fidelity work |
| [OFFLINE_CONTENT_LOADING_PLAN.md](OFFLINE_CONTENT_LOADING_PLAN.md) | Plan for direct content loading through the Windows offline proxy |
| [../RELEASE.md](../RELEASE.md) | Building the portable Windows release ZIP |

## Feature specifications

One document per feature under [features/](features/). Each names the handlers, schemas,
migrations and wire tests involved, and marks which claims are observed in the client, inferred,
or authored by this server.

| Area | Documents |
|---|---|
| Accounts | [ACCOUNT_RESET.md](features/ACCOUNT_RESET.md) |
| Overlay files (`balance_root`) | [BALANCE_OVERLAYS.md](features/BALANCE_OVERLAYS.md), [BATTLE_BUILDER.md](features/BATTLE_BUILDER.md), [NEWS_BOARD.md](features/NEWS_BOARD.md) |
| Summons | [FAIR_SUMMON.md](features/FAIR_SUMMON.md), [SUMMON_ROTATION.md](features/SUMMON_ROTATION.md), [EVENT_SUMMON.md](features/EVENT_SUMMON.md), [COLLAB_SUMMON.md](features/COLLAB_SUMMON.md) |
| Social | [FRIENDS_SPEC.md](features/FRIENDS_SPEC.md), [GUILDS_SOCIAL_2026-10-17.md](features/GUILDS_SOCIAL_2026-10-17.md), [GUILD_INVITE_2026-09-26.md](features/GUILD_INVITE_2026-09-26.md) |
| Raids | [RAIDS_SPEC.md](features/RAIDS_SPEC.md), [RAIDS_ROUND2_CORE.md](features/RAIDS_ROUND2_CORE.md), [RAIDS_ROUND2_MULTIPLAYER.md](features/RAIDS_ROUND2_MULTIPLAYER.md), [RAIDS_ROUND2_CAMP_ITEMS.md](features/RAIDS_ROUND2_CAMP_ITEMS.md) |
| PvP and rankings | [ARENA_SPEC.md](features/ARENA_SPEC.md), [COLOSSEUM.md](features/COLOSSEUM.md), [FRONTIER_HUNTER.md](features/FRONTIER_HUNTER.md) |
| Research Lab | [RESEARCH_LAB.md](features/RESEARCH_LAB.md), [TRIAL_003.md](features/TRIAL_003.md), [TRIAL_X1.md](features/TRIAL_X1.md), [CREATION_GOD.md](features/CREATION_GOD.md) |
| Other systems | [DAILY_CHALLENGES.md](features/DAILY_CHALLENGES.md), [EXCHANGE_UI_2026-09-26.md](features/EXCHANGE_UI_2026-09-26.md), [FEATURE_VISITS.md](features/FEATURE_VISITS.md), [SERVER_STUB_AUDIT.md](features/SERVER_STUB_AUDIT.md) |

