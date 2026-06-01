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
- [ ] Default `cajeta init` template.
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

- [ ] Built-in property resolver: `${details.name}`,
      `${details.version}`, `${details.group}`,
      `${details.library}`, `${flavor}`, `${profile}`,
      `${target}`, `${env.NAME}`, `${workspace.root}`,
      `${cajeta.version}`.
- [ ] User-defined property resolver from `properties` block.
- [ ] Topological resolution for property-references-property.
- [ ] Cycle detection with citation.
- [ ] Missing-property hard error with reference-site citation.
- [ ] CLI `-P NAME=VALUE` override.
- [ ] `CAJETA_PROPERTY_NAME` env override.
- [ ] Override precedence: CLI → env → profile → manifest.
- [ ] `cajeta info --properties` prints the resolved set.

### Acceptance

- [ ] Built-ins resolve correctly across the test matrix.
- [ ] Property-references-property resolves topologically.
- [ ] Cyclic property references fail at load time with a clear
      cycle diagram.
- [ ] CLI and env overrides apply in the documented precedence
      order.
- [ ] User property collision with a built-in is a hard error.

---

## Phase 2 — Lockfile generation + reading

**Goal:** `cajeta.lock` captures resolved state;
manifest-checksum detects drift.

### Deliverables

- [ ] `cajeta.lock` reader/writer (strict JSON, schema
      `lockfile-v1.json`).
- [ ] Manifest SHA-256 checksum computation.
- [ ] Drift detection: mismatched checksum → warning or
      auto-upgrade per config.
- [ ] Resolved property set persisted in lockfile.
- [ ] Plugin version pinning in lockfile (top-level `plugins`
      array).

### Acceptance

- [ ] Lockfile round-trip preserves byte-identity given
      identical inputs.
- [ ] Modifying the manifest without re-resolving surfaces the
      drift on next build.
- [ ] Lockfile diff in code review surfaces version changes,
      capability changes, and override changes.

---

## Phase 3 — Task runner + action invocation

**Goal:** `cajeta <task>` runs a task that calls the `exec`
action. End-to-end the simplest possible task works.

### Deliverables

- [ ] Task DAG builder from `tasks.*.depends-on` references.
- [ ] Task-DAG cycle detection at load time.
- [ ] Action invocation infrastructure: param binding,
      type-check against action schema.
- [ ] Sandbox harness (placeholder; full sandbox in Phase 11).
- [ ] `exec` action implementation.
- [ ] `parallel` composition wrapper.
- [ ] `run-task` composition wrapper.
- [ ] Output threading via `${id.field}` substitution.
- [ ] `cajeta task <name> --show` prints the resolved action
      sequence with substitutions applied.
- [ ] `cajeta tasks` lists task names + descriptions from the
      manifest.
- [ ] `when` / `skip-when` action-level conditional skipping.

### Acceptance

- [ ] A task containing only `exec` calls runs end-to-end.
- [ ] Parallel children run concurrently and merge outputs.
- [ ] `run-task` composes one task's outputs into another's
      input chain.
- [ ] A reference to an undefined `${id.field}` fails at load
      time with a citation.
- [ ] A cycle in the implicit action graph fails at load time.

---

## Phase 4 — Native action catalog: filesystem + crypto

**Goal:** ship the action verbs that don't depend on the
compiler/repository surface.

### Deliverables

- [ ] `copy` action with sandbox.
- [ ] `delete` action.
- [ ] `mkdir` action.
- [ ] `sign` action wiring into existing
      `cajeta archive sign` (ArchiveManagement.md §8).
- [ ] `verify-sig` action wiring into existing
      `cajeta archive verify-sig`.
- [ ] `version` action — semver bump (major/minor/patch) or
      explicit set; writes back to manifest.
- [ ] `download` action with optional SHA-256 verify.

### Acceptance

- [ ] Each action passes happy-path + failure-mode unit tests.
- [ ] Cacheable actions (`copy`, `sign`, `mkdir`) participate in
      the IR cache once Phase 5 lands.
- [ ] `sign` + `verify-sig` round-trip a real `.cja`.

---

## Phase 5 — `build` action + IR cache

**Goal:** `cajeta build` produces a `.cja`; incremental builds
work.

### Deliverables

- [ ] `build` action wrapping the existing compiler binary.
- [ ] Compiler-version + flag-set canonicalization for cache
      discriminator.
- [ ] Per-file SHA-256-keyed IR cache under
      `.cajeta/cache/ir/<discriminator>.bc`.
- [ ] Transitive-imports digest computation (post-order DFS
      with cycle-break fixed point).
- [ ] Cache eviction: LRU + size cap from `settings.build.cache`.
- [ ] Cache TTL eviction.
- [ ] `clean` action (removes `.cajeta/work/`).
- [ ] `cajeta clean --deep` flag (also wipes `.cajeta/cache/`)
      with confirmation prompt.

### Acceptance

- [ ] First build of the cajeta stdlib succeeds end-to-end.
- [ ] Touching one source file rebuilds only that file +
      dependents.
- [ ] Cache size cap enforces eviction.
- [ ] Rebuild after eviction produces byte-identical IR.
- [ ] Flag-set order doesn't bust the cache (canonicalization
      works).

---

## Phase 6 — Repositories + dependency resolution

**Goal:** `cajeta build` can fetch transitive deps.

### Deliverables

- [ ] Filesystem repository driver.
- [ ] HTTP repository driver — bearer token auth.
- [ ] HTTP repository driver — mutual-TLS auth.
- [ ] MVS constraint solver.
- [ ] Transitive override mechanism from `settings.overrides`.
- [ ] Override: pin to version.
- [ ] Override: version range constraint.
- [ ] Override: local path replacement.
- [ ] Override: Git replacement.
- [ ] Major-version-downgrade guard (with `allow-major-downgrade`
      escape).
- [ ] Local artifact cache at `.cajeta/cache/artifacts/<sha256>.cja`.
- [ ] Workstation-wide cache fallback at `~/.cajeta/cache/`.
- [ ] `cajeta add <dep>` subcommand.
- [ ] `cajeta remove <dep>` subcommand.
- [ ] `cajeta upgrade [dep]` subcommand with capability-change
      prompt.
- [ ] `cajeta info --resolve-time` for diagnosing pathological
      resolution.
- [ ] Maven-compat shim (`type: maven-compat`).

### Acceptance

- [ ] Three-deep transitive dep graph resolves correctly.
- [ ] MVS picks the lowest acceptable on a conflict.
- [ ] Override forces a specific version transitively.
- [ ] Major-version-downgrade guard fires when expected.
- [ ] Maven-compat shim fetches a known artifact from a Maven
      Central mirror.
- [ ] Local override beats remote on priority.

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

## Phase 8 — Profiles + flavors

**Goal:** `cajeta build --profile=ci` activates the CI overlay.

### Deliverables

- [ ] Profile overlay resolver — JSON-merge-patch semantics
      (RFC 7396).
- [ ] `+field` append-rather-than-replace semantics.
- [ ] Deletion via `null` in overlay.
- [ ] Profile activation: CLI `--profile=<name>`.
- [ ] Profile activation: `CAJETA_PROFILE` env var.
- [ ] Sticky profile via `.cajeta/profile` (one-line file, not
      committed).
- [ ] Manifest default profile (`profiles.default`).
- [ ] Multi-profile composition (`--profile=a,b,c`).
- [ ] `cajeta profile activate <name>` subcommand.
- [ ] `cajeta profile deactivate` subcommand.
- [ ] `cajeta profile show <name>` (prints merged manifest).
- [ ] `cajeta profile diff <a> <b>` subcommand.
- [ ] Built-in build flavors: release, debug, debug-release,
      fast, minimal, instrumented.
- [ ] Custom flavor support via `settings.build.custom-flavors`.

### Acceptance

- [ ] Active profile + flavor materializes a fully resolved
      manifest; `cajeta info --resolved` shows it.
- [ ] Two profiles composed merge in declared order.
- [ ] Sticky profile survives across invocations until cleared.
- [ ] Custom flavor extends a built-in correctly.

---

## Phase 9 — Distribution: upload + publish

**Goal:** the release pipeline (sign → upload → publish) works
end-to-end against real backends.

### Deliverables

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

- [ ] Artifact + signature upload to S3; URLs consumable by
      downstream actions.
- [ ] Artifact upload to Azure Blob.
- [ ] Artifact upload to GCS.
- [ ] HTTP PUT and POST both work against a mock server.
- [ ] SFTP works against a containerized SSH endpoint in CI.
- [ ] Transient failure recovers via retry; persistent failure
      surfaces clearly.

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
6. Repos + deps ←─────────────┘         │
                                        ├─→ 7. test + plugins
                                        │         │
8. Profiles + flavors ←─────────────────┘         │
                                                  │
9. Distribution: upload + publish ←───────────────┘
         │
         ├─→ 10. Signing + launcher
         │
11. Sandbox + reproducibility ← runs alongside 4-10
         │
12. Workspaces ← runs alongside 6-9
         │
13. Git + attestation ← final phase before v1 cut
```

The big serial dependencies: `0 → 1 → 2 → 3 → 5 → 6 → 7`. The
rest can be parallelized once their predecessors are stable.

---

## v1 cut criteria

A v1 release means all of the following are checked:

- [ ] A non-trivial cajeta project (the stdlib itself or a
      sample app) builds, tests, and publishes end-to-end via
      `cajeta` with no external scripting.
- [ ] The default `cajeta init` template ships and works.
- [ ] First-party plugins ship and work.
- [ ] Signing + verification path is end-to-end with a real
      trust store.
- [ ] Reproducible-build CI passes for 7 consecutive nights.
- [ ] Documentation in sync: BuildTool.md (spec), this plan
      (status all checked through Phase 13), Tour entry.

Anything beyond that is v1.x or v2.
