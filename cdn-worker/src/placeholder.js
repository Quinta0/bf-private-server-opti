// Stand-in images for art the archive does not have.
//
// The client recurses until it crashes when an image fails to decode
// (CommonUtils::getTexture), so everything here must be a strictly valid file
// of the type its extension says. test/worker.test.mjs decodes what the
// worker serves with pngjs / jpeg-js and again with Pillow.

const DEFAULT_DIM = 4;
const MAX_DIM = 2048; // clamp: never build a placeholder bigger than this per side
const ABSURD_DIM = 8192; // a suffix above this is not a real texture size: use the default

// Width/height from a trailing _<W>x<H> in the file name (foo_640x384.png).
export function placeholderSize(name) {
  const m = /_(\d{1,6})x(\d{1,6})\.png$/i.exec(name);
  const w = m ? Number(m[1]) : 0;
  const h = m ? Number(m[2]) : 0;
  if (w < 1 || h < 1 || w > ABSURD_DIM || h > ABSURD_DIM) return [DEFAULT_DIM, DEFAULT_DIM];
  return [Math.min(w, MAX_DIM), Math.min(h, MAX_DIM)];
}

let crcTable = null;

function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// One PNG chunk: length, type, data, CRC over type+data.
function pngChunk(type, data) {
  const out = new Uint8Array(12 + data.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  view.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

// zlib stream (CompressionStream 'deflate' = RFC 1950, as PNG requires) of
// `count` zero bytes, fed in 1 MB blocks so a 2048x2048 image (16 MB raw)
// never needs one huge buffer. Each block is a fresh array, so nothing depends
// on whether the stream keeps or detaches what it was given.
async function zlibOfZeros(count) {
  const stream = new CompressionStream('deflate');
  const output = new Response(stream.readable).arrayBuffer(); // drain while writing
  const writer = stream.writable.getWriter();
  const BLOCK = 1 << 20;
  for (let left = count; left > 0; left -= BLOCK) {
    await writer.write(new Uint8Array(Math.min(left, BLOCK)));
  }
  await writer.close();
  return new Uint8Array(await output);
}

// Fully transparent RGBA8 PNG. Every raw scanline is filter byte 0 followed by
// zeroed pixels, so the whole image body is just zeros.
async function encodeTransparentPng(width, height) {
  const ihdr = new Uint8Array(13);
  const view = new DataView(ihdr.buffer);
  view.setUint32(0, width);
  view.setUint32(4, height);
  ihdr.set([8, 6, 0, 0, 0], 8); // bit depth 8, colour type 6 (RGBA), deflate, no filter, no interlace
  const parts = [
    new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', await zlibOfZeros(height * (1 + 4 * width))),
    pngChunk('IEND', new Uint8Array(0)),
  ];
  const png = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    png.set(p, at);
    at += p.length;
  }
  return png;
}

// Finished PNGs per size, so a popular missing texture is encoded once per
// isolate. Only plain bytes are shared between requests (never a pending
// promise), and the map is bounded.
const pngCache = new Map();
const PNG_CACHE_MAX = 32;

export async function transparentPng(width, height) {
  const key = `${width}x${height}`;
  let png = pngCache.get(key);
  if (!png) {
    png = await encodeTransparentPng(width, height);
    if (pngCache.size >= PNG_CACHE_MAX) pngCache.delete(pngCache.keys().next().value);
    pngCache.set(key, png);
  }
  return png;
}

// 16x16 black baseline JFIF JPEG (3-component YCbCr, 4:2:0, standard Huffman
// tables), the most common layout among the game's own .jpg files. Generated
// once with Pillow:
//   Image.new('RGB', (16, 16), (0, 0, 0)).save(f, 'JPEG', quality=75,
//     subsampling=2, progressive=False, optimize=False)
// Not grayscale and not Adobe-RGB on purpose: cocos2d-x 2.x CCImage only
// converts YCbCr/grayscale JPEGs, and old libjpeg cannot expand gray to RGB.
const JPEG_B64 =
  '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/' +
  '2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAQABADASIAAhEBAxEB/8QA' +
  'HwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkK' +
  'FhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG' +
  'x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAEC' +
  'AxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOE' +
  'hYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5/ooooA//2Q==';

export const PLACEHOLDER_JPEG = Uint8Array.from(atob(JPEG_B64), (c) => c.charCodeAt(0));
