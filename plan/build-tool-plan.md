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

- [ ] Compiler-version + flag-set canonicalization for cache
      discriminator.
- [ ] Per-file SHA-256-keyed IR cache under
      `.cajeta/cache/ir/<discriminator>.bc`.
- [ ] Transitive-imports digest computation (post-order DFS
      with cycle-break fixed point).
- [ ] Cache eviction: LRU + size cap from `settings.build.cache`.
- [ ] Cache TTL eviction.
- [ ] Custom-flavor map composition
      (`{ "base": "release", "debug-info": "full" }`).
- [ ] `cajeta clean --deep` flag (also wipes `.cajeta/cache/`)
      with confirmation prompt.

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
- [ ] Touching one source file rebuilds only that file +
      dependents (Phase 5b).
- [ ] Cache size cap enforces eviction (Phase 5b).
- [ ] Rebuild after eviction produces byte-identical IR
      (Phase 5b).
- [ ] Flag-set order doesn't bust the cache (Phase 5b).
- [ ] `emit: "executable"` with no resolvable entry method
      fails the build at action-validation time, not after a
      partial compile (5a — implemented).

### Acceptance

- [ ] First build of the cajeta stdlib succeeds end-to-end with
      default `emit` (`archived-ir`).
- [ ] Each `emit` value produces output at the documented path
      with the documented `format` output field.
- [ ] Multi-binary project (3 binaries in `settings.build.binaries`)
      builds each binary via a separate task; outputs at
      distinct paths.
- [ ] `cajeta build --binary=cli` produces the CLI executable;
      `cajeta build --binary=server` produces the server
      executable; both reuse the IR cache where source
      overlaps.
- [ ] Touching one source file rebuilds only that file +
      dependents.
- [ ] Cache size cap enforces eviction.
- [ ] Rebuild after eviction produces byte-identical IR.
- [ ] Flag-set order doesn't bust the cache (canonicalization
      works).
- [ ] `emit: "executable"` with no resolvable entry method
      fails the build at action-validation time, not after a
      partial compile.

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

- [ ] HTTP repository driver — bearer token auth.
- [ ] HTTP repository driver — mutual-TLS auth.
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
- [ ] `cajeta upgrade [dep]` subcommand with capability-change
      prompt.
- [ ] `cajeta info --resolve-time` for diagnosing pathological
      resolution.
- [ ] Maven-compat shim (`type: maven-compat`).
- [x] BuildAction integration: pass `--classpath` from resolver
      output to the compiler.

### Deliverables — Phase 6c (Git + melts)

- [ ] Git repository driver — clone-and-build at
      branch/tag/rev.
- [ ] Override: local path replacement.
- [ ] Override: Git replacement.
- [ ] `melt` top-level manifest block recognized.
- [ ] `settings.melts` array parsed.
- [ ] Melt-provided dependency constraint table built in
      declaration order.
- [ ] Melt-provided properties / actions / repositories merged
      into the consumer.
- [ ] `"*"` version in dependencies looks up from melt table.
- [ ] Transitive melt imports (post-order, cycle detection).
- [ ] Resolved melts recorded in lockfile.
- [ ] `cajeta info --melts` / `--melt-tree` output.
- [ ] `cajeta upgrade --melt <name>`.
- [ ] `cajeta publish --as-melt`.

### Melts (deliverables added)

- [ ] `melt` top-level manifest block recognized; melt packages
      can be published + fetched like regular artifacts but
      contain no source/tasks. Validator rejects melt manifests
      that also declare `tasks` or `workspace`.
- [ ] `settings.melts` array parsed; each entry pinned to a
      concrete `name@version` (no range resolution at the
      melt-import layer).
- [ ] Each declared melt resolved through the repository
      machinery; melt-payload validated against the `melt.*`
      schema.
- [ ] Melt-provided dependency constraint table built in
      declaration order; later overrides earlier on conflicts.
- [ ] Melt-provided properties merged into the consumer's
      property table (inert-inherits rule); shadowed by
      consumer's own properties.
- [ ] Melt-provided action presets merged into the consumer's
      `actions` namespace; shadowed by consumer's own.
- [ ] Melt-provided repositories appended to the consumer's
      resolution list (priority field honored).
- [ ] `"*"` version in `settings.dependencies` looks up from
      the melt constraint table; hard error if not present in
      any imported melt.
- [ ] Transitive melt imports (`melt.melts`) processed via
      post-order traversal with cycle detection.
- [ ] Resolved melts recorded in lockfile under a top-level
      `melts` array (each entry includes `transitive-melts`).
- [ ] Per-resolved-package `provided-by` field in lockfile
      naming the melt that supplied the version (or `"explicit"`).
- [ ] `cajeta info --melts` / `--melt-tree` output.
- [ ] `cajeta upgrade --melt <name>` subcommand.
- [ ] `cajeta publish --as-melt` subcommand (publishes a
      manifest-only `.cja`).

### Acceptance

- [ ] Three-deep transitive dep graph resolves correctly.
- [ ] MVS picks the lowest acceptable on a conflict.
- [ ] Override forces a specific version transitively.
- [ ] Major-version-downgrade guard fires when expected.
- [ ] Maven-compat shim fetches a known artifact from a Maven
      Central mirror.
- [ ] Local override beats remote on priority.

### Melts (acceptance criteria added)

- [ ] A package importing two melts that constrain the same
      dep to different versions resolves to the later-listed
      melt's version (and the lockfile records `provided-by`).
- [ ] A consumer declaring `"*"` for a dep that's in an
      imported melt resolves to the melt-provided version.
- [ ] A consumer declaring `"*"` for a dep that's in NO
      imported melt fails the build with a clear error citing
      the dep name.
- [ ] A melt that transitively imports itself fails the build
      with a cycle citation.
- [ ] An explicit version on a consumer's dep overrides the
      melt-provided constraint; the build emits a warning
      naming the divergence.
- [ ] A melt manifest that also declares `tasks` or
      `workspace` is rejected at validation time.

### Deliverables — Phase 6d (Repository protocol v2 — deferred)

Spec lives in `cajeta-docs/BuildTool.md` under "Repository
protocol — v2 enhancements (deferred)". v1 (Phase 6a-c) is
what initial release needs; v2 lands once registry traffic
justifies the compute and storage. Backward compatibility is
permanent — a v1-only client and a v2-only client both keep
working against a server that advertises both.

- [ ] `/.well-known/cajeta-capabilities.json` capability probe
      on first contact with a repository (cached for the TTL
      the server returns); v2 paths preferred when advertised.
- [ ] `POST /v2/bundle` client + server: streamed tar.zst with
      `have`/`want`/`transitive`/`format`; client unpacks into
      the workstation cache as the stream arrives.
- [ ] `GET /v2/resolve` + `GET /v2/blob/<sha256>` content-
      addressed surface (metadata indirection + immutable blob
      storage); workstation cache keys already match.
- [ ] Retraction metadata (`retracted: true` + reason) surfaced
      in resolve responses; new resolves emit a warning, old
      lockfile entries keep resolving.
- [ ] Pre-computed well-known bundles
      (`GET /v2/bundle/well-known/<key>.tar.zst`) for the
      stdlib and registered melts; key derived from melt
      identity.
- [ ] `POST /v2/lockfile-diff` differential fetch (with
      fallback to full bundle on snapshot miss).
- [ ] Opt-in `supercompress.zst` bundle format (cross-file
      zstd over decompressed `.cja` payloads).
- [ ] Transparency-log endpoint + verification on artifact
      install; signing-launcher hooks the check in alongside
      the per-artifact signature verify.
- [ ] Mirror federation: client latency-probes the advertised
      mirrors, prefers the closest, falls back to primary.
- [ ] Namespace verification at publish: DNS TXT record
      (`_cajeta-publish.<domain>`) or
      `.github/cajeta-publish.txt`, verified once per
      publisher.
- [ ] CLI: `cajeta info --capabilities <repo>` prints the
      v1/v2 surface a given repo advertises.

### Acceptance — Phase 6d

- [ ] A 50-dep cold install issues exactly one `/v2/bundle`
      request (plus the capabilities probe + resolve calls)
      against a v2-capable repository.
- [ ] A subsequent install with one dep bumped fetches only
      that dep's artifact (plus any new transitives), proven
      by request log inspection.
- [ ] A retracted artifact is still installable when its
      sha256 is already in a downstream lockfile; new
      resolves emit the retraction warning.
- [ ] A v1-only client successfully installs from a v2-
      capable server (capability probe + fallback path).
- [ ] A v2-only client successfully installs from a v1-only
      server (capability probe returns no `v2`, client uses
      v1 endpoints).
- [ ] Differential lockfile fetch over a one-dep-bump
      transfers substantially less than the full bundle.
- [ ] Transparency-log check fails the install when the log
      entry's signature is invalid or absent.

---

## Phase 7 — Test action + first-party plugins

**Goal:** `cajeta test` runs and reports coverage.

### Deliverables

- [ ] `test` action wrapping the test runner.
- [ ] Plugin runtime: subprocess isolation.
- [ ] Plugin capability allowlist enforcement against
      `settings.plugins-allowed-capabilities`.
- [ ] Structured-findings stream from plugin to build tool.
- [ ] Plugin lockfile entry (top-level `plugins` array).
- [ ] `cajeta.coverage` plugin — `cajeta.coverage.instrument`
      action.
- [ ] `cajeta.coverage` plugin — `cajeta.coverage.collect`
      action.
- [ ] `cajeta.coverage` plugin — `cajeta.coverage.report` action
      with HTML / SARIF / lcov / console outputs.
- [ ] Coverage grain options: line, branch, region.
- [ ] Coverage `min` threshold gate.
- [ ] Coverage `min-per-file` floor.
- [ ] Coverage `exclude` patterns.
- [ ] `@nocoverage(reason)` source annotation honored by
      coverage; mandatory reason; lint warns on generic.
- [ ] `cajeta.lint.security` plugin — banned-imports scan.
- [ ] `cajeta.lint.security` plugin — secret-pattern scan.
- [ ] `lint` task default template wires natives + plugins.

### Acceptance

- [ ] `cajeta test` builds with coverage instrumentation, runs
      tests, emits HTML + console + SARIF reports.
- [ ] A `min: 80` threshold violation fails the task with a
      bottom-N file citation.
- [ ] The security plugin flags a known secret pattern in a
      test fixture.
- [ ] A plugin requesting a capability not in the allowlist
      fails to load with a clear error.

---

## Phase 8 — Build flavors

**Goal:** `build` action accepts string or map flavor values;
custom flavor definitions compose by name. Profile is per-task
literal (no overlay machinery).

### Deliverables

- [ ] Two built-in flavors: `release`, `debug`. Property
      bundles defined as in BuildTool.md "Built-in flavors".
- [ ] Property vocabulary parser: `opt`, `lto`, `debug-info`,
      `strip-symbols`, `bounds-check`, `null-checks`,
      `overflow-checks`, `asan`/`tsan`/`msan`/`ubsan`,
      `analytics`, `source-tags`.
- [ ] Unknown property key at manifest-load is a hard error.
- [ ] `flavor` accepts string (name) form.
- [ ] `flavor` accepts map (composition) form with `base` +
      property overrides.
- [ ] `settings.build.custom-flavors` block: project-named
      composition maps.
- [ ] Custom-flavor cycle detection (`A.base == B` and
      `B.base == A` rejected at load time).
- [ ] Resolved flavor passed to the compiler as the
      corresponding flag set.
- [ ] `build` action accepts `profile` string param; passes
      through to compiler as `--profile=<name>`.

### Acceptance

- [ ] `flavor: "release"` resolves to the built-in property
      bundle.
- [ ] `flavor: { "base": "release", "debug-info": "full" }`
      resolves to release's bundle with debug-info overridden.
- [ ] `flavor: "integration"` referencing a custom-flavor map
      resolves through the named composition.
- [ ] Unknown property key (`debg-info`) produces a citation
      naming the offending key + the allowed vocabulary.
- [ ] Two custom flavors with `base` cycling fail load-time
      validation.
- [ ] `build` action with `profile: "test"` invokes the
      compiler with `--profile=test`; `@Profile`-gated DI
      resolution sees the right components.

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

- [ ] `package` action with `input` + `format` required params,
      `spec` + format-specific params optional.
- [ ] Input/format mismatch detected at action-validation time
      (not mid-pipeline).
- [ ] `format: "obj-tree"` — IR → per-source `.o` tree.
- [ ] `format: "uber-ir"` — IR → linked `.bc`.
- [ ] `format: "uber-archive"` — `.cja` + resolved deps →
      single `.cja` with transitive contents.
- [ ] `format: "static-lib"` — IR → `.a`.
- [ ] `format: "shared-lib"` — IR → `.so` / `.dylib` / `.dll`
      per host.
- [ ] `format: "tarball"` — file or directory → `.tar.zst`
      (`.tar.gz` if requested).
- [ ] `format: "zip"` — file or directory → `.zip`.
- [ ] `format: "container"` — executable → OCI image; supports
      `base`, `tag`, `expose`, `env`, `labels`; outputs include
      `manifest` + (when pushed) `registry-url`.
- [ ] Inline-params metadata + `spec`-file metadata both
      supported.
- [ ] Output cache key on `(input-sha256, format,
      format-specific-params)` so repeated packages of unchanged
      input are skipped.

### `package` action — deferred slices

Each is a self-contained follow-on; not in v1 cut.

- [ ] `format: "deb"`
- [ ] `format: "rpm"`
- [ ] `format: "msi"`
- [ ] `format: "app-bundle"`
- [ ] `format: "pkg"`
- [ ] `format: "dmg"`
- [ ] `format: "appimage"` / `"flatpak"` / `"snap"` (lowest
      priority)

### Upload + publish deliverables

- [ ] `upload` action — `target: s3`.
- [ ] `upload` action — `target: azure`.
- [ ] `upload` action — `target: gcs`.
- [ ] `upload` action — `target: http` (PUT).
- [ ] `upload` action — `target: http` (POST + multipart form).
- [ ] `upload` action — `target: sftp`.
- [ ] Upload `also` array (multi-file uploads — e.g. `.sig`
      alongside `.cja`).
- [ ] `publish` action speaking the cajeta repository protocol
      POST endpoint.
- [ ] `cajeta publish` built-in subcommand as sugar over a
      publish action.
- [ ] Variable substitution `${env.NAME}` + `${<id>.<field>}` in
      every upload-action param.
- [ ] Retry with exponential backoff for transient network
      failures.

### Acceptance

- [ ] A `release` task pipelines build (executable) →
      sign → package (`container`) → upload (HTTP PUT to a
      mock registry) without any intermediate manual steps.
- [ ] `format: "uber-archive"` produces a `.cja` that contains
      every transitive dep at the resolved version; a fresh
      `cajeta install` against it has no further network
      fetches.
- [ ] `format: "container"` produces a valid OCI image that
      runs the executable on `docker run` / `podman run`.
- [ ] Input/format mismatch (`format: "obj-tree"` with an
      `executable` input) fails at action-validation, with a
      citation naming the expected input shape.
- [ ] Artifact + signature upload to S3; URLs consumable by
      downstream actions.
- [ ] Artifact upload to Azure Blob.
- [ ] Artifact upload to GCS.
- [ ] HTTP PUT and POST both work against a mock server.
- [ ] SFTP works against a containerized SSH endpoint in CI.
- [ ] Transient failure recovers via retry; persistent failure
      surfaces clearly.
- [ ] `package` cache hits skip work when input is unchanged
      (same `(input-sha256, format, params)` triple).

---

## Phase 10 — Archive signing + launcher verification

**Goal:** signed archives ship; the launcher refuses tampered
ones.

### Deliverables

- [ ] `sign` action wires into `cajeta archive sign`.
- [ ] Key-id recorded in archive's `.cajeta-manifest` section.
- [ ] Launcher mode: `off` (default for local builds).
- [ ] Launcher mode: `warn`.
- [ ] Launcher mode: `strict`.
- [ ] `--verify-signature[=mode]` CLI flag.
- [ ] Trust store layout: `~/.cajeta/trust/keys/`.
- [ ] Trust store layout: `/etc/cajeta/trust/keys/` (Windows:
      `%ProgramData%`).
- [ ] `CAJETA_TRUST_KEYS_DIR` env override.
- [ ] Trust-store lookup precedence: env → user → system.
- [ ] `cajeta trust list` subcommand.
- [ ] `cajeta trust add <id> <pem-path>` subcommand.
- [ ] `cajeta trust remove <id>` subcommand.
- [ ] `cajeta trust show <id>` (prints fingerprint).
- [ ] `cajeta trust verify <archive>` (one-shot).
- [ ] `CAJETA_REQUIRE_SIGNATURE=strict` env enforcement.

### Acceptance

- [ ] Signed archive verifies under `strict` mode.
- [ ] Unsigned archive fails under `strict` mode with the
      expected error message.
- [ ] Tampered archive fails verification with computed-vs-
      expected digest pair printed.
- [ ] Trust-add then verify works end-to-end against a fresh
      keypair.
- [ ] System trust store unaffected when a user adds/removes
      keys.
- [ ] `CAJETA_REQUIRE_SIGNATURE=strict` overrides a laxer CLI
      flag.

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
| Maven-compat shim hits Maven Central rate limits in CI | Workstation-wide cache fallback; mirror corporate Nexus in CI |
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
