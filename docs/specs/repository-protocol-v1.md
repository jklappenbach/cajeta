# Cajeta repository protocol — v1

This document is the wire-shape contract for cajeta repositories. It captures
what is shipped in the v1 build (Phases 6a/6b/6c) and how v2-capable
registries advertise enhanced endpoints (Phase 6d). The authoritative C++
client lives at `src/cajeta/buildtool/repo/` (`FilesystemRepository`,
`HttpRepository`, `GitRepository`) and the consumer-facing narrative is
`docs/BuildTool.md` under "Dependencies & repositories".

## Driver matrix

| Driver       | Identifier (`type:` field) | Auth modes              | Backing store               |
|--------------|----------------------------|-------------------------|-----------------------------|
| filesystem   | `filesystem`               | none                    | local directory tree         |
| http         | `http`                     | bearer token, mutual TLS| any HTTP(S) endpoint         |
| git          | `git`                      | (driven by `git` config)| Git remote                   |
| maven-compat | `maven-compat`             | n/a — **deferred**      | Maven Central / Nexus layout |

A repository entry's `type` may be omitted; the loader infers from the field
set (`path` ⇒ filesystem, `url` ⇒ http, repository URL ending in `.git` ⇒ git).

## v1 endpoints (HTTP driver)

For an HTTP repository rooted at `<base>`:

```
GET <base>/<package>/<version>/<package>-<version>.cja          → artifact
GET <base>/<package>/<version>/<package>-<version>.cja.sig      → detached signature
GET <base>/<package>/<version>/<package>-<version>.cja.sig.keyid → key id
GET <base>/<package>/<version>/cajeta.json                       → sidecar manifest (until embed-in-archive)
GET <base>/<package>/                                            → version index (HTML or JSON; client tolerates both)
POST <base>/v2/publish                                           → publish action target (mixed v1/v2 endpoint)
```

Headers:

- `Authorization: Bearer <token>` when `auth.type = bearer`. Token read from
  the env var named by `auth.token-env`.
- `User-Agent: cajeta/<version>` always.

Mutual TLS: the client presents `auth.cert-path` + `auth.key-path` when the
auth block requests it.

## v2 capability probe

A v2-capable registry serves `/.well-known/cajeta-capabilities.json`:

```json
{
  "capabilities": {
    "v1": true,
    "v2": true,
    "bundle": true,
    "lockfile-diff": true,
    "supercompress": false,
    "transparency-log": true,
    "well-known-bundles": ["stdlib@1.0.0"]
  },
  "mirrors": [
    {"url": "https://mirror-eu.example.org", "region": "eu"},
    {"url": "https://mirror-us.example.org", "region": "us"}
  ],
  "ttl-seconds": 3600
}
```

The client (`HttpRepository::capabilities()`) caches the response for the
advertised TTL. v1-only servers either omit the file (404) or respond with
`v2: false`; the client falls back to v1 paths.

## v2 endpoints

### `POST /v2/bundle`

Request body (JSON):

```json
{
  "have": ["sha256:<hex>", "..."],
  "want": [{"name": "foo", "version-constraint": ">=1.2.0,<2.0.0"}],
  "transitive": true,
  "format": "tar.zst"
}
```

Response: `application/x-tar-zstd` stream. The tar holds one entry per
included artifact:

```
foo-1.2.3.cja
foo-1.2.3.cja.sig
foo-1.2.3.cja.sig.keyid
bar-0.4.0.cja
...
bundle.json
```

`bundle.json` is the index:

```json
{
  "entries": [
    {"name": "foo", "version": "1.2.3", "sha256": "sha256:..."},
    {"name": "bar", "version": "0.4.0", "sha256": "sha256:..."}
  ]
}
```

`have` short-circuits transfer for blobs the client already has by digest.
`transitive: true` asks the server to expand transitives and pin them via
MVS; the client may still re-run MVS locally as a sanity check.

### `GET /v2/resolve?name=foo&version-constraint=>=1.2.0`

Returns JSON metadata for the resolution without transferring the archive:

```json
{
  "resolved": [
    {"name": "foo", "version": "1.2.3", "sha256": "sha256:...", "retracted": false}
  ]
}
```

A retracted version surfaces:

```json
{
  "name": "foo",
  "version": "1.2.3",
  "sha256": "sha256:...",
  "retracted": true,
  "retracted-reason": "CVE-2026-12345"
}
```

Existing lockfile entries keep resolving — retraction only emits a warning
on new resolves.

### `GET /v2/blob/<sha256>`

Content-addressed blob fetch. Returns the raw bytes. Workstation cache keys
match by construction.

### `POST /v2/lockfile-diff`

```json
{
  "from": [{"name": "foo", "version": "1.2.3"}, ...],
  "to":   [{"name": "foo", "version": "1.2.4"}, ...]
}
```

Response: same shape as `/v2/bundle` (tar.zst stream), but contains only the
changed blobs. On snapshot miss the server may respond 404 with a hint to
re-fetch the full bundle; the client retries against `/v2/bundle`.

### `GET /v2/transparency-log/<sha256>`

Returns the transparency-log entry for an artifact's digest:

```json
{
  "sha256": "sha256:...",
  "signed-at": "2026-05-30T17:42:00Z",
  "log-entry-signature": "<base64>",
  "log-entry-key-id": "..."
}
```

Install fails when the signature is missing or doesn't verify against a
trusted log key.

## Capability advertisement

Every v2 response carries a `Cajeta-Capability-Version: 1` header so clients
caching aggressively can invalidate when the server bumps protocol. v1
responses don't carry the header.

## Backward compatibility

- v1-only clients keep working against v2-capable servers — the well-known
  endpoint returns 404 from their perspective (they never request it),
  v1 paths are still served.
- v2-only clients (no v1 paths at all) MUST honor the capability probe
  before issuing any v2 request, and fall back to v1 paths against
  v1-only servers.

## Publishing

`POST /v2/publish` is the same endpoint used by the `publish` action
(`PublishAction.cpp`):

- multipart form
- field `archive` carries the `.cja`
- field `signature` carries the detached `.sig` (when signed)
- field `key-id` carries the signing key id
- field `attestation` carries the `<archive>.attestation` (when produced)
- field `metadata` carries a JSON object with `name`, `version`, `sha256`

Namespace verification (DNS TXT `_cajeta-publish.<domain>` or
`.github/cajeta-publish.txt`) is enforced server-side and is opaque to the
client. The publishing user's auth is the same bearer-or-mTLS shape as
fetch.

## Reference implementations

- `src/cajeta/buildtool/repo/FilesystemRepository.{h,cpp}`
- `src/cajeta/buildtool/repo/HttpRepository.{h,cpp}` (v1 + v2)
- `src/cajeta/buildtool/repo/GitRepository.{h,cpp}`
- `src/cajeta/buildtool/repo/TarZstd.{h,cpp}` — bundle codec
- Tests: `HttpRepositoryTests.cpp`, `HttpRepositoryV2Tests.cpp`,
  `GitRepositoryTests.cpp`, `GitOverrideTests.cpp`.
