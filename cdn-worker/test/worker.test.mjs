// Runs src/index.js in workerd (Miniflare) with an in-memory R2 bucket and
// checks lookups, placeholders and miss logging. `npm test`.
//
// Placeholder images are decoded with pngjs / jpeg-js here and, when Python
// Pillow is installed, again with Pillow (test/decode_image.py).

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';
import jpeg from 'jpeg-js';
import { Miniflare, convertV4MiniflareOptions } from 'miniflare';
import pngjs from 'pngjs';
import { placeholderSize } from '../src/placeholder.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const PILLOW_HELPER = join(ROOT, 'test', 'decode_image.py');
const GAME_UA = 'Apache-HttpClient/UNAVAILABLE (java 1.5)';
const ORIGIN = 'http://cdn.example.com'; // plain HTTP, like the game

const hasPillow = spawnSync('python3', ['-I', '-c', 'import PIL'], { stdio: 'ignore' }).status === 0;

function wranglerConfig() {
  const text = readFileSync(join(ROOT, 'wrangler.jsonc'), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
    .replace(/,(\s*[}\]])/g, '$1');
  return JSON.parse(text);
}

function solidPng(width, height, rgba) {
  const png = new pngjs.PNG({ width, height });
  for (let i = 0; i < png.data.length; i += 4) png.data.set(rgba, i);
  return new Uint8Array(pngjs.PNG.sync.write(png));
}

// One workerd instance with its own in-memory R2 and a captured log stream.
async function startWorker(objects) {
  const config = wranglerConfig();
  const logs = [];
  const mf = new Miniflare(convertV4MiniflareOptions({
    // Entry first, then every other ES module under src/ (what wrangler bundles).
    modules: [config.main, ...readdirSync(join(ROOT, 'src')).map((f) => `src/${f}`)]
      .filter((path, i, all) => path.endsWith('.js') && all.indexOf(path) === i)
      .map((path) => ({ type: 'ESModule', path: join(ROOT, path) })),
    modulesRoot: ROOT,
    compatibilityDate: config.compatibility_date,
    compatibilityFlags: config.compatibility_flags ?? [],
    r2Buckets: config.r2_buckets.map((b) => b.binding),
    handleStructuredLogs: (log) => logs.push(log),
  }));
  const bucket = await mf.getR2Bucket('ASSETS');
  for (const [key, value] of Object.entries(objects)) await bucket.put(key, value);

  const fetchPath = (path, init = {}) =>
    mf.dispatchFetch(ORIGIN + path, { ...init, headers: { 'user-agent': GAME_UA, ...init.headers } });

  // workerd's console.log of {bf_miss, result, ua} arrives as util.inspect text.
  const misses = () =>
    logs
      .map((log) => log.message)
      .filter((m) => m.includes('bf_miss'))
      .map((m) => {
        const field = (k) => new RegExp(`${k}: '((?:[^'\\\\]|\\\\.)*)'`).exec(m)?.[1];
        return { bf_miss: field('bf_miss'), result: field('result'), ua: field('ua') };
      });

  // Misses logged after `mark` (a misses().length snapshot); waits until the
  // request with key `lastKey` has logged, since logs arrive asynchronously.
  async function missesAfter(mark, lastKey) {
    const deadline = Date.now() + 5000;
    while (!misses().slice(mark).some((m) => m.bf_miss === lastKey)) {
      if (Date.now() > deadline) assert.fail(`no bf_miss log for ${lastKey}; logs: ${JSON.stringify(logs)}`);
      await new Promise((r) => setTimeout(r, 20));
    }
    return misses().slice(mark);
  }

  return { mf, bucket, fetchPath, misses, missesAfter, logs };
}

async function bytesOf(response) {
  return new Uint8Array(await response.arrayBuffer());
}

function decodePng(bytes) {
  // pngjs verifies every chunk CRC (checkCRC) and zlib's adler32.
  const png = pngjs.PNG.sync.read(Buffer.from(bytes), { checkCRC: true });
  assert.equal(bytes[24], 8, 'bit depth 8');
  assert.equal(bytes[25], 6, 'colour type 6 (RGBA)');
  return png;
}

function pillow(bytes) {
  const r = spawnSync('python3', ['-I', PILLOW_HELPER], { input: Buffer.from(bytes) });
  assert.equal(r.status, 0, `Pillow failed: ${r.stderr}`);
  return JSON.parse(r.stdout);
}

// JPEG marker walk up to the scan: [{marker, payload}].
function jpegSegments(bytes) {
  assert.equal(bytes[0], 0xff);
  assert.equal(bytes[1], 0xd8, 'SOI');
  const segments = [];
  let at = 2;
  while (at < bytes.length) {
    assert.equal(bytes[at], 0xff, `marker at ${at}`);
    const marker = bytes[at + 1];
    const length = (bytes[at + 2] << 8) | bytes[at + 3];
    segments.push({ marker, payload: bytes.subarray(at + 4, at + 2 + length) });
    if (marker === 0xda) break; // SOS: entropy-coded data follows
    at += 2 + length;
  }
  return segments;
}

const REAL_PNG = solidPng(2, 2, [255, 0, 0, 255]);
const OTHER_PNG = solidPng(3, 1, [0, 0, 255, 255]);
const ANIM_PNG = solidPng(1, 1, [0, 255, 0, 255]);
const SECRET = new TextEncoder().encode('not for clients');

const SEED = {
  'content/unit/img/real.png': REAL_PNG,
  'content/other/other.png': OTHER_PNG,
  'content/sam/MapAnim/anim_tex.png': ANIM_PNG,
  'content/a/dup.png': REAL_PNG, // the file a client asks for
  'content/b/dup.png': OTHER_PNG, // what the name index prefers for dup.png
  'content/bg/real.jpg': new Uint8Array([0xff, 0xd8, 0xff, 0xd9]),
  'content/data/real.txt': 'hello',
  'mst/Ver1_real.dat': 'mst bytes',
  '_meta/secret.png': SECRET,
  '_meta/name-index.json': JSON.stringify({
    'anim_tex.png': 'content/sam/MapAnim/anim_tex.png',
    'dup.png': 'content/b/dup.png',
    'evil.png': '_meta/secret.png',
    'idx_traverse.png': 'content/%2e%2e/_meta/secret.png',
    'nonstring.png': 42,
  }),
  '_meta/aliases.json': JSON.stringify({
    'content/_dlcbundle/full_alias.png': 'content/unit/img/real.png',
    'base_alias.png': 'content/unit/img/real.png',
    'content/_dlcbundle/both.png': 'content/unit/img/real.png',
    'both.png': 'content/other/other.png',
    'outside.png': '_meta/secret.png',
    'traverse.png': 'content/../_meta/secret.png',
    'wrongext.png': 'content/bg/real.jpg',
    'dangling.png': 'content/nothere/dangling.png',
    'mst_alias.dat': 'mst/Ver1_real.dat',
  }),
};

describe('cdn worker', () => {
  let w;
  before(async () => {
    w = await startWorker(SEED);
  });
  after(async () => {
    await w?.mf.dispose();
  });

  it('serves an exact hit with type, etag and the 1-day cache', async () => {
    const res = await w.fetchPath('/content/unit/img/real.png');
    assert.equal(res.status, 200);
    assert.deepEqual(await bytesOf(res), REAL_PNG);
    assert.equal(res.headers.get('content-type'), 'image/png');
    assert.equal(res.headers.get('cache-control'), 'public, max-age=86400, no-transform');
    assert.equal(res.headers.get('content-length'), String(REAL_PNG.length));
    assert.match(res.headers.get('etag'), /^".+"$/);
    assert.equal(res.headers.get('x-bf-placeholder'), null);

    const txt = await w.fetchPath('/content/data/real.txt');
    assert.equal(txt.headers.get('content-type'), 'text/plain; charset=utf-8');
    assert.equal(await txt.text(), 'hello');
    const mst = await w.fetchPath('/mst/Ver1_real.dat');
    assert.equal(mst.headers.get('content-type'), 'application/octet-stream');
    assert.equal(await mst.text(), 'mst bytes');
  });

  it('ignores query strings', async () => {
    const res = await w.fetchPath('/content/unit/img/real.png?v=123');
    assert.equal(res.status, 200);
    assert.deepEqual(await bytesOf(res), REAL_PNG);
    const byName = await w.fetchPath('/content/_dlcbundle/anim_tex.png?v=0');
    assert.deepEqual(await bytesOf(byName), ANIM_PNG);
  });

  it('falls back to the by-name index, with the short cache', async () => {
    const res = await w.fetchPath('/content/_dlcbundle/MapAnim/anim_tex.png');
    assert.equal(res.status, 200);
    assert.deepEqual(await bytesOf(res), ANIM_PNG);
    assert.equal(res.headers.get('x-bf-placeholder'), null);
    assert.equal(res.headers.get('cache-control'), 'public, max-age=300, no-transform');
  });

  it('never stores an answer under another file\'s cache entry (encoded dot segments)', async () => {
    // %252e%252e decodes to a literal "%2e%2e" key segment; re-parsed as a URL
    // that would be "..", so the cache entry would be content/a/dup.png's.
    const sneaky = await w.fetchPath('/content/q/%252e%252e/a/dup.png');
    assert.deepEqual(await bytesOf(sneaky), OTHER_PNG, 'by-name fallback for the odd key');
    const dot = await w.fetchPath('/content/a/.%2fdup.png');
    assert.equal(dot.status, 404, 'a "." segment is rejected');
    await dot.arrayBuffer();
    for (let i = 0; i < 2; i++) {
      const res = await w.fetchPath('/content/a/dup.png');
      assert.deepEqual(await bytesOf(res), REAL_PNG, `request ${i + 1} gets the exact file`);
      assert.equal(res.headers.get('cache-control'), 'public, max-age=86400, no-transform');
    }
  });

  it('resolves a full-key alias only for that exact key', async () => {
    const res = await w.fetchPath('/content/_dlcbundle/full_alias.png');
    assert.equal(res.status, 200);
    assert.deepEqual(await bytesOf(res), REAL_PNG);
    assert.equal(res.headers.get('cache-control'), 'public, max-age=300, no-transform');
    const elsewhere = await w.fetchPath('/content/x/full_alias.png');
    assert.equal(elsewhere.headers.get('x-bf-placeholder'), '1');
  });

  it('resolves a basename alias from any folder', async () => {
    for (const path of ['/content/a/base_alias.png', '/content/_dlcbundle/b/base_alias.png?v=9']) {
      const res = await w.fetchPath(path);
      assert.equal(res.status, 200);
      assert.deepEqual(await bytesOf(res), REAL_PNG);
    }
    const mst = await w.fetchPath('/mst/x/mst_alias.dat');
    assert.equal(await mst.text(), 'mst bytes');
  });

  it('prefers a full-key alias over a basename alias', async () => {
    assert.deepEqual(await bytesOf(await w.fetchPath('/content/_dlcbundle/both.png')), REAL_PNG);
    assert.deepEqual(await bytesOf(await w.fetchPath('/content/elsewhere/both.png')), OTHER_PNG);
  });

  it('ignores alias and index targets outside content/ and mst/ or with another extension', async () => {
    for (const name of ['outside.png', 'traverse.png', 'evil.png', 'idx_traverse.png', 'nonstring.png', 'wrongext.png']) {
      const res = await w.fetchPath(`/content/_dlcbundle/${name}`);
      assert.equal(res.status, 200, name);
      assert.equal(res.headers.get('x-bf-placeholder'), '1', name);
      const png = decodePng(await bytesOf(res));
      assert.deepEqual([png.width, png.height], [4, 4], name);
    }
  });

  it('continues to the placeholder when an alias target is itself missing', async () => {
    const res = await w.fetchPath('/content/_dlcbundle/dangling.png');
    assert.equal(res.headers.get('x-bf-placeholder'), '1');
  });

  it('never resolves inherited names such as constructor or __proto__', async () => {
    for (const name of ['constructor', '__proto__', 'toString', 'hasOwnProperty']) {
      const res = await w.fetchPath(`/content/x/${name}`);
      assert.equal(res.status, 404, name);
    }
  });

  it('serves a decodable transparent placeholder PNG sized from _WxH', async () => {
    const res = await w.fetchPath('/content/_dlcbundle/MapVillage/Missing_640x384.png?v=3');
    assert.equal(res.status, 200);
    assert.equal(res.headers.get('x-bf-placeholder'), '1');
    assert.equal(res.headers.get('cache-control'), 'public, max-age=300, no-transform');
    assert.equal(res.headers.get('content-type'), 'image/png');
    assert.equal(res.headers.get('etag'), null);
    const bytes = await bytesOf(res);
    assert.equal(res.headers.get('content-length'), String(bytes.length));
    const png = decodePng(bytes);
    assert.deepEqual([png.width, png.height], [640, 384]);
    assert.ok(png.data.every((b) => b === 0), 'every pixel is transparent black');

    // A second request is served from the in-isolate memo; still identical.
    const again = await bytesOf(await w.fetchPath('/content/other/Missing2_640x384.png'));
    assert.deepEqual(again, bytes);
  });

  it('uses a 4x4 placeholder when the name has no size', async () => {
    const png = decodePng(await bytesOf(await w.fetchPath('/content/_dlcbundle/slot_freespin_base.png')));
    assert.deepEqual([png.width, png.height], [4, 4]);
    assert.ok(png.data.every((b) => b === 0));
    const upper = decodePng(await bytesOf(await w.fetchPath('/content/_dlcbundle/UPPER_8x2.PNG')));
    assert.deepEqual([upper.width, upper.height], [8, 2]);
  });

  it('clamps large sizes and refuses absurd ones', async () => {
    const sizeOf = async (name) => {
      const res = await w.fetchPath(`/content/_dlcbundle/${name}`);
      assert.equal(res.headers.get('x-bf-placeholder'), '1', name);
      const png = decodePng(await bytesOf(res));
      return [png.width, png.height];
    };
    assert.deepEqual(await sizeOf('huge_99999x99999.png'), [4, 4]);
    assert.deepEqual(await sizeOf('huge_999999x1.png'), [4, 4]);
    assert.deepEqual(await sizeOf('huge_99999999999x99999999999.png'), [4, 4]);
    assert.deepEqual(await sizeOf('zero_0x10.png'), [4, 4]);
    assert.deepEqual(await sizeOf('wide_4096x100.png'), [2048, 100]);
    assert.deepEqual(await sizeOf('max_2048x2048.png'), [2048, 2048]);
  });

  it('does not cache placeholders: a file uploaded later is served at once', async () => {
    const first = await w.fetchPath('/content/late/arrive_16x16.png');
    assert.equal(first.headers.get('x-bf-placeholder'), '1');
    await first.arrayBuffer();
    await w.bucket.put('content/late/arrive_16x16.png', OTHER_PNG);
    const second = await w.fetchPath('/content/late/arrive_16x16.png');
    assert.equal(second.headers.get('x-bf-placeholder'), null);
    assert.deepEqual(await bytesOf(second), OTHER_PNG);
  });

  it('serves a decodable baseline JPEG placeholder for .jpg misses', async () => {
    const res = await w.fetchPath('/content/_dlcbundle/dungeon_battle_999999.jpg?v=1');
    assert.equal(res.status, 200);
    assert.equal(res.headers.get('x-bf-placeholder'), '1');
    assert.equal(res.headers.get('content-type'), 'image/jpeg');
    assert.equal(res.headers.get('cache-control'), 'public, max-age=300, no-transform');
    const bytes = await bytesOf(res);

    const segments = jpegSegments(bytes);
    const markers = segments.map((s) => s.marker);
    assert.ok(markers.includes(0xc0), 'SOF0 (baseline)');
    assert.ok(!markers.some((m) => m >= 0xc1 && m <= 0xcf && m !== 0xc4 && m !== 0xcc), 'no other SOF');
    const app0 = segments.find((s) => s.marker === 0xe0);
    assert.equal(new TextDecoder().decode(app0.payload.subarray(0, 4)), 'JFIF');
    assert.ok(!segments.some((s) => s.marker === 0xee), 'no Adobe APP14');
    const sof = segments.find((s) => s.marker === 0xc0).payload;
    assert.equal(sof[0], 8, '8-bit samples');
    assert.equal(sof[5], 3, '3 components (YCbCr)');
    assert.deepEqual([bytes.at(-2), bytes.at(-1)], [0xff, 0xd9], 'EOI');

    const img = jpeg.decode(bytes, { useTArray: true, formatAsRGBA: true });
    assert.deepEqual([img.width, img.height], [16, 16]);
    for (let i = 0; i < img.data.length; i += 4) {
      assert.ok(img.data[i] <= 4 && img.data[i + 1] <= 4 && img.data[i + 2] <= 4, 'black');
    }
  });

  it('decodes the placeholders with Pillow too', { skip: !hasPillow && 'python3 + Pillow not installed' }, async () => {
    for (const [name, size] of [['p_640x384.png', [640, 384]], ['p.png', [4, 4]], ['p_99999x99999.png', [4, 4]]]) {
      const info = pillow(await bytesOf(await w.fetchPath(`/content/pillow/${name}`)));
      assert.equal(info.format, 'PNG', name);
      assert.equal(info.mode, 'RGBA', name);
      assert.deepEqual(info.size, size, name);
      assert.deepEqual(info.extrema, [[0, 0], [0, 0], [0, 0], [0, 0]], name);
    }
    const info = pillow(await bytesOf(await w.fetchPath('/content/pillow/bg.jpg')));
    assert.equal(info.format, 'JPEG');
    assert.equal(info.mode, 'RGB');
    assert.equal(info.progressive, false);
    assert.deepEqual(info.size, [16, 16]);
    assert.ok(info.extrema.every(([, max]) => max <= 4));
  });

  it('404s non-image misses and logs exactly one miss line each', async () => {
    const mark = w.misses().length;
    for (const path of ['/content/_dlcbundle/missing.txt?v=2', '/content/_dlcbundle/m.csv', '/content/sam/x.sam',
      '/content/sound/m.mp3', '/mst/Ver0_probe.dat', '/mst/x/Missing_640x384.png']) {
      const res = await w.fetchPath(path);
      assert.equal(res.status, 404, path);
      assert.equal(res.headers.get('x-bf-placeholder'), null, path);
      assert.equal(res.headers.get('cache-control'), 'public, max-age=60', path);
      await res.arrayBuffer();
    }
    const logged = await w.missesAfter(mark, 'mst/x/Missing_640x384.png');
    assert.deepEqual(logged, [
      { bf_miss: 'content/_dlcbundle/missing.txt', result: '404', ua: GAME_UA },
      { bf_miss: 'content/_dlcbundle/m.csv', result: '404', ua: GAME_UA },
      { bf_miss: 'content/sam/x.sam', result: '404', ua: GAME_UA },
      { bf_miss: 'content/sound/m.mp3', result: '404', ua: GAME_UA },
      { bf_miss: 'mst/Ver0_probe.dat', result: '404', ua: GAME_UA },
      { bf_miss: 'mst/x/Missing_640x384.png', result: '404', ua: GAME_UA },
    ]);
  });

  it('keeps art that needs real files at 404 instead of a placeholder, in any folder', async () => {
    const mark = w.misses().length;
    for (const path of ['/content/_dlcbundle/TUT_SYN_IMG_PG1.png', '/content/dungeon/sp_quest_banner_800141_2.png?v=3',
      '/content/_dlcbundle/dungeon_battle_701100.jpg']) {
      const res = await w.fetchPath(path);
      assert.equal(res.status, 404, path);
      assert.equal(res.headers.get('x-bf-placeholder'), null, path);
      await res.arrayBuffer();
    }
    const logged = await w.missesAfter(mark, 'content/_dlcbundle/dungeon_battle_701100.jpg');
    assert.deepEqual(logged.map((l) => l.result), ['404', '404', '404']);
    // A similar name that is not on the list still gets one.
    const other = await w.fetchPath('/content/_dlcbundle/TUT_SYN_IMG_PG9.png');
    assert.equal(other.status, 200);
    assert.equal(other.headers.get('x-bf-placeholder'), '1');
    await other.arrayBuffer();
  });

  it('logs placeholders once, and never logs hits', async () => {
    const mark = w.misses().length;
    await (await w.fetchPath('/content/unit/img/real.png')).arrayBuffer();
    await (await w.fetchPath('/content/_dlcbundle/anim_tex.png')).arrayBuffer();
    await (await w.fetchPath('/content/log/ph_32x32.png', { headers: { 'user-agent': 'x'.repeat(200) } })).arrayBuffer();
    const logged = await w.missesAfter(mark, 'content/log/ph_32x32.png');
    assert.deepEqual(logged, [{ bf_miss: 'content/log/ph_32x32.png', result: 'placeholder', ua: 'x'.repeat(64) }]);
  });

  it('404s traversal, backslash, NUL, bad escapes and non-asset paths without logging', async () => {
    const mark = w.misses().length;
    for (const path of ['/content/%2e%2e/_meta/aliases.json', '/content/..%2f_meta/secret.png', '/mst/%2E%2E/x.png',
      '/content/a%5cb.png', '/content/a%00.png', '/content/%E0%A4%A.png', '/_meta/aliases.json',
      '/_meta/secret.png', '/', '/favicon.ico', '/contentx/a.png', '/content/.%2fx.png', '/mst/a/.%2F/b.dat',
      '/content%2f..%2f_meta/secret.png', '/%2e%2e/_meta/secret.png']) {
      const res = await w.fetchPath(path);
      assert.equal(res.status, 404, path);
      assert.equal(res.headers.get('x-bf-placeholder'), null, path);
      await res.arrayBuffer();
    }
    // A marker miss: once it is logged, anything the requests above logged is in too.
    await (await w.fetchPath('/content/marker.txt')).arrayBuffer();
    assert.deepEqual((await w.missesAfter(mark, 'content/marker.txt')).map((m) => m.bf_miss), ['content/marker.txt']);
  });

  it('rejects other methods with 405', async () => {
    for (const method of ['POST', 'PUT', 'DELETE', 'OPTIONS']) {
      const res = await w.fetchPath('/content/unit/img/real.png', { method });
      assert.equal(res.status, 405, method);
      assert.equal(res.headers.get('allow'), 'GET, HEAD');
      await res.arrayBuffer();
    }
  });

  it('answers HEAD for real files and placeholders without a body', async () => {
    const real = await w.fetchPath('/content/other/other.png', { method: 'HEAD' });
    assert.equal(real.status, 200);
    assert.equal(real.headers.get('content-length'), String(OTHER_PNG.length));
    assert.ok(real.headers.get('etag'));
    assert.equal((await bytesOf(real)).length, 0);
    const cached = await w.fetchPath('/content/other/other.png', { method: 'HEAD' }); // now from caches.default
    assert.equal(cached.status, 200);
    assert.equal((await bytesOf(cached)).length, 0);

    const get = await bytesOf(await w.fetchPath('/content/head/h_64x32.png'));
    const head = await w.fetchPath('/content/head/h_64x32.png', { method: 'HEAD' });
    assert.equal(head.status, 200);
    assert.equal(head.headers.get('x-bf-placeholder'), '1');
    assert.equal(head.headers.get('content-length'), String(get.length));
    assert.equal((await bytesOf(head)).length, 0);

    const missing = await w.fetchPath('/content/head/none.txt', { method: 'HEAD' });
    assert.equal(missing.status, 404);
  });
});

describe('cdn worker with a missing or broken aliases file or name index', () => {
  async function withSeed(change, check) {
    const seed = { ...SEED, ...change };
    for (const [key, value] of Object.entries(change)) if (value === undefined) delete seed[key];
    const w = await startWorker(seed);
    try {
      await check(w);
    } finally {
      await w.mf.dispose();
    }
  }

  it('treats a missing aliases.json as "no aliases"', () =>
    withSeed({ '_meta/aliases.json': undefined }, async (w) => {
      assert.deepEqual(await bytesOf(await w.fetchPath('/content/unit/img/real.png')), REAL_PNG);
      assert.deepEqual(await bytesOf(await w.fetchPath('/content/_dlcbundle/anim_tex.png')), ANIM_PNG);
      const alias = await w.fetchPath('/content/a/base_alias.png');
      assert.equal(alias.headers.get('x-bf-placeholder'), '1');
      assert.equal(decodePng(await bytesOf(alias)).width, 4);
      assert.equal((await w.fetchPath('/content/a/missing.txt')).status, 404);
    }));

  // An unusable file means a miss may not be real: 503 (the client retries
  // later) rather than a placeholder that would stay on the phone.
  const broken = {
    'invalid JSON': '{"base_alias.png": ',
    'JSON array': '["content/unit/img/real.png"]',
    'JSON null': 'null',
  };
  for (const [label, aliases] of Object.entries(broken)) {
    it(`serves hits but no placeholders when aliases.json is ${label}`, () =>
      withSeed({ '_meta/aliases.json': aliases }, async (w) => {
        assert.deepEqual(await bytesOf(await w.fetchPath('/content/unit/img/real.png')), REAL_PNG);
        assert.deepEqual(await bytesOf(await w.fetchPath('/content/_dlcbundle/anim_tex.png')), ANIM_PNG);
        for (const path of ['/content/a/base_alias.png', '/content/a/missing.txt', '/content/a/gone.jpg']) {
          const res = await w.fetchPath(path);
          assert.equal(res.status, 503, path);
          assert.equal(res.headers.get('cache-control'), 'no-store', path);
          assert.equal(res.headers.get('x-bf-placeholder'), null, path);
          await res.arrayBuffer();
        }
      }));
  }

  it('serves exact hits and aliases but no placeholders when the name index is missing', () =>
    withSeed({ '_meta/name-index.json': undefined }, async (w) => {
      assert.deepEqual(await bytesOf(await w.fetchPath('/content/unit/img/real.png')), REAL_PNG);
      assert.deepEqual(await bytesOf(await w.fetchPath('/content/a/base_alias.png')), REAL_PNG);
      const mark = w.misses().length;
      const res = await w.fetchPath('/content/_dlcbundle/anim_tex.png');
      assert.equal(res.status, 503);
      await res.arrayBuffer();
      const logged = await w.missesAfter(mark, 'content/_dlcbundle/anim_tex.png');
      assert.deepEqual(logged.map((m) => m.result), ['503']);
    }));
});

describe('placeholderSize', () => {
  const cases = {
    'foo_640x384.png': [640, 384],
    'MapVillage_640x384.PNG': [640, 384],
    'a_1x1.png': [1, 1],
    'a_2048x2048.png': [2048, 2048],
    'a_2049x10.png': [2048, 10],
    'a_8192x8192.png': [2048, 2048],
    'a_8193x10.png': [4, 4],
    'a_99999x99999.png': [4, 4],
    'a_0x0.png': [4, 4],
    'a_640x384.jpg': [4, 4],
    'a640x384.png': [4, 4],
    'a_640x384_b.png': [4, 4],
    'a_-1x5.png': [4, 4],
    'plain.png': [4, 4],
  };
  for (const [name, size] of Object.entries(cases)) {
    it(`${name} -> ${size.join('x')}`, () => assert.deepEqual(placeholderSize(name), size));
  }
});

describe('/diag download check', () => {
  const PART = 'Ver1102_2r9cNSdt_1.dat';
  let w;
  before(async () => {
    w = await startWorker({ [`mst/${PART}`]: 'abc' });
  });
  after(async () => {
    await w?.mf.dispose();
  });

  it('lists the expected size and crc of each named part', async () => {
    const res = await w.fetchPath(`/diag?f=${PART}`);
    assert.equal(res.status, 200);
    assert.equal(res.headers.get('cache-control'), 'no-store, no-transform');
    const html = await res.text();
    // crc32('abc') = 352441c2
    assert.match(html, /"name":"Ver1102_2r9cNSdt_1\.dat","size":3,"crc":"352441c2"/);
  });

  it('refuses names that are not mst parts, and unknown parts', async () => {
    for (const f of ['', '../secret', 'Ver1_x.dat', `${PART},<script>`, Array(100).fill(PART).join(',')]) {
      const res = await w.fetchPath(`/diag?f=${encodeURIComponent(f)}`);
      assert.equal(res.status, 400, f);
      assert.doesNotMatch(await res.text(), /<script>/);
    }
    const missing = await w.fetchPath('/diag?f=Ver9_2r9cNSdt.dat');
    assert.equal(missing.status, 404);
  });

  it('echoes request headers without secrets, IPs or subscriber ids', async () => {
    const res = await w.fetchPath('/diag/echo', {
      headers: { cookie: 'a=b', authorization: 'x', via: '1.1 carrier-proxy', 'x-forwarded-for': '198.51.100.7',
                 'x-msisdn': '15551234567' },
    });
    const text = await res.text();
    assert.match(text, /via: 1\.1 carrier-proxy/);
    assert.match(text, /x-forwarded-for: \(hidden\)/);
    assert.doesNotMatch(text, /cookie|authorization|107\.123|15551234567/i);
  });

  it('sends https visits back to http, where the check means something', async () => {
    const res = await w.mf.dispatchFetch(`https://cdn.example.com/diag?f=${PART}`, { redirect: 'manual' });
    assert.equal(res.status, 302);
    assert.equal(res.headers.get('location'), `http://cdn.example.com/diag?f=${PART}`);
  });

  it('serves an entry cached before no-transform with the current header', async () => {
    const cache = await w.mf.getCaches();
    await cache.default.put(new Request(`${ORIGIN}/mst/old.dat`),
      new Response('old', { headers: { 'cache-control': 'public, max-age=86400' } }));
    const res = await w.fetchPath('/mst/old.dat');
    assert.equal(await res.text(), 'old');
    assert.equal(res.headers.get('cache-control'), 'public, max-age=86400, no-transform');
  });
});
