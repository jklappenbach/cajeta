# Publishing Cajeta libraries to Olla from CI

On a `v*` tag, each `cajeta-*` library's `release.yml` builds the `.cja`,
publishes a GitHub Release, **and** does a signed publish to the Olla registry
(`olla.cajeta.dev`). CI must sign with an Ed25519 key that appears in the
publishing organization's **root-signed key document**. Olla refuses any upload
it cannot bind to a publisher that way, and no environment variable relaxes
that (`publisher-trust` §5.1.8).

## Pieces

- **Reusable workflow:** `cajeta/.github/workflows/lib-release.yml` — generic;
  installs the toolchain, runs `cajeta build` (emits
  `build/archive/<name>-<version>.cja`), GH Release, then the publish action.
- **Composite action:** `cajeta/.github/actions/olla-publish` — signs the `.cja`
  (detached Ed25519 over raw bytes) and POSTs `/v2/publish` with the manifest
  from `cajeta.json`. Refuses to publish `cajeta.*` (stdlib is embedded).
- **Caller:** each lib's `.github/workflows/release.yml` is ~10 lines:
  `uses: jklappenbach/cajeta/.github/workflows/lib-release.yml@ci/v1` +
  `secrets: inherit`. `cajeta-codec` keeps its bespoke workflow (GPU oracle,
  typed-codec build) and just adds the `olla-publish` step.

## Versioning the shared pieces

Reference the workflow and the action by their `ci/vN` tag, not `@main`.

`ci/v1` **moves**: a compatible change re-points it, so every caller picks
the fix up on its next tag without editing anything. That central-upgrade
property is most of why one shared workflow is worth having, and pinning to
an immutable SHA would throw it away. An **incompatible** change gets
`ci/v2`, and callers move when they are ready.

Two details that are easy to get wrong:

- **`lib-release.yml` pins the action it calls to the same tag.** Tagging
  the workflow alone achieves nothing: a caller on `ci/v1` would still pick
  up a breaking change to `olla-publish` from main. The two move together.
- **The `ci/` prefix is not decoration.** Every other tag here is a
  toolchain release (`v0.24.0`), and `lib-release.yml` downloads one by tag.
  A bare `v1` would sit in that namespace and read as a toolchain release.

`@main` still works and is not blocked. It is the unpinned option — whatever
is on main when your tag fires, breaking changes included.

## One-time setup (do this before the first tagged release)

1. **Generate the CI publisher key** (keep the private key only in CI secrets):
   ```sh
   openssl genpkey -algorithm ed25519 -out olla-ci.key
   openssl pkey -in olla-ci.key -pubout -out olla-ci.pub
   ```
2. **Have the owner sign an organization key document naming that key**, and
   upload it. This is an owner-operated onboarding step, done once per
   organization, and it precedes the first upload.

   `POST /v2/keys` used to do this with a publish token — which meant the
   same credential that uploaded artifacts could register the key that signed
   them, so one stolen `OLLA_TOKEN` bought both. It is **removed**, not
   deprecated: a deprecated endpoint that still answers is still a bypass.

   The document is signed **offline, with the repository root key**, which
   Olla does not hold and must never hold. Its payload names the organization,
   the namespaces it owns, and the keys allowed to sign uploads:

   ```json
   {
     "organization": "dev.cajeta",
     "namespaces": ["dev.cajeta"],
     "issued-at": "2026-09-03T00:00:00Z",
     "not-after": "2027-09-03T00:00:00Z",
     "keys": [{
       "id": "olla-ci-1",
       "algorithm": "ed25519",
       "public-key": "-----BEGIN PUBLIC KEY-----\n…\n-----END PUBLIC KEY-----\n",
       "not-before": "2026-09-03T00:00:00Z",
       "not-after": "2027-09-03T00:00:00Z"
     }]
   }
   ```

   Wrap it in the signed envelope (the same one every trust document uses:
   base64 payload, signature over the decoded bytes) and upload it with an
   **owner** token — a separate credential in a separate table, which a
   publish token cannot reach:

   ```sh
   curl -sf -X POST https://olla.cajeta.dev/v2/admin/org-keys/dev.cajeta \
     -H "Authorization: Bearer $OLLA_ADMIN_TOKEN" \
     -H 'content-type: application/json' --data-binary @org-keys.json
   ```

   Check it the way a client will before you rely on it:

   ```sh
   cajeta trust verify-document org-keys.json
   ```

   It reads the document type from the signed payload, not the filename, and
   checks everything a client checks — expiry and per-key windows included, any
   of which rejects a perfectly signed file. (An organization document needs no
   `--origin`; a delegation does.)

   **Namespaces are checked from this list**, segment-aware: `dev.cajeta` owns
   `dev.cajeta.http` and does not own `dev.cajetaevil`. Nothing derives an
   owner from a package name. If you need to show control of a domain first,
   `POST /v2/namespaces/verify` still runs the DNS-TXT and GitHub-file checks
   — as evidence for the owner before signing, not as anything the publish
   path consults.
3. **Add three secrets to each library repo** (Settings → Secrets → Actions).
   GitHub user accounts have no org-level secrets, so add them per repo — or move
   the repos under an org and use org secrets:
   - `OLLA_TOKEN` — a publish bearer token (a `publish_tokens` row in D1).
   - `OLLA_SIGNING_KEY_PEM` — contents of `olla-ci.key`.
   - `OLLA_SIGNING_KEY_ID` — `olla-ci-1` (must match the key `id` in step 2's
     document; a `key-id` not in that document is refused).

## Versioning

The reusable workflow takes the version from the **tag** (`vX.Y.Z` → `X.Y.Z`).
Keep `cajeta.json`'s `details.version` in sync, or it'll mismatch the tag.
`cajeta-codec` reads its `VERSION` file instead (its own convention).

## Dependency ordering

`cajeta build` resolves a lib's runtime dependencies. A lib that depends on
another `dev.cajeta.*` package (e.g. `cajeta-cluster` → `dev.cajeta.gossip`)
can only build in CI once that dependency is published to Olla and Olla is
configured as a repository in the lib's `cajeta.json`. Publish leaf libraries
first (`dev.cajeta.unit`, `dev.cajeta.logging`, …), then dependents.

## Not yet wired

- `cajeta-gossip`, `cajeta-caramelo`, `cajeta-robotica` — no `cajeta.json` yet
  (not publishable libraries). Add a manifest, then drop in the caller workflow.
- `cajeta-collection`, `cajeta-cloud-objectstore` — not git repos yet.

## Cross-repo access caveat

The caller workflows reference `jklappenbach/cajeta/...@main` for the reusable
workflow and action. This works when `cajeta` is public, or for same-owner
private repos with **Settings → Actions → "Accessible from repositories owned by
the user"** enabled on `cajeta`. Otherwise vendor the action/script into each lib.
