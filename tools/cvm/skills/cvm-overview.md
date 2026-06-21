---
id: cvm-overview
applies-to: [cvm]
title: cvm — Cajeta toolchain installer/switcher (overview & routing)
description: What cvm is, how to build/invoke it, the ~/.cajeta layout, config precedence, exit codes, and a task→command routing table.
---

# cvm — the Cajeta Version Manager

cvm installs and switches between cajeta toolchains the way `rustup` does for Rust.
It is **version-independent by contract**: it bakes in no knowledge of any cajeta
version, resolving a selector (`latest` / `<version>`) against a self-describing
release manifest, downloading the host-triple binary, verifying its SHA-256, and
repointing the active `cajeta` shim. A 1.0 cvm installs an 8.0 cajeta, unchanged.

cvm is itself **written in cajeta** and compiled `--emit=exe` by the cajeta build
tool (dogfooding). Source: `src/main/cajeta/cvm/` (entry `cvm.Cvm::main`).

## Task → command routing

| You want to… | Run |
| --- | --- |
| Install the newest toolchain | `cvm install` (or `cvm install latest`) |
| Install a specific version | `cvm install <version>` (e.g. `cvm install 1.0`) |
| Switch the active toolchain to one already installed | `cvm default <version>` |
| See cvm home, active toolchain, shim, versions dir | `cvm which` |
| Diagnose the environment cvm resolved (env vars, home, active) | `cvm doctor` |
| Print cvm's own version | `cvm --version` (also `-V`, `version`) |
| Show usage | `cvm --help` (also `-h`, `help`) |

### Not provided (avoid the dead end)

- **No shell PATH wiring.** cvm installs the shim at `<home>/bin/cajeta` but does
  **not** edit your shell profile — add `<home>/bin` to `PATH` yourself.
- **No `cvm self update` yet.** It prints intent and exits **2** (install-method
  detection is pending).
- **No `cvm list`/`uninstall`/channel verbs.** The only commands are those above.
- **`cvm which` does not** walk `PATH` to list every `cajeta` and its owner — it
  reports only cvm's own home/active/shim.
- **`cvm default` does not** download anything; it only repoints among
  already-installed toolchains. Network install is the separate `install` verb.

## Build & invoke

cvm is built by the cajeta build tool (`cajeta build` reading `cajeta.json`),
which requires the cajeta compiler already built at `../../build/src/cajeta`.

```sh
# from tools/cvm
./build.sh             # → build/cvm  (wraps `cajeta build`)
./run.sh -- doctor     # build + run; cvm args go after `--`
./build/cvm install 1.0
```

Override the compiler the build scripts use with `CAJETA=/path/to/cajeta`
(this is a *build-time* knob, not a cvm runtime override).

## ~/.cajeta layout

cvm manages everything under one home directory:

```
<home>/bin/cvm                  the manager (on PATH only when shim-installed)
<home>/bin/cajeta               shim → active toolchain (the cajeta that lands on PATH)
<home>/versions/<version>/cajeta  each installed toolchain's binary
<home>/settings                 line-based `key = value`; Phase 1 writes `default = <version>`
```

Note: `<home>/settings` is the real file name (a minimal `key = value` file, a
single `default =` key today). The README's `settings.toml` is aspirational — the
TOML reader is not wired yet.

## Configuration & precedence

**Home resolution** (`Home.dir()`), first match wins:

1. `$CAJETA_HOME` — explicit override, used verbatim.
2. `$XDG_DATA_HOME/cajeta` — when set.
3. `$HOME/.cajeta` — the default.

If none resolves (no `$CAJETA_HOME` and no `$HOME`), commands needing a home fail
with exit **1** and a message telling you to set `$CAJETA_HOME` or `$HOME`.

**`$CVM_DIST_BASE`** — overrides the release base URL (point it at a local mock or
a fork) for `cvm install`. Default base:
`https://github.com/jklappenbach/cajeta/releases`. Manifest URLs:
`latest` → `<base>/latest/download/index.json`; a version →
`<base>/download/v<version>/index.json`. cvm fetches one static `index.json`
asset (a CDN redirect, not the GitHub API), schema-gated at `schemaVersion` 1 —
a higher schema is rejected, not mis-parsed.

**Host triple** comes from the `cajeta.host.triple` system property the runtime
publishes at startup; if unset, `install` fails (exit 1).

## Exit codes (global scheme)

- **0** — success.
- **1** — usage error (missing/unknown command or args) **or** any runtime
  failure (no resolvable home, HTTP non-200, checksum mismatch, no asset for the
  host triple, toolchain not installed, shim repoint failed). Diagnostics print to
  stderr.
- **2** — not-yet-implemented (`cvm self update`).

## Worked example

Install a specific toolchain against a local mock release server, then activate it:

```sh
export CAJETA_HOME="$HOME/.cajeta-test"
export CVM_DIST_BASE="http://localhost:8080/releases"
./build/cvm install 1.0
# cvm: downloading cajeta 1.0.0 (x86_64-linux-gnu)...
# cvm: installed cajeta 1.0.0 and set it as the default.
#   /home/you/.cajeta-test/bin/cajeta -> /home/you/.cajeta-test/versions/1.0.0/cajeta
./build/cvm which
./build/cvm default 1.0.0   # repoint among installed toolchains
```

`install` checksum-verifies the download (SHA-256 vs the manifest's `sha256`) and
**refuses to install on mismatch**. On success it repoints the shim and records
the version as `default` in `<home>/settings`.

## Downward pointers

There are no per-command skills yet; for exact behavior read the source under
`src/main/cajeta/cvm/`: `Cvm` (dispatch/exit codes), `Home` (paths/precedence),
`Catalog` (URLs/`CVM_DIST_BASE`), `Installer` (network install flow),
`Toolchains` (shim + settings).
