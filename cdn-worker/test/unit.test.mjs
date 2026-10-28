// Node-level checks of src/index.js with a fake R2 binding, for what workerd's
// local R2 cannot simulate (failing or slow reads, the clock) and to see the
// exact log objects.

import assert from 'node:assert/strict';
import { describe, it, mock } from 'node:test';

const GAME_UA = 'Apache-HttpClient/UNAVAILABLE (java 1.5)';
const FIVE_MIN = 5 * 60 * 1000;

globalThis.caches ??= { default: { match: async () => undefined, put: async () => {} } };

function recordingCtx() {
  const waited = [];
  return { waited, waitUntil: (p) => waited.push(p) };
}
const ctx = recordingCtx();

// A fresh module instance per test, so per-isolate state (aliases, name index) starts empty.
let instance = 0;
async function freshWorker() {
  return (await import(`../src/index.js?instance=${instance++}`)).default;
}

// In-memory R2 stand-in: get/head with etags that change on every put, keys in
// `failing` throw, and `gate` (a promise) holds back reads of `gatedKey`.
function fakeBucket(initial, failing = new Set()) {
  const objects = new Map();
  let version = 0;
  const put = (key, text) => objects.set(key, { text, etag: `v${++version}` });
  for (const [key, text] of Object.entries(initial)) put(key, text);
  const bucket = {
    reads: [],
    heads: [],
    failing,
    put,
    gate: null,
    gatedKey: null,
    async head(key) {
      bucket.heads.push(key);
      if (failing.has(key)) throw new Error('R2 unavailable');
      const o = objects.get(key);
      return o ? { etag: o.etag } : null;
    },
    async get(key) {
      bucket.reads.push(key);
      if (key === bucket.gatedKey && bucket.gate) await bucket.gate;
      if (failing.has(key)) throw new Error('R2 unavailable');
      const o = objects.get(key);
      if (!o) return null;
      const bytes = new TextEncoder().encode(o.text);
      return {
        size: bytes.length,
        etag: o.etag,
        httpEtag: `"${o.etag}"`,
        body: bytes,
        writeHttpMetadata() {},
        text: async () => o.text,
      };
    },
  };
  return bucket;
}

const get = (worker, env, path, context = ctx) =>
  worker.fetch(new Request(`http://cdn.example.com${path}`, { headers: { 'user-agent': GAME_UA } }), env, context);

const readsOf = (bucket, key) => bucket.reads.filter((k) => k === key).length;

describe('worker internals', () => {
  it('answers 503 (not a placeholder) when aliases.json cannot be read, and retries on the next miss', async (t) => {
    const warn = t.mock.method(console, 'warn', () => {});
    const error = t.mock.method(console, 'error', () => {});
    t.mock.method(console, 'log', () => {});
    const ASSETS = fakeBucket(
      {
        '_meta/name-index.json': '{}',
        '_meta/aliases.json': '{"a_8x8.png": "content/real/a_8x8.png"}',
        'content/real/a_8x8.png': 'real',
      },
      new Set(['_meta/aliases.json']),
    );
    const env = { ASSETS };
    const worker = await freshWorker();

    const first = await get(worker, env, '/content/x/a_8x8.png');
    assert.equal(first.status, 503);
    assert.equal(first.headers.get('cache-control'), 'no-store');
    assert.equal(first.headers.get('x-bf-placeholder'), null);
    assert.equal(error.mock.callCount(), 1);
    assert.equal(warn.mock.callCount(), 0);

    ASSETS.failing.clear();
    const second = await get(worker, env, '/content/x/a_8x8.png');
    assert.equal(second.status, 200);
    assert.equal(await second.text(), 'real');
    assert.equal(second.headers.get('cache-control'), 'public, max-age=300, no-transform', 'alias answers are short-lived');

    await get(worker, env, '/content/y/a_8x8.png');
    assert.equal(readsOf(ASSETS, '_meta/aliases.json'), 2, 'loaded once after success');
  });

  it('answers 503 when the name index cannot be read, and retries on the next miss', async (t) => {
    t.mock.method(console, 'error', () => {});
    t.mock.method(console, 'log', () => {});
    const ASSETS = fakeBucket(
      { '_meta/name-index.json': '{"b.png": "content/sam/B/b.png"}', 'content/sam/B/b.png': 'png', 'content/hit.png': 'h' },
      new Set(['_meta/name-index.json']),
    );
    const worker = await freshWorker();
    assert.equal((await get(worker, { ASSETS }, '/content/_dlcbundle/b.png')).status, 503);
    assert.equal((await get(worker, { ASSETS }, '/content/_dlcbundle/none.png')).status, 503);
    assert.equal((await get(worker, { ASSETS }, '/content/hit.png')).status, 200, 'exact hits need no index');
    ASSETS.failing.clear();
    const res = await get(worker, { ASSETS }, '/content/_dlcbundle/b.png');
    assert.equal(res.status, 200);
    assert.equal(await res.text(), 'png');
  });

  it('answers 503 for every unresolved miss while the name index is missing or broken', async (t) => {
    const warn = t.mock.method(console, 'warn', () => {});
    t.mock.method(console, 'log', () => {});
    for (const index of [undefined, '{broken', '[]']) {
      const seed = { 'content/real.png': 'r', '_meta/aliases.json': '{"al.png": "content/real.png"}' };
      if (index !== undefined) seed['_meta/name-index.json'] = index;
      const ASSETS = fakeBucket(seed);
      const worker = await freshWorker();
      for (const path of ['/content/x/gone_8x8.png', '/content/x/gone.txt', '/content/y/gone.jpg']) {
        const res = await get(worker, { ASSETS }, path);
        assert.equal(res.status, 503, `${index}: ${path}`);
        assert.equal(res.headers.get('x-bf-placeholder'), null);
      }
      assert.equal(await (await get(worker, { ASSETS }, '/content/real.png')).text(), 'r');
      assert.equal(await (await get(worker, { ASSETS }, '/content/x/al.png')).text(), 'r', 'aliases still work');
      assert.equal(readsOf(ASSETS, '_meta/name-index.json'), 1, `${index}: not re-read until it changes`);
    }
    assert.equal(warn.mock.callCount(), 3);
  });

  it('keeps a malformed aliases.json as unusable (503, no placeholder) until it changes', async (t) => {
    const warn = t.mock.method(console, 'warn', () => {});
    t.mock.method(console, 'log', () => {});
    t.mock.timers.enable({ apis: ['Date'], now: 1_000_000 });
    const ASSETS = fakeBucket({
      '_meta/name-index.json': '{"idx.png": "content/i/idx.png"}',
      '_meta/aliases.json': '{oops',
      'content/i/idx.png': 'index hit',
      'content/real/c.png': 'aliased',
    });
    const worker = await freshWorker();
    for (const path of ['/content/a.txt', '/content/b.txt', '/content/c.png']) {
      assert.equal((await get(worker, { ASSETS }, path)).status, 503, path);
    }
    assert.equal(await (await get(worker, { ASSETS }, '/content/_dlcbundle/idx.png')).text(), 'index hit');
    assert.equal(readsOf(ASSETS, '_meta/aliases.json'), 1);
    assert.equal(warn.mock.callCount(), 1);

    ASSETS.put('_meta/aliases.json', '{"c.png": "content/real/c.png"}');
    t.mock.timers.tick(FIVE_MIN);
    assert.equal(await (await get(worker, { ASSETS }, '/content/c.png')).text(), 'aliased');
    const missing = await get(worker, { ASSETS }, '/content/b.txt');
    assert.equal(missing.status, 404);
  });

  it('picks up an aliases.json uploaded after the isolate started, within 5 minutes', async (t) => {
    t.mock.method(console, 'log', () => {});
    t.mock.timers.enable({ apis: ['Date'], now: 1_000_000 });
    const ASSETS = fakeBucket({ '_meta/name-index.json': '{}', 'content/sam/X/x_4x4.png': 'x' });
    const worker = await freshWorker();

    const before = await get(worker, { ASSETS }, '/content/_dlcbundle/x_4x4.png');
    assert.equal(before.headers.get('x-bf-placeholder'), '1');
    ASSETS.put('_meta/aliases.json', '{"x_4x4.png": "content/sam/X/x_4x4.png"}');

    t.mock.timers.tick(FIVE_MIN - 1);
    assert.equal((await get(worker, { ASSETS }, '/content/_dlcbundle/x_4x4.png')).headers.get('x-bf-placeholder'), '1');
    assert.equal(ASSETS.heads.length, 0, 'no R2 head within the TTL');

    t.mock.timers.tick(1);
    const after = await get(worker, { ASSETS }, '/content/_dlcbundle/x_4x4.png');
    assert.equal(await after.text(), 'x');

    // Unchanged files are only re-checked with a head, not re-read.
    const indexReads = readsOf(ASSETS, '_meta/name-index.json');
    t.mock.timers.tick(FIVE_MIN);
    await get(worker, { ASSETS }, '/content/_dlcbundle/x_4x4.png');
    await get(worker, { ASSETS }, '/content/_dlcbundle/nothing_4x4.png');
    assert.equal(readsOf(ASSETS, '_meta/name-index.json'), indexReads);
    assert.equal(readsOf(ASSETS, '_meta/aliases.json'), 2);
  });

  it('keeps the cached copy when the 5-minute re-check fails', async (t) => {
    const warn = t.mock.method(console, 'warn', () => {});
    t.mock.method(console, 'log', () => {});
    t.mock.timers.enable({ apis: ['Date'], now: 1_000_000 });
    const ASSETS = fakeBucket({ '_meta/name-index.json': '{"n.png": "content/n/n.png"}', 'content/n/n.png': 'n' });
    const worker = await freshWorker();
    assert.equal(await (await get(worker, { ASSETS }, '/content/_dlcbundle/n.png')).text(), 'n');
    t.mock.timers.tick(FIVE_MIN);
    ASSETS.failing.add('_meta/name-index.json');
    const res = await get(worker, { ASSETS }, '/content/_dlcbundle/n.png');
    assert.equal(res.status, 200);
    assert.equal(await res.text(), 'n');
    assert.ok(warn.mock.calls.some((c) => c.arguments[0].bf_meta === 'head failed, using cached copy'));
  });

  it('shares one name-index read between concurrent misses and hands it to ctx.waitUntil', async (t) => {
    t.mock.method(console, 'log', () => {});
    const ASSETS = fakeBucket({ '_meta/name-index.json': '{"s.png": "content/s/s.png"}', 'content/s/s.png': 's' });
    let release;
    ASSETS.gate = new Promise((r) => {
      release = r;
    });
    ASSETS.gatedKey = '_meta/name-index.json';
    const worker = await freshWorker();
    const contexts = [recordingCtx(), recordingCtx(), recordingCtx()];
    const responses = ['a', 'b', 'c'].map((dir, i) => get(worker, { ASSETS }, `/content/${dir}/s.png`, contexts[i]));
    await new Promise((r) => setTimeout(r, 20));
    assert.equal(readsOf(ASSETS, '_meta/name-index.json'), 1, 'one read in flight');
    const waited = contexts.reduce((n, c) => n + c.waited.length, 0);
    assert.equal(waited, 2, 'the aliases read and the index read were each handed to one waitUntil');
    release();
    for (const res of await Promise.all(responses)) assert.equal(await res.text(), 's');
    assert.equal(readsOf(ASSETS, '_meta/name-index.json'), 1);
  });

  it('logs exactly one {bf_miss, result, ua} object per miss and nothing for hits', async (t) => {
    const log = t.mock.method(console, 'log', () => {});
    t.mock.method(console, 'warn', () => {});
    const ASSETS = fakeBucket({ '_meta/name-index.json': '{}', 'content/hit.txt': 'x' });
    const worker = await freshWorker();

    assert.equal((await get(worker, { ASSETS }, '/content/hit.txt?v=1')).status, 200);
    assert.equal((await get(worker, { ASSETS }, '/content/gone.txt?v=1')).status, 404);
    assert.equal((await get(worker, { ASSETS }, '/content/gone_8x8.png')).status, 200);
    assert.equal((await get(worker, { ASSETS }, '/content/%2e%2e/x.png')).status, 404);
    ASSETS.put('_meta/name-index.json', 'broken');
    const unusable = await freshWorker();
    assert.equal((await get(unusable, { ASSETS }, '/content/gone2.png')).status, 503);
    assert.deepEqual(
      log.mock.calls.map((c) => c.arguments),
      [
        [{ bf_miss: 'content/gone.txt', result: '404', ua: GAME_UA }],
        [{ bf_miss: 'content/gone_8x8.png', result: 'placeholder', ua: GAME_UA }],
        [{ bf_miss: 'content/gone2.png', result: '503', ua: GAME_UA }],
      ],
    );
  });
});

mock.restoreAll();
