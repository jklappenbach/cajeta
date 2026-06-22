# olla local repository — spec (SRD)

## 1. Definition

### 1.1 Purpose
Give the cajeta build tool a **Maven-`.m2`-style local repository** at `~/.olla/`:
a single machine-global store of resolved library artifacts (`.cja`) that the
resolver consults **before** any network access, and into which it **writes
through** every artifact fetched from the remote **olla** registry. This makes
locally-developed sibling libraries (e.g. `dev.cajeta.codec`) resolvable by other
projects the same way published libraries are — by declaring an ordinary version
dependency — with no per-consumer `path` override.

### 1.2 Problem it solves
Today a consumer can only link an unpublished local library via a `path` override
in its manifest, which hard-codes a relative filesystem path into the consumer and
requires the dependency to be pre-built by hand. There is also no single, machine-
global artifact store: downloads land in a per-project content-hash cache
(`.cajeta/cache/artifacts/<sha256>.cja`), so the same artifact is re-fetched per
checkout and a developer's own library cannot be "installed" for local consumers.
This blocks `tools/mcp` from linking `dev.cajeta.codec.compress.Gzip`
(mcp-compression unit 3).

### 1.3 Scope
- A well-known local repository directory `~/.olla/` with the existing filesystem-
  repository on-disk layout.
- Resolution that checks `~/.olla/` first, then falls back to the remote olla
  registry over HTTP, writing the fetched artifact through into `~/.olla/`.
- A `cajeta install` action that publishes a project's built `.cja` into `~/.olla/`.
- Unifying the existing download cache into `~/.olla/` (it becomes the single
  artifact store).

### 1.4 Non-goals
- **Publishing to the remote olla registry** (authenticated `POST`, version-exists
  rejection) — that is the registry/publish flow, out of scope here.
- Changing version-constraint matching or the lockfile format (reused as-is).
- SNAPSHOT/mutable-version semantics — released versions are treated as immutable;
  `cajeta install` overwrites a local version in place (see 5.4). A richer
  snapshot model is deferred.
- The remote olla registry server itself (`cpp/web/cajeta-olla`) — unchanged.

### 1.5 Constraints
- Reuse the documented filesystem-repository layout and the existing
  `repository-protocol-v1` HTTP wire protocol; do not invent new formats.
- Artifact writes into `~/.olla/` must be atomic (temp + rename) so concurrent
  builds on one machine never observe a partial `.cja`.
- Fail loud: a declared dependency that is neither in `~/.olla/` nor obtainable
  from olla is a hard resolution error, never a silent skip.

---

## 2. The local repository store (`~/.olla/`)

### 2.1 Requirements
- The local repository root is `~/.olla/`, overridable by the `OLLA_HOME`
  environment variable (absolute path).
- On-disk layout matches the existing filesystem repository:
  `~/.olla/<name>/<version>/<name>-<version>.cja`, with a cached
  `manifest.json` beside each artifact (for offline transitive resolution) and a
  per-package `versions.json`.
- `~/.olla/` replaces `.cajeta/cache/artifacts/<sha256>.cja` as the artifact
  landing store. The per-project `.cajeta/cache/` retains only build IR (`ir/`)
  and git clones (`git/`).
- The store survives `cajeta clean`; only an explicit deep clean clears it.

### 2.2 Use cases
- 2.2.1 As the build tool, when I need `dev.cajeta.codec@0.5.0` and
  `~/.olla/dev.cajeta.codec/0.5.0/dev.cajeta.codec-0.5.0.cja` exists, then I
  resolve it from the local repository with no network access.
- 2.2.2 As a developer, when `OLLA_HOME=/srv/olla-cache` is set, then all local-
  repository reads and writes target `/srv/olla-cache` instead of `~/.olla`.
- 2.2.3 As the build tool, when the local repository directory does not yet exist,
  then the first write creates `~/.olla/` (and the needed `<name>/<version>/`
  subtree) rather than erroring.
- 2.2.4 As a developer, when I run `cajeta clean` in a project, then artifacts in
  `~/.olla/` are untouched.

---

## 3. Resolution order (local-first, HTTP fallback, write-through)

### 3.1 Requirements
- For each dependency `name@version` to resolve, the build tool checks `~/.olla/`
  **first**; a hit resolves immediately with no network access.
- On a local miss, the tool fetches the artifact (and `manifest.json`) from the
  remote olla registry over HTTP per `repository-protocol-v1`, honoring the
  manifest's declared `repositories` in priority order.
- A successfully fetched artifact is **written through** into `~/.olla/` (atomic
  write) so the next resolution of the same `name@version` is a local hit.
- Integrity: the fetched artifact's content hash is verified against the
  lockfile/`Content-SHA256` before it is committed to `~/.olla/`; a mismatch is a
  hard error and nothing is written.
- A dependency present neither locally nor remotely is a hard resolution error
  naming the missing `name@version` and the repositories tried.

### 3.2 Use cases
- 3.2.1 As the build tool, on a local miss for `cajeta.math@1.2.4`, when olla has
  it, then I download it, verify its hash, write it to
  `~/.olla/cajeta.math/1.2.4/`, and resolve from there.
- 3.2.2 As the build tool, after 3.2.1, when the same dependency is resolved again
  (this or another project), then it is served from `~/.olla/` with no network
  call.
- 3.2.3 As the build tool, when a fetched artifact's hash does not match the
  expected hash, then I fail the build and leave `~/.olla/` unchanged.
- 3.2.4 As the build tool, when a dependency is in neither `~/.olla/` nor any
  configured remote, then I fail with an error naming the artifact and the
  repositories consulted.
- 3.2.5 As a developer working offline, when every dependency is already in
  `~/.olla/`, then the build succeeds without any network access.

---

## 4. Linking a locally-installed library

### 4.1 Requirements
- A consumer declares an ordinary version dependency
  (`"dev.cajeta.codec": "0.5.0"`) — no `path`/`overrides` entry required once the
  library is installed locally.
- The build tool resolves it via the order in §3 and passes the resolved `.cja`
  to the compiler via the existing `--classpath` handoff, so a static call into
  the library (e.g. `Gzip.compress`) links into the consumer's executable.

### 4.2 Use cases
- 4.2.1 As `tools/mcp`, when I declare `"dev.cajeta.codec": "0.5.0"` and that
  version is installed in `~/.olla/`, then `cajeta build` resolves it locally and
  links `dev.cajeta.codec.compress.Gzip` into `build/cajeta-mcp`.
- 4.2.2 As a consumer, when I call a static method on a locally-installed library
  class, then the emitted executable runs it correctly (no undefined symbol at
  link, no path override in my manifest).

---

## 5. `cajeta install` — publish a local build into `~/.olla/`

### 5.1 Requirements
- A new build-tool action/task `install` builds the current project's library
  `.cja` (release or debug flavor as configured) and copies it into
  `~/.olla/<name>/<version>/<name>-<version>.cja`, deriving `<name>` and
  `<version>` from the project's `cajeta.json` `details`.
- `install` also writes/updates the cached `manifest.json` and the package's
  `versions.json` alongside the artifact.
- `install` is opt-in (a `cajeta install` invocation); an ordinary `cajeta build`
  does **not** mutate `~/.olla/`.
- Installing into a version directory that already exists overwrites it in place
  (local development convenience; released remote versions remain immutable).
- Installing a project with no library artifact (an executable project with an
  `entry-method`) is an error.

### 5.2 Use cases
- 5.2.1 As a developer in `cpp/cajeta-codec`, when I run `cajeta install`, then
  `dev.cajeta.codec-0.5.0.cja` is built and copied to
  `~/.olla/dev.cajeta.codec/0.5.0/`, and `versions.json` lists `0.5.0`.
- 5.2.2 As a developer, after editing the library and re-running `cajeta install`,
  then the artifact in `~/.olla/` is replaced and the next consumer build links
  the updated code.
- 5.2.3 As a developer, when I run `cajeta install` in an executable project
  (has `entry-method`), then it fails with a message that only libraries can be
  installed.
- 5.2.4 As a developer, when I run a plain `cajeta build`, then `~/.olla/` is not
  modified.

---

## 6. Backward compatibility & migration

### 6.1 Requirements
- Existing manifests that declare only published version dependencies resolve
  unchanged (now served from `~/.olla/` after first fetch).
- The `path` override mechanism, if still present in the resolver, continues to
  work but is no longer the recommended way to consume a local library; docs point
  to `cajeta install`.

### 6.2 Use cases
- 6.2.1 As an existing project, when I build after this change, then my published
  dependencies resolve and link exactly as before (transparently via `~/.olla/`).
- 6.2.2 As a developer with an existing `path` override, when I build, then it
  still resolves (no forced migration), though docs recommend `cajeta install`.
