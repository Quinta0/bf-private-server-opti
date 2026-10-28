# Production container stack

Files for running the game server on a Linux host with Docker Compose.

| File | Purpose |
|---|---|
| [`Dockerfile`](Dockerfile) | Multi-stage image `bf-server:prod` (linux/amd64): builds `gimuserverw` in Release with GCC 14, CMake 4, Rust and a pinned vcpkg, then copies it into a minimal Ubuntu 24.04 runtime that runs as uid 10001. |
| [`compose.yaml`](compose.yaml) | Services `game` (the live server, read-only root, save in volume `bf-data`), `game-test` (an optional test server with its own save, CPU and memory caps) and `caddy` (the only service publishing ports, 80/443). |
| [`config.json`](config.json) | Server config for `game`: listener, Drogon settings, SQLite at `/data/gme.sqlite`, `public_host` / `resource_host`, `balance_root`. |
| [`config.test.json`](config.test.json) | The same for `game-test`, with its own host name, save file and balance folder. |

> The image needs game data that is not in this repository: `server/deploy/mst/`, which you
> obtain separately, and `server/deploy/archive/`, the `archive/` folder of
> [bf-private-server-data](https://github.com/joppe2001/bf-private-server-data). See the
> [root README](../README.md#game-data).

## Requirements

- Docker with BuildKit (Compose v2).
- A build context laid out like the repo root: `server/` (including the vendored `packet-generator`
  directory and the game data folders above), `prod/`, and a `.vcpkg-downloads/` directory
  (may be empty; it seeds vcpkg's download cache, for example with a source tarball whose upstream
  URL no longer resolves).
- You provide two things that are not in the repo: a `Caddyfile` and a `tls/` folder next to
  `compose.yaml` (mounted into the `caddy` service).

## Configure

Edit `config.json` (and `config.test.json` if you run the test server):

- `plugins[0].config.server.public_host`: the host the client is told to use, for example
  `api.example.com`.
- `resource_host`: where clients download assets, for example your CDN Worker's hostname
  ([cdn-worker/README.md](../cdn-worker/README.md)).
- `balance_root`: keep `./balance` to load optional overlay files you author yourself
  ([BALANCE_OVERLAYS.md](../server/docs/features/BALANCE_OVERLAYS.md)), remove it to
  turn that off.

`compose.yaml` bind-mounts `/opt/bf/prod/balance` (and `/opt/bf/test/balance` for `game-test`)
read-only. Create those folders on your host or change the paths. The image sets `TZ=Asia/Tokyo`,
which the server's weekday-based schedules use; change it in the `Dockerfile` if you need another
zone.

## Build and run

```bash
# from the repo root
mkdir -p .vcpkg-downloads
docker build --build-arg JOBS=$(nproc) -f prod/Dockerfile -t bf-server:prod .

cd prod
docker compose up -d game caddy
docker compose logs -f game
```

Object files are kept in a BuildKit cache mount, so later builds only recompile what changed.

To build the amd64 image on another machine (for example an Apple Silicon workstation with
Rosetta enabled in Docker Desktop) and ship it to your host over SSH, use
[`tools/build_prod_image.sh`](../tools/build_prod_image.sh): it refuses uncommitted changes, checks
that your local `prod/config.json` matches the one on the host, builds `bf-server:next` and loads it
there (`VPS=<user>@<your-host>`, `UPLOAD=0` to build only). Swapping the new tag into service is a
separate, deliberate step.

## Data and backups

The save database lives in the named volume `bf-data` (`/data/gme.sqlite` inside the container).
Never write to it from the host while the server runs (SQLite WAL on a bind mount can split); work
through `docker exec` instead, for example with SQLite's online `.backup` command. Gzipped snapshots
can be pushed to the `bf-backup` Worker ([cdn-worker/README.md](../cdn-worker/README.md)).

## Related documents

- [Root README](../README.md)
- [server/README.MD](../server/README.MD): building the server itself
- [docker/README.md](../docker/README.md): the local development build
- [docs/DEPLOYMENT.md](../docs/DEPLOYMENT.md): deploying the whole stack
- [SECURITY.md](../SECURITY.md)
