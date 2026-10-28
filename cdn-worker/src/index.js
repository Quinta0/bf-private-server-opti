// Brave Frontier asset CDN: serves /content/* and /mst/* from R2.
//
// Most requests never reach this code: archive files in static/ (wrangler.jsonc
// "assets") are served by Cloudflare first. A static/ file wins over R2, so a
// replaced R2 object needs tools/build_cdn_static.py and a redeploy.
//
// Lookup order for a request key:
//   1. the exact R2 key;
//   2. _meta/aliases.json, hand-maintained, keys and values are R2 keys
//      without a leading slash or query, e.g.
//        {"content/_dlcbundle/a.png": "content/sam/A/a.png", "b.png": "content/x/b.png"}
//      a full-key entry wins over a basename entry;
//   3. _meta/name-index.json (basename -> preferred key, built at upload time).
//      The client asks for some files under a different folder than the
//      archive stores them in (e.g. _dlcbundle/X.png lives at sam/<anim>/X.png),
//      matching the game server's findContentByName/findMstPart behaviour;
//   4. a generated placeholder, for content/ .png and .jpg only (src/placeholder.js),
//      except the names in src/no-placeholder.js;
//   5. 404.
// Steps 2-3 only ever resolve to content/ or mst/ keys with the same extension.
//
// The client keeps whatever it downloads, so a placeholder can stay on a phone
// for good. One is served only when the miss is certain: if R2 fails, or
// aliases.json is unreadable, or the name index is missing or unreadable, the
// answer is a 503 (no-store) instead, and the client simply tries again later.
//
// Every placeholder, 404 or 503 for a content/ or mst/ key logs one {bf_miss}
// line (Workers Logs, wrangler tail); tools/cdn_misses.py counts them.
// Plain HTTP must keep working: the game's asset loader is hard-coded http://.

import { diag } from './diag.js';
import { NO_PLACEHOLDER } from './no-placeholder.js';
import { PLACEHOLDER_JPEG, placeholderSize, transparentPng } from './placeholder.js';

const CONTENT_TYPES = {
  png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
  mp3: 'audio/mpeg', ogg: 'audio/ogg', csv: 'text/csv; charset=utf-8',
  txt: 'text/plain; charset=utf-8', json: 'application/json', plist: 'application/xml',
  zip: 'application/zip', ttf: 'font/ttf',
};

// no-transform: the game fetches over plain HTTP, and a carrier proxy that
// "optimises" or recompresses a response hands the client damaged bytes (a
// player on T-Mobile data could not load new unit tables, fine on Wi-Fi).
const CACHE_EXACT = 'public, max-age=86400, no-transform';
// Alias / name-index answers and placeholders: short, so a file uploaded to
// the exact key later (or an alias edit) is not hidden for a day.
const CACHE_SHORT = 'public, max-age=300, no-transform';

const ALIASES_KEY = '_meta/aliases.json';
const NAME_INDEX_KEY = '_meta/name-index.json';
const META_TTL_MS = 5 * 60 * 1000;

// ---------------------------------------------------------------- _meta files
//
// Per-isolate copies of the two _meta JSON files, as plain data plus the R2
// etag they were read at. One read is shared by every request that needs it
// (the name index is ~7 MB of JSON, ~12 MB once parsed: a cold isolate must not
// parse it once per concurrent miss). The request that starts a read hands it
// to ctx.waitUntil, so the read still completes if that client disconnects;
// otherwise it would be canceled with its request and every other request
// awaiting it would hang. After META_TTL_MS the next miss re-checks the etag
// with one R2 head and re-reads the file only if it changed. R2 errors are
// never remembered.
//
// A parsed file is an object, or null when it is unusable (see parse* below);
// null makes every unresolved miss a 503 instead of a placeholder.

const metaFiles = new Map(); // key -> { data, etag, checkedAt }
const metaReads = new Map(); // key -> Promise<data>, while one read is running

async function readMeta(env, key, parse, current) {
  if (current) {
    try {
      const head = await env.ASSETS.head(key);
      if ((head?.etag ?? null) === current.etag) return { ...current, checkedAt: Date.now() };
    } catch (err) {
      // Keep the copy we have and re-check on the next miss.
      console.warn({ bf_meta: 'head failed, using cached copy', key, error: String(err) });
      return current;
    }
  }
  const object = await env.ASSETS.get(key);
  const data = parse(object === null ? null : await object.text());
  return { data, etag: object?.etag ?? null, checkedAt: Date.now() };
}

function loadMeta(env, ctx, key, parse) {
  const current = metaFiles.get(key);
  if (current && Date.now() - current.checkedAt < META_TTL_MS) return Promise.resolve(current.data);
  let pending = metaReads.get(key);
  if (!pending) {
    pending = readMeta(env, key, parse, current)
      .then((next) => {
        metaFiles.set(key, next);
        return next.data;
      })
      .finally(() => metaReads.delete(key));
    metaReads.set(key, pending);
    ctx.waitUntil(pending.catch(() => {})); // the caller sees (and reports) the error
  }
  return pending;
}

function parseJsonObject(text, key) {
  try {
    const value = JSON.parse(text);
    if (value && typeof value === 'object' && !Array.isArray(value)) return value;
  } catch {
    // fall through
  }
  console.warn({ bf_meta: 'not a JSON object, ignored until it changes', key });
  return null;
}

// Optional: a missing aliases.json means "no aliases".
const parseAliases = (text) => (text === null ? {} : parseJsonObject(text, ALIASES_KEY));

// Required: without the index a miss is not proof that a file is absent.
function parseNameIndex(text) {
  if (text !== null) return parseJsonObject(text, NAME_INDEX_KEY);
  console.warn({ bf_meta: 'missing', key: NAME_INDEX_KEY });
  return null;
}

const loadAliases = (env, ctx) => loadMeta(env, ctx, ALIASES_KEY, parseAliases);
const loadNameIndex = (env, ctx) => loadMeta(env, ctx, NAME_INDEX_KEY, parseNameIndex);

// ---------------------------------------------------------------- keys

// content/ or mst/ only; no "." or ".." segment, backslash or NUL.
function isServableKey(key) {
  return /^(content|mst)\//.test(key) && !/(^|\/)\.\.?(\/|$)|\\|\0/.test(key);
}

function baseName(key) {
  return key.slice(key.lastIndexOf('/') + 1);
}

function extOf(key) {
  const name = baseName(key);
  const dot = name.lastIndexOf('.');
  return dot < 0 ? '' : name.slice(dot + 1).toLowerCase();
}

// Own properties only, so "constructor" or "__proto__" never resolve to builtins.
function lookup(map, name) {
  const value = Object.hasOwn(map, name) ? map[name] : undefined;
  return typeof value === 'string' ? value : undefined;
}

// Alias / index targets are data, so check them on every use: they must stay
// inside content/ or mst/ and keep the extension the client asked for (a .jpg
// served under a .png name would not decode).
function isFallbackFor(key, target) {
  return target !== undefined && target !== key && isServableKey(target) && extOf(target) === extOf(key);
}

async function getTarget(env, key, target) {
  return isFallbackFor(key, target) ? env.ASSETS.get(target) : null;
}

// -> {object, fallback} on a hit, {object: null, certain} on a miss. certain is
// false when aliases.json or the name index could not be used. Throws on R2 errors.
async function resolve(env, ctx, key) {
  const exact = await env.ASSETS.get(key);
  if (exact) return { object: exact, fallback: false };

  const name = baseName(key);
  const aliasMap = await loadAliases(env, ctx);
  if (aliasMap) {
    for (const target of [lookup(aliasMap, key), lookup(aliasMap, name)]) {
      const object = await getTarget(env, key, target);
      if (object) return { object, fallback: true };
    }
  }
  const index = await loadNameIndex(env, ctx);
  const object = index ? await getTarget(env, key, lookup(index, name)) : null;
  if (object) return { object, fallback: true };
  return { object: null, certain: aliasMap !== null && index !== null };
}

// ---------------------------------------------------------------- responses

async function placeholderBody(key) {
  if (!key.startsWith('content/') || NO_PLACEHOLDER.has(baseName(key))) return null;
  const ext = extOf(key);
  if (ext === 'png') return transparentPng(...placeholderSize(baseName(key)));
  if (ext === 'jpg' || ext === 'jpeg') return PLACEHOLDER_JPEG;
  return null;
}

function logMiss(request, key, result) {
  const ua = request.headers.get('user-agent') ?? '';
  console.log({ bf_miss: key.slice(0, 512), result, ua: ua.slice(0, 64) });
}

function notFound() {
  return new Response('Not found', {
    status: 404,
    headers: { 'cache-control': 'public, max-age=60' },
  });
}

function unavailable() {
  return new Response('Temporarily unavailable', {
    status: 503,
    headers: { 'cache-control': 'no-store', 'retry-after': '60' },
  });
}

// Placeholders are short-lived and never go into caches.default, so a real
// file uploaded later is served as soon as it lands in R2.
async function miss(request, key) {
  const body = await placeholderBody(key);
  if (!body) {
    logMiss(request, key, '404');
    return notFound();
  }
  logMiss(request, key, 'placeholder');
  const headers = {
    'content-type': CONTENT_TYPES[extOf(key)],
    'content-length': String(body.length),
    'cache-control': CACHE_SHORT,
    'x-bf-placeholder': '1',
  };
  return new Response(request.method === 'HEAD' ? null : body.slice(), { headers });
}

// Entries stored before no-transform was added keep their old header until they
// expire (up to a day); answer them with today's header so a carrier proxy sees it.
const OLD_CACHE_CONTROL = {
  'public, max-age=86400': CACHE_EXACT,
  'public, max-age=300': CACHE_SHORT,
};

function withCurrentCacheControl(response) {
  const current = OLD_CACHE_CONTROL[response.headers.get('cache-control')];
  if (!current) return response;
  const headers = new Headers(response.headers);
  headers.set('cache-control', current);
  return new Response(response.body, { status: response.status, headers });
}

async function serve(request, env, ctx, url, key) {
  // Cached under the request's own (normalised, still-encoded) path, query
  // dropped: ?v= cache busters never select a different file. Not under the
  // decoded key: re-parsing that as a URL would collapse "%2e%2e" or "."
  // segments and let one request store its answer under another file's entry.
  const cacheKey = new Request(url.origin + url.pathname, { method: 'GET' });
  const cache = caches.default;
  const stored = await cache.match(cacheKey);
  if (stored) {
    const cached = withCurrentCacheControl(stored);
    return request.method === 'HEAD' ? new Response(null, cached) : cached;
  }

  const found = await resolve(env, ctx, key);
  if (!found.object) {
    if (found.certain) return miss(request, key);
    logMiss(request, key, '503');
    return unavailable();
  }

  const { object } = found;
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  if (!headers.has('content-type')) {
    headers.set('content-type', CONTENT_TYPES[extOf(key)] ?? 'application/octet-stream');
  }
  headers.set('etag', object.httpEtag);
  headers.set('cache-control', found.fallback ? CACHE_SHORT : CACHE_EXACT);
  headers.set('content-length', String(object.size));

  const response = new Response(object.body, { headers });
  ctx.waitUntil(cache.put(cacheKey, response.clone()));
  return request.method === 'HEAD' ? new Response(null, response) : response;
}

export default {
  async fetch(request, env, ctx) {
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response('Method not allowed', { status: 405, headers: { allow: 'GET, HEAD' } });
    }

    const url = new URL(request.url);
    if (url.pathname === '/diag' || url.pathname === '/diag/echo') {
      try {
        return await diag(request, env, url);
      } catch (err) {
        console.error({ bf_error: String(err), diag: url.pathname });
        return unavailable();
      }
    }
    let key;
    try {
      key = decodeURIComponent(url.pathname).replace(/^\/+/, '');
    } catch {
      return notFound();
    }
    // Rejected paths are not asset lookups, so they are not logged as misses.
    if (!isServableKey(key)) return notFound();

    try {
      return await serve(request, env, ctx, url, key);
    } catch (err) {
      // An R2 error is not a miss: never answer it with a placeholder.
      console.error({ bf_error: String(err), key: key.slice(0, 512) });
      return unavailable();
    }
  },
};
