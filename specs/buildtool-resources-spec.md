# buildtool-resources — bundled resources and resolved assets

## 1. Definition

### 1.1 Purpose

Cajeta projects can ship code and nothing else. There is no `resources/`
convention, no way to package a non-source file into a `.cja`, no way to read
one back, and no way to declare a third-party binary artifact as a dependency.
The project layout is `src/main/cajeta/…` and `src/test/cajeta/…`; `cajeta.json`
carries `details`, `settings`, and `tasks`.

This spec adds two distinct mechanisms:

- **Bundled resources** (§2–§3) — files the author writes, commits, and ships
  inside the `.cja`.
- **Resolved assets** (§4–§8) — third-party artifacts fetched from a provider,
  pinned by content hash, and cached. Dependencies, not source.

They are deliberately separate. Conflating them means committing binaries you
did not author and losing the hash-pinning that makes resolution reproducible.

### 1.2 What drove this

`cajeta-chart-spec` §3.9 must ship a default font inside the library, and §3.10
must let a developer name a font from Google Fonts and have the toolchain obtain
it. Neither is possible today. Both generalize immediately: bundled palettes,
icon sets, tour fixtures and default configs on one side; fetched fonts,
reference datasets and pretrained model weights (`cajeta-ml-v3` §13.6) on the
other.

### 1.3 Prerequisite — dependency resolution must work first

**`buildtool-dependency-classpath-spec` reports that `cajeta.json`
`dependencies` are currently inert**: `cajeta build` does not resolve them and
the tour hand-plumbs `--classpath`. §4–§8 extend that resolver. If it does not
resolve `.cja` archives, it cannot resolve assets. That spec is a hard
prerequisite and must land first.

### 1.4 Scope

Resource layout and packaging; the read API and its capability properties;
asset declaration, resolution, hashing, and caching; providers; vendoring;
licence capture.

### 1.5 Non-goals

- **1.5.1** A general virtual filesystem. Reads are by resource path, not
  arbitrary traversal.
- **1.5.2** Mutable resources. Bundled resources are read-only.
- **1.5.3** Hosting or publishing assets. Olla publishes `.cja` archives; this
  spec consumes assets from providers.
- **1.5.4** Localization and per-locale resource selection.
- **1.5.5** Lazy download at run time. Resolution is a build-time activity,
  always (§4.2).

### 1.6 Systems

The cajeta build tool, `cajeta.json`, the `.cja` archive format, Olla,
`cajeta.io`, `cajeta.io.net.http`, `cajeta.hash` (**SHA-256** — the algorithm
the existing artifact cache uses, §6.2), `cajeta.wire` (compression),
the local cajeta repository at `~/.cajeta` (§6), `dev.cajeta.unit`.

---

## 2. Feature: bundled resources — layout and packaging

- **2.1** When a file is placed under `src/main/resources/`, `cajeta build`
  packages it into the `.cja` under a stable resource path mirroring its
  directory layout — the same relationship `src/main/cajeta/` already has to
  packages.
- **2.2** When a file is placed under `src/test/resources/`, it is available to
  tests and is **not** packaged into the published archive.
- **2.3** When the same project is built twice from identical inputs, the
  packaged bytes are identical, so archives stay reproducible.
- **2.4** When a resource path collides with another on the classpath, the
  collision is reported rather than silently shadowed — the failure mode
  `classpath-signature-shortname-rebind` already demonstrates for code.
- **2.5** When a `.cja` is inspected, its resources are listable, so what
  shipped is auditable.
- **2.6** When a resource is large, whether it is stored compressed is a
  declared choice, since already-compressed formats gain nothing and cost
  decode time.

---

## 3. Feature: bundled resources — the read API

- **3.1** When a resource is opened by path, the result is a readable stream,
  or a clear error naming the path and the archive searched.
- **3.2** When a bundled resource is read, it requires **no filesystem
  capability**. This is the property the whole mechanism exists for:
  `dev.cajeta.chart` ships a default font while declaring `"capabilities": []`,
  so drawing a chart never implies filesystem access. A resource read is a read
  from the archive, not from the environment.
- **3.3** When a resource is read from a dependency's archive rather than the
  reading project's own, that works and the owning archive is identifiable.
- **3.4** When resources are enumerated under a prefix, they are returned, so a
  library can discover its own bundled palettes or fonts without hardcoding a
  list.
- **3.5** When a resource is absent, the failure is immediate and explicit —
  never an empty stream, which turns a packaging error into a mysterious
  downstream defect.

---

## 4. Feature: resolved assets — declaration

- **4.1** When an asset is declared in `cajeta.json` alongside code
  dependencies, the declaration names its kind, provider, identity, version,
  and any kind-specific parameters (a font's weight, style, and Unicode
  subset).
- **4.2** When assets are resolved, it happens **at build time only**. Nothing
  in this spec fetches at run time, so no consuming library needs a network
  capability (`cajeta-chart-spec` §3.10.4).
- **4.3** When an asset is declared, its kind is explicit (`font`, `dataset`,
  `weights`, …) so the toolchain can apply kind-specific validation such as
  §9.2's embedding check.
- **4.4** When a resolved asset is available, it is read through the **same API
  as §3**, so consuming code does not care whether an asset was bundled or
  resolved.

---

## 5. Feature: resolution, hashing, and the lockfile

- **5.1** When an asset is resolved for the first time, its **content hash is
  written to a lockfile** that is committed.
- **5.2** When a later resolution produces different bytes than the lockfile
  records, it is a **hard error**. Providers re-cut artifacts without notice;
  unpinned assets would silently change rendered output and break golden-file
  tests with no visible cause.
- **5.3** When new bytes are intended, updating the lock is an explicit,
  separate action — never a side effect of building.
- **5.4** When the lockfile is present and the cache is warm, a build performs
  **no network access at all**.
- **5.5** When a lockfile is reviewed, it records for each asset the provider,
  identity, version, hash, size, and licence, so a diff shows exactly what
  changed.
- **5.6** When two dependencies request the same asset at different versions,
  the conflict is reported rather than resolved by silent precedence.

---

## 6. Feature: the cache

Assets are cached **in the local cajeta repository, addressed exactly as
libraries already are** — same store, same scheme, different namespace:

```
~/.cajeta/
├── cache/
│   ├── artifacts/<sha256>.cja    existing — resolved libraries
│   └── assets/<sha256>           new — resolved assets
├── settings                       user-level config
└── versions/  locks/  bin/  native/
```

- **6.1** When an asset is resolved, it is stored in the local cajeta repository
  under `cache/assets/`, keyed by the **SHA-256 of its content**, so multiple
  projects fetch it once and an asset is addressed the same way a library is.
- **6.2** When an asset is keyed, the hash is **SHA-256** — the algorithm the
  existing `cache/artifacts/` namespace already uses. One store must not carry
  two key spaces.
- **6.3** When a cached entry's bytes do not match its key, it is treated as
  corrupt and refetched.
- **6.4** When the cache root must move, it is resolved by precedence:
  an explicit **CLI flag**, then a **`~/.cajeta/settings` property**, then the
  default `~/.cajeta/cache`. Relocating `CAJETA_HOME` moves the whole
  repository, cache included.
- **6.5** When the cache location is configured, it is a **machine-level**
  setting in `~/.cajeta/settings`, never a project's `cajeta.json` — a cache
  path committed to a project manifest breaks on every other machine.
- **6.6** When an environment variable is used to override the cache, it is
  supported as the CI and container escape hatch but is **not** the primary
  mechanism. A build whose behaviour depends on ambient environment state cuts
  against the same reproducibility discipline as §5.2's hash pinning.
- **6.7** When a build runs, it can **report which cache root it used**. This is
  the first diagnostic question when a hash mismatch appears, and it must not
  require guessing.
- **6.8** When a build runs offline and every asset is cached, the build
  succeeds.
- **6.9** When a build runs offline and an asset is missing, the error names the
  asset, its hash, and the cache path checked — never a silent substitution or a
  skipped asset.
- **6.10** When a clean slate is needed, the cache can be inspected and pruned
  through the build tool, per namespace.

### 6.11 Nothing is written to the working directory

- **6.11.1** When a project is built, the build tool writes in exactly two
  places: the cache root above, and `cajeta.lock` beside `cajeta.json`. It never
  writes into the source tree except on explicit vendoring (§8.1), and the
  process working directory is never used as storage.
- **6.11.2** When a bundled resource is read at run time, it comes from the
  `.cja` archive — **not** the filesystem, and never a path relative to the
  working directory (§3.2). Runtime has no cache and needs none.

---

## 7. Feature: providers

- **7.1** When a provider is named, Google Fonts, a generic HTTPS URL plus
  hash, a local path, and Olla are all expressible behind one provider
  abstraction — no provider is privileged in the design.
- **7.2** When a provider is added, it implements one interface and no
  consuming code changes.
- **7.3** When an asset is fetched over the network, the transport is HTTPS
  with certificate verification.
- **7.4** When a provider is unreachable, the failure names the provider and
  the asset, and the build fails rather than degrading.
- **7.5** When assets are mirrored internally, pointing at that mirror is
  configuration, not a code change.

---

## 8. Feature: vendoring — the bridge between the two mechanisms

- **8.1** When a resolved asset is vendored, the build tool copies it into
  `src/main/resources/` and the declaration may be dropped — the asset becomes
  an ordinary bundled resource (§2).
- **8.2** When an asset is vendored, the provenance recorded in §5.5 is
  preserved alongside the file, so a committed binary is still attributable.
- **8.3** When everything is vendored, the project builds with no network
  access and no cache — the fully reproducible path.

---

## 9. Feature: licence and provenance

- **9.1** When an asset is resolved, its licence is recorded in the lockfile,
  and its licence text is retrievable.
- **9.2** When an asset kind carries a redistribution constraint, it is checked
  at resolve time. **A font whose licence forbids embedding fails the build
  with the reason named** — discovering that after distributing PDFs is the
  wrong ordering (`cajeta-chart-spec` §3.10.7).
- **9.3** When an archive ships containing bundled or vendored third-party
  assets, their licences are enumerable, so attribution obligations are
  dischargeable.

---

## 10. Feature: failure behaviour

- **10.1** When any asset fails to resolve, verify, or parse, the build fails
  loudly. There is **no** fallback substitution anywhere in this spec: a
  substituted font changes every glyph, every text measurement, and therefore
  every layout decision, while looking superficially fine.
- **10.2** When an artifact is malformed, it is rejected before any consumer
  parses it — untrusted binaries are hash-checked first.
- **10.3** When an error is reported, it names the asset, the provider, the
  expected hash, and the path examined.

---

## 11. Open questions (resolve at plan time)

- **11.1** *(resolved 2026-08-01 — a separate file.)* A committed
  `cajeta.lock` beside `cajeta.json`, not a section inside it, so
  machine-updated content never churns a hand-edited manifest.
- **11.2** *(resolved 2026-08-01 — SHA-256.)* An earlier draft recommended
  Blake3 as the canonical key. That was wrong: the existing
  `~/.cajeta/cache/artifacts/` namespace is already SHA-256-addressed (one
  cached artifact is `e3b0c44298fc…b7852b855`, the SHA-256 of the empty input).
  Assets use the same algorithm; a single store must not carry two key spaces.
- **11.3** *(resolved 2026-08-01 — its own package.)* The resource API is
  **`cajeta.resource`**, not part of `cajeta.io`. It is an archive concern, not
  a filesystem one, and §3.2's capability property — reading a bundled resource
  must not require filesystem capability — depends on that distinction being
  visible in the package structure.
- **11.4** **Still open — deferred, not decided.** Typed resource handles
  generated at build time would turn §3.5's run-time "resource not found" into a
  compile error. Genuinely attractive and genuinely larger: it means codegen in
  the build tool and a new generated-source story. Recommendation: ship string
  paths in v1, and revisit once §3's API has real consumers — the handle type
  can be added over the string API without breaking it.
- **11.5** *(resolved 2026-08-01.)* One cache, two namespaces —
  `~/.cajeta/cache/{artifacts,assets}`. The local cajeta repository already
  exists and is already content-addressed, so assets need no new location
  concept (§6).
- **11.6** *(resolved 2026-08-01 — no.)* `src/test/resources/` (§2.2) does not
  participate in vendoring. Test fixtures are committed with the source and are
  not fetched, so the vendoring machinery has nothing to do for them.

---

## 12. Acceptance criteria (spec-level)

- **12.1** A library ships a bundled binary resource, reads it back, and
  declares `"capabilities": []` — proving §3.2, the property the mechanism
  exists for.
- **12.2** Building the same project twice produces byte-identical archives.
- **12.3** A build with a warm cache and a complete lockfile performs zero
  network access, asserted by test.
- **12.3.1** Assets land in `~/.cajeta/cache/assets/` keyed by SHA-256, sharing
  the repository the `.cja` artifact cache already uses (§6.1, §6.2).
- **12.3.2** Nothing is written to the process working directory, at build time
  or run time (§6.11).
- **12.3.3** The cache root honours flag → settings → default precedence, and
  the build reports which root it used (§6.4, §6.7).
- **12.4** A tampered cache entry and a lockfile hash mismatch are both
  detected and both fail the build.
- **12.5** No failure path substitutes an asset or silently continues (§10.1).
- **12.6** A resolved asset, once vendored, builds identically with the
  declaration removed and the network unavailable.
- **12.7** An asset whose licence forbids redistribution fails at resolve time,
  not at publish time.
- **12.8** `cajeta-chart-spec` §3.9 and §3.10 are satisfiable using only this
  spec's mechanisms, with no chart-specific special case.
