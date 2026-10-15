// olla-trusted-publishing plan 4.1.1 – 4.1.3 (cajeta-olla/specs/
// olla-trusted-publishing-spec.md §4.2). Run with:
//   node --test .github/actions/olla-publish/olla-publish.test.mjs
//
// Two local stubs stand in for GitHub's identity endpoint and for Olla, so
// the script's choice of bearer is observed, not inferred. The signing key
// is a throwaway Ed25519 key: the script still signs the archive (spec §7).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { generateKeyPairSync } from 'node:crypto';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const script = join(here, 'olla-publish.mjs');

function fixture() {
  const dir = mkdtempSync(join(tmpdir(), 'olla-publish-'));
  writeFileSync(join(dir, 'lib.cja'), 'CAJETA-ARCHIVE\nfixture\n');
  writeFileSync(join(dir, 'cajeta.json'), '{ "details": { "name": "dev.example.lib" } }\n');
  return dir;
}

const pem = generateKeyPairSync('ed25519').privateKey.export({ type: 'pkcs8', format: 'pem' });

/** One server for both roles: /token answers GitHub's shape, /v2/publish Olla's. */
async function stubs() {
  const seen = { tokenRequests: [], publishBearer: null };
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    if (url.pathname === '/token') {
      seen.tokenRequests.push({ audience: url.searchParams.get('audience'), auth: req.headers.authorization });
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ value: 'eyJhbGciOiJSUzI1NiJ9.e30.c2ln' }));
      return;
    }
    if (url.pathname === '/v2/publish') {
      seen.publishBearer = req.headers.authorization ?? null;
      req.resume();
      req.on('end', () => {
        res.statusCode = 201;
        res.end('{}');
      });
      return;
    }
    res.statusCode = 404;
    res.end();
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}`;
  // fetch keeps its sockets alive; close() alone would wait on them forever.
  const close = () =>
    new Promise((r) => {
      server.close(r);
      server.closeAllConnections();
    });
  return { base, seen, close };
}

// Asynchronous on purpose: the stubs live in THIS process, so a blocking
// spawn would starve the server the child is waiting on.
function run(dir, env) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [script, join(dir, 'lib.cja'), join(dir, 'cajeta.json'), '0.1.0'], {
      env: {
        PATH: process.env.PATH,
        OLLA_SIGNING_KEY_PEM: pem,
        OLLA_SIGNING_KEY_ID: 'ci-key-1',
        ...env,
      },
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += d));
    child.stderr.on('data', (d) => (stderr += d));
    child.on('close', (status) => resolve({ status, stdout, stderr }));
  });
}

test('4.1.1 with no OLLA_TOKEN it requests an identity token for Olla and sends it as the bearer', async () => {
  const s = await stubs();
  try {
    const r = await run(fixture(), {
      OLLA_URL: s.base,
      ACTIONS_ID_TOKEN_REQUEST_URL: `${s.base}/token?api-version=2`,
      ACTIONS_ID_TOKEN_REQUEST_TOKEN: 'runner-secret',
    });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(s.seen.tokenRequests.length, 1);
    assert.equal(s.seen.tokenRequests[0].audience, 'olla.cajeta.dev');
    assert.equal(s.seen.tokenRequests[0].auth, 'bearer runner-secret');
    assert.equal(s.seen.publishBearer, 'Bearer eyJhbGciOiJSUzI1NiJ9.e30.c2ln');
  } finally {
    await s.close();
  }
});

test('4.1.2 with OLLA_TOKEN set it sends that token and asks GitHub for nothing', async () => {
  const s = await stubs();
  try {
    const r = await run(fixture(), {
      OLLA_URL: s.base,
      OLLA_TOKEN: 'olla-publish-1234',
      ACTIONS_ID_TOKEN_REQUEST_URL: `${s.base}/token?api-version=2`,
      ACTIONS_ID_TOKEN_REQUEST_TOKEN: 'runner-secret',
    });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(s.seen.tokenRequests.length, 0);
    assert.equal(s.seen.publishBearer, 'Bearer olla-publish-1234');
  } finally {
    await s.close();
  }
});

test('4.1.3 with neither it fails naming the id-token permission', async () => {
  const s = await stubs();
  try {
    const r = await run(fixture(), { OLLA_URL: s.base });
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /id-token: write/);
    assert.equal(s.seen.publishBearer, null);
  } finally {
    await s.close();
  }
});
