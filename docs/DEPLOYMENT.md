# Deployment

How to run the server online. This describes the layout the repository's files are written for:
one Linux host running Docker Compose for the game API, and Cloudflare (Workers and R2) for game
files and backups. Every host name, address and account id below is a placeholder; substitute
your own. Read [ARCHITECTURE.md](ARCHITECTURE.md) first for how the pieces fit.

> [!NOTE]
> Running a public server means hosting game data you obtained separately. Make sure you are
> entitled to do so where you operate. This project distributes no game assets.

- [Topology](#topology)
- [Production image](#production-image)
- [Compose stack](#compose-stack)
- [Game files on the CDN](#game-files-on-the-cdn)
- [Backups and restore](#backups-and-restore)
- [HTTPS client build](#https-client-build)
- [Accounts and transfer codes](#accounts-and-transfer-codes)
- [Test server](#test-server)
- [Windows portable release](#windows-portable-release)

## Topology

| Piece | Placeholder | Where |
|---|---|---|
| Game API | `api.example.com` | Host `203.0.113.11`: Caddy on ports 80/443 in front of the game container |
| Game files | `cdn.example.com` | Cloudflare Worker `bf-cdn` (`cdn-worker/`) serving the R2 bucket `bf-assets` |
| Table publishes | `bf-mst-publish` Worker | Add-only writes of client table parts into `bf-assets` |
| Backups | `bf-backup` Worker | Write-only sink into the R2 bucket `bf-backups` |
| Test server | `test.example.com` | Second game container on the same host, behind the same Caddy |

On the host, the files are laid out as `/opt/bf/server` (source), `/opt/bf/prod` (`compose.yaml`,
`Caddyfile`, `config.json`, `tls/`, `balance/`) and the save database lives in the Docker volume
`prod_bf-data`. Use SSH keys for an unprivileged deploy user and disable root and password logins.

## Production image

`prod/Dockerfile` is a two-stage build: Ubuntu 24.04 with GCC 14, CMake 4, Rust and a pinned
vcpkg builds a stripped Release binary (`x64-linux-release`, developer console off), and a slim
runtime stage runs it as uid 10001 with `/data` as its only volume.

The build context is a folder holding `server/`, `prod/` and `.vcpkg-downloads/` (create it empty
if you have nothing to seed; it is where vcpkg source tarballs that no longer download can be
placed). The image copies `server/deploy/mst/`, `server/deploy/system/` and
`server/deploy/archive/` into `/app`, so the game data must be present in the context when you
build it:

```bash
docker build --build-arg JOBS=$(nproc) -f prod/Dockerfile -t bf-server:prod .
```

Object files are kept in a BuildKit cache mount, so a source change only recompiles what it
touches.

`tools/build_prod_image.sh` wraps this for building on a separate machine: it refuses to build a
checkout with uncommitted changes, assembles the context, builds `linux/amd64` with
`docker buildx` (on Apple Silicon this needs Docker Desktop's Rosetta emulation), labels the image
with its commit, checks that `prod/config.json` matches the host's copy, and uploads the result as
`bf-server:next` over SSH. Swapping `next` into production stays a separate, deliberate step.

```bash
VPS=deploy@203.0.113.11 tools/build_prod_image.sh   # build server/ and upload
UPLOAD=0 tools/build_prod_image.sh                  # build only
SRC=path/to/worktree tools/build_prod_image.sh      # build another checkout
```

Building on a separate machine keeps compiler load off the host that serves players.

## Compose stack

`prod/compose.yaml` defines three services:

- **`game`**: `bf-server:prod`, read-only root filesystem with a tmpfs `/tmp`, the save volume at
  `/data`, `config.json` mounted read-only, and the optional overlay folder `balance/` (`balance_root`) mounted
  read-only at `/app/balance` (a folder mount, so atomic renames show up inside). It is not published on any
  host port; only Caddy reaches it.
- **`game-test`**: an optional test server (see [Test server](#test-server)), with its
  own save volume and overlay folder, all capabilities dropped, and CPU, memory and log caps.
- **`caddy`**: `caddy:2.10` on ports 80 and 443, reading `./Caddyfile` and the certificates in
  `./tls`. The Caddyfile is not tracked in this repository; write one that proxies your API host
  names to `game:9960` (and the test host to `game-test:9960`).

`prod/config.json` is the server configuration baked into the image. Set `public_host` to your API
host and `resource_host` to your CDN host; these are the names `/dls` gives the client.

Deploy a new image:

```bash
cd /opt/bf/prod && docker compose up -d game
```

`up -d game` and `restart game` never touch `game-test`. Recreating a container deletes its old
logs, so save them first if you need them.

## Game files on the CDN

1. Create the R2 bucket `bf-assets` and deploy the temporary uploader Worker
   (`cdn-worker/uploader/`, secret `UPLOAD_TOKEN`).
2. Upload your archive's `content/` and `mst/` with `tools/upload_assets.py`. It resumes an
   interrupted run and finally writes `_meta/name-index.json`, the by-name fallback the CDN Worker
   uses. The upload secret is read from `cdn-worker/.upload-token` (git-ignored).

   ```bash
   python3 -I tools/upload_assets.py --assets /path/to/assets \
       --endpoint https://bf-asset-uploader.<account>.workers.dev
   ```

3. Delete the uploader Worker when done (`npx wrangler delete --name bf-asset-uploader`).
4. Optionally build the static bundle with `tools/build_cdn_static.py`: files in
   `cdn-worker/static/` are answered by Cloudflare without running the Worker.
5. Deploy `bf-cdn` from `cdn-worker/` with `npx wrangler deploy`, after setting its route in
   `wrangler.jsonc` to your CDN host.

Keep the CDN reachable over **plain HTTP**: the client's asset loader is hard-coded to `http://`.
`tools/cdn_misses.py` counts the misses (404s and placeholders) in saved `wrangler tail` output,
and `tools/cdn_compare.py` compares two CDN deployments byte for byte, for example a new Worker
version's preview URL against the live one, before it takes traffic.

The `bf-mst-publish` Worker (`cdn-worker/mst-publish/`) needs the secret `UPLOAD_TOKEN` and the
variable `ALLOWED_IPS` set to the game host's addresses; with `ALLOWED_IPS` unset it refuses
every request.

## Backups and restore

Schedule your own snapshot job on the host. A safe pattern: copy the live database with SQLite's
online backup API from inside a container that runs as the game's uid 10001 (never read or write
the save from the host while the server runs), check the copy with `PRAGMA integrity_check`, keep
a number of gzip copies locally, and send the new one to `bf-backups/db/` through the write-only
`bf-backup` Worker (`cdn-worker/backup/`). Keep the Worker's URL and token in a root-only file
(mode 0600), never in the repository. Snapshots over Cloudflare's 100 MB request limit go up as an
R2 multipart upload. A leaked token cannot read, list or delete old backups.

Restore:

```bash
# from cdn-worker/
npx wrangler r2 object get bf-backups/db/<name>.sqlite.gz --remote --file restore.sqlite.gz
gunzip restore.sqlite.gz && sqlite3 restore.sqlite 'pragma integrity_check'
# on the host: docker compose stop game, copy the file into the prod_bf-data volume as
# gme.sqlite (owner 10001, mode 600, remove gme.sqlite-wal and gme.sqlite-shm),
# then docker compose start game
```

Do not write to a running save from the host: write through `docker exec` instead.

## HTTPS client build

The client can keep HTTPS for the game API by pinning your own private root CA instead of the
original one:

```bash
python3 -I tools/patch_apk.py --apk /path/to/your-client.apk \
    --out build/BraveFrontier-https.apk --host api.example.com \
    --https --root-ca signing/bf-root-ca.pem
```

- Keep the root key (`signing/bf-root-ca.key`) offline and backed up. A new root means a new APK.
- `tools/mint_leaf_cert.sh <host>` issues a server certificate for one host from that root into
  `signing/` (both files 0600, refuses to overwrite). Install it in the host's `tls/` folder.
  The certificate profile is described in `re/notes/https_transfer_re.json`.
- Keep TLS 1.2 enabled: the client's OpenSSL 1.1.0 has no TLS 1.3.
- Keep the CDN host on plain HTTP (see above).
- If the API host is behind Cloudflare DNS, keep its record **DNS only**. Cloudflare's proxy
  presents its own certificate, which the pinned client rejects.
- Plain-HTTP API access is only needed for older plain-HTTP builds; remove it from the Caddyfile
  once every player runs an HTTPS build.

`signing/` is git-ignored; never commit keys or certificates.

## Accounts and transfer codes

- Guest login keys an account to a hash of the device's Android ID and issues a new random session
  token on each login; every game request must carry it.
- Transfer codes are 10 digits plus a 4 to 12 character password, stored only as hashes (PBKDF2
  for the password), single-use, with a configurable expiry (`transfer_code_ttl_days`, default
  30). Ten wrong passwords lock a code for a day. The wire test is `tools/test_transfer.py`.

## Test server

The optional `game-test` service runs the same image with its own config
(`prod/config.test.json`), its own overlay folder (`/opt/bf/test/balance`) and its own save
volume, which you can fill with a copy of a recent live backup. Create the config file and the
folder before starting it; the service refuses to start without them. Point its Caddy host
(`test.example.com`) at `game-test:9960` and connect with a separately patched test client. Give
the test server's client table versions a large offset above live's, so their parts never
collide on the CDN.

## Windows portable release

For single-player use on Windows, `server/rebuild_release.bat` builds a standalone server ZIP that
players start with `Start Server.bat`, without any development toolchain. See
[server/RELEASE.md](../server/RELEASE.md).
