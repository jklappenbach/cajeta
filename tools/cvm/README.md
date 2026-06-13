# cvm — the Cajeta Version Manager

`cvm` installs and switches Cajeta toolchains, the way `rustup` does for
Rust. It is **written in Cajeta** and compiled to a native binary
(`--emit=exe`) by the cajeta compiler — dogfooding the toolchain, and
serving as the release workflow's real-world test of exe emission on every
target.

Governed by [`plans/installer/installer-plan.md`](../../plans/installer/installer-plan.md)
decision **D12**.

## Version-independent by contract

cvm bakes in **no** knowledge of any cajeta version. It resolves a selector
(`latest` / a version / a channel) against a stable release manifest at a
well-known URL, downloads the self-contained binary for the host triple,
verifies its checksum, installs it under `~/.cajeta/versions/<ver>/`, and
repoints the active shim. So a **1.0 cvm installs an 8.0 cajeta**, unchanged.

It manages cajeta under `~/.cajeta` (honoring `$CAJETA_HOME` /
`$XDG_DATA_HOME`):

```
~/.cajeta/bin/cvm                 the manager (on PATH only when shim-installed)
~/.cajeta/bin/cajeta              shim → the active toolchain (on PATH)
~/.cajeta/versions/<version>/     each installed toolchain
~/.cajeta/settings.toml           default toolchain + config
```

## Build

From this directory, with the cajeta compiler already built
(`../../build/src/cajeta`):

```sh
./build.sh            # → build/cvm
./run.sh -- doctor    # build + run `cvm doctor`
```

Both scripts drive the cajeta build tool (`cajeta build` / `cajeta run`),
which reads `cajeta.json`. Override the compiler with `CAJETA=/path/to/cajeta`.

## Commands

```
cvm install [latest|<version>]   install a cajeta toolchain (default: latest)
cvm default <version>            make an installed toolchain the active one
cvm which                        show the cvm home + active toolchain shim
cvm doctor                       diagnose the environment cvm sees
cvm self update                  update cvm itself (shim installs only)
cvm --version | --help
```

## Status

**Phase 0 (this commit) — bootstrap skeleton.** Real CLI dispatch + working
environment/path resolution (`which`, `doctor`). The verbs that fetch and
install a toolchain (`install`, `default`, `self update`) are honest stubs
that announce intent and exit `2`; they do not yet touch the network or the
filesystem.

**Phase 1 (next)** — manifest resolve → download → checksum verify → install
to `~/.cajeta/versions/<ver>` → wire PATH; install-method detection;
coexist/takeover reconciliation with a system-wide cajeta. See D12 Phase 1.
