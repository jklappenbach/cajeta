# 24 — Notebooks

Cajeta runs as a Jupyter kernel. `cajeta kernel` speaks the v5.3 protocol over
ZeroMQ, and a notebook cell is a script unit: top-level code, compiled and
JIT'd into a session that accumulates. Bindings made in one cell are there in
the next, `Out[N]` renders the cell's value, and a traceback names the cell it
came from.

Install the kernelspec once:

```
cajeta init --kernel
```

Then start Jupyter Lab as usual and pick the Cajeta kernel. The kernel adopts
the project it was launched in, so run it from your notebook's directory and
that project's dependencies are on the classpath from the first cell.

To scaffold a project shaped for this:

```
cajeta init notebook my-analysis
```

---

## Installing a library mid-session

A notebook is an exploratory place, and needing a restart to add a dependency
breaks the session you were exploring in. `cajeta.session.Packages` acquires a
library into the running kernel:

```cajeta
// cell 1
import cajeta.session.Packages;
Packages.install("dev.cajeta.ml", "0.10.*");
```

```cajeta
// cell 2 — the import resolves now
import dev.cajeta.ml.Frame;
Frame f = Frame.fromCsv("sales.csv");
```

`install` returns the resolved version as a `String`, and prints its phases as
it goes so a network fetch is never a silent stall:

```
  resolving dev.cajeta.ml 0.10.*
  fetching dev.cajeta.ml 0.10.4 from central
  verifying dev.cajeta.ml 0.10.4
  splicing dev.cajeta.ml 0.10.4
```

### Acquisition and binding are separate

`install` puts an archive on the session's classpath. `import` names packages
from archives already acquired. They are the same two layers a compiled
project has, and `install` never imports anything itself.

The practical consequence is the one rule to remember: **a cell cannot import
what it installs.** The cell is compiled before its code runs, so the import
is resolved before the install has happened. Put the import in the next cell.
The diagnostic says so if you forget.

### Re-running is safe

Installing a library already loaded at a satisfying version is a no-op that
returns the loaded version, so running a notebook top to bottom a second time
does not re-fetch anything.

Installs are additive. A session cannot unload or replace an archive it has
already loaded — JIT'd code from it may be live — so moving to a version the
loaded one does not satisfy needs a restart, and the error says as much.

---

## Keeping the dependency

By default an install lasts as long as the session. The manifest remains the
reproducible record, so a notebook you intend to share should record what it
needs:

```cajeta
import cajeta.session.Packages;
Packages.installAndSave("dev.cajeta.ml", "0.10.*");
```

That installs exactly as `install` does, and also writes the dependency into
the governing project's `cajeta.json`. Your comments and formatting survive —
it is the same editor `cajeta add` uses. After that the library loads at
session start and the notebook needs no install cell at all.

It is a separate method rather than a flag on `install` so that reading the
call tells you whether a file was written.

---

## Verification

Installing code into a running session is a supply-chain surface, so an
archive is checked before it is spliced.

Its sha256 is verified against the checksum the repository publishes. A
mismatch discards the bytes and fails the install; nothing is half-installed.

When the repository publishes a signature, it is verified too — ed25519,
against the keys **your machine** trusts, not a key shipped alongside the
artifact. Manage those with `cajeta trust`:

```
cajeta trust add acme-releases ./acme-releases.pem
cajeta trust list
```

A signature that does not match a trusted key rejects the install. So does a
signed archive on a machine with no trusted keys at all: unverifiable is not
the same as fine.

Signatures are opportunistic by default — an unsigned archive with a good
checksum installs. To make them mandatory, set the floor in `cajeta.json`:

```json
{
    "settings": {
        "require-signatures": true
    }
}
```

Note what the default does and does not buy you. A checksum authenticates the
mirror: it says these are the bytes the repository meant to serve. Only a
signature says a publisher you chose to trust stands behind them.

---

## When an install fails

Every rejection leaves the kernel serving. You lose the install and nothing
else — the session keeps its bindings and the next cell runs. That is
deliberate: a notebook session can represent hours of work, and a bad version
constraint is not a reason to lose it.

Outside a session — `cajeta run`, a compiled program — `Packages.install`
throws a recoverable `PackageInstallException` saying there is no live session
to install into. Declare the dependency in `cajeta.json` instead.

---

## See also

- [`cajeta.session.Packages`](../stdlib/session/Packages.md) — the API reference
- [03 — Your first project](03-your-first-project.md) — manifests and dependencies
- [05 — Debugging](05-debugging.md) — the kernel's place in the tooling
