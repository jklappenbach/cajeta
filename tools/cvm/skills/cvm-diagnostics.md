---
id: cvm-diagnostics
applies-to: [cvm/which, cvm/doctor]
title: cvm which & doctor — read-only home/toolchain inspection
description: How to run cvm's which and doctor commands and read their exact output and exit codes.
---

# cvm diagnostics: `which` and `doctor`

Two read-only inspection commands. Neither touches the network or writes any
file. Pick by what you need:

| Want to know… | Run | Reads |
|---|---|---|
| Resolved cvm home, active toolchain, shim & versions paths | `cvm which` | env + `<home>/settings` |
| Why a home resolved (which env var won) + active-toolchain health | `cvm doctor` | `$CAJETA_HOME`, `$XDG_DATA_HOME`, `$HOME`, `<home>/settings` |

Both share the same **home resolution** (`Home.dir`), checked in order:

1. `$CAJETA_HOME` (non-empty) → used verbatim.
2. `$XDG_DATA_HOME` (non-empty) → `$XDG_DATA_HOME/cajeta`.
3. `$HOME` (non-empty) → `$HOME/.cajeta`.
4. none of the above set → **no home resolves**.

The **active toolchain** is read from `<home>/settings`, a line-based
`default = <version>` file. No file, or no `=` line, means none is set. (Note:
the on-disk file is `<home>/settings`, not `settings.toml`.)

## `cvm which`

Invocation (no args, no flags):

```sh
$ cvm which
cvm home:   /home/julian/.cajeta
active:     0.7.0
  binary:   /home/julian/.cajeta/versions/0.7.0/cajeta
shim:       /home/julian/.cajeta/bin/cajeta
toolchains: /home/julian/.cajeta/versions
```

Output contract (stdout), in order:
- `cvm home:   <home>`
- `active:     <none set>  (run 'cvm default <version>')` when settings has no
  default; otherwise `active:     <version>` followed by
  `  binary:   <home>/versions/<version>/cajeta`, and — only if that binary is
  missing — `  warning:  the active toolchain's binary is missing — reinstall or pick another.`
- `shim:       <home>/bin/cajeta`
- `toolchains: <home>/versions`

Exit codes: **0** always when a home resolves (even with no active toolchain or
a missing binary — the warning is informational, not an error). **1** only when
no home resolves (see below).

## `cvm doctor`

Invocation (no args, no flags):

```sh
$ cvm doctor
cvm doctor
  version:        0.1.0
  $CAJETA_HOME: <unset>
  $XDG_DATA_HOME: <unset>
  $HOME: /home/julian
  resolved home:  /home/julian/.cajeta
  active toolchain: 0.7.0
```

Output contract (stdout), in order:
- `cvm doctor`
- `  version:        <cvm version>` (cvm's own build version, not a toolchain version)
- `  $CAJETA_HOME: <value>` / `  $XDG_DATA_HOME: <value>` / `  $HOME: <value>` —
  each prints `<unset>` when the var is unset or empty.
- `  resolved home:  <home>`, or `  resolved home:  <none> (set $CAJETA_HOME or $HOME)`.
- `  active toolchain: <none set>` | `<version>` | `<version>  (binary MISSING)`.
  The last line is omitted when no home resolved.

Exit codes: **0** when a home resolves; **1** when no home resolves.

## No-home failure (both commands)

When none of `$CAJETA_HOME`/`$XDG_DATA_HOME`/`$HOME` is set, `which` prints to
**stderr** and exits 1:

```
cvm: cannot resolve a home directory (set $CAJETA_HOME or $HOME).
```

`doctor` instead prints its diagnostic block, ends with the
`resolved home:  <none> ...` line on stdout, and exits 1. Use this to
distinguish: `which` is terse and fails fast; `doctor` always prints the env it
saw, even on failure.

## What these do NOT do

- They never write `settings`, create the home, or repoint the shim — that's
  `cvm default <version>`. The paths printed (`bin/cajeta`, `versions/…`) may
  not exist on disk; these commands report intent, not presence (except the
  binary-missing checks above, which stat the file).
- They do **not** walk `$PATH` to list every `cajeta` and who owns each (a
  later phase). `which` reports only cvm's own shim path.
- No network, no install, no version resolution against the release manifest —
  use `cvm install`.
- No flags are parsed; extra args are ignored.
