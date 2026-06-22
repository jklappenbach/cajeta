---
id: cvm-default
applies-to: [cvm/default]
title: cvm default — switch the active cajeta toolchain
description: Repoint the bin/cajeta shim at an already-installed toolchain and record it as the default.
---

# `cvm default <version>`

Makes an **already-installed** toolchain the active one: repoints the
`<home>/bin/cajeta` symlink shim at `<home>/versions/<version>/cajeta` and
records `default = <version>` in `<home>/settings`. This is the local,
no-network half of switching toolchains.

## Invocation

```sh
cvm default 0.7.0
```

`<version>` is required and must name a directory already present under
`<home>/versions/`. It is matched literally — the version string you pass is
the directory name; there is no `latest`/channel resolution here (that is
`cvm install`'s job).

## I/O contract

- **Precondition:** the toolchain binary must exist at
  `<home>/versions/<version>/cajeta`. `default` does **not** download or
  install anything — run `cvm install <version>` first (see `cvm/install`).
- **Home (`<home>`) resolution**, in order: `$CAJETA_HOME` (verbatim) →
  `$XDG_DATA_HOME/cajeta` → `$HOME/.cajeta`.
- **Side effects on success:**
  - creates `<home>/bin/` if missing, then replaces `<home>/bin/cajeta` with a
    symlink to `<home>/versions/<version>/cajeta`;
  - creates `<home>` if missing, then truncates `<home>/settings` to the single
    line `default = <version>` (line-based `key = value`; this verb writes only
    the `default` key).
- **Stdout on success:**
  ```
  cvm: default toolchain set to '0.7.0'.
    /home/you/.cajeta/bin/cajeta -> /home/you/.cajeta/versions/0.7.0/cajeta
  ```

## Exit codes

- `0` — shim repointed and default recorded.
- `1` — any error below (missing arg, unresolvable home, not installed, or
  symlink failure).

## Errors (all stderr, exit 1)

- Missing version arg →
  `cvm: 'default' needs a version (e.g. 'cvm default 1.0').`
- No resolvable home (neither `$CAJETA_HOME` nor `$HOME` set) →
  `cvm: cannot resolve a home directory (set $CAJETA_HOME or $HOME).`
- Not installed →
  `cvm: toolchain '<version>' is not installed (<home>/versions/<version>/cajeta not found). Install it first.`
- Symlink could not be created (e.g. a platform without elevation-free
  symlinks) →
  `cvm: failed to repoint the shim <home>/bin/cajeta (symlinks may be unsupported on this platform).`
  Note: if the symlink succeeds but settings can't be written, the shim is
  still repointed (settings is written after, and `writeDefault` always
  reports success).

## What it does NOT do

- No install/download/checksum — that is `cvm install`.
- Does not add `<home>/bin` to your shell `PATH`; do that yourself so the shim
  is found.
- Does not validate that `<version>` is a well-formed version, only that the
  binary directory exists.
- Does not update cvm itself (`cvm self update`).
