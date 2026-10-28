// Add-only sink for MST client table parts.
// PUT /mst/Ver<N>_<fileKey>[_<part>].dat with header x-upload-token stores into
// bf-assets, which the CDN Worker serves to clients. It can never overwrite:
// the CDN caches each part for a day, so a changed file under an old name
// would leave players with mismatched data. Re-sending identical bytes is
// accepted (200) so an interrupted publish can be retried; different bytes
// under an existing name are refused (409). No read, list or delete.
// Callers must also come from an address in the ALLOWED_IPS variable (the
// production host's IPv4/IPv6), so a leaked token alone is not enough; with
// ALLOWED_IPS unset every request is refused.

function sameSecret(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

function sameBytes(a, b) {
  if (a.byteLength !== b.byteLength) return false;
  const x = new Uint8Array(a);
  const y = new Uint8Array(b);
  for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
  return true;
}

const NAME = /^mst\/Ver\d{1,6}_[A-Za-z0-9]{8}(_\d{1,2})?\.dat$/;
const MAX_BYTES = 8 * 1024 * 1024;

export default {
  async fetch(request, env) {
    if (request.method !== 'PUT') return new Response('Method not allowed', { status: 405 });
    const allowed = (env.ALLOWED_IPS ?? '').split(',').map((ip) => ip.trim()).filter(Boolean);
    if (!allowed.includes(request.headers.get('cf-connecting-ip') ?? '')) {
      return new Response('Forbidden', { status: 403 });
    }
    if (!env.UPLOAD_TOKEN || !sameSecret(request.headers.get('x-upload-token'), env.UPLOAD_TOKEN)) {
      return new Response('Forbidden', { status: 403 });
    }
    const key = new URL(request.url).pathname.slice(1);
    if (!NAME.test(key)) return new Response('Bad name', { status: 400 });
    const size = Number(request.headers.get('content-length') ?? 0);
    if (!size || size > MAX_BYTES) return new Response('Bad size', { status: 413 });
    const body = await request.arrayBuffer();
    if (body.byteLength !== size) return new Response('Bad size', { status: 400 });
    const existing = await env.ASSETS.get(key);
    if (existing) {
      return sameBytes(await existing.arrayBuffer(), body)
        ? new Response('identical', { status: 200 })
        : new Response('Exists with different bytes', { status: 409 });
    }
    await env.ASSETS.put(key, body, { httpMetadata: { contentType: 'application/octet-stream' } });
    return new Response('created', { status: 201 });
  },
};
