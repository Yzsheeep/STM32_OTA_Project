import assert from 'node:assert/strict';
import test from 'node:test';
import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { request } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { DatabaseSync } from 'node:sqlite';
import { crc32 } from 'node:zlib';

const service = fileURLToPath(new URL('./service.mjs', import.meta.url));
const defaultBin = new URL('../app_f411/MDK-ARM/app_f411/app_f411.bin', import.meta.url);

test('F411 firmware HTTP service', { timeout: 30000 }, async t => {
  const original = await readFile(process.env.OTA_TEST_BIN || defaultBin);
  const marker = original.toString('latin1').match(/APP_VERSION=([0-9]+\.[0-9]+\.[0-9]+)\r\n\x00/);
  assert.ok(marker, 'Expected the Keil F411 App BIN');
  const version = marker[1];
  const folder = await mkdtemp(join(tmpdir(), 'f411-api-'));
  const token = randomBytes(32).toString('base64url');
  const secret = join(folder, 'secret');
  const database = join(folder, 'firmware.sqlite');
  let child;
  let api;
  async function stop() {
    if (child && child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill();
      await exited;
    }
  }
  t.after(async () => { await stop(); await rm(folder, { recursive: true, force: true }); });
  await writeFile(secret, token, { mode: 0o600 });
  async function start() {
    child = spawn(process.execPath, [service], {
      env: { ...process.env, OTA_DB_PATH: database, OTA_TOKEN_FILE: secret,
        OTA_BIND: '127.0.0.1', OTA_PORT: '0' }, windowsHide: true,
      stdio: ['ignore', 'pipe', 'inherit']
    });
    const port = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('exit', code => reject(new Error('Server exited: ' + code)));
      child.stdout.on('data', chunk => {
        const match = /Listening on port (\d+)/.exec(chunk);
        if (match) resolve(match[1]);
      });
    });
    api = 'http://127.0.0.1:' + port + '/api/v1/targets/f411-ota';
  }
  function admin(path, method, body, auth = token) {
    return fetch(api + '/admin/releases' + path, { method, body,
      headers: { Authorization: 'Bearer ' + auth, 'Content-Type': 'application/octet-stream' } });
  }
  const upload = (v, bytes) => admin('?version=' + v, 'POST', bytes);
  const publish = v => admin('/' + v + '/publish', 'POST');
  async function status(response, expected) {
    const res = await response;
    assert.equal(res.status, expected, await res.text());
  }
  await start();

  await t.test('empty catalog and all administrative routes require credentials', async () => {
    await status(fetch(api + '/latest'), 404);
    for (const [suffix, method] of [['?version=1.0.0', 'POST'], ['/1.0.0/publish', 'POST'], ['/1.0.0', 'DELETE']]) {
      await status(fetch(api + '/admin/releases' + suffix, { method }), 401);
      await status(admin(suffix, method, undefined, 'wrong'), 401);
    }
    await status(fetch(api + '/releases/0.0.0/firmware.bin'), 404);
  });
  await t.test('rejects oversized, truncated, incompatible and mislabeled uploads', async () => {
    await status(upload(version, Buffer.alloc(16385)), 413);
    await status(upload(version, original.subarray(0, -1)), 422);
    for (const offset of [0, 4]) {
      const bad = Buffer.from(original);
      bad.writeUInt32LE(0, offset);
      await status(upload(version, bad), 422);
    }
    await status(upload('256.0.0', original), 422);
    await status(upload(version === '0.0.0' ? '0.0.1' : '0.0.0', original), 422);
    await status(fetch(api + '/admin/releases?version=' + version, { method: 'POST',
      headers: { Authorization: 'Bearer ' + token }, body: original }), 415);
  });
  await t.test('aborted upload leaves no release or version reservation', async () => {
    const req = request(api + '/admin/releases?version=' + version, { method: 'POST', headers: {
      Authorization: 'Bearer ' + token, 'Content-Type': 'application/octet-stream',
      'Content-Length': original.length } });
    req.on('error', () => {});
    await new Promise(resolve => req.write(original.subarray(0, 128), resolve));
    req.destroy();
    await status(publish(version), 404);
  });
  await t.test('draft isolation, deletion, concurrent duplicate, publish and immutability', async () => {
    await status(upload(version, original), 201);
    await status(fetch(api + '/releases/' + version), 404);
    await status(fetch(api + '/releases/' + version + '/firmware.bin'), 404);
    await status(fetch(api + '/latest'), 404);
    await status(admin('/' + version, 'DELETE'), 204);
    const results = await Promise.all([upload(version, original), upload(version, original)]);
    assert.deepEqual(results.map(res => res.status).sort(), [201, 409]);
    await status(publish(version), 200);
    await status(publish(version), 200);
    await status(admin('/' + version, 'DELETE'), 409);
    await status(upload(version, original), 409);
  });
  await t.test('fresh service process returns identical manifest and byte-for-byte BIN', async () => {
    await stop();
    await start();
    const manifest = await (await fetch(api + '/latest')).json();
    assert.deepEqual(manifest, await (await fetch(api + '/releases/' + version)).json());
    assert.equal(manifest.latest_version, version);
    assert.equal(manifest.size, original.length);
    assert.equal(manifest.sha256, createHash('sha256').update(original).digest('hex'));
    assert.equal(manifest.crc32, crc32(original).toString(16).toUpperCase().padStart(8,'0'));
    const file = await fetch(new URL(manifest.download_url, api));
    assert.equal(file.status, 200);
    assert.equal(Number(file.headers.get('content-length')), original.length);
    assert.deepEqual(Buffer.from(await file.arrayBuffer()), original);
  });
  await t.test('numeric latest selection independent of publish order', async () => {
    for (const v of ['254.10.0', '254.9.0']) {
      if (v === version) continue;
      // Change only an in-memory test copy. These bytes never leave localhost.
      const raw = Buffer.from(original.toString('latin1').replace(marker[0],
        'APP_VERSION=' + v + '\r\n\0'), 'latin1');
      const aligned = Buffer.concat([raw, Buffer.alloc((4 - raw.length % 4) % 4)]);
      await status(upload(v, aligned), 201);
      await status(publish(v), 200);
    }
    const latest = await (await fetch(api + '/latest')).json();
    const order = v => v.split('.').reduce((a, b) => 256 * a + Number(b), 0);
    assert.equal(latest.latest_version, order(version) > order('254.10.0') ? version : '254.10.0');
  });
  await t.test('damaged stored image is refused after restart', async () => {
    await stop();
    const db = new DatabaseSync(database);
    const bad = Buffer.from(original);
    bad[bad.length - 1] ^= 1;
    db.prepare('UPDATE releases SET image=? WHERE version=?').run(bad, version);
    db.close();
    await start();
    await status(fetch(api + '/releases/' + version + '/firmware.bin'), 500);
    await status(publish(version), 500);
  });
});
