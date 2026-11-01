// Smoke test for a Node.js build on Haiku. Works on Node 10 and later.
// Prints one line per check and exits non-zero if any failed.
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const zlib = require('zlib');
const https = require('https');
const dns = require('dns');
const major = Number(process.versions.node.split('.')[0]);

const results = [];
async function check(name, fn) {
  try {
    const detail = await fn();
    results.push([name, true, detail]);
  } catch (e) {
    results.push([name, false, (e && e.stack) || String(e)]);
  }
}

function httpsGet(url) {
  return new Promise((resolve, reject) => {
    https.get(url, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, body: Buffer.concat(chunks) }));
    }).on('error', reject).setTimeout(30000, function () { this.destroy(new Error('timeout')); });
  });
}

(async () => {
  await check('platform', () => {
    assert.strictEqual(process.platform, 'haiku');
    assert.strictEqual(process.arch, 'x64');
    return `${process.version} ${process.platform}-${process.arch} v8 ${process.versions.v8} uv ${process.versions.uv} openssl ${process.versions.openssl} icu ${process.versions.icu}`;
  });
  await check('fs', async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nodetest-'));
    const f = path.join(dir, 'a.txt');
    fs.writeFileSync(f, 'hello haiku\n');
    assert.strictEqual(fs.readFileSync(f, 'utf8'), 'hello haiku\n');
    await fs.promises.appendFile(f, 'more\n');
    assert.strictEqual((await fs.promises.stat(f)).size, 17);
    fs.renameSync(f, f + '.2');
    assert.deepStrictEqual(fs.readdirSync(dir), ['a.txt.2']);
    const watcher = typeof fs.watch === 'function';
    fs.unlinkSync(f + '.2');
    fs.rmdirSync(dir);
    return `ok (tmpdir ${os.tmpdir()}, fs.watch present: ${watcher})`;
  });
  await check('child_process', async () => {
    const out = cp.execSync('uname -s', { encoding: 'utf8' }).trim();
    assert.strictEqual(out, 'Haiku');
    const r = cp.spawnSync(process.execPath, ['-e', 'process.stdout.write(String(6*7))'], { encoding: 'utf8' });
    assert.strictEqual(r.stdout, '42');
    const code = await new Promise((resolve) => cp.spawn('sh', ['-c', 'exit 3']).on('exit', resolve));
    assert.strictEqual(code, 3);
    return `uname=${out}, spawn/exec ok`;
  });
  await check('crypto', () => {
    const h = crypto.createHash('sha256').update('abc').digest('hex');
    assert.strictEqual(h, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    const key = crypto.randomBytes(32), iv = crypto.randomBytes(12);
    const c = crypto.createCipheriv('aes-256-gcm', key, iv);
    const enc = Buffer.concat([c.update('secret'), c.final()]);
    const d = crypto.createDecipheriv('aes-256-gcm', key, iv);
    d.setAuthTag(c.getAuthTag());
    assert.strictEqual(Buffer.concat([d.update(enc), d.final()]).toString(), 'secret');
    const { publicKey, privateKey } = crypto.generateKeyPairSync('ec', { namedCurve: 'P-256' });
    const sig = crypto.createSign('sha256').update('x').sign(privateKey);
    assert(crypto.createVerify('sha256').update('x').verify(publicKey, sig));
    return `sha256 ok, aes-256-gcm ok, ecdsa ok`;
  });
  await check('tls-https', async () => {
    const r = await httpsGet('https://nodejs.org/dist/index.json');
    assert.strictEqual(r.status, 200);
    const n = JSON.parse(r.body.toString()).length;
    assert(n > 100);
    return `https.get index.json: ${n} releases`;
  });
  await check('tls-fetch', async () => {
    if (typeof fetch !== 'function') return 'skipped: no global fetch in this version';
    const r = await fetch('https://nodejs.org/dist/index.json');
    assert.strictEqual(r.status, 200);
    return `fetch index.json: ${(await r.json()).length} releases`;
  });
  await check('dns', async () => {
    const a = await dns.promises.lookup('nodejs.org');
    const r = await dns.promises.resolve4('nodejs.org');
    return `lookup ${a.address}, resolve4 ${r.length} addresses`;
  });
  await check('zlib', () => {
    const data = Buffer.from('haiku '.repeat(1000));
    assert(zlib.gunzipSync(zlib.gzipSync(data)).equals(data));
    assert(zlib.inflateRawSync(zlib.deflateRawSync(data)).equals(data));
    if (!zlib.brotliCompressSync) return `gzip ${zlib.gzipSync(data).length} bytes, no brotli in this version`;
    assert(zlib.brotliDecompressSync(zlib.brotliCompressSync(data)).equals(data));
    return `gzip ${zlib.gzipSync(data).length} bytes, brotli ${zlib.brotliCompressSync(data).length} bytes`;
  });
  await check('intl', () => {
    // Node.js 10 to 12 bundle "small ICU" (English only), like nodejs.org's own builds.
    if (process.config.variables.icu_small) {
      assert.strictEqual((1234567.891).toLocaleString('en-US'), '1,234,567.891');
      return `small ICU ${process.versions.icu}: en-US 1,234,567.891; tz ${Intl.DateTimeFormat().resolvedOptions().timeZone}`;
    }
    assert.strictEqual((1234567.891).toLocaleString('de-DE'), '1.234.567,891');
    const d = new Date(Date.UTC(2026, 8, 26, 12, 0, 0));
    const ja = new Intl.DateTimeFormat('ja-JP', { dateStyle: 'full', timeZone: 'Asia/Tokyo' }).format(d);
    assert(ja.includes('2026年9月26日'), ja);
    const fr = d.toLocaleDateString('fr-FR', { month: 'long', timeZone: 'UTC' });
    assert.strictEqual(fr, 'septembre');
    const plural = new Intl.PluralRules('ar-EG').select(3);
    assert.strictEqual(plural, 'few');
    return `de-DE 1.234.567,891; ja-JP ${ja}; fr-FR ${fr}; tz ${Intl.DateTimeFormat().resolvedOptions().timeZone}`;
  });
  await check('worker_threads', async () => {
    if (major < 11) {
      // Node 10 has worker_threads behind --experimental-worker.
      const r = cp.spawnSync(process.execPath, ['--experimental-worker', '-e',
        "const { Worker } = require('worker_threads'); const w = new Worker(\"require('worker_threads').parentPort.postMessage(42)\", { eval: true }); w.on('message', (m) => { process.stdout.write(String(m)); w.terminate(); });"],
        { encoding: 'utf8' });
      assert.strictEqual(r.stdout, '42', r.stderr);
      return 'worker round trip ok (--experimental-worker)';
    }
    const { Worker } = require('worker_threads');
    const w = new Worker("const { parentPort } = require('worker_threads'); parentPort.on('message', (m) => parentPort.postMessage(m * 2));", { eval: true });
    const v = await new Promise((resolve, reject) => {
      w.on('message', resolve); w.on('error', reject); w.postMessage(21);
    });
    await w.terminate();
    assert.strictEqual(v, 42);
    return 'worker round trip ok';
  });
  await check('os', () => {
    const cpus = os.cpus();
    assert(cpus.length > 0);
    return `${cpus.length} cpus (${cpus[0].model.trim()}), ${Math.round(os.totalmem() / 2 ** 20)} MiB, uptime ${Math.round(os.uptime())} s, ${Object.keys(os.networkInterfaces()).length} interfaces`;
  });
  let ok = true;
  for (const [name, pass, detail] of results) {
    if (!pass) ok = false;
    console.log(`${pass ? 'PASS' : 'FAIL'} ${name}: ${detail}`);
  }
  process.exit(ok ? 0 : 1);
})();
