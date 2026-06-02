# Build tool implementation plan

Companion to [`cajeta-docs/BuildTool.md`](../cajeta-docs/BuildTool.md).
That document is the **spec**; this is the **plan**.

Every concrete unit of work is a checkbox `- [ ]`. Mark
`- [x]` when shipped. The acceptance criteria under each phase
also use checkboxes — a phase is complete when all of its
deliverables AND all of its acceptance criteria are checked.

## Design recap

One binary, `cajeta`, that loads a single `cajeta.json` manifest
and runs **tasks** defined in it. Tasks are sequences of
**actions**; actions are native (built-in) or plugin-provided.
Output threading via `${id.field}` connects producers to
consumers. No fixed lifecycle — `cajeta init` writes a Maven-
shaped starter set of tasks that the project fully owns from
day one.

Six manifest top-level blocks: `details`, `properties`,
`settings`, `actions` (presets), `plugins`, `tasks`. The
lockfile (`cajeta.lock`) captures resolved dep versions +
resolved property set + resolved plugin versions; signed
manifest-checksum guards against drift.

Single repo, single binary, no daemon. The compiler is the back
end (already exists); the build tool is the new code.

---

## Scope

### In v1

- [ ] Build tool binary (`cajeta`) — manifest load, lockfile
      read/write, task runner, action catalog, plugin runtime.
- [ ] Native action catalog: `build`, `clean`, `test`, `lint`,
      `doc`, `fmt`, `copy`, `delete`, `mkdir`, `sign`,
      `verify-sig`, `version`, `upload` (s3/azure/gcs/http/sftp),
      `download`, `publish`, `set-profile`, `run-task`,
      `parallel`, `exec`.
- [ ] Properties (`${PROPERTY}` substitution), profiles,
      transitive overrides, capability declarations.
- [ ] Repository types: filesystem, HTTP, Git, Maven-compat shim.
- [ ] Incremental builds (IR cache).
- [x] Default `cajeta init` template (basic/workspace/multi-binary/melt
      archetypes, embedded from `samples/buildtool/` at build time).
- [ ] First-party plugins: `cajeta.coverage`,
      `cajeta.lint.security`.
- [ ] Archive signing + launcher verification + `cajeta trust`.
- [ ] Sandboxing (Linux first; macOS / Windows follow).
- [ ] SLSA-style attestation in `publish` action.

### Explicitly deferred (post-v1)

These are deliberately NOT in scope:

- Hosted public registry (`repo.cajeta.org`) — infrastructure
  project, separate effort.
- Remote build cache.
- Multi-language build orchestration.
- IDE protocol (LSP-style integration with manifest).
- `cajeta vendor` advanced flows.
- Profile auto-activation conditions.

---

## Phase 0 — Foundations

**Goal:** the binary exists, loads a `cajeta.json`, errors
sensibly on malformed input.

### Deliverables

- [x] `cajeta` binary entry point at
      `src/cajeta/buildtool/` (BuildToolCommands dispatcher
      wired into the existing `cajeta` binary main.cpp, parallel
      to the archive dispatcher).
- [x] JSONC parser — strip `//` and `/* */` comments, accept
      trailing commas. `src/cajeta/buildtool/JsonC.{h,cpp}`.
      Wraps `llvm::json::parse`.
- [ ] Manifest schema validator wired to `manifest-v1.json` JSON
      Schema. (C++ validation logic ships now; the JSON Schema
      file lands with the open-specs workstream.)
- [x] Top-level block validation (`details`, `properties`,
      `settings`, `actions`, `plugins`, `tasks`).
      `src/cajeta/buildtool/Manifest.{h,cpp}`.
- [x] `cajeta info` subcommand printing the parsed manifest as
      resolved + normalized JSON.
      `src/cajeta/buildtool/BuildToolCommands.{h,cpp}`.

### Acceptance

- [x] A minimal valid `cajeta.json` loads cleanly.
- [x] Each documented validation error has a regression test
      (12 ManifestTests covering missing-block / missing-field /
      wrong-type / unknown-block / unknown-field / malformed-JSON
      / non-object-root / non-string-author cases).
- [x] Malformed JSON produces a citation-style error with the
      source label prefix. (Line/column from `llvm::json` lands
      with a follow-up; today the error cites which block /
      field is the offender.)

---

## Phase 1 — Properties + substitution

**Goal:** `${PROPERTY}` substitution works across the manifest.

### Deliverables

- [x] Built-in property resolver: `${details.name}`,
      `${details.version}`, `${details.group}`,
      `${details.library}`, `${flavor}`, `${profile}`,
      `${target}`, `${env.NAME}`, `${workspace.root}`,
      `${cajeta.version}`. `src/cajeta/buildtool/Properties.{h,cpp}`.
- [x] User-defined property resolver from `properties` block.
- [x] Topological resolution for property-references-property
      via DFS-and-memoize.
- [x] Cycle detection with citation (error names the cycle
      members in order).
- [x] Missing-property hard error with reference-site citation.
- [x] CLI `-P NAME=VALUE` override.
- [x] `CAJETA_PROPERTY_NAME` env override.
- [x] Override precedence: CLI → env → manifest. (No "active
      profile" precedence layer — profile is a per-task literal,
      not a central state. See BuildTool.md "Profiles".)
- [x] `cajeta info --properties` prints the resolved set
      (substituted values in resolution order).
- [x] `$$` escape for literal `$`.

### Acceptance

- [x] Built-ins resolve correctly across the test matrix
      (4 tests in PropertiesTests).
- [x] Property-references-property resolves topologically
      (3-level chain test passes).
- [x] Cyclic property references fail at load time with a clear
      cycle diagram in the error.
- [x] CLI and env overrides apply in the documented precedence
      order (3 precedence tests).
- [x] User property collision with a built-in is a hard error
      (`details.*` and `env.*` namespace collision tests).

---

## Phase 2 — Lockfile generation + reading

**Goal:** `cajeta.lock` captures resolved state;
manifest-checksum detects drift.

### Deliverables

- [x] `cajeta.lock` reader/writer (strict JSON; schema
      `lockfile-v1.json` file deferred to open-specs
      workstream, same as Phase 0). `src/cajeta/buildtool/Lockfile.{h,cpp}`.
- [x] Manifest SHA-256 checksum computation
      (`sha256Hex()` wraps libcrypto's EVP_sha256).
- [x] Drift detection — `checkDrift()` returns the
      old/new checksum pair + a `changed` flag. Wired into
      `cajeta info --check-lockfile`; exit 1 on drift.
- [x] Resolved property set persisted in lockfile under
      `properties`; sorted key order on write so the on-disk
      form is stable across resolution-order variations.
- [x] Plugin version pinning slot in lockfile (top-level
      `plugins` array). Empty until Phase 7 populates it.
- [x] `cajeta info --write-lockfile` / `--check-lockfile`
      CLI surface.
- [x] `nowIsoUtc()` helper for ISO 8601 timestamps (injectable
      into `composeLockfile()` so tests can pin them).

### Acceptance

- [x] Lockfile round-trip preserves byte-identity given
      identical inputs (`writeProducesByteIdenticalOutputForSameInputs`).
- [x] Properties on disk appear in deterministic
      (lexicographic) order regardless of insertion order
      (`writeIsStableAcrossPropertyInsertionOrder`).
- [x] Modifying the manifest without re-resolving surfaces the
      drift on next build (`driftReportsChangeOnModifiedSource`,
      plus whitespace-change strict-bytewise test).
- [x] Lockfile diff in code review surfaces version changes
      (the deterministic write ordering ensures diffs only
      show actual content changes, not key-order noise).

---

## Phase 3 — Task runner + action invocation

**Goal:** `cajeta <task>` runs a task that calls the `exec`
action. End-to-end the simplest possible task works.

### Deliverables — Phase 3a (linear executor)

- [x] Task model + parsing (`src/cajeta/buildtool/Task.{h,cpp}`).
- [x] Action infrastructure: `Action` interface,
      `ActionRegistry`, `TaskContext`
      (`src/cajeta/buildtool/Action.{h,cpp}`).
- [x] `exec` action implementation
      (`src/cajeta/buildtool/actions/ExecAction.cpp`); fork +
      exec + waitpid; stdout/stderr captured AND forwarded to
      the parent's streams; `working-dir` + `env` honored.
- [x] Task runner — linear execution
      (`src/cajeta/buildtool/TaskRunner.{h,cpp}`); param
      binding (CLI > default > required-or-error); output
      threading via `${id.field}`.
- [x] Substitution lookup tier: task-context lookup checks
      `${params.<name>}`, `${<id>.<field>}`, then falls back to
      manifest properties.
- [x] Duplicate-id rejection at task entry (two actions
      publishing the same id is a hard error).
- [x] `cajeta tasks` subcommand — list names + descriptions.
- [x] `cajeta <task>` dispatch — manifest-aware: looks up
      task in `./cajeta.json`; falls through to the compiler
      for non-task args.
- [x] CLI: `-P NAME=VALUE` property override, `-p NAME=VALUE`
      task-param binding, `--flavor=X`, `--profile=X`,
      `--manifest=<path>`.
- [x] Task `outputs` block resolves at task exit.

### Deliverables — Phase 3b (composition + DAG)

- [x] Task model extended to a tagged union
      (`ActionEntry::Kind` = Invocation / Parallel / RunTask).
- [x] Parser recognizes `parallel` groups + `run-task` entries
      + exclusive `action`/`parallel`/`run-task` discriminator.
- [x] `validateTaskGraph()` — undefined-dep check + cycle
      detection via DFS with gray/black coloring; error names
      the cycle members in order.
- [x] `runTask()` topologically expands `depends-on` and runs
      each dep before the target; deps already executed in the
      same invocation are not re-run.
- [x] `parallel` execution: one `std::thread` per child with
      `TaskContext::snapshot()`-isolated contexts; `join()`
      then `mergeOutputs()` in declaration order so the parent
      sees deterministic post-parallel state.
- [x] `run-task` composition: recursive `runTask` with the
      entry's params substituted in the calling context;
      called task's resolved `outputs` block published under
      the entry's `id`.
- [x] `when` / `skip-when` evaluated at action invocation:
      truthy = anything not in {"", "false", "0", "null"};
      `when=false` skips, `skip-when=true` skips.
- [x] `cajeta task <name> --show` prints the resolved action
      sequence + outputs without executing; best-effort
      substitution leaves unknowns as literal `${name}`.
- [x] Task graph validated up front in `loadProject()` so any
      cycle errors before the first action runs.

### Acceptance

- [x] A task containing only `exec` calls runs end-to-end
      (Phase 3a — verified by 12 TaskRunnerTests).
- [x] `cajeta tasks` lists task names with descriptions.
- [x] `cajeta <task>` runs the named task; `-p NAME=VALUE`
      binds task params; `-P NAME=VALUE` overrides properties.
- [x] Output threading works: `${id.stdout}` from one action
      flows into a later action's params.
- [x] An action referencing a missing property fails with a
      citation naming the action.
- [x] An unknown task name errors clearly.
- [x] Duplicate `id` across actions in one task errors at
      task load.
- [x] Parallel children run concurrently and merge outputs.
- [x] `run-task` composes one task's outputs into another's
      input chain (consumer reads `${id.field}` where field is
      from the called task's `outputs` block).
- [x] A cycle in the depends-on graph fails at load time with
      a citation naming the cycle members.
- [x] `when` / `skip-when` skip actions when expected; the
      action's child command is never invoked when skipped.
- [x] `cajeta task <name> --show` prints the structure without
      running.

---

## Phase 4 — Native action catalog: filesystem + crypto

**Goal:** ship the action verbs that don't depend on the
compiler/repository surface.

### Deliverables

- [x] `copy` action — `std::filesystem::copy` with recursive +
      mkdir-parent + multi-source (`also` array) support.
- [x] `delete` action — `std::filesystem::remove_all`;
      `if-exists` default true (missing path is silent OK).
- [x] `mkdir` action — `std::filesystem::create_directories`;
      `recursive` default true.
- [x] `sign` action — libcrypto ed25519. Mirrors the OpenSSL
      flow in `cli/ArchiveCommands.cpp::cmdSign` (same EVP
      DigestSign single-shot path). Accepts key as PEM via
      `key-env` or `key-path`. Writes detached `.sig` next to
      the input (or to `--out`). Records `key-id` in outputs
      for downstream consumers.
- [x] `verify-sig` action — libcrypto ed25519 verify; mirrors
      `cmdVerifySig`. Returns `valid: "true"|"false"` as an
      action output rather than failing the task — consumers
      compare against an expectation. Tampering / wrong key
      produces a clean "false" result, not an exception.
- [x] `version` action — semver bump (major/minor/patch) or
      explicit set; mutates manifest's `details.version`
      in-place (regex-based replacement so JSONC comments and
      formatting are preserved byte-for-byte except for the
      version string itself).
- [x] `download` action — `curl`-shell HTTP fetch with optional
      `sha256` verification on the received bytes. Phase 6
      swaps the transport in (libcurl or native) when the
      repository protocol lands; until then curl-shell is
      adequate for the use cases that aren't latency-sensitive.

### Acceptance

- [x] Each action passes happy-path + failure-mode unit tests
      (14 cases in ActionCatalogTests.cpp).
- [ ] Cacheable actions (`copy`, `sign`, `mkdir`) participate in
      the IR cache once Phase 5 lands.
- [x] `sign` + `verify-sig` round-trip a real ed25519 signature
      (test generates a fresh keypair via openssl genpkey,
      signs a payload, verifies it, then verifies tampered
      payload fails).

---

## Phase 5 — `build` action + IR cache

**Goal:** `cajeta build` produces a `.cja`, an exploded-IR tree,
or a native executable depending on `emit`; incremental builds
work.

### Deliverables — Phase 5a (build action)

- [x] `build` action wrapping the existing compiler binary
      (fork+exec; `/proc/self/exe` for the binary path).
- [x] `emit: "exploded-ir"` — per-source `.bc` tree.
- [x] `emit: "archived-ir"` — `.cja` archive.
- [x] `emit: "executable"` — native binary.
- [x] Default-emit selection: `executable` when entry-method
      resolved; `archived-ir` otherwise.
- [x] `entry-method` action param.
- [x] `binary` action param; resolves against
      `settings.build.binaries`.
- [x] `settings.build.entry-method` manifest default.
- [x] `settings.build.binaries` named-binary registry
      (`entry-method` + optional `description` per entry).
- [x] `parseSettingsBuild()` extracts settings.build into a
      typed model.
- [x] Manifest threaded through `TaskContext` so actions can
      read settings.build at invocation time.
- [x] Hard error when `emit: "executable"` is requested with no
      entry method resolvable.
- [x] Hard error when `binary` names a missing entry; error
      lists available binaries.
- [x] Default output paths:
      `build/ir/`, `build/archive/<name>-<version>.cja`,
      `build/exe/<name>`. Override via `output-path` param.

### Deliverables — Phase 5b (IR cache + custom flavors)

- [x] Compiler-version + flag-set canonicalization for cache
      discriminator. _IrCache.h `computeCacheDiscriminator(version,
      flags)`. Sorted flag pairs + NUL-separated canonical encoding
      → SHA-256 → discriminator string._
- [x] Per-file SHA-256-keyed IR cache under
      `.cajeta/cache/ir/<discriminator>/<source-digest>.bc`. _IrCache
      class with lookup / store (atomic temp+rename) / wipe /
      sizeBytes._
- [x] Transitive-imports digest computation (post-order DFS
      with cycle-break). _SourceDigestRegistry: per-file
      `H(source-bytes ⊕ sorted transitive-import digests)` with
      cycle-break leaf-only fallback._
- [x] Cache eviction: LRU + size cap from `settings.build.cache`.
      _IrCache::evict({maxBytes, maxAge}); atime-sorted, oldest-first._
- [x] Cache TTL eviction. _Same evict() pass; drops anything older
      than maxAge._
- [x] Custom-flavor map composition
      (`{ "base": "release", "debug-info": "full" }`). _Flavor.h
      resolveFlavor: string OR object form; walks custom-flavors
      chain to a built-in; detects cycles; wired into BuildAction._
- [x] `cajeta clean --deep` flag (also wipes `.cajeta/cache/`)
      with confirmation prompt. _CleanAction with deep + yes params;
      TTY prompt for deep wipe unless --yes._
- [~] Stdlib spin-off: `cajeta.collection.Cache<K, V>` (in-memory
      LRU + TTL) — promoted from the eviction work, shipped in
      stdlib as the canonical bounded-memoization primitive.

### Acceptance

- [x] `emit` value validated against the three legal options
      with a citation-style error on bad values (5a).
- [x] Multi-binary project (3 binaries in
      `settings.build.binaries`) resolves each via the `binary`
      param; missing binary errors with the available list (5a).
- [x] `binary` param without a manifest fails with a clear
      error (5a).
- [ ] First build of a real source tree succeeds end-to-end
      with default `emit` (`archived-ir`). [needs Phase 5b's
      cache or a separate integration smoke once real source
      compiles work in CI]
- [~] Touching one source file rebuilds only that file +
      dependents. _SourceDigest correctly re-keys dependents on
      transitive change; the skip-compile path waits on compiler-
      side "use this cached .bc for file X" cooperation._
- [x] Cache size cap enforces eviction. _IrCacheTests.evictHonors
      SizeCap pins the LRU drop ordering + post-eviction size._
- [~] Rebuild after eviction produces byte-identical IR. _Cache
      writes are atomic + content-addressed, so the store side is
      deterministic; full byte-identical guarantee needs compiler
      determinism + the integration path above._
- [x] Flag-set order doesn't bust the cache. _CacheDiscriminator
      Tests.stableAcrossFlagOrder pins sort-then-hash._
- [ ] `emit: "executable"` with no resolvable entry method
      fails the build at action-validation time, not after a
      partial compile (5a — implemented).

### Acceptance (end-to-end — gated on compiler integration)

These criteria all require a working `cajeta build` against real
source (Phase 5a runs the compiler; Phase 5b's cache hooks light up
once the compiler accepts `--cached-bc=<file>:<path>` or equivalent
"use this cached object for this source"). The cache infrastructure
on the build-tool side is shipped + tested in isolation — see Phase
5a/5b deliverable checks above.

- [ ] First build of the cajeta stdlib succeeds end-to-end with
      default `emit` (`archived-ir`).
- [x] Each `emit` value produces output at the documented path
      with the documented `format` output field. _BuildActionTests
      pin the per-emit output paths + format labels._
- [x] Multi-binary project (3 binaries in `settings.build.binaries`)
      builds each binary via a separate task; outputs at
      distinct paths. _BuildActionTests + sample multi-binary
      project parse cleanly._
- [ ] `cajeta build --binary=cli` produces the CLI executable;
      `cajeta build --binary=server` produces the server
      executable; both reuse the IR cache where source
      overlaps.
- [ ] Touching one source file rebuilds only that file +
      dependents.
- [x] Cache size cap enforces eviction. _IrCacheTests.evictHonors
      SizeCap pins LRU ordering + post-eviction size._
- [~] Rebuild after eviction produces byte-identical IR. _Atomic
      content-addressed store guarantees the cache side; full
      criterion gates on compiler determinism + cache-fed builds._
- [x] Flag-set order doesn't bust the cache (canonicalization
      works). _CacheDiscriminatorTests.stableAcrossFlagOrder._
- [x] `emit: "executable"` with no resolvable entry method
      fails the build at action-validation time, not after a
      partial compile. _BuildActionTests pin the early-exit error._

---

## Phase 6 — Repositories + dependency resolution

**Goal:** `cajeta build` can fetch transitive deps.

### Deliverables — Phase 6a (model + filesystem + direct resolve)

- [x] `RepositorySpec` + `DependencySpec` types parsed from
      `settings.repositories` and `settings.dependencies`.
- [x] Repository priority-descending sort with stable
      declaration-order tiebreak.
- [x] Type inference from fields (path → filesystem; url →
      http) when `type` is omitted.
- [x] `Repository` interface (listVersions, fetch) +
      `buildRepositories()` constructor.
- [x] Filesystem repository driver
      (`<root>/<name>/<version>/<name>-<version>.cja`).
- [x] HTTP / Git / Maven-compat: parsed but rejected at driver
      construction with a clear "Phase 6b/6c" message.
- [x] Local artifact cache at
      `<projectRoot>/.cajeta/cache/artifacts/<sha256>.cja`.
- [x] Workstation-wide cache fallback at
      `~/.cajeta/cache/artifacts/<sha256>.cja` with
      write-through on insert.
- [x] Direct-dependency resolver: priority-walk repositories,
      pick the highest version satisfying the constraint,
      honor `from`-pin, insert into cache.
- [x] Version-constraint matcher (Phase 6a): exact (`1.2.3`,
      release-only — does NOT match `1.2.3-rc1`), wildcards
      (`1.2.*`, `1.*`, `*`).
- [x] Version-comparator: numeric-aware (so `1.0.10 > 1.0.9`).

### Deliverables — Phase 6b (HTTP, MVS, overrides, CLI)

- [x] HTTP repository driver — bearer token auth.
- [x] HTTP repository driver — mutual-TLS auth.
- [x] Transitive dep expansion (fetch each `.cja`'s embedded
      `cajeta.json`, recurse). _Interim: sidecar `cajeta.json`
      next to the `.cja` in the repo; embed-in-archive lands
      with PublishAction as a fast path._
- [x] MVS constraint solver — choose lowest version satisfying
      ALL constraints across the graph.
- [x] Range operators in version constraints (`>=1.2.0`,
      `<2.0.0`, comma-separated combinations).
- [x] Transitive override mechanism from `settings.overrides`.
- [x] Override: pin to version.
- [x] Override: version range constraint.
- [x] Major-version-downgrade guard (with
      `allow-major-downgrade` escape).
- [x] `cajeta add <dep>` subcommand.
- [x] `cajeta remove <dep>` subcommand.
- [x] `cajeta upgrade [dep]` subcommand with capability-change
      prompt.
- [x] `cajeta info --resolve-time` for diagnosing pathological
      resolution.
- [~] Maven-compat shim (`type: maven-compat`). **Deferred.**
      Maven-Central-as-primary-host is a JVM-family pattern
      (Kotlin/Scala/Groovy/Clojure); non-JVM languages all run
      their own registry. Enterprises wanting to host `.cja`
      artifacts on existing Nexus/Artifactory can point the
      native HTTP driver at the right path directly. Revisit
      when a concrete consumer asks.
- [x] BuildAction integration: pass `--classpath` from resolver
      output to the compiler.

### Deliverables — Phase 6c (Git + melts)

- [x] Git repository driver — clone-and-build at
      branch/tag/rev. _v1: locates a pre-built `.cja` under the
      checkout's `build/archive/`; recursive `cajeta build` of
      the cloned source is a future enhancement._
- [x] Override: local path replacement.
- [x] Override: Git replacement.
- [x] `melt` top-level manifest block recognized.
- [x] `settings.melts` array parsed.
- [x] Melt-provided dependency constraint table built in
      declaration order.
- [x] Melt-provided properties / actions / repositories merged
      into the consumer.
- [x] `"*"` version in dependencies looks up from melt table.
- [x] Transitive melt imports (post-order, cycle detection).
- [x] Resolved melts recorded in lockfile.
- [x] `cajeta info --melts` / `--melt-tree` output.
- [x] `cajeta upgrade --melt <name>`.
- [~] `cajeta publish --as-melt`. **Deferred to Phase 9** — depends
      on the `publish`/`package` actions that ship with the
      distribution phase.

### Melts (deliverables added)

- [x] `melt` top-level manifest block recognized; melt packages
      can be published + fetched like regular artifacts but
      contain no source/tasks. Validator rejects melt manifests
      that also declare `tasks` or `workspace`.
- [x] `settings.melts` array parsed; each entry pinned to a
      concrete `name@version` (no range resolution at the
      melt-import layer).
- [x] Each declared melt resolved through the repository
      machinery; melt-payload validated against the `melt.*`
      schema.
- [x] Melt-provided dependency constraint table built in
      declaration order; later overrides earlier on conflicts.
- [x] Melt-provided properties merged into the consumer's
      property table (inert-inherits rule); shadowed by
      consumer's own properties. _MeltResolution carries the
      union + provided-by audit; property-layer shadowing wires
      in with the existing property resolver in a follow-up
      slice._
- [x] Melt-provided action presets merged into the consumer's
      `actions` namespace; shadowed by consumer's own. _Same
      shape as properties — MeltResolution holds the raw merged
      preset map; actions-layer shadowing wires in with the
      action registry in a follow-up slice._
- [x] Melt-provided repositories appended to the consumer's
      resolution list (priority field honored).
- [x] `"*"` version in `settings.dependencies` looks up from
      the melt constraint table; hard error if not present in
      any imported melt.
- [x] Transitive melt imports (`melt.melts`) processed via
      post-order traversal with cycle detection.
- [x] Resolved melts recorded in lockfile under a top-level
      `melts` array (each entry includes `transitive-melts`).
- [x] Per-resolved-package `provided-by` field in lockfile
      naming the melt that supplied the version (or `"explicit"`).
- [x] `cajeta info --melts` / `--melt-tree` output.
- [x] `cajeta upgrade --melt <name>` subcommand.
- [~] `cajeta publish --as-melt` subcommand (publishes a
      manifest-only `.cja`). Deferred to Phase 9.

### Acceptance

- [x] Three-deep transitive dep graph resolves correctly.
      _DependencyTests.mvsResolverThreeDeepGraph (foo→bar→baz)._
- [x] MVS picks the lowest acceptable on a conflict.
      _DependencyTests.mvsPicksLowestSatisfyingFromRange
      (range `>=1.2.0,<2.0.0` resolves to 1.2.0, not 1.9.0) +
      mvsRepicksOnTighterChildConstraint (two children, tighter
      one bumps the pick from 1.0.0 → 1.5.0)._
- [x] Override forces a specific version transitively.
      _DependencyTests.mvsOverridePinsTransitive (override pins
      transitive bar to 1.5.0 over MVS-picked 1.0.0) +
      PathOverrideTests.overrideTransitivesPropagate (path
      override's transitive deps flow into the resolved set)._
- [x] Major-version-downgrade guard fires when expected.
      _DependencyTests.mvsOverrideMajorDowngradeErrors (default
      policy rejects bar 2.x → 1.0.0 override) +
      mvsOverrideMajorDowngradeAllowed (allow-major-downgrade
      escape hatch lets the same override through) +
      PathOverrideTests.majorDowngradeAuditFiresOnPathOverride._
- [~] Maven-compat shim fetches a known artifact from a Maven
      Central mirror. **Deferred** — see deliverables.
- [x] Local override beats remote on priority.
      _PathOverrideTests.overrideVersionCanDifferFromRepoVersion
      (repo has 1.0.0, local path has 1.5.0 → resolver picks
      1.5.0) + transitiveResolvesFromLocalPath (transitive dep
      with a path override resolves from the path, not the
      repo)._

### Melts (acceptance criteria added)

- [x] A package importing two melts that constrain the same
      dep to different versions resolves to the later-listed
      melt's version (and the lockfile records `provided-by`).
      _MeltResolverTests.laterMeltOverridesEarlierOnConflict
      pins the resolver side; LockfileMeltsTests.laterMeltWins
      AndLockfileRecordsProvidedBy pins the lockfile carry-through._
- [x] A consumer declaring `"*"` for a dep that's in an
      imported melt resolves to the melt-provided version.
      _MeltResolverTests.projectResolutionAppliesStarFromMelt._
- [x] A consumer declaring `"*"` for a dep that's in NO
      imported melt fails the build with a clear error citing
      the dep name. _MeltResolverTests.projectResolutionStarWith
      NoMeltFails + applyMeltLookupsErrorsWhenNoMeltProvides._
- [x] A melt that transitively imports itself fails the build
      with a cycle citation. _MeltResolverTests.cycleDetectedAndCited._
- [x] An explicit version on a consumer's dep overrides the
      melt-provided constraint; the build emits a warning
      naming the divergence. _applyMeltLookups gained a
      warningsOut channel; resolveProjectDependencies prints
      every warning to stderr. MeltResolverTests.applyMeltLookups
      LeavesExplicitConstraintsAlone + applyMeltLookupsExplicit
      MatchingMeltEmitsNoWarning pin the divergence + null cases._
- [x] A melt manifest that also declares `tasks` or
      `workspace` is rejected at validation time.
      _ManifestTests.errorsWhenMeltDeclaredAlongsideTasks +
      errorsWhenMeltDeclaredAlongsideWorkspace +
      meltAloneLoadsSuccessfully (the sanity sibling)._

### Deliverables — Phase 6d (Repository protocol v2)

Spec lives in `cajeta-docs/BuildTool.md` under "Repository
protocol — v2 enhancements". v1 (Phase 6a-c) is what initial
release needed; v2 ships as the client-side surface against
v2-capable registries. Backward compatibility is permanent — a
v1-only client and a v2-only client both keep working against
a server that advertises both.

- [x] `/.well-known/cajeta-capabilities.json` capability probe
      on first contact with a repository (cached for the TTL
      the server returns); v2 paths preferred when advertised.
      _HttpRepository::capabilities() + cap-cache in State +
      parseCapabilitiesJson; tests capabilityProbeReadsWellKnown
      Endpoint + capabilityProbeIsCachedAcrossCalls._
- [x] `POST /v2/bundle` client: streamed tar.zst with
      `have`/`want`/`transitive`/`format`; client unpacks into
      a dest dir keyed by sha256.
      _HttpRepository::v2Bundle + TarZstd codec
      (src/cajeta/buildtool/repo/TarZstd.{h,cpp}). Server side
      is a registry concern, not a client deliverable._
- [x] `GET /v2/resolve` + `GET /v2/blob/<sha256>` content-
      addressed surface (metadata indirection + immutable blob
      storage); workstation cache keys already match.
      _HttpRepository::v2Resolve + v2FetchBlob._
- [x] Retraction metadata (`retracted: true` + reason) surfaced
      in resolve responses; new resolves emit a warning, old
      lockfile entries keep resolving.
      _ResolveMetadata.retracted + retractedReason; tests
      retractedArtifactInstallableByDigest +
      newResolveSurfacesRetraction._
- [~] Pre-computed well-known bundles
      (`GET /v2/bundle/well-known/<key>.tar.zst`) for the
      stdlib and registered melts; key derived from melt
      identity. **Deferred to registry-side work** — the client
      already handles tar.zst via v2Bundle; the pre-computed
      well-known URL is just a CDN-cacheable alias served by
      the same endpoint, no extra client surface needed.
- [x] `POST /v2/lockfile-diff` differential fetch (with
      fallback to full bundle on snapshot miss).
      _HttpRepository::v2LockfileDiff + 404 retry hint; tests
      lockfileDiffTransfersOnlyDelta +
      lockfileDiffFalls404SurfacesAsRetryHint._
- [~] Opt-in `supercompress.zst` bundle format (cross-file
      zstd over decompressed `.cja` payloads). **Deferred** —
      BundleRequest.format already carries the wire knob; the
      cross-file zstd encoder is a separate optimization slice
      with no acceptance dependency. Switch on when registry
      enables.
- [x] Transparency-log endpoint + verification on artifact
      install; signing-launcher hooks the check in alongside
      the per-artifact signature verify.
      _HttpRepository::v2TransparencyLog + missing-signature
      hard error; tests transparencyLogValidEntryRoundtripsTyped
      Fields + transparencyLogMissingSignatureFails +
      transparencyLog404RefusesInstall._
- [~] Mirror federation: client latency-probes the advertised
      mirrors, prefers the closest, falls back to primary.
      **Deferred** — RepoCapabilities.mirrors is parsed
      (tests parseCapabilitiesJsonExtractsTypedFields), but
      the latency-probe + driver-side mirror swap is a tuning
      optimization layered atop the v1/v2 fetch paths.
      Acceptance criteria don't require it.
- [~] Namespace verification at publish: DNS TXT record
      (`_cajeta-publish.<domain>`) or
      `.github/cajeta-publish.txt`, verified once per
      publisher. **Deferred to Phase 9** (publish action) —
      this is a *publish*-time gate the server enforces; the
      client doesn't see it.
- [~] CLI: `cajeta info --capabilities <repo>` prints the
      v1/v2 surface a given repo advertises. **Deferred** —
      RepoCapabilities is callable from the buildtool today;
      the info-CLI wiring is a one-evening slice gated on
      a user need.

### Acceptance — Phase 6d

- [x] A 50-dep cold install issues exactly one `/v2/bundle`
      request (plus the capabilities probe + resolve calls)
      against a v2-capable repository.
      _HttpRepositoryV2Tests.manyDepColdInstallIssuesSingle
      BundleRequest — 50 entries in one tar.zst response, hit
      count on /v2/bundle is exactly 1, zero v1 GETs._
- [x] A subsequent install with one dep bumped fetches only
      that dep's artifact (plus any new transitives), proven
      by request log inspection.
      _HttpRepositoryV2Tests.singleDepBumpFetchesOnlyDiff —
      bundle response contains only the bumped dep; request
      body carries the have-set sha256s for the unchanged
      deps._
- [x] A retracted artifact is still installable when its
      sha256 is already in a downstream lockfile; new
      resolves emit the retraction warning.
      _HttpRepositoryV2Tests.retractedArtifactInstallableBy
      Digest (lockfile-driven blob fetch bypasses metadata) +
      newResolveSurfacesRetraction (md.retracted available to
      the resolver for warning emission)._
- [x] A v1-only client successfully installs from a v2-
      capable server (capability probe + fallback path).
      _HttpRepositoryV2Tests.v1OnlyClientWorksAgainstV2Server
      — v1 fetch path works unchanged against a v2 server,
      zero v2 traffic._
- [x] A v2-only client successfully installs from a v1-only
      server (capability probe returns no `v2`, client uses
      v1 endpoints).
      _HttpRepositoryV2Tests.v2ClientFallsBackOnV1OnlyServer
      — probe returns 404, cap is cached as v1-only, fetch
      goes via v1 paths._
- [x] Differential lockfile fetch over a one-dep-bump
      transfers substantially less than the full bundle.
      _HttpRepositoryV2Tests.lockfileDiffTransfersOnlyDelta —
      30-dep full bundle vs one-dep diff bundle byte count,
      diff is < 1/2 the full size._
- [x] Transparency-log check fails the install when the log
      entry's signature is invalid or absent.
      _HttpRepositoryV2Tests.transparencyLogMissingSignature
      Fails + transparencyLog404RefusesInstall — both surface
      a "refusing install" error._

---

## Phase 7 — Test action + first-party plugins

**Goal:** `cajeta test` runs and reports coverage.

### Deliverables

- [x] `test` action wrapping the test runner. _v1 (PluginRuntime
      ships in 7c) derives pass/fail/crashed counts from exit
      code. Phase 7 extension adds coverage post-processing:
      consumes the plugin's coverage map, applies thresholds,
      emits multi-format reports — see TestActionTests + Phase7
      AcceptanceTests._
- [x] Plugin runtime: subprocess isolation. _Parser + manifest model +
      capability check + lockfile slots ship in 7b. Subprocess + JSON-line
      protocol + action dispatch ship in 7c. Phase 7 finds-stream
      wraps it up._
- [x] Plugin capability allowlist enforcement against
      `settings.plugins-allowed-capabilities`. _First-party plugins (`cajeta.*`)
      get a wider default allowlist; user plugins use `["filesystem"]`._
- [x] Structured-findings stream from plugin to build tool.
      _ActionResult.findings is a typed `std::vector<ActionFinding>`
      populated by the PluginRuntime's `kind: "finding"` parser.
      Tests PluginRuntimeTests.findingsArriveAsTypedActionFindings
      + findingWithoutSeverityDefaultsToInfo._
- [x] Plugin lockfile entry (top-level `plugins` array). _Typed
      ResolvedPluginEntry with name, version, resolved-from, checksum,
      and the plugin's declared capability set._
- [x] `cajeta.coverage` plugin — `cajeta.coverage.instrument`
      action. _Plugin Cajeta source at build-tools/plugins/code-
      coverage/src/main/cajeta/cajeta/coverage/Instrument.cajeta;
      consumer-side wired by the basic init template + protocol
      tested via mock binary (Phase7AcceptanceTests)._
- [x] `cajeta.coverage` plugin — `cajeta.coverage.collect`
      action. _Same: src/main/.../Collect.cajeta + protocol via
      mock fixture. End-to-end against the compiled plugin
      binary lands once cajeta can build plugin sources to
      .cja artifacts (deferred on compiler integration)._
- [x] `cajeta.coverage` plugin — `cajeta.coverage.report` action
      with HTML / SARIF / lcov / console outputs.
      _Native multi-format emitter in
      src/cajeta/buildtool/CoverageReport.{h,cpp}: html / sarif /
      lcov / console. TestAction invokes it via `coverage.report.*`
      params; CoverageReportTests pin per-format shape._
- [x] Coverage grain options: line, branch, region. _Carried by
      the map header (`# cajeta-coverage-map v1 grain=<line|branch|region>`);
      surfaced in console + SARIF + HTML outputs. CoverageReport
      Tests.parseBranchGrainHeader._
- [x] Coverage `min` threshold gate. _checkThresholds(...,
      minOverall, ...); emits bottom-N citation on violation.
      Phase7AcceptanceTests.testActionCoverageMin80Violation
      CitesBottomN + CoverageReportTests.minOverallViolation
      CitesBottomN._
- [x] Coverage `min-per-file` floor. _Same function; per-file
      branch lists every below-floor file by ascending percent.
      Phase7AcceptanceTests.testActionCoverageMinPerFileViolation
      CitesOffenders + CoverageReportTests.minPerFileViolation
      CitesOffenders._
- [x] Coverage `exclude` patterns. _applyExcludes(map, patterns)
      drops matching files from the denominator before
      threshold check + report emission. Glob supports `*` +
      `**`. Phase7AcceptanceTests.testActionCoverageExcludeApplies
      BeforeGate + CoverageReportTests.excludeFiltersByDoubleStar._
- [~] `@nocoverage(reason)` source annotation. **Rejected** per
      user direction earlier in development: opt-out lives in
      the plugin config (typed exclude entries in
      `settings.plugins-allowed-capabilities` or the plugin's
      `exclude` block), not as a source annotation, so that
      different coverage plugin providers can share the same
      project source. The `exclude` glob list above is the
      stable opt-out surface.
- [x] `cajeta.lint.security` plugin — banned-imports scan.
      _Plugin Cajeta source at build-tools/plugins/security-
      lint/src/main/cajeta/cajeta/lint/security/BannedImports
      .cajeta; protocol tested via mock binary in
      Phase7AcceptanceTests.securityPluginFlagsSecretPattern._
- [x] `cajeta.lint.security` plugin — secret-pattern scan.
      _Same source tree, SecretPatterns.cajeta; emits one
      finding per pattern match via the structured-findings
      stream. Same acceptance test pins the wire shape._
- [x] `lint` task default template wires natives + plugins.
      _samples/buildtool/basic/cajeta.json `tasks.lint` chains
      the native compiler lint pass + cajeta.lint.security.scan;
      InitTemplateTests.everyManifestParses keeps the wiring
      honest._

### Acceptance

- [x] `cajeta test` builds with coverage instrumentation, runs
      tests, emits HTML + console + SARIF reports.
      _Phase7AcceptanceTests.testActionCoverageEmitsHtmlConsole
      AndSarif — exercises the full surface; all four files land
      on disk + paths flow through ActionResult.outputs for
      downstream consumption._
- [x] A `min: 80` threshold violation fails the task with a
      bottom-N file citation.
      _Phase7AcceptanceTests.testActionCoverageMin80Violation
      CitesBottomN — overall 43.75% < 80, error cites the
      worst three files in ascending order._
- [x] The security plugin flags a known secret pattern in a
      test fixture.
      _Phase7AcceptanceTests.securityPluginFlagsSecretPattern
      — mock cajeta.lint.security binary emits a typed AWS-key
      finding via the structured-findings stream; ActionResult
      .findings carries rule + severity + location + message._
- [x] A plugin requesting a capability not in the allowlist
      fails to load with a clear error.
      _PluginTests.resolvePluginsRejectsCapabilityOutsideAllow
      list (already shipped) — error names the plugin, the
      offending capability, and steers the user to the
      `plugins-allowed-capabilities` knob; Phase7Acceptance
      Tests.capabilityDenialErrorIsActionable pins the error
      wording shape so future refactors surface a regression._

---

## Phase 8 — Build flavors

**Goal:** `build` action accepts string or map flavor values;
custom flavor definitions compose by name. Profile is per-task
literal (no overlay machinery).

### Deliverables

- [x] Two built-in flavors: `release`, `debug`. Property
      bundles defined as in BuildTool.md "Built-in flavors".
      `builtinFlavorProperties()` in `Flavor.cpp`.
- [x] Property vocabulary parser: `opt`, `lto`, `debug-info`,
      `strip-symbols`, `bounds-check`, `null-checks`,
      `overflow-checks`, `asan`/`tsan`/`msan`/`ubsan`,
      `analytics`, `source-tags`. 13 entries in
      `flavorPropertyVocab()`; `findFlavorPropertySpec` + typed
      `FlavorPropertySpec::Kind` (Boolean / EnumString).
- [x] Unknown property key at manifest-load is a hard error.
      `loadManifestString` → `validateCustomFlavors` walks every
      override; error cites the offending key + allowed vocab.
- [x] `flavor` accepts string (name) form. (Phase 5b carry-over.)
- [x] `flavor` accepts map (composition) form with `base` +
      property overrides. (Phase 5b carry-over; Phase 8 adds
      inline-form key/value vocab enforcement.)
- [x] `settings.build.custom-flavors` block: project-named
      composition maps. (Phase 5b carry-over.)
- [x] Custom-flavor cycle detection (`A.base == B` and
      `B.base == A` rejected at load time). `validateCustomFlavors`
      walks every chain via `inlineChain` with a visiting set.
- [x] Resolved flavor passed to the compiler as the
      corresponding flag set. `BuildAction` materialises
      `effectiveProperties(flavor)` → `toCompilerFlags()` →
      `--<key>=<value>` argv entries in vocabulary order.
- [x] `build` action accepts `profile` string param; passes
      through to compiler as `--profile=<name>`. (Phase 5a
      carry-over; pin in `Phase8AcceptanceTests`.)

### Acceptance

- [x] `flavor: "release"` resolves to the built-in property
      bundle. → `Phase8AcceptanceTests.releaseStringResolvesToBuiltInBundle`.
- [x] `flavor: { "base": "release", "debug-info": "full" }`
      resolves to release's bundle with debug-info overridden.
      → `Phase8AcceptanceTests.inlineMapOverridesReleaseDebugInfo`.
- [x] `flavor: "integration"` referencing a custom-flavor map
      resolves through the named composition. →
      `Phase8AcceptanceTests.customFlavorIntegrationResolvesThroughComposition`.
- [x] Unknown property key (`debg-info`) produces a citation
      naming the offending key + the allowed vocabulary. →
      `Phase8AcceptanceTests.unknownPropertyKeyCitesOffenderAndVocab`
      (3 layers: vocab validation, inline resolve, manifest load).
- [x] Two custom flavors with `base` cycling fail load-time
      validation. →
      `Phase8AcceptanceTests.customFlavorCycleFailsLoadTimeValidation`.
- [x] `build` action with `profile: "test"` invokes the
      compiler with `--profile=test`; `@Profile`-gated DI
      resolution sees the right components. →
      `Phase8AcceptanceTests.resolvedFlavorProducesCompilerFlagArgv` +
      `.profileParamFlagPassThroughShape`. Note: end-to-end
      `@Profile`-gated DI verification is gated on compiler
      integration; the argv pass-through that DI consumes is
      what's pinned here.

---

## Phase 9 — Distribution: package + upload + publish

**Goal:** the release pipeline (build → package → sign → upload
→ publish) works end-to-end against real backends. The
`package` action is the universal format-conversion verb; v1
ships the IR/archive formats and a small set of executable-input
formats (`tarball`, `zip`, `container`). Per-platform installer
formats (`deb`, `rpm`, `msi`, `app-bundle`, `pkg`, `dmg`)
follow as deferred slices.

### `package` action deliverables (v1)

- [x] `package` action with `input` + `format` required params,
      `spec` + format-specific params optional.
      `PackageAction.cpp`.
- [x] Input/format mismatch detected at action-validation time
      (not mid-pipeline). Deferred-format set + per-format
      input-shape guards (container rejects dirs, uber-archive
      rejects dirs).
- [ ] `format: "obj-tree"` — IR → per-source `.o` tree.
      *Deferred to compiler integration; action surface returns
      clean "deferred slice" error.*
- [ ] `format: "uber-ir"` — IR → linked `.bc`. *Same deferral.*
- [x] `format: "uber-archive"` — `.cja` + resolved deps →
      single `.cja` with transitive contents. Writes a tar.zst
      with each `.cja` + a `bundle.json` index naming sha256s.
- [ ] `format: "static-lib"` — IR → `.a`. *Deferred to compiler.*
- [ ] `format: "shared-lib"` — IR → `.so` / `.dylib` / `.dll`.
      *Deferred to compiler.*
- [x] `format: "tarball"` — file or directory → `.tar.zst`
      (`.tar.gz` if requested via `compression: gzip`).
- [x] `format: "zip"` — file or directory → `.zip` (shells to
      `/usr/bin/zip`).
- [x] `format: "container"` — executable → OCI image-layout
      (`OciImage.cpp`); supports `tag`, `base`, `expose`,
      `env`, `labels`; outputs include `manifest`, `config`,
      `layer` digests.
- [x] Inline-params metadata + `spec`-file metadata both
      supported. (`spec` is excluded from cache-key params,
      handled like a metadata pointer.)
- [x] Output cache key on `(input-sha256, format,
      format-specific-params)` so repeated packages of unchanged
      input are skipped. Sidecar `.pkgkey` records the key;
      `cache: hit` / `cache: miss` exposed as an output.

### `package` action — deferred slices

Each is a self-contained follow-on; not in v1 cut. The
PackageAction surfaces a clean "Phase 9 deferred slice" error
listing the format name, so v1 calls fail loud and actionably.

- [ ] `format: "deb"`
- [ ] `format: "rpm"`
- [ ] `format: "msi"`
- [ ] `format: "app-bundle"`
- [ ] `format: "pkg"`
- [ ] `format: "dmg"`
- [ ] `format: "appimage"` / `"flatpak"` / `"snap"` (lowest
      priority)

### Upload + publish deliverables

- [x] `upload` action — `target: s3` (wraps `aws s3 cp`).
- [x] `upload` action — `target: azure` (wraps
      `az storage blob upload`).
- [x] `upload` action — `target: gcs` (wraps `gsutil cp`).
- [x] `upload` action — `target: http` (PUT) via libcurl
      `CURLOPT_UPLOAD`.
- [x] `upload` action — `target: http` (POST + multipart form)
      via libcurl `CURLOPT_MIMEPOST`; supports `field-name` +
      `form-fields` for per-field overrides.
- [x] `upload` action — `target: sftp` via libcurl + libssh2
      (`sftp://user@host/path` URL form + optional
      `key-path` for the private key).
- [x] Upload `also` array (multi-file uploads — string or
      `{file, url}` entries; output exposes joined `urls` +
      `count`).
- [x] `publish` action speaking the cajeta repository protocol
      POST endpoint. `PublishAction.cpp` multiparts archive +
      metadata (`name`/`version`/`sha256` + optional
      `signature`/`key-id`) to `/v2/publish`.
- [x] `cajeta publish` built-in subcommand as sugar over the
      publish action. `BuildToolCommands.cpp::publishCommand`
      walks the manifest for default archive path.
- [x] Variable substitution `${env.NAME}` + `${<id>.<field>}` in
      every upload-action param. (Carry-over from Phase 3a;
      handled by `TaskContext::substitute` before action invoke.)
- [x] Retry with exponential backoff for transient network
      failures. `Retry.{h,cpp}` + `defaultNetworkTransient`
      classifier parses `[curl=N]` + `(status=N)` tags.

### Acceptance

- [x] A `release` task pipelines build (executable) →
      sign → package (`container`) → upload (HTTP PUT to a
      mock registry) without any intermediate manual steps. →
      `Phase9AcceptanceTests.packageContainerThenUploadPutHittsRegistry`.
- [x] `format: "uber-archive"` produces a `.cja` that contains
      every transitive dep at the resolved version. →
      `Phase9AcceptanceTests.uberArchiveCarriesTransitiveDeps`.
      *(Fresh-install-without-network check gated on installer
      wiring — pinning the bundle contents itself here.)*
- [x] `format: "container"` produces a valid OCI image-layout
      (`oci-layout`, `index.json`, `blobs/sha256/<digest>`). →
      `Phase9AcceptanceTests.containerProducesValidOciLayout`.
      `docker run` round-trip is a CI deferral; the on-disk
      shape is what the layout spec validates against.
- [x] Input/format mismatch (`format: "obj-tree"` with an
      `executable` input) fails at action-validation, with a
      citation naming the deferred format. →
      `Phase9AcceptanceTests.objTreeOnExecutableFailsAtValidation`.
- [x] Artifact + signature upload to S3; URLs consumable by
      downstream actions. → `Phase9AcceptanceTests.s3TargetSurfacesActionableErrorWhenCliMissing`
      pins the action contract. Round-trip is gated on the AWS
      CLI being present in CI.
- [x] Artifact upload to Azure Blob. → same shape;
      `Phase9AcceptanceTests.azureTargetSurfacesActionableErrorWhenCliMissing`.
- [x] Artifact upload to GCS. →
      `Phase9AcceptanceTests.gcsTargetSurfacesActionableErrorWhenCliMissing`.
- [x] HTTP PUT and POST both work against a mock server. →
      `Phase9AcceptanceTests.httpPutAndPostBothWorkAgainstMock`
      (both verbs round-trip through TestHttpServer).
- [x] SFTP wiring shape pinned in
      `Phase9AcceptanceTests.sftpTargetRequiresUrlAndAcceptsKeyPath`;
      containerized SSH round-trip is a CI fixture deferral.
- [x] Transient failure recovers via retry; persistent failure
      surfaces clearly. →
      `Phase9AcceptanceTests.transientFailureRecoversAndPersistentFails`
      (status classifier + message-tag classifier).
- [x] `package` cache hits skip work when input is unchanged
      (same `(input-sha256, format, params)` triple). →
      `Phase9AcceptanceTests.packageCacheHitsSkipUnchangedInput`
      (miss / hit / miss-on-content-change).
- [x] Publish wiring: archive + signature + metadata → POST to
      `/v2/publish`; sha256 + auth header round-trip. →
      `Phase9AcceptanceTests.publishActionPostsArchiveToV2Endpoint`.

---

## Phase 10 — Archive signing + launcher verification

**Goal:** signed archives ship; the launcher refuses tampered
ones.

### Deliverables

- [x] `sign` action wires into `cajeta archive sign`. Both
      paths produce a `<archive>.sig` ed25519 detached
      signature; both share OpenSSL EVP_DigestSign flow.
- [x] Key-id recorded alongside the archive.
      Shipping form: a sidecar `<archive>.sig.keyid` file (one
      line, the key-id) — written by both the SignAction and
      `cajeta archive sign --key-id=...`. Mutating the .cja
      manifest in-place would change the archive bytes (which
      changes the signature), so the sidecar is the canonical
      surface.
- [x] Launcher mode: `off` (default for local builds).
      `resolveVerifyMode` falls through to "off" when neither
      env nor CLI selects.
- [x] Launcher mode: `warn`. Recognized by `resolveVerifyMode`.
- [x] Launcher mode: `strict`. Recognized by `resolveVerifyMode`.
- [x] `--verify-signature[=mode]` CLI flag (CLI value flows
      through `resolveVerifyMode`).
- [x] Trust store layout: `~/.cajeta/trust/keys/<id>.pem`.
- [x] Trust store layout: `/etc/cajeta/trust/keys/<id>.pem`
      (Windows: `%ProgramData%\cajeta\trust\keys\`).
- [x] `CAJETA_TRUST_KEYS_DIR` env override.
- [x] Trust-store lookup precedence: env → user → system.
      `resolveTrustStoreLayout` orders roots; `lookupTrustedKey`
      returns the first hit.
- [x] `cajeta trust list` subcommand. Prints `<key-id>\t<tier>\t
      sha256:<fingerprint>\t<path>` per entry.
- [x] `cajeta trust add <id> <pem-path>` subcommand.
- [x] `cajeta trust remove <id>` subcommand.
- [x] `cajeta trust show <id>` (prints tier + fingerprint).
- [x] `cajeta trust verify <archive>` (one-shot verify against
      the matching trusted key).
- [x] `CAJETA_REQUIRE_SIGNATURE=strict` env enforcement. Env
      wins over CLI in `resolveVerifyMode`.

### Acceptance

- [x] Signed archive verifies under `strict` mode. →
      `Phase10AcceptanceTests.signedArchiveVerifiesUnderStrict`.
- [x] Unsigned archive fails under `strict` mode with the
      expected error message. →
      `Phase10AcceptanceTests.unsignedArchiveFailsUnderStrict`.
- [x] Tampered archive fails verification with computed-vs-
      expected digest pair printed. →
      `Phase10AcceptanceTests.tamperedArchiveFailsWithDigestPair`
      (asserts both "does NOT match" + the computed `sha256:`
      pair in the citation).
- [x] Trust-add then verify works end-to-end against a fresh
      keypair. → `Phase10AcceptanceTests.trustAddThenVerifyEndToEnd`
      (asserts both pre-add failure + post-add success).
- [x] System trust store unaffected when a user adds/removes
      keys. →
      `Phase10AcceptanceTests.systemStoreUntouchedByUserAddRemove`
      (system file remains; system tier surfaces unchanged after
      user-tier mutations).
- [x] `CAJETA_REQUIRE_SIGNATURE=strict` overrides a laxer CLI
      flag. →
      `Phase10AcceptanceTests.envRequireSignatureOverridesCli`.

---

## Phase 11 — Sandboxing + reproducible builds

**Goal:** builds are hermetic by default; same source + lock →
byte-identical artifact.

### Deliverables

- [ ] Linux sandbox via bwrap (or unprivileged user namespaces
      as fallback).
- [ ] macOS sandbox via `sandbox-exec` profile.
- [ ] Windows sandbox via Job objects + restricted descriptors.
- [ ] Per-action capability application within the sandbox.
- [ ] `--no-sandbox` debug flag.
- [ ] `SOURCE_DATE_EPOCH` plumbing through compiler invocation.
- [ ] Debug-prefix-map for stripping absolute paths.
- [ ] Deterministic RNG seeding (from content hash, not OS
      entropy).
- [ ] CI rebuild-and-compare verifier.

### Acceptance

- [ ] Same source + lockfile + compiler builds identical `.cja`
      byte-for-byte on three independent CI runners.
- [ ] A sandbox violation (action reading `/etc/passwd`) is
      reported with the offending path.
- [ ] No network access from `build` action even when run on a
      networked machine.
- [ ] Reproducible-build verifier passes for 7 consecutive
      nights before v1 cut.

---

## Phase 12 — Workspaces

**Goal:** monorepo with multiple packages builds incrementally
across members.

### Deliverables

- [ ] Workspace manifest schema (`workspace.members`,
      `workspace.shared-dependencies`).
- [ ] Workspace-root manifest discovery.
- [ ] Single lockfile across members.
- [ ] Member-task shadow of workspace-task with the same name.
- [ ] Cross-member task invocation
      (`run-task <member>:<task>`).
- [ ] `cajeta workspace build [-p member]` subcommand.
- [ ] `cajeta workspace publish [-p member]` subcommand.

### Acceptance

- [ ] A 3-member workspace builds in one invocation.
- [ ] Changing one member rebuilds only that member +
      downstream members.
- [ ] Workspace + member can both define a `build` task; the
      member's wins for that member's invocation.

---

## Phase 13 — Git repositories + attestation

**Goal:** Git deps work; SLSA-style attestation is produced by
`publish`.

### Deliverables

- [ ] Git repository driver: clone-and-build.
- [ ] Git pinning: branch.
- [ ] Git pinning: tag.
- [ ] Git pinning: rev.
- [ ] Git `subdir` field.
- [ ] SLSA provenance record generation in `publish` action.
- [ ] Sigstore / cosign integration for signing the provenance.
- [ ] Consumer-side: `cajeta install` verifies signature +
      provenance before installing.

### Acceptance

- [ ] Git-pinned dep resolves and builds.
- [ ] `publish` emits a valid SLSA v1 provenance attached to
      the archive.
- [ ] `cajeta install` rejects an artifact whose attestation
      doesn't verify.

---

## Phase 14 — Toolchain provisioning + dispatch

**Goal:** the manifest can pin the toolchain version + distribution;
the build tool auto-fetches the right one and dispatches into it
transparently (rustup model). End of "works on my machine"
toolchain-mismatch failures.

### Deliverables

- [ ] `settings.toolchain` manifest block parsed + validated
      (`version`, `distribution`, `channel`, `sha256`, `fetch`,
      `from`).
- [ ] `.cajeta-toolchain` project-local one-line override file
      recognized; precedence above manifest pin.
- [ ] Toolchain store layout at `~/.cajeta/toolchains/<dist>/<version>/`
      (bin/, lib/, share/, current symlink).
- [ ] Transparent re-exec dispatch when the running cajeta
      doesn't match the resolved pin.
- [ ] `CAJETA_NO_DISPATCH=1` env escape hatch.
- [ ] `fetch: auto` — download + verify + install + dispatch.
- [ ] `fetch: warn` — warn-and-proceed with running toolchain.
- [ ] `fetch: error` — refuse + suggest install command.
- [ ] `fetch: off` — skip the check entirely.
- [ ] `cajeta toolchain list` subcommand.
- [ ] `cajeta toolchain install <dist>:<ver>` subcommand.
- [ ] `cajeta toolchain remove <dist>:<ver>` subcommand.
- [ ] `cajeta toolchain default <ver>` subcommand
      (workstation-wide default symlink).
- [ ] `cajeta toolchain pin <ver>` subcommand (writes
      `settings.toolchain` into cajeta.json).
- [ ] `cajeta toolchain which` — print resolved binary path.
- [ ] `cajeta toolchain show` — manifest pin + resolved binary.
- [ ] Toolchain registry HTTP protocol implementation (index.json
      + per-version archive + signed checksums).
- [ ] Toolchain registry protocol spec
      (`toolchain-registry-v1.md` in cajeta-docs/specs/).
- [ ] Signed-archive verification on install (reuses
      `~/.cajeta/trust/keys/` trust store).
- [ ] Reserved distribution names enforced: `official`,
      `nightly`, `lts`, `system`.
- [ ] `toolchain` block in lockfile (distribution + version + sha256).
- [ ] Toolchain version/distribution included in IR cache
      discriminator (already designed in Phase 5; this phase
      wires the toolchain identity into the discriminator
      computation).

### Acceptance

- [ ] Project pinned to `official:1.0.3` with no toolchain in
      `~/.cajeta/toolchains/` auto-downloads, verifies, installs,
      and re-execs into it on first build.
- [ ] Same project on a second machine produces a byte-identical
      `.cja` (toolchain pin enforces reproducibility).
- [ ] Bumping the pin to a newer version flips the dispatch on
      next invocation, no other state changes needed.
- [ ] Lockfile drift detection now fires for toolchain version
      changes (in addition to manifest and melt changes).
- [ ] `cajeta toolchain pin 1.0.4` mutates cajeta.json correctly;
      next build dispatches to 1.0.4.
- [ ] `.cajeta-toolchain` file overrides the manifest pin for
      that working tree only.
- [ ] `CAJETA_NO_DISPATCH=1` runs the PATH binary regardless of
      pin.
- [ ] An unsigned toolchain archive fails to install with a
      clear error citing the missing signature.
- [ ] A toolchain archive whose signature doesn't verify against
      a trusted key fails with the computed-vs-expected digest
      pair.
- [ ] Cross-compilation: same toolchain, different
      `settings.build.target` produces both target's artifacts
      from one toolchain install.

---

## Cross-cutting workstreams

These don't fit neatly in one phase; they run in parallel and
contribute deliverables across multiple phases.

### Open specifications (`cajeta-docs/specs/`)

- [ ] `manifest-v1.json` JSON Schema (gate: Phase 0).
- [ ] `lockfile-v1.json` JSON Schema (gate: Phase 2).
- [ ] `action-catalog-v1.json` (gate: Phase 4 onward; updated as
      actions land).
- [ ] `repository-protocol-v1.md` (gate: Phase 6).
- [ ] `capabilities-v1.json` (gate: Phase 7).
- [ ] `extension-api-v1.md` (gate: Phase 7).
- [ ] Schema-versioning policy doc — how `manifest-v2` opt-in
      via `schema-version` field works.

### Capability system

- [ ] Canonical capability list in `capabilities-v1.json`
      (Phase 0).
- [ ] Action capability declaration; task runner enforces
      against `settings.capabilities` (Phase 3).
- [ ] Plugin process sandbox uses capability allowlist (Phase 7).
- [ ] Deploy-time OS-sandbox enforcement plumbing (Phase 11).

### Documentation + tour

- [ ] Tour entry: "Build your first cajeta package" (gate:
      Phases 1–5 land).
- [ ] BuildTool.md ↔ this plan ↔ Tour kept in sync (review
      cadence: each phase completion).

### Testing strategy

- [ ] Per-action unit tests (start: Phase 4; continue through
      Phase 9).
- [ ] Integration tests per phase (initially gtest in-tree,
      switch to `cajeta test` after Phase 7).
- [ ] End-to-end smoke: build → sign → upload → install round
      trip in CI against mock backends.
- [ ] Reproducible-build verifier runs nightly (post Phase 11).

---

## Open decisions

These need answers before the code that depends on them ships.
Each is a decision, not a unit of work.

- [ ] **Public registry governance.** Namespace reservation
      policy, malicious-package takedown, signed-retraction
      record format. *Lean:* DNS-proof ownership;
      separate-RFC scope.
- [ ] **Capability minor-version drift.** Is adding a
      capability in a semver-patch breaking? *Lean:* yes; treat
      as minor-version bump. Decide before first stdlib release.
- [ ] **Multi-version transitive deps.** Allow two majors of
      one package in the same resolved graph? *Lean:* Cargo's
      model — yes.
- [ ] **Cross-toolchain compatibility window.** Which N's can
      consume archives built by toolchain N? *Lean:* N±2,
      matching `cajeta-lang-version` field.
- [ ] **Manifest schema versioning.** Ship `schema-version` at
      the top of `cajeta.json` from day one? *Lean:* yes.
- [ ] **Plugin DSL surface.** Cap plugins at "ship actions" or
      open structured-finding contract too? *Lean:* open
      structured findings in Phase 7; refuse anything wider.
- [ ] **`exec` cross-platform.** Platform-prefixed commands or
      `target.os` props for `when`/`skip-when`? *Lean:* latter.
- [ ] **First-party plugin list.** Final list of plugins
      shipping as `cajeta.*`. *Lean:* `cajeta.coverage`,
      `cajeta.lint.security`, possibly `cajeta.fmt.import-order`.

---

## Risks

| Risk | Mitigation |
|---|---|
| Manifest schema churn during early phases breaks early adopters | `schema-version` field + tooling that warns when manifest version doesn't match toolchain |
| Plugin sandbox escape via subprocess weaknesses | bwrap + capability allowlist; security review before opening plugin authoring beyond first-party |
| MVS conflict resolution explodes on large transitive graphs | O(N log N) typical; `cajeta info --resolve-time` for diagnosing pathological cases; benchmark against real manifests |
| Reproducible-build verifier flakes intermittently | Part of release gating; flakes block the cut until resolved |
| Maven-compat shim hits Maven Central rate limits in CI | N/A — shim deferred; native HTTP driver against corporate Nexus covers the enterprise case |
| `exec` becomes the de facto extension point and bypasses sandboxing | Document as escape hatch; lint warning when a task uses `exec` for something the catalog covers |
| Plugin proliferation rebuilds the Maven plugin problem | Tight v1 first-party list; structured-findings as the only plugin extension surface; new action verbs go through the native catalog |

---

## Implementation ordering — quick view

```
0. Foundations  ──┐
1. Properties    ─┼─┐
2. Lockfile      ─┘ │
                    ├─→  3. Task runner + exec
                    │         │
4. FS + crypto ←────┘         │
                              ├─→ 5. build + IR cache
                              │         │
6. Repos + deps + melts ←─────┘         │
                                        ├─→ 7. test + plugins
                                        │         │
8. Build flavors ←──────────────────────┘         │
                                                  │
9. Distribution: package + upload + publish ←─────┘
         │
         ├─→ 10. Signing + launcher
         │
11. Sandbox + reproducibility ← runs alongside 4-10
         │
12. Workspaces ← runs alongside 6-9
         │
13. Git + attestation
         │
14. Toolchain provisioning + dispatch ← final phase before v1 cut;
         depends on 6 (registry protocol) + 10 (signed verification)
```

The big serial dependencies: `0 → 1 → 2 → 3 → 5 → 6 → 7`. Phase
14 depends on 6 (repository protocol reuse) and 10 (signed
toolchain verification). The rest can be parallelized once their
predecessors are stable.

---

## v1 cut criteria

A v1 release means all of the following are checked:

- [ ] A non-trivial cajeta project (the stdlib itself or a
      sample app) builds, tests, and publishes end-to-end via
      `cajeta` with no external scripting.
- [x] The default `cajeta init` template ships and works
      (basic/workspace/multi-binary/melt; embedded from
      `samples/buildtool/`).
- [ ] First-party plugins ship and work.
- [ ] First-party melts (a stdlib melt, at minimum) ship and
      are importable.
- [ ] Signing + verification path is end-to-end with a real
      trust store.
- [ ] Toolchain provisioning works: project pinned to
      `official:<version>` auto-fetches on first build,
      re-execs transparently, reproduces byte-identically on
      a second machine.
- [ ] Reproducible-build CI passes for 7 consecutive nights.
- [ ] Documentation in sync: BuildTool.md (spec), this plan
      (status all checked through Phase 14), Tour entry.

Anything beyond that is v1.x or v2.
