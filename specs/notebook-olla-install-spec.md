# notebook-olla-install — installing Olla libraries from the notebook prompt

Spec status: **draft** (jupyter-kernel plan Unit 8; surface decided with
Julian 2026-08-19: stdlib API, session-only with explicit save).

## 1. Definition

A running notebook session can install a library from `olla.cajeta.dev`
(or any repository the governing manifest declares) without leaving the
notebook: resolve, verify, fetch, and splice the archive into the live
session so later cells can import it.

The surface is a **stdlib API** — `cajeta.session.Packages` — not a cell
directive and not a `%`-magic. One semantics everywhere: the call behaves
identically in any session host, is testable like any API, and preserves
the jupyter-kernel spec §1.4 non-goal ("no magics"). A directive form, if
ever wanted, is later sugar that desugars to this API.

- **1.1 Scope.** Session hosts (the Jupyter kernel today). The API exists
  everywhere the stdlib does; in a host with no live session it throws a
  located recoverable error rather than crashing or silently no-opping.
- **1.2 Constraints.** Resolution, cache, and verification reuse the
  buildtool's machinery (`NativeResolver`, the artifact cache, lockfile
  sha256) — no second fetch path. Installs are additive: a session cannot
  unload or replace an already-loaded archive (JIT'd code from it may be
  live); changing versions requires a session restart.
- **1.3 Non-goals.** No `%`-magics (unchanged). No directive syntax in
  v1. No arbitrary-URL installs — repositories come from the governing
  manifest (or the default central). No unloading. No transitive
  dev-dependency installation.

## 2. The API surface

`cajeta.session.Packages`, static methods.

- **2.1** When `Packages.install("dev.cajeta.ml", "0.10.*")` runs in a
  session, the constraint is resolved against the session's repositories,
  the archive is fetched (or served from cache), verified, and spliced
  into the session; the call returns the resolved version as a `String`.
- **2.2** When the next cell imports a package from the installed
  archive, the import resolves and its code runs — the splice is visible
  to every cell compiled after the installing cell completes.
- **2.3** When the same cell that called `install` also imports the new
  package, the import fails exactly as it would have before the install
  (the cell was compiled first). The error's hint names the fix: import
  from the next cell.
- **2.4** When `install` is called for an archive already in the session
  at a satisfying version, it is a no-op returning the loaded version —
  re-running a notebook top to bottom is safe.
- **2.5** When `install` names an archive already loaded at a version the
  new constraint excludes, it throws a located recoverable error naming
  both versions and stating that a session restart is required (1.2).
- **2.6** When the constraint matches nothing in any repository, it
  throws a located recoverable error naming the constraint and the
  repositories consulted.
- **2.7** When called in a host with no live session, it throws a located
  recoverable error (1.1).

## 3. Resolution, verification, and cache

- **3.1** Repositories are the governing project's `settings.repositories`
  when a project governs the session, else the default central
  (`olla.cajeta.dev`).
- **3.2** When an archive is fetched, its sha256 is verified against the
  repository's published checksum; a mismatch discards the bytes and
  fails the install with a located error — never a half-installed state.
- **3.3** When the repository publishes a signature for the archive, the
  signature is verified and a failure rejects the install. When the
  session's policy requires signatures (3.5) and none is published, the
  install is rejected.
- **3.4** When the archive is already in the artifact cache with a
  matching checksum, it is served offline — no network touch. When it is
  not cached and the network is unavailable, the install fails with an
  error naming the cache location and the miss.
- **3.5** Policy: a notebook installing code is a supply-chain surface.
  The default policy verifies checksums always and signatures when
  published; a `require-signatures` setting in the governing manifest
  makes 3.3's strict arm the floor. (Signature plumbing that does not
  exist yet is plan work, not assumed here.)

## 4. Live-session splice

- **4.1** When an archive is spliced, its declarations become resolvable
  and its definitions linkable exactly as a session-start classpath entry
  would be (KernelSession's ingest path) — one mechanism, not two.
- **4.2** When the archive's signatures pull lazy stdlib packages that the
  session has not yet materialized, they materialize as they would at
  session start; instantiation dedupe holds (no duplicate-definition
  errors against the accumulating world).
- **4.3** When an installed archive declares a class whose canonical name
  the session already holds (from an earlier cell or another archive),
  the install fails with a located error naming the collision — installed
  code never silently shadows session state, and generational
  redefinition (script-units §5.3) stays a cell-code feature.
- **4.4** When the install succeeds, bodies from the archive are emitted
  on demand like all session code (lazy codegen) — installing a large
  library does not stall the cell that installed it beyond resolve +
  fetch + splice.

## 5. Persistence

- **5.1** Default: an install affects only the running session. A restart
  loses it; the manifest is the reproducibility record.
- **5.2** When `Packages.install(name, constraint, /*persist=*/true)` is
  called under a governing project, the dependency is also written to
  that project's `cajeta.json` (`settings.dependencies`) with the same
  comment-and-format-preserving editor `cajeta add` uses, and the return
  value is unchanged.
- **5.3** When persist is requested with no governing project, the call
  throws a located recoverable error suggesting `cajeta init notebook`.
- **5.4** When the manifest already pins the dependency, persist rewrites
  the constraint only if it differs, and says so in the session's stream
  output.

## 6. Feedback

- **6.1** When an install runs, its phases stream to the cell's output
  (the 7.2.8 narration channel): resolving, fetching (with size),
  verifying, splicing — a network fetch is never a silent stall.
- **6.2** When an install fails, the error is a located diagnostic in the
  cell's output (the compiler-jsonl bridge), never a dead kernel; the
  session remains usable.
