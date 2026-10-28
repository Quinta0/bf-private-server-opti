// Write-only off-site backup sink for the game server's SQLite snapshots.
// PUT /db/<name>.sqlite.gz with header x-backup-token stores into bf-backups.
// Cloudflare refuses request bodies over 100 MB (HTTP 413 before this code runs), so a
// bigger snapshot goes up as an R2 multipart upload and still lands as one object:
//   POST /db/<name>?uploads    x-content-sha256                      -> upload id
//   PUT  /db/<name>?part=<n>   x-upload-id, one part                 -> part etag
//   POST /db/<name>?complete   x-upload-id, "<n> <etag>" line per part -> ok
// R2 wants every part but the last the same size, and at least 5 MiB.
// No read, list or delete: a leaked token cannot read or destroy old backups,
// and object names are unique per snapshot so nothing is overwritten.

function sameSecret(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

const NAME = /^db\/gme-\d{8}T\d{6}Z\.sqlite\.gz$/;
const MAX_BYTES = 500 * 1024 * 1024;
const MAX_PARTS = 100;
const PART_LINE = /^(\d{1,3}) (\S{1,200})$/;

function storeOptions(request) {
  return {
    httpMetadata: { contentType: 'application/gzip' },
    customMetadata: { sha256: request.headers.get('x-content-sha256') ?? '' },
  };
}

function bodySize(request) {
  const size = Number(request.headers.get('content-length') ?? 0);
  return size && size <= MAX_BYTES ? size : 0;
}

async function multipart(request, env, key, params) {
  if (request.method === 'POST' && params.has('uploads')) {
    if (await env.BACKUPS.head(key)) return new Response('Exists', { status: 409 });
    const upload = await env.BACKUPS.createMultipartUpload(key, storeOptions(request));
    return new Response(upload.uploadId);
  }
  const uploadId = request.headers.get('x-upload-id');
  if (!uploadId) return new Response('Bad request', { status: 400 });
  const upload = env.BACKUPS.resumeMultipartUpload(key, uploadId);
  if (request.method === 'PUT' && params.has('part')) {
    const partNumber = Number(params.get('part'));
    if (!Number.isInteger(partNumber) || partNumber < 1 || partNumber > MAX_PARTS) {
      return new Response('Bad part', { status: 400 });
    }
    if (!bodySize(request)) return new Response('Bad size', { status: 413 });
    const part = await upload.uploadPart(partNumber, request.body);
    return new Response(part.etag);
  }
  if (request.method !== 'POST' || !params.has('complete')) {
    return new Response('Bad request', { status: 400 });
  }
  const lines = (await request.text()).split('\n').filter(Boolean);
  const parts = lines.map((line) => PART_LINE.exec(line));
  if (!parts.length || parts.length > MAX_PARTS || parts.some((m) => !m)) {
    return new Response('Bad parts', { status: 400 });
  }
  // The name may have been taken while the parts went up; never overwrite it.
  if (await env.BACKUPS.head(key)) return new Response('Exists', { status: 409 });
  await upload.complete(parts.map((m) => ({ partNumber: Number(m[1]), etag: m[2] })));
  return new Response('ok');
}

export default {
  async fetch(request, env) {
    if (request.method !== 'PUT' && request.method !== 'POST') {
      return new Response('Method not allowed', { status: 405 });
    }
    if (!env.BACKUP_TOKEN || !sameSecret(request.headers.get('x-backup-token'), env.BACKUP_TOKEN)) {
      return new Response('Forbidden', { status: 403 });
    }
    const url = new URL(request.url);
    const key = url.pathname.slice(1);
    if (!NAME.test(key)) return new Response('Bad name', { status: 400 });
    if (url.search) return multipart(request, env, key, url.searchParams);
    if (request.method !== 'PUT') return new Response('Method not allowed', { status: 405 });
    if (!bodySize(request)) return new Response('Bad size', { status: 413 });
    if (await env.BACKUPS.head(key)) return new Response('Exists', { status: 409 });
    await env.BACKUPS.put(key, request.body, storeOptions(request));
    return new Response('ok');
  },
};
