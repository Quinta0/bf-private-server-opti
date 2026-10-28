// Download check for players whose network may damage plain-HTTP downloads.
//
//   http://cdn.example.com/diag?f=Ver1102_2r9cNSdt_1.dat,Ver1102_2r9cNSdt_2.dat
//
// The page fetches each named mst/ part over plain HTTP, like the game does,
// and compares the byte count and CRC-32 with the object in R2. /diag/echo
// shows the request headers that reached Cloudflare (a carrier proxy often
// adds Via / X-Forwarded-For). Read-only, no cookies, nothing stored.
// Written for the 2026-10-16 case: one player's phone crashed on new unit
// tables only on T-Mobile mobile data, and loaded them fine on Wi-Fi.

const PART = /^Ver\d{1,6}_[A-Za-z0-9]{8}(_\d{1,2})?\.dat$/;
const MAX_FILES = 99; // the most parts one table is published in
// The page gets screenshotted and shared: hide the player's IP and any subscriber
// id a carrier proxy injects; proxy fingerprints (Via, X-Forwarded-Proto) stay.
const SECRET = /^(cookie|authorization|proxy-authorization)$/i;
const PERSONAL = /^(cf-connecting-ip(v6)?|x-forwarded-for|x-real-ip|true-client-ip|forwarded)$|msisdn|calling-line|subscriber|imsi|imei|nokia-|x-up-/i;
// mst/ keys are add-only, so a part's size and crc never change.
const known = new Map();

let table = null;
function crcTable() {
  if (!table) {
    table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  return table;
}

export function crc32(bytes) {
  const t = crcTable();
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = t[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return ((c ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0');
}

const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (ch) => `&#${ch.charCodeAt(0)};`);

function page(expected) {
  const data = JSON.stringify(expected).replace(/</g, '\\u003c');
  return `<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Download check</title>
<style>body{font:15px/1.4 sans-serif;margin:16px;background:#111;color:#eee}
td,th{padding:4px 6px;border-bottom:1px solid #333;text-align:left}.ok{color:#7d7}.bad{color:#f77}
pre{white-space:pre-wrap;background:#222;padding:8px;font-size:12px}</style></head><body>
<h2>Download check</h2><p id="note">Keep this page open until every row says OK or BAD, then take a screenshot.</p>
<table id="t"><tr><th>file</th><th>expected</th><th>received</th><th>result</th></tr></table>
<h3>Headers the CDN received</h3><pre id="echo">loading…</pre>
<script>
var EXPECTED = ${data};
if (location.protocol !== 'http:') {
  document.getElementById('note').innerHTML = '<b class="bad">This page must be opened over plain http, like the game.</b> '
    + 'In Chrome turn off Settings &gt; Privacy and security &gt; Always use secure connections, then open the http:// link again.';
  EXPECTED = [];
}
var T = new Uint32Array(256);
for (var n = 0; n < 256; n++) { var c = n; for (var k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; T[n] = c >>> 0; }
function crc(b) { var c = 0xffffffff; for (var i = 0; i < b.length; i++) c = T[(c ^ b[i]) & 0xff] ^ (c >>> 8); return ((c ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0'); }
function row(cells, cls) { var tr = document.createElement('tr'); cells.forEach(function (v, i) { var td = document.createElement('td'); td.textContent = v; if (i === 3 && cls) td.className = cls; tr.appendChild(td); }); document.getElementById('t').appendChild(tr); return tr; }
EXPECTED.forEach(function (e) {
  var start = Date.now();
  var x = new XMLHttpRequest();
  x.open('GET', 'http://' + location.host + '/mst/' + e.name + '?diag=' + start);
  x.responseType = 'arraybuffer';
  x.onload = function () {
    var b = new Uint8Array(x.response || new ArrayBuffer(0));
    var got = b.length + ' B, crc ' + crc(b) + ', ' + (Date.now() - start) + ' ms, HTTP ' + x.status
      + (x.getResponseHeader('content-encoding') ? ', enc ' + x.getResponseHeader('content-encoding') : '');
    var ok = b.length === e.size && crc(b) === e.crc;
    row([e.name, e.size + ' B, crc ' + e.crc, got, ok ? 'OK' : 'BAD'], ok ? 'ok' : 'bad');
  };
  x.onerror = function () { row([e.name, e.size + ' B', 'network error after ' + (Date.now() - start) + ' ms', 'BAD'], 'bad'); };
  x.send();
});
var y = new XMLHttpRequest();
y.open('GET', 'http://' + location.host + '/diag/echo?t=' + Date.now());
y.onload = function () { document.getElementById('echo').textContent = y.responseText; };
y.onerror = function () { document.getElementById('echo').textContent = 'could not load the headers (network error)'; };
y.send();
</script></body></html>`;
}

function html(body, status = 200) {
  return new Response(body, {
    status,
    headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store, no-transform' },
  });
}

export async function diag(request, env, url) {
  if (url.pathname === '/diag/echo') {
    const lines = [...request.headers]
      .filter(([k]) => !SECRET.test(k))
      .map(([k, v]) => `${k}: ${PERSONAL.test(k) ? '(hidden)' : v}`);
    const cf = request.cf ?? {};
    lines.push('', `colo: ${cf.colo ?? '?'}  asn: ${cf.asn ?? '?'} (${cf.asOrganization ?? '?'})`);
    return new Response(lines.join('\n'), {
      headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store, no-transform' },
    });
  }
  if (url.protocol === 'https:') {
    // Chrome upgrades typed http links to https; a redirect back to http makes it
    // fall back to http, which the check needs (the game never uses TLS).
    return new Response(null, {
      status: 302,
      headers: { location: `http://${url.host}${url.pathname}${url.search}`, 'cache-control': 'no-store' },
    });
  }
  if (request.method === 'HEAD') return html('');
  const names = (url.searchParams.get('f') ?? '').split(',').map((s) => s.trim()).filter(Boolean);
  if (!names.length || names.length > MAX_FILES || !names.every((n) => PART.test(n))) {
    return html(`<p>Usage: /diag?f=Ver1102_2r9cNSdt_1.dat,Ver1102_2r9cNSdt_2.dat (up to ${MAX_FILES} mst parts)</p>`, 400);
  }
  const expected = [];
  for (const name of names) {
    if (!known.has(name)) {
      const object = await env.ASSETS.get(`mst/${name}`);
      if (!object) return html(`<p>No such part: ${escapeHtml(name)}</p>`, 404);
      const bytes = new Uint8Array(await object.arrayBuffer());
      known.set(name, { size: bytes.length, crc: crc32(bytes) });
    }
    expected.push({ name, ...known.get(name) });
  }
  return html(page(expected));
}
