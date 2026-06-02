# Toolchain registry protocol — v1

This is the wire-shape contract for fetching cajeta toolchains. It piggybacks
on the cajeta repository protocol (`repository-protocol-v1.md`) — the
endpoints, auth, and signed-archive plumbing are reused. What's new is the
toolchain-specific index shape and store layout. Authoritative C++ client:
`src/cajeta/buildtool/Toolchain.{h,cpp}`.

## Store layout

The on-disk install location is `~/.cajeta/toolchains/` by default
(override via `CAJETA_TOOLCHAIN_HOME`). Layout:

```
~/.cajeta/toolchains/
├── official/
│   ├── 1.0.3/
│   │   ├── bin/cajeta
│   │   ├── lib/
│   │   └── share/
│   └── 1.0.4/
│       └── ...
├── nightly/
│   └── 2026-05-30/
│       └── ...
└── current → official/1.0.4         # workstation-wide default
```

`current` is a symlink (Windows: a directory junction) maintained by
`cajeta toolchain default`. Resolving it gives the binary the launcher
re-execs into when no project pin applies.

Reserved distributions: `official`, `nightly`, `lts`, `system`. These names
cannot be used for user-installed channels.

## Pinning

A project pins its toolchain in `settings.toolchain`:

```json
{
  "settings": {
    "toolchain": {
      "version": "1.0.3",
      "distribution": "official",
      "fetch": "auto",
      "sha256": "sha256:abc...",
      "from": "<repository-name>"
    }
  }
}
```

Override file (workstation-local, untracked):

```
.cajeta-toolchain
official:1.0.3
```

Precedence: `.cajeta-toolchain` → manifest `settings.toolchain` → no pin.

Fetch policies:

| Policy | Behavior when pinned binary isn't installed                                   |
|--------|-------------------------------------------------------------------------------|
| `auto` | Download + verify + install, then re-exec.                                    |
| `warn` | Continue with the PATH binary; emit a warning naming the install command.     |
| `error`| Refuse; print the install command.                                            |
| `off`  | Continue with the PATH binary silently (no warning, no install).              |

`CAJETA_NO_DISPATCH=1` is the highest-precedence escape hatch — it runs the
PATH binary regardless of pin or policy.

## Index endpoint

A toolchain registry rooted at `<base>` serves:

```
GET <base>/index.json
```

Response:

```json
{
  "index-version": 1,
  "distributions": {
    "official": {
      "channel": "stable",
      "versions": [
        {
          "version": "1.0.3",
          "released-at": "2026-04-12T00:00:00Z",
          "platforms": {
            "linux-x86_64":  {"sha256": "sha256:...", "path": "v1.0.3/cajeta-1.0.3-linux-x86_64.tar.zst"},
            "linux-aarch64": {"sha256": "sha256:...", "path": "v1.0.3/cajeta-1.0.3-linux-aarch64.tar.zst"},
            "darwin-arm64":  {"sha256": "sha256:...", "path": "v1.0.3/cajeta-1.0.3-darwin-arm64.tar.zst"},
            "windows-x86_64":{"sha256": "sha256:...", "path": "v1.0.3/cajeta-1.0.3-windows-x86_64.tar.zst"}
          },
          "signing-key-id": "official-2026",
          "cajeta-lang-version": "1"
        }
      ]
    },
    "nightly": {
      "channel": "nightly",
      "versions": [...]
    },
    "lts": {
      "channel": "lts",
      "versions": [...]
    }
  }
}
```

Notes:

- `sha256` is the digest of the archive bytes (not the unpacked tree).
- `path` is relative to `<base>`.
- `signing-key-id` names the trust-store key used to sign the archive. Its
  PEM lives in `~/.cajeta/trust/keys/<id>.pem` (Phase 10 trust store).
- `cajeta-lang-version` is the toolchain's source-language major version;
  the N±k compatibility window (open decision) applies.

## Archive endpoints

```
GET <base>/<path>                 → the .tar.zst toolchain archive
GET <base>/<path>.sig             → detached ed25519 signature
GET <base>/<path>.sig.keyid       → key id (matches signing-key-id in index)
GET <base>/<path>.attestation     → SLSA v1 provenance (Phase 13 shape)
```

The signature + attestation + key-id sidecars match the cajeta archive
sidecar convention so the existing `verifyArchiveSignature` and
`verifyProvenanceJson` paths handle toolchain archives unchanged.

## Install flow

`cajeta toolchain install <distribution>:<version>` (or auto-fetch under
`fetch: auto`):

1. Probe `<base>/index.json`; cache for the server-advertised TTL.
2. Look up the requested distribution/version/platform; fail loud on
   "unknown distribution" or "unknown version" with the available list.
3. Fetch archive + .sig + .sig.keyid + .attestation. Network failures
   retry with exponential backoff (`Retry.cpp`).
4. Verify sha256 against the index entry's digest. Mismatch is a hard fail.
5. Verify signature against the trust store. Missing or invalid sig is a
   hard fail when `--require-signature` is requested (default `strict` for
   toolchain installs).
6. Verify attestation (Phase 13 in-toto Statement v1). Missing attestation
   is a warning when `--require-attestation` is `warn`, hard fail when
   `strict`.
7. Unpack to `<store-root>/<distribution>/<version>/`.
8. (Optional) `cajeta toolchain default <distribution>:<version>` atomically
   updates the `current` symlink.

## Dispatch

`Toolchain.cpp::computeDispatchDecision` returns one of:

- `Continue` — the running binary satisfies the pin (or no pin / fetch=off /
  `CAJETA_NO_DISPATCH=1`). The current process keeps going.
- `ReExec(path)` — the pinned binary is installed at `path`; the launcher
  re-execs into it.
- `NeedsInstall(hint)` — auto-fetch is required; the hint is the install
  command to run (or to invoke programmatically).
- `Error` — `fetch: error` and the pinned binary isn't installed.

## IR cache discriminator

The toolchain identity (`<distribution>:<version>[:<sha256>]`) is part of
the input to the IR cache discriminator (Phase 5b). Different toolchains
never share IR cache slots; bumping the pin invalidates exactly the right
set.

## Cross-toolchain compatibility window

The toolchain pin is target-independent — `settings.build.target` is the
BuildAction param, so one install root can produce artifacts for many
targets.

Inter-version compatibility (which toolchain N can consume archives built
by toolchain M) is the **N±k window** open decision in
`plan/build-tool-plan.md`. The current lean is N±2 against the
`cajeta-lang-version` field carried in `index.json`.

## Reference implementation

- `src/cajeta/buildtool/Toolchain.{h,cpp}` — pin parsing, store layout,
  dispatch decision, identity string.
- `src/cajeta/buildtool/BuildToolCommands.cpp` — `cajeta toolchain
  list/install/remove/default/pin/which/show` subcommands.
- Tests: `Phase14AcceptanceTests.cpp` (21 cases).
