// Signed publish of a built .cja to the Olla registry (olla.cajeta.dev).
//
//   node olla-publish.mjs <cja-path> <manifest-path> <version> [readme-path]
//
// Env (set by the composite action):
//   OLLA_URL              registry base (default https://olla.cajeta.dev)
//   OLLA_TOKEN            optional publish bearer token (publish_tokens row).
//                         Without it the script asks GitHub for an OIDC
//                         identity token for Olla's audience and sends that
//                         (trusted publishing, cajeta-olla/specs/
//                         olla-trusted-publishing-spec.md §4.2); the job needs
//                         `permissions: id-token: write`.
//   OLLA_AUDIENCE         audience for the identity token (default olla.cajeta.dev)
//   OLLA_SIGNING_KEY_PEM  Ed25519 private key, PKCS#8 PEM (CI publisher key)
//   OLLA_SIGNING_KEY_ID   key-id inside the organization's signed key document
//
// The manifest file is the repo's cajeta.json (JSONC — comments/trailing commas
// are stripped before parsing). Package name comes from manifest.details.name;
// the .cja bytes are signed raw (detached Ed25519, 64 bytes) per §15.
import { readFileSync, existsSync } from 'node:fs';
import { createHash, sign as edSign, createPrivateKey } from 'node:crypto';

const [cjaPath, manifestPath, version, readmePath] = process.argv.slice(2);
const BASE = (process.env.OLLA_URL ?? 'https://olla.cajeta.dev').replace(/\/$/, '');
const TOKEN = process.env.OLLA_TOKEN;
const KEY_PEM = process.env.OLLA_SIGNING_KEY_PEM;
const KEY_ID = process.env.OLLA_SIGNING_KEY_ID;

function die(msg) { console.error(`olla-publish: ${msg}`); process.exit(1); }
if (!cjaPath || !manifestPath || !version) die('usage: olla-publish.mjs <cja> <manifest> <version> [readme]');
for (const [k, v] of [['OLLA_SIGNING_KEY_PEM', KEY_PEM], ['OLLA_SIGNING_KEY_ID', KEY_ID]])
  if (!v) die(`missing ${k}`);

// The bearer: a configured publish token, else a GitHub Actions identity token
// minted for Olla's audience. GitHub exposes the request endpoint only when the
// job grants `id-token: write`, which is the message when neither exists.
async function bearer() {
  if (TOKEN) return TOKEN;
  const url = process.env.ACTIONS_ID_TOKEN_REQUEST_URL;
  const reqToken = process.env.ACTIONS_ID_TOKEN_REQUEST_TOKEN;
  if (!url || !reqToken) {
    die('no OLLA_TOKEN and no identity token endpoint: grant the job `permissions: id-token: write` (trusted publishing) or set OLLA_TOKEN');
  }
  const audience = process.env.OLLA_AUDIENCE ?? 'olla.cajeta.dev';
  const sep = url.includes('?') ? '&' : '?';
  const res = await fetch(`${url}${sep}audience=${encodeURIComponent(audience)}`, {
    headers: { Authorization: `bearer ${reqToken}`, Accept: 'application/json' },
  });
  if (!res.ok) die(`identity token request failed: HTTP ${res.status} ${await res.text()}`);
  const body = await res.json();
  if (!body || typeof body.value !== 'string') die('identity token response carried no value');
  return body.value;
}
const BEARER = await bearer();

// Strip JSONC comments/trailing commas so JSON.parse accepts cajeta.json.
function parseJsonc(text) {
  let out = '', i = 0, inStr = false, q = '', esc = false;
  while (i < text.length) {
    const c = text[i];
    if (inStr) { out += c; if (esc) esc = false; else if (c === '\\') esc = true; else if (c === q) inStr = false; i++; continue; }
    if (c === '"' || c === "'") { inStr = true; q = c; out += c; i++; continue; }
    if (c === '/' && text[i + 1] === '/') { while (i < text.length && text[i] !== '\n') i++; continue; }
    if (c === '/' && text[i + 1] === '*') { i += 2; while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) i++; i += 2; continue; }
    out += c; i++;
  }
  return JSON.parse(out.replace(/,(\s*[}\]])/g, '$1'));
}

const cja = readFileSync(cjaPath);
const manifestObj = parseJsonc(readFileSync(manifestPath, 'utf8'));
const name = manifestObj?.details?.name;
if (!name) die(`${manifestPath} has no details.name`);
if (name.startsWith('cajeta.')) die(`refusing to publish stdlib namespace '${name}' (cajeta.* is toolchain-embedded)`);

const hex = createHash('sha256').update(cja).digest('hex');
const sha = `sha256:${hex}`;
const key = createPrivateKey({ key: KEY_PEM });
const sig = edSign(null, cja, key); // raw 64-byte detached Ed25519 signature

const form = new FormData();
form.set('archive', new Blob([cja], { type: 'application/octet-stream' }), `${name}-${version}.cja`);
form.set('metadata', JSON.stringify({ name, version, sha256: sha }));
form.set('manifest', JSON.stringify(manifestObj));
form.set('signature', new Blob([sig], { type: 'application/octet-stream' }), `${name}-${version}.cja.sig`);
form.set('key-id', KEY_ID);
if (readmePath && existsSync(readmePath)) form.set('readme', readFileSync(readmePath, 'utf8'));

const res = await fetch(`${BASE}/v2/publish`, { method: 'POST', headers: { Authorization: `Bearer ${BEARER}` }, body: form });
const body = await res.text();
if (res.status === 201) { console.log(`published ${name}@${version} (${sha})`); process.exit(0); }
if (res.status === 409) { console.log(`already published ${name}@${version} — treating as success`); process.exit(0); }
die(`publish failed: HTTP ${res.status} ${body}`);
