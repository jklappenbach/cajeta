# Publishing Cajeta libraries to Olla from CI

On a `v*` tag, each `cajeta-*` library's `release.yml` builds the `.cja`,
publishes a GitHub Release, **and** does a signed publish to the Olla registry
(`olla.cajeta.dev`). Olla runs with `ALLOW_UNSIGNED=0`, so CI must sign with an
Ed25519 key registered in Olla's trust store.

## Pieces

- **Reusable workflow:** `cajeta/.github/workflows/lib-release.yml` — generic;
  installs the toolchain, runs `cajeta build` (emits
  `build/archive/<name>-<version>.cja`), GH Release, then the publish action.
- **Composite action:** `cajeta/.github/actions/olla-publish` — signs the `.cja`
  (detached Ed25519 over raw bytes) and POSTs `/v2/publish` with the manifest
  from `cajeta.json`. Refuses to publish `cajeta.*` (stdlib is embedded).
- **Caller:** each lib's `.github/workflows/release.yml` is ~10 lines:
  `uses: jklappenbach/cajeta/.github/workflows/lib-release.yml@main` +
  `secrets: inherit`. `cajeta-codec` keeps its bespoke workflow (GPU oracle,
  typed-codec build) and just adds the `olla-publish` step.

## One-time setup (do this before the first tagged release)

1. **Generate the CI publisher key** (keep the private key only in CI secrets):
   ```sh
   openssl genpkey -algorithm ed25519 -out olla-ci.key
   openssl pkey -in olla-ci.key -pubout -out olla-ci.pub
   ```
2. **Register the public key** in Olla's trust store (needs a publish token):
   ```sh
   curl -sf -X POST https://olla.cajeta.dev/v2/keys \
     -H "Authorization: Bearer $OLLA_TOKEN" -H 'content-type: application/json' \
     -d "$(jq -n --arg k 'olla-ci-1' --arg p "$(cat olla-ci.pub)" \
            '{"key-id":$k,"public-key":$p}')"
   ```
3. **Add three secrets to each library repo** (Settings → Secrets → Actions).
   GitHub user accounts have no org-level secrets, so add them per repo — or move
   the repos under an org and use org secrets:
   - `OLLA_TOKEN` — a publish bearer token (a `publish_tokens` row in D1).
   - `OLLA_SIGNING_KEY_PEM` — contents of `olla-ci.key`.
   - `OLLA_SIGNING_KEY_ID` — `olla-ci-1` (must match step 2).

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
