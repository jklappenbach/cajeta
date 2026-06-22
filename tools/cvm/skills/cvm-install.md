---
id: cvm-install
applies-to: [cvm/install]
title: cvm install — fetch, verify, and activate a cajeta toolchain
description: Resolve a release manifest, download + SHA-256-verify the host binary, install it under ~/.cajeta/versions/<ver>, and repoint the active shim.
---

# `cvm install [latest|<version>]`

Installs a cajeta toolchain end to end: resolve the release manifest → match
the host triple → download the self-contained binary → verify its SHA-256 →
write it to `versions/<ver>/cajeta` → make it the active shim. `cvm` bakes in
no version knowledge; everything comes from the manifest.

## Invocation

```sh
cvm install            # selector defaults to "latest"
cvm install 0.7.0      # a concrete version (no leading "v")
```

The selector is the only argument. `latest` (or omitted) and any other string
treated as a version are the two cases — there is no channel/range support.

## I/O contract

Input — the **release manifest** (`index.json`), a static GitHub Releases
download asset (a CDN redirect, **not** the GitHub API — so no rate limit).
URL is built by `Catalog`:

- base = `$CVM_DIST_BASE` if set and non-empty, else
  `https://github.com/jklappenbach/cajeta/releases`
- `latest` → `<base>/latest/download/index.json`
- `<version>` → `<base>/download/v<version>/index.json`

Manifest schema (schemaVersion-gated; cvm understands `schemaVersion` ≤ **1**):

```json
{ "schemaVersion": 1,
  "version": "0.7.0",
  "assets": {
    "x86_64-linux-gnu": {
      "url": "https://.../cajeta-0.7.0-x86_64-linux-gnu",
      "sha256": "<lowercase hex>" } } }
```

Host match — the asset key is the `cajeta.host.triple` system property the
runtime publishes at startup (uname-derived, e.g. `x86_64-linux-gnu`).
Checksum — `Sha256.hashHex(binary)` must equal the asset's `sha256` exactly
(lowercase hex); mismatch aborts before any file is written.

Output — on success:

```
cvm: downloading cajeta 0.7.0 (x86_64-linux-gnu)...
cvm: installed cajeta 0.7.0 and set it as the default.
  ~/.cajeta/bin/cajeta -> ~/.cajeta/versions/0.7.0/cajeta
```

Filesystem effects (home resolved as `$CAJETA_HOME`, else
`$XDG_DATA_HOME/cajeta`, else `$HOME/.cajeta`):

- writes `versions/<ver>/cajeta`, marks it executable (a failed
  `setExecutable` is a warning, not a failure)
- repoints the `bin/cajeta` shim (a **symlink**) to that binary
- records `default = <ver>` in `settings`

## Exit codes

- `0` — installed and activated.
- `1` — any failure, diagnostics already on stderr: host triple unset; home
  unresolvable; manifest HTTP ≠ 200; `schemaVersion` missing or > 1 ("needs a
  newer cvm"); missing `version`/`assets`; no asset for this triple; asset
  missing `url`/`sha256`; download HTTP ≠ 200; **checksum mismatch** (prints
  expected vs got).

(`install` never returns 2; that code is only `cvm self update`.)

## Does NOT

- Does **not** edit your shell profile / wire `PATH`. After install, add
  `~/.cajeta/bin` to `PATH` yourself.
- Does **not** use the GitHub API, build from source, or install from a local
  file — it fetches one static `index.json` + one binary asset over HTTP.
- Does **not** reconcile with a system-wide cajeta, and the symlink shim has
  no elevation-free Windows path.
- Selecting a toolchain already on disk without downloading is
  `cvm default <version>`, not `install`.
