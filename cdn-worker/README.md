# Cloudflare Workers (CDN, uploads, backups)

Four small [Cloudflare Workers](https://developers.cloudflare.com/workers/) that serve game files
to clients and accept uploads from your own tooling. They all use R2 for storage and are deployed
with `wrangler` to **your own Cloudflare account**.

| Worker | Config | Purpose |
|---|---|---|
| `bf-cdn` | [`wrangler.jsonc`](wrangler.jsonc), [`src/`](src) | Serves `/content/*` and `/mst/*` from the R2 bucket `bf-assets` to game clients over plain HTTP. |
| `bf-mst-publish` | [`mst-publish/wrangler.jsonc`](mst-publish/wrangler.jsonc) | Add-only upload endpoint for new client table parts (see [BALANCE_OVERLAYS.md](../server/docs/features/BALANCE_OVERLAYS.md) for the manifests that announce them). |
| `bf-backup` | [`backup/wrangler.jsonc`](backup/wrangler.jsonc) | Write-only sink for gzipped database snapshots in the R2 bucket `bf-backups`. |
| `bf-asset-uploader` | [`uploader/wrangler.jsonc`](uploader/wrangler.jsonc) | Temporary endpoint for the initial bulk upload of your game archive into R2. Delete it after use. |

> No game files are included here. You obtain the game data separately and upload it into your
> own bucket (see the [root README](../README.md)).

## Requirements

- Node.js 22 or newer (required by the pinned `wrangler` and `miniflare`; the tests use `node --test`).
- A Cloudflare account with R2 enabled, and `wrangler` logged in (`npx wrangler login`).
- Python 3 with Pillow is optional: when present, the tests also decode placeholder images with it.

```bash
cd cdn-worker
npm ci
```

## bf-cdn: the asset CDN

Lookup order for a requested key (`src/index.js`):

1. the exact R2 key;
2. `_meta/aliases.json` in R2 (hand-maintained full-key or basename aliases);
3. `_meta/name-index.json` in R2 (basename to preferred key, written by `tools/upload_assets.py`),
   because the client asks for some files under a different folder than the archive stores them in;
4. a generated placeholder, for `content/` `.png` and `.jpg` only, except the names listed in
   `src/no-placeholder.js`;
5. 404.

Because the client keeps whatever it downloads, a placeholder is served only when the miss is
certain. If R2 or one of the `_meta` files cannot be read, the answer is a `503` with `no-store`
and the client retries later. Every placeholder, 404 or 503 logs one `{"bf_miss": ...}` object to
Workers Logs; `tools/cdn_misses.py` counts them from saved `wrangler tail --format json` output.

Responses carry `Cache-Control: ... no-transform` so intermediary proxies do not recompress
plain-HTTP downloads. Plain HTTP must keep working: the game's asset loader is hard-coded to
`http://`.

`/diag?f=<part>,<part>` is a read-only download check for players: the page fetches the named
`mst/` parts over plain HTTP like the game does and compares size and CRC-32 with R2.
`/diag/echo` shows the request headers that reached Cloudflare, with IPs and subscriber
identifiers hidden.

### Static assets

`wrangler.jsonc` also declares `"assets": { "directory": "./static" }`. Files in `static/` are
answered by Cloudflare without running the Worker (and without per-request Worker billing). The
directory is not in git. `wrangler deploy` needs it to exist, so either create it empty
(`mkdir -p static`, everything is then served from R2 by the Worker) or build it with
`tools/build_cdn_static.py`, which adds only files whose bytes it has verified against what the
CDN serves from R2. A file in `static/` wins over R2, so replacing an existing R2 key needs a
rebuild of `static/` and a redeploy.

## Configure for your account

Every config uses placeholders. Before deploying, edit:

| File | Setting |
|---|---|
| `wrangler.jsonc` | `routes[0].pattern`: your CDN hostname instead of `cdn.example.com` (a Worker custom domain creates the DNS record and certificate). R2 bucket name if not `bf-assets`. |
| `mst-publish/wrangler.jsonc` | `vars.ALLOWED_IPS`: the public IPv4/IPv6 of the host that publishes (comma separated). With it unset, every request is refused. |
| `backup/wrangler.jsonc` | R2 bucket name if not `bf-backups`. |
| `uploader/wrangler.jsonc` | R2 bucket name if not `bf-assets`. |

Create the buckets once:

```bash
npx wrangler r2 bucket create bf-assets
npx wrangler r2 bucket create bf-backups
```

Secrets are set with `wrangler secret put` and never committed. Local token files such as
`.upload-token` and `.mst-publish-token` are gitignored.

## Deploy

```bash
cd cdn-worker

# CDN
mkdir -p static                      # or build it with tools/build_cdn_static.py
npx wrangler deploy

# Table-part publisher (for your own table-part uploads)
openssl rand -hex 32 | tr -d '\n' > .mst-publish-token
npx wrangler deploy -c mst-publish/wrangler.jsonc
npx wrangler secret put UPLOAD_TOKEN -c mst-publish/wrangler.jsonc < .mst-publish-token

# Backup sink (for your own snapshot job)
npx wrangler deploy -c backup/wrangler.jsonc
npx wrangler secret put BACKUP_TOKEN -c backup/wrangler.jsonc

# One-off bulk upload of your archive, then remove the endpoint
openssl rand -hex 32 | tr -d '\n' > .upload-token
npx wrangler deploy -c uploader/wrangler.jsonc
npx wrangler secret put UPLOAD_TOKEN -c uploader/wrangler.jsonc < .upload-token
python3 -I ../tools/upload_assets.py --assets /path/to/assets \
    --endpoint https://bf-asset-uploader.<account>.workers.dev
npx wrangler delete --name bf-asset-uploader
```

You can check a configuration without deploying with `npx wrangler deploy --dry-run` (add
`-c <dir>/wrangler.jsonc` for the other Workers).

To verify a new `bf-cdn` version before it takes traffic, compare it with the current one using
`tools/cdn_compare.py` (byte-for-byte status, body hash and placeholder marker).

## Upload rules

- **bf-mst-publish** accepts only `PUT /mst/Ver<N>_<fileKey>[_<part>].dat` (at most 8 MiB) with the
  `x-upload-token` header, from an address in `ALLOWED_IPS`. It never overwrites: identical bytes
  under an existing name return 200 so an interrupted publish can be retried, different bytes
  return 409. It has no read, list or delete.
- **bf-backup** accepts only `PUT /db/gme-<YYYYMMDDTHHMMSSZ>.sqlite.gz` with `x-backup-token`, up to
  500 MiB. Bodies over Cloudflare's 100 MB request limit go up as an R2 multipart upload
  (`POST ?uploads`, `PUT ?part=<n>`, `POST ?complete`). Existing objects are never replaced, and
  there is no read, list or delete, so a leaked token cannot read or destroy old backups. Restore a
  backup with `npx wrangler r2 object get bf-backups/db/<name>.sqlite.gz --remote --file <out>`.
- **bf-asset-uploader** accepts `PUT /<key>` under `content/`, `mst/` or `_meta/` with
  `x-upload-token`. It is intended to exist only for the duration of the initial upload.

All token comparisons are constant time.

## Test

```bash
cd cdn-worker
npm ci
npm test
```

`test/worker.test.mjs` runs `src/index.js` in workerd (Miniflare) with an in-memory R2 bucket and
checks lookups, aliases, the name index, placeholders and miss logging. `test/unit.test.mjs` uses a
fake R2 binding for failure modes workerd cannot simulate (failing or slow reads, the clock).

## Related documents

- [Root README](../README.md): project overview, data sources, legal notice
- [tools/README.md](../tools/README.md): `upload_assets.py`, `build_cdn_static.py`, `cdn_compare.py`, `cdn_misses.py`
- [site/README.md](../site/README.md): the static site, which reuses this folder's `wrangler`
- [docs/DEPLOYMENT.md](../docs/DEPLOYMENT.md): deploying the whole stack
- [CONTRIBUTING.md](../CONTRIBUTING.md)
