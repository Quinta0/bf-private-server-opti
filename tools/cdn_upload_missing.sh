#!/usr/bin/env bash
# Upload the verified missing-asset substitutes to the R2 bucket behind cdn.example.com.
#
# Input (built by re/work/assets-sweep/build_upload_manifest.py, read its report.md first):
#   upload/manifest.json          [{key, file, content_type, bytes, sha256, reason, ...}]
#   upload/aliases.json           -> _meta/aliases.json
#   upload/name-index.new.json    -> _meta/name-index.json
#   upload/name-index.current.json  backup of the live index the new one was built from
#
# Default is a DRY RUN that only prints the wrangler commands. Pass --apply to upload.
# Safety: before anything runs, every manifest file is re-hashed and the JSON files are
# validated; before each put the key is re-checked to be in the manifest, under content/,
# and NOT an archive key or one of the two hand-added keys, so nothing that is already in
# R2 is ever overwritten. With --apply the live name index is downloaded first and must be
# byte-identical to name-index.current.json (otherwise the new index is stale: abort), and
# _meta/aliases.json must still be absent (or equal to aliases.current.json). Both are checked
# again right before the meta puts, since the content puts take minutes. Any failed command
# stops the run; re-running resumes (our own content keys are simply put again).
# Order: content files, then aliases.json, then the name index last, so the index never
# points at a key that is not uploaded yet.
#
# Usage: tools/cdn_upload_missing.sh [--apply]
#   env: UPLOAD_DIR (default re/work/assets-sweep/upload), ARCHIVE (default the owner's archive)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPLOAD_DIR="${UPLOAD_DIR:-$REPO/re/work/assets-sweep/upload}"
ARCHIVE="${ARCHIVE:-$HOME/bf-archive/assets}"
WORKER_DIR="$REPO/cdn-worker"
BUCKET="bf-assets"
MANIFEST="$UPLOAD_DIR/manifest.json"
ALIASES="$UPLOAD_DIR/aliases.json"
ALIASES_CUR="$UPLOAD_DIR/aliases.current.json"
INDEX_NEW="$UPLOAD_DIR/name-index.new.json"
INDEX_CUR="$UPLOAD_DIR/name-index.current.json"
HAND_ADDED=(
  "content/_dlcbundle/MapVillage/MapVillage_640x384.png"
  "content/_dlcbundle/event_total_token_earned1.png"
)
CDN="http://cdn.example.com"
UA="Apache-HttpClient/UNAVAILABLE (java 1.5)"

APPLY=0
for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,/^set -euo/{/^set -euo/!p;}' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg (use --apply or --help)" >&2; exit 2 ;;
  esac
done

die() { echo "ABORT: $*" >&2; exit 1; }

# Shell-quote for display: plain words stay bare, anything else is single-quoted with
# each ' written as '\''. A loop, not ${1//\'/...}: bash 3.2 and 5.2 treat backslashes in a
# pattern-substitution replacement differently.
q() {
  if [[ "$1" =~ ^[A-Za-z0-9_./:=@%+,-]+$ ]]; then printf '%s' "$1"; return; fi
  local s="$1" out="'"
  while [[ "$s" == *\'* ]]; do out+="${s%%\'*}'\\''"; s="${s#*\'}"; done
  printf '%s' "$out$s'"
}

show() {  # print a command as it would be typed, from the worker directory
  local out="" a
  for a in "$@"; do out+="$(q "$a") "; done
  echo "  ${out% }"
}

run() {  # dry run: print; --apply: print and run from cdn-worker/
  show "$@"
  if [[ $APPLY -eq 1 ]]; then (cd "$WORKER_DIR" && "$@" </dev/null); fi
}

for f in "$MANIFEST" "$ALIASES" "$INDEX_NEW" "$INDEX_CUR"; do
  [[ -f "$f" ]] || die "missing $f (run build_upload_manifest.py first)"
done
[[ -d "$ARCHIVE/content" && -d "$ARCHIVE/mst" ]] || die "archive not found at $ARCHIVE (needed to refuse archive keys)"
command -v python3 >/dev/null || die "python3 is required"

# ---------------------------------------------------------------- preflight
# Validates everything and writes one line per upload: key<TAB>file<TAB>content_type.
# (A temp file, not $(...): macOS bash 3.2 mis-parses heredocs inside command substitution.)
PLAN_FILE="$(mktemp -t cdn_upload_plan)"
trap 'rm -f "$PLAN_FILE"' EXIT
python3 -I - "$MANIFEST" "$ALIASES" "$INDEX_NEW" "$INDEX_CUR" "$ARCHIVE" "$UPLOAD_DIR/files" "${HAND_ADDED[@]}" >"$PLAN_FILE" <<'PY' || die "preflight failed"
import functools, hashlib, json, os, re, sys
manifest_p, aliases_p, new_p, cur_p, archive, files_dir, *hand = sys.argv[1:]
files_dir = os.path.realpath(files_dir)
TYPES = {'image/png', 'image/jpeg', 'image/gif', 'audio/mpeg', 'audio/ogg', 'text/csv; charset=utf-8',
         'text/plain; charset=utf-8', 'application/json', 'application/xml', 'application/zip', 'font/ttf',
         'application/octet-stream'}
def fail(msg):
    print(f'ABORT: {msg}', file=sys.stderr); sys.exit(1)
def servable(k):
    return isinstance(k, str) and re.match(r'^(content|mst)/', k) and not re.search(r'(^|/)\.\.(/|$)|\\|\0|[\t\r\n]', k)
def ext(k):
    n = k.rsplit('/', 1)[-1]
    return n.rsplit('.', 1)[-1].lower() if '.' in n else ''
@functools.lru_cache(maxsize=None)
def listing(path):
    try:
        return frozenset(os.listdir(path))
    except (NotADirectoryError, FileNotFoundError):
        return frozenset()
def in_archive(k):  # exact-case check, component by component
    path = archive
    for part in k.split('/'):
        if part not in listing(path):
            return False
        path = os.path.join(path, part)
    return True
manifest = json.load(open(manifest_p))
if not isinstance(manifest, list) or not manifest:
    fail('manifest.json is not a non-empty list')
keys = set()
for e in manifest:
    k, f = e.get('key'), e.get('file')
    if not servable(k) or not k.startswith('content/'):
        fail(f'bad key {k!r}')
    if k in keys:
        fail(f'duplicate key {k}')
    keys.add(k)
    if k in hand or in_archive(k):
        fail(f'{k} is an archive or hand-added key; refusing to overwrite')
    if e.get('content_type') not in TYPES:
        fail(f'{k}: unexpected content type {e.get("content_type")!r}')
    real = os.path.realpath(f or '')
    if not real.startswith(files_dir + os.sep) or not os.path.isfile(real):
        fail(f'{k}: file {f!r} missing or outside {files_dir}')
    data = open(real, 'rb').read()
    if len(data) != e.get('bytes') or hashlib.sha256(data).hexdigest() != e.get('sha256'):
        fail(f'{k}: size/sha256 differ from the manifest')
aliases = json.load(open(aliases_p))
if not isinstance(aliases, dict):
    fail('aliases.json is not an object')
for k, v in aliases.items():
    if not servable(v) or not in_archive(v):
        fail(f'alias {k!r} -> {v!r}: target is not an archive key')
    if ext(k) != ext(v) or ('/' in k and not servable(k)):
        fail(f'alias {k!r} -> {v!r}: extension or key invalid')
cur, new = json.load(open(cur_p)), json.load(open(new_p))
if not isinstance(cur, dict) or not isinstance(new, dict):
    fail('name index files are not objects')
changed = [k for k, v in cur.items() if new.get(k) != v]
if changed:
    fail(f'name-index.new.json changes {len(changed)} existing mappings, e.g. {changed[:3]}')
for k in new.keys() - cur.keys():
    if new[k] not in keys or new[k].rsplit('/', 1)[-1] != k:
        fail(f'new index entry {k!r} -> {new[k]!r} is not an uploaded file of that name')
for e in manifest:
    print(f"{e['key']}\t{e['file']}\t{e['content_type']}")
print(f'preflight ok: {len(manifest)} files, {len(aliases)} aliases, index {len(cur)} -> {len(new)} names', file=sys.stderr)
PY

in_manifest() {
  python3 -I -c 'import json,sys; sys.exit(0 if any(e.get("key") == sys.argv[2] for e in json.load(open(sys.argv[1]))) else 1)' "$MANIFEST" "$1"
}

assert_uploadable() {  # re-checked right before each put
  local key="$1" h
  [[ "$key" == content/* ]] || die "$key is not under content/"
  in_manifest "$key" || die "$key is not in $MANIFEST"
  # -e is case-insensitive on macOS, so a case variant also refuses: errs on the safe side.
  if [[ -e "$ARCHIVE/$key" || -L "$ARCHIVE/$key" ]]; then die "$key exists in the archive (already in R2); refusing to overwrite"; fi
  for h in "${HAND_ADDED[@]}"; do [[ "$key" != "$h" ]] || die "$key is a hand-added key; refusing to overwrite"; done
}

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

# Check every key once before any network call, so a refusal never leaves a half-done upload.
while IFS=$'\t' read -r key _ _ <&3; do
  if [[ -n "$key" ]]; then assert_uploadable "$key"; fi
done 3<"$PLAN_FILE"

if [[ $APPLY -eq 1 ]]; then
  echo "# APPLY: uploading to R2 bucket $BUCKET (remote)."
else
  echo "# DRY RUN: nothing is uploaded. Re-run with --apply to upload. Commands run from:"
fi
echo "cd $(q "$WORKER_DIR")"

# ---------------------------------------------------------------- 1. backup / freshness
# Both checks run twice: before the first put, and again right before the meta puts,
# because the content puts take minutes and another upload could change the index meanwhile.
check_live_index() {  # $1: where to save the live copy (kept as the backup / evidence)
  run npx wrangler r2 object get "$BUCKET/_meta/name-index.json" --remote --file "$1"
  if [[ $APPLY -eq 1 ]]; then
    [[ "$(sha "$1")" == "$(sha "$INDEX_CUR")" ]] \
      || die "live name index changed since the manifest was built (live copy kept at $1); rebuild first"
    echo "  live index unchanged: ok ($1)"
  fi
}

check_live_aliases() {  # $1: probe file
  local out
  show npx wrangler r2 object get "$BUCKET/_meta/aliases.json" --remote --file "$1"
  [[ $APPLY -eq 1 ]] || return 0
  if out="$(cd "$WORKER_DIR" && npx wrangler r2 object get "$BUCKET/_meta/aliases.json" --remote --file "$1" 2>&1 </dev/null)"; then
    # Equal to what we built from (or to our own aliases.json, when resuming an interrupted run).
    [[ ( -f "$ALIASES_CUR" && "$(sha "$1")" == "$(sha "$ALIASES_CUR")" ) || "$(sha "$1")" == "$(sha "$ALIASES")" ]] \
      || die "_meta/aliases.json now exists in R2 and was not merged (kept at $1); rebuild first"
    rm -f "$1"
    echo "  aliases.json unchanged: ok"
  elif grep -q "specified key does not exist" <<<"$out"; then
    # wrangler creates the --file target before it finds the key missing: drop the empty file.
    rm -f "$1"
    [[ ! -f "$ALIASES_CUR" ]] || die "_meta/aliases.json disappeared from R2 since the build; rebuild first"
    echo "  aliases.json absent: ok"
  else
    die "could not check _meta/aliases.json: $out"
  fi
}

STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$UPLOAD_DIR/name-index.backup-$STAMP.json"
echo "# 1. backup: live _meta/name-index.json must equal name-index.current.json (sha256 $(sha "$INDEX_CUR"))"
check_live_index "$BACKUP"
echo "# 1b. _meta/aliases.json must still be absent (or equal to aliases.current.json)"
check_live_aliases "$UPLOAD_DIR/aliases.live-$STAMP.json"

# ---------------------------------------------------------------- 2. content files
COUNT=0
echo "# 2. content files"
while IFS=$'\t' read -r key file ctype <&3; do
  [[ -n "$key" ]] || continue
  assert_uploadable "$key"
  run npx wrangler r2 object put "$BUCKET/$key" --file "$file" --content-type "$ctype" --remote
  COUNT=$((COUNT + 1))
done 3<"$PLAN_FILE"

# ---------------------------------------------------------------- 3. meta files
echo "# 3a. re-check right before the meta puts (nothing else may have changed them meanwhile)"
RECHECK="$UPLOAD_DIR/name-index.recheck-$STAMP.json"
check_live_index "$RECHECK"
if [[ $APPLY -eq 1 ]]; then rm -f "$RECHECK"; fi
check_live_aliases "$UPLOAD_DIR/aliases.recheck-$STAMP.json"
echo "# 3. aliases, then the name index (last)"
run npx wrangler r2 object put "$BUCKET/_meta/aliases.json" --file "$ALIASES" --content-type application/json --remote
run npx wrangler r2 object put "$BUCKET/_meta/name-index.json" --file "$INDEX_NEW" --content-type application/json --remote

if [[ $APPLY -eq 1 ]]; then
  echo "# 4. read-only check of the uploaded keys on $CDN"
  ok=0; bad=0
  while IFS=$'\t' read -r key _ _ <&3; do
    [[ -n "$key" ]] || continue
    enc="$(python3 -I -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe="/"))' "$key")"
    code="$(curl -s -o /dev/null -w '%{http_code}' -I -A "$UA" "$CDN/$enc" || true)"
    if [[ "$code" == 200 ]]; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  $code $key"; fi
  done 3<"$PLAN_FILE"
  echo "uploaded $COUNT files + aliases.json + name-index.json; CDN check: $ok x 200, $bad other"
  echo "previous index kept at $BACKUP"
else
  echo "# DRY RUN done: $COUNT content puts + 2 meta puts printed; nothing uploaded. Use --apply to upload."
fi
