// TEMPORARY upload endpoint for the initial asset load into R2. Delete after use:
//   npx wrangler delete --name bf-asset-uploader
// PUT /<key> with header x-upload-token: <secret> stores the body at <key>.

function sameSecret(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

export default {
  async fetch(request, env) {
    if (request.method !== 'PUT') return new Response('Method not allowed', { status: 405 });
    if (!env.UPLOAD_TOKEN || !sameSecret(request.headers.get('x-upload-token'), env.UPLOAD_TOKEN)) {
      return new Response('Forbidden', { status: 403 });
    }
    const key = decodeURIComponent(new URL(request.url).pathname.slice(1));
    if (!/^(content|mst|_meta)\/[^\\\0]+$/.test(key) || key.split('/').includes('..')) {
      return new Response('Bad key', { status: 400 });
    }
    const contentType = request.headers.get('content-type') ?? 'application/octet-stream';
    await env.ASSETS.put(key, request.body, { httpMetadata: { contentType } });
    return new Response('ok');
  },
};
