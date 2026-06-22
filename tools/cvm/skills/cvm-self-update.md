---
id: cvm-self-update
applies-to: [cvm/self]
title: cvm self update — update cvm itself (Phase-1 stub)
description: Invocation and exit contract for `cvm self update`, a not-yet-implemented stub that defers to the installer.
---

# `cvm self update`

Updates cvm itself. **Not implemented in Phase 1** — it is an honest stub
that announces intent and defers to your installer; it does **not** touch the
network or filesystem. Do not depend on it to upgrade cvm yet.

## Invocation

```sh
cvm self update
```

`self` is the only subcommand; `update` is its only verb. There are no flags,
no arguments, no env config, no input read from stdin.

## I/O contract

On the implemented path (`cvm self update`):

```
$ cvm self update
cvm: self-update defers to your installer.          # stdout
cvm: self update (install-method detection) is not implemented yet (installer-plan.md D12, Phase 1).   # stderr
$ echo $?
2
```

| outcome | invocation | exit | stream |
|---|---|---|---|
| stub ran (not implemented) | `cvm self update` | **2** | message on stdout + stderr |
| usage error (bad/missing subverb) | `cvm self`, `cvm self foo` | **1** | `cvm: usage: cvm self update` on stderr |

Exit codes follow cvm's convention: `0` ok, `1` usage error, `2`
not-yet-built. So a healthy run of this command is exit **2**, by design —
treat 2 here as "stub, expected," not a crash.

## What it does NOT do

- Does not download, replace, or verify any cvm binary.
- Does not detect the install method yet — install-method detection (PM
  prefix vs `~/.cajeta/bin` shim install) is the pending Phase-1 work that
  gates this command (see `Cvm.self`, `installer-plan.md` D12).
- Does not manage cajeta **toolchains** — that is `cvm install` /
  `cvm default` (different commands). This verb is about cvm itself.

## To actually update cvm now

Re-run whatever installed cvm: rebuild from `tools/cvm` (`./build.sh` →
`build/cvm`) for a source checkout, or re-run your package manager for a
PM-managed install. Because cvm is version-independent by contract, a stale
cvm still installs current cajeta toolchains, so updating cvm is rarely
urgent.

Source: `tools/cvm/src/main/cajeta/cvm/Cvm.cajeta` (`self`), `README.md`.
