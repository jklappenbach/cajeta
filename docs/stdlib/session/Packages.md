# Packages

`cajeta.session.Packages` — install a library into a **live session** so later
cells can import it. Static methods; there is nothing to construct.

Acquisition and binding stay separate, exactly as they are in a compiled
project. `install` puts an archive on the session's classpath; `import` names
packages from archives already acquired. `install` never imports anything
itself.

```cajeta
// cell 1
import cajeta.session.Packages;
Packages.install("dev.cajeta.ml", "0.10.*");
```

```cajeta
// cell 2 — the import resolves now, not before
import dev.cajeta.ml.Frame;
```

A cell **cannot import what it installs**: the cell is compiled before its
code runs, so the import is resolved before the install happens. The
unresolved-import diagnostic says so when a cell calls `install`.

This is an ordinary stdlib API rather than a `%`-magic, so it behaves
identically in every host and is testable like any other call. In a host with
no live session — `cajeta run`, a compiled binary — it throws
[PackageInstallException](PackageInstallException.md) rather than silently
doing nothing.

## Resolution and verification

The constraint resolves against the governing project's
`settings.repositories`, or the default central repository when no project
governs the session. The highest satisfying version from the first repository
carrying one wins.

A fetched archive is verified against the repository's published sha256 before
it is spliced; a mismatch discards the bytes and fails the install, so there
is never a half-installed state. When the repository publishes an ed25519
signature it is verified against the machine's trust store (`cajeta trust`) —
never against a key supplied with the artifact. Setting
`"require-signatures": true` in the governing manifest makes a signature
mandatory rather than opportunistic.

A cached artifact is served without touching the network, and is held to the
same verification as a freshly fetched one.

## Limits

Installs are additive. A session cannot unload or replace an archive it has
already loaded, because JIT'd code from it may be live: changing to a version
the loaded one does not satisfy requires a session restart. Installing an
archive that declares a class the session already holds is rejected — an
install never shadows session state.

Re-installing at a version already loaded and satisfying is a no-op returning
the loaded version, so re-running a notebook top to bottom is safe.

## Methods

| Signature | |
|---|---|
| `static #String install(String name, String constraint)` ⚑ | Acquire `name` at `constraint` for the running session; returns the resolved version. Session-only — a restart loses it |
| `static #String installAndSave(String name, String constraint)` ⚑ | As `install`, and also record the dependency in the governing project's `cajeta.json`, preserving its comments and formatting. Throws when no project governs the session |

Both return an owned `#String`, so bind the result with `#=`:

```cajeta
String version #= Packages.install("dev.cajeta.ml", "0.10.*");
```

`installAndSave` is a separate method rather than a flag so that reading the
call site tells you whether a file was written.

## Failure

Every rejection leaves the kernel serving: the next cell still runs and the
session keeps its bindings. Failures arrive as
[PackageInstallException](PackageInstallException.md), a
[RecoverableException](../error/RecoverableException.md), and can be caught:

```cajeta
try {
    Packages.install("dev.cajeta.ml", "0.10.*");
} catch (PackageInstallException e) {
    // the install was rejected; the session is still usable
}
```

## See also

- [PackageInstallException](PackageInstallException.md)
- [24 — Notebooks](../../guide/24-notebooks.md) — the guide, with the install → import → save walkthrough
