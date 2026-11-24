import http from 'node:http';
import { createHash, timingSafeEqual } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';
import { crc32 } from 'node:zlib';

const target = 'f411-ota';
const base = '/api/v1/targets/' + target;
const appAddress = 0x08010000;
const maxSize = 16384;
const versionPattern = /^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$/;

class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function versionCode(version) {
  if (!versionPattern.test(version) || version.split('.').some(part => Number(part) > 255))
    throw new ApiError(422, 'Expected a canonical x.y.z version with components 0..255');
  return version.split('.').reduce((value, part) => value * 256 + Number(part), 0);
}

function checkImage(image, version) {
  if (image.length < 8 || image.length > maxSize || image.length % 4)
    throw new ApiError(422, 'Expected an aligned F411 App BIN of 8..16384 bytes');
  const stack = image.readUInt32LE(0);
  const entry = image.readUInt32LE(4);
  if (stack <= 0x20000000 || stack > 0x20020000 || stack % 8 ||
      !(entry & 1) || entry < appAddress || entry - appAddress >= image.length)
    throw new ApiError(422, 'Invalid F411 stack or reset vector');
  const markers = image.toString('latin1').matchAll(/APP_VERSION=([0-9]+\.[0-9]+\.[0-9]+)\r\n\x00/g);
  const versions = new Set(Array.from(markers, match => match[1]));
  if (versions.size !== 1 || !versions.has(version))
    throw new ApiError(422, 'Embedded APP_VERSION must match the release version');
}

function digest(bytes) {
  return createHash('sha256').update(bytes).digest();
}

function checksum(bytes) {
  return crc32(bytes).toString(16).toUpperCase().padStart(8, '0');
}

function verifyStored(row) {
  const bytes = Buffer.from(row.image);
  if (bytes.length !== row.size || digest(bytes).toString('hex') !== row.sha256 ||
      checksum(bytes) !== row.crc32)
    throw new ApiError(500, 'Stored firmware integrity check failed');
  checkImage(bytes, row.version);
  return bytes;
}

function manifest(row) {
  return { target, app_address: '0x08010000', latest_version: row.version,
    size: row.size, crc32: row.crc32, sha256: row.sha256, changelog: row.changelog,
    download_url: base + '/releases/' + row.version + '/firmware.bin' };
}

async function readImage(req) {
  if (req.headers['content-type'] !== 'application/octet-stream')
    throw new ApiError(415, 'Send the raw BIN as application/octet-stream');
  if (Number(req.headers['content-length']) > maxSize)
    throw new ApiError(413, 'Firmware exceeds 16384 bytes');
  const chunks = [];
  let size = 0;
  for await (const chunk of req.iterator({ destroyOnReturn: false })) {
    size += chunk.length;
    if (size > maxSize) throw new ApiError(413, 'Firmware exceeds 16384 bytes');
    chunks.push(chunk);
  }
  return Buffer.concat(chunks, size);
}

function json(res, status, body) {
  const data = Buffer.from(JSON.stringify(body));
  res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': data.length });
  res.end(data);
}

const token = readFileSync(process.env.OTA_TOKEN_FILE, 'utf8').trim();
if (!/^[A-Za-z0-9_-]{43,128}$/.test(token))
  throw new Error('OTA_TOKEN_FILE must contain a random base64url token (43..128 characters)');
const expectedAuth = digest('Bearer ' + token);
const db = new DatabaseSync(process.env.OTA_DB_PATH || 'firmware.sqlite');
db.exec(`
  PRAGMA busy_timeout=5000;
  CREATE TABLE IF NOT EXISTS releases (
    version TEXT PRIMARY KEY, code INTEGER NOT NULL UNIQUE, changelog TEXT NOT NULL,
    image BLOB NOT NULL, size INTEGER NOT NULL, sha256 TEXT NOT NULL, crc32 TEXT NOT NULL,
    published INTEGER NOT NULL DEFAULT 0 CHECK (published IN (0,1))
  ) STRICT;
`);

async function handle(req, res) {
  res.setHeader('Cache-Control', 'no-store');
  const url = new URL(req.url, 'http://localhost');
  const path = url.pathname;
  if (!path.startsWith(base + '/')) throw new ApiError(404, 'Unknown route');
  const parts = path.slice(base.length + 1).split('/');
  if (parts[0] === 'admin') {
    if (!timingSafeEqual(expectedAuth, digest(req.headers.authorization || ''))) {
      res.setHeader('WWW-Authenticate', 'Bearer');
      throw new ApiError(401, 'Admin authorization required');
    }
    if (parts[1] === 'releases' && parts.length === 2 && req.method === 'POST') {
      const version = url.searchParams.get('version');
      const code = versionCode(version);
      const changelog = url.searchParams.get('changelog') || '';
      if (Buffer.byteLength(changelog) > 1024) throw new ApiError(422, 'Changelog exceeds 1024 bytes');
      const bytes = await readImage(req);
      checkImage(bytes, version);
      const result = db.prepare(`INSERT INTO releases
        (version, code, changelog, image, size, sha256, crc32) VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(version) DO NOTHING`).run(version, code, changelog, bytes, bytes.length,
          digest(bytes).toString('hex'), checksum(bytes));
      if (!result.changes) throw new ApiError(409, 'Version already exists');
      return json(res, 201, { version, state: 'draft' });
    }
    const publishing = req.method === 'POST' && parts.length === 4 && parts[3] === 'publish';
    const deleting = req.method === 'DELETE' && parts.length === 3;
    if (parts[1] === 'releases' && (publishing || deleting)) {
      const row = db.prepare('SELECT * FROM releases WHERE version=?').get(parts[2]);
      if (!row) throw new ApiError(404, 'Version not found');
      if (deleting) {
        if (row.published) throw new ApiError(409, 'Published releases are immutable');
        db.prepare('DELETE FROM releases WHERE version=? AND published=0').run(row.version);
        res.writeHead(204).end();
        return;
      }
      verifyStored(row);
      db.prepare('UPDATE releases SET published=1 WHERE version=?').run(row.version);
      return json(res, 200, manifest(row));
    }
  }
  if (req.method === 'GET') {
    const latest = parts.length === 1 && parts[0] === 'latest';
    const file = parts.length === 3 && parts[2] === 'firmware.bin';
    if (latest || (parts[0] === 'releases' && (parts.length === 2 || file))) {
      const row = latest
        ? db.prepare('SELECT * FROM releases WHERE published=1 ORDER BY code DESC LIMIT 1').get()
        : db.prepare('SELECT * FROM releases WHERE published=1 AND version=?').get(parts[1]);
      if (!row) throw new ApiError(404, 'No matching published release');
      const bytes = verifyStored(row);
      if (!file) return json(res, 200, manifest(row));
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': bytes.length });
      res.end(bytes);
      return;
    }
  }
  throw new ApiError(404, 'Unknown route');
}

const server = http.createServer({ requestTimeout: 15000, headersTimeout: 10000 }, (req, res) => {
  handle(req, res).catch(err => {
    if (err instanceof ApiError) {
      if (err.status >= 500) console.error('Firmware service error:', err.message);
    } else {
      console.error('Request failed:', err.code || err.name);
    }
    if (res.destroyed) return;
    res.setHeader('Connection', 'close');
    json(res, err.status || 500, { error: err instanceof ApiError ? err.message : 'Request failed' });
  });
});
server.listen(Number(process.env.OTA_PORT || 18080), process.env.OTA_BIND || '127.0.0.1', () => {
  console.log('Listening on port ' + server.address().port);
});
for (const signal of ['SIGINT', 'SIGTERM'])
  process.on(signal, () => server.close(() => db.close()));
