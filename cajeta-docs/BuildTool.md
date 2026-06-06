# BuildTool.md

The cajeta toolchain ships a single binary, `cajeta`, that handles
project initialization, dependency management, builds, tests,
docs, formatting, linting, and publication. One tool to install,
consistent flag semantics across operations, no plugin
fragmentation for the common case.

The model is **task-based, not lifecycle-based**. There is no
fixed `validate → compile → test → package → install → deploy`
sequence the tool enforces (that was Maven's downfall — projects
that didn't fit the lifecycle fought it forever). Instead, the
manifest declares **tasks** the project defines, each composed of
**actions** that the tool runs in order:

- **Tasks** are entries in the manifest's `tasks` block. A task
  is a named, declarative sequence of action invocations.
- **Actions** come in two flavors:
  - **Native actions** — built into the build tool
    (`build`, `copy`, `upload`, `sign`, `test`, ...).
  - **Plugin actions** — provided by plugins declared in the
    manifest, resolved through the same repository machinery as
    runtime dependencies.
  The manifest's `actions` block lets the project register
  **action presets** (named partial bindings of a native or
  plugin action) to DRY up repeated invocations.
- **Pipelining** — each action declares the outputs it produces
  (file paths, hashes, URLs, status codes). Subsequent actions
  in the same task consume those outputs via `${id.field}`
  substitution. A `build` action produces `${art.path}`; a
  `sign` action takes it as `input`; an `upload` action takes
  it (and the resulting `.sig`) and posts to S3/HTTP/SFTP/etc.

Project-level CLI invocations are task names:
`cajeta build`, `cajeta test`, `cajeta release` — each runs the
task by that name from the manifest. `cajeta init` ships a
default `tasks` block that implements the Maven-style lifecycle
as a starter set; **everything in the template is editable**.
A project that needs a custom release flow rewrites `tasks.release`;
one that doesn't need documentation generation deletes
`tasks.doc`.

Lessons taken from Maven, Gradle, Cargo, npm, pip, Bazel, and
Nix — see [Comparative analysis](#comparative-analysis) near the
end for which patterns came from where, what worked, what
didn't, and why we made the tradeoffs we made. The task model is
Gradle's contribution — minus the Turing-complete script.

The compiler (`cajetac` — the binary at `build/src/cajeta` in the
current source tree, see [Compilation.md](Compilation.md)) is the
back end. The `cajeta` build tool wraps it: discovers the project,
resolves dependencies, materializes the build graph, drives
parallel compilation, runs the linker, and ships the result. The
compiler is fully usable without the build tool — but the build
tool is what users normally invoke.

## Table of contents

1. [Goals](#goals)
2. [Non-goals](#non-goals)
3. [CLI surface](#cli-surface)
4. [Project layout](#project-layout)
5. [Manifest — `cajeta.json`](#manifest--cajetajson)
6. [Properties](#properties)
7. [Lockfile — `cajeta.lock`](#lockfile--cajetalock)
8. [Dependency resolution](#dependency-resolution)
9. [Overriding transitive dependencies](#overriding-transitive-dependencies)
10. [Melts](#melts)
11. [Repositories](#repositories)
12. [Capability system](#capability-system)
13. [Build flavors](#build-flavors)
14. [Profiles](#profiles)
15. [Incremental builds](#incremental-builds)
16. [Workspaces](#workspaces)
17. [Sandboxing and reproducibility](#sandboxing-and-reproducibility)
18. [Action catalog](#action-catalog)
19. [Action pipelining](#action-pipelining)
20. [Default `cajeta init` manifest](#default-cajeta-init-manifest)
21. [Archive signing and launcher verification](#archive-signing-and-launcher-verification)
22. [Toolchain provisioning](#toolchain-provisioning)
23. [Plugins](#plugins)
24. [Open specifications](#open-specifications)
25. [Comparative analysis](#comparative-analysis)
26. [Implementation sequence](#implementation-sequence)
27. [Open questions](#open-questions)

---

## Goals

- **One tool covers the common case.** `cajeta build`,
  `cajeta test`, `cajeta run`, `cajeta doc`, `cajeta add <dep>`,
  `cajeta publish` cover ~95% of what a developer does day-to-day.
  No separate `cajetafmt`, `cajetadoc`, `cajeta-deps` binaries to
  install.
- **No fixed lifecycle.** The build tool does not enforce a
  fixed `validate → compile → test → package → install →
  deploy` ordering. Projects define their own tasks; the
  default template provides a Maven-shaped starter set that
  exists only as editable task files, not as engine-enforced
  phases. The escape valve is "edit the task," not "configure
  the lifecycle".
- **Declarative all the way down.** Tasks and actions are JSON;
  there is no embedded scripting language. Compositions of
  actions are themselves declarative (user-defined actions
  compose built-in actions through parameter passing and output
  threading). The escape hatch is the `exec` action — explicit
  shell-out, sandboxed, no pretense at being part of the
  analyzable surface.
- **JSON everywhere.** Manifest (`cajeta.json`), lockfile
  (`cajeta.lock`), capability declarations, registry API
  responses — all JSON. JSONC (JSON-with-comments) for the
  manifest (which humans edit); strict JSON for machine-only.
  Tasks, action presets, and plugin declarations all live as
  blocks within the manifest — one document, not a directory
  tree.
- **Repository-based dependency resolution.** Manifest declares
  ordered repository list; resolution walks repositories in
  priority order until each `name@Version` is found. Standard
  Maven / npm / Cargo pattern. Local file:// repos for dev
  overrides; corporate Nexus-style internal repos for inner-source;
  public registry as the bottom tier.
- **Capabilities as a first-class security model.** Every library
  declares which capabilities (`network`, `filesystem`, etc.) it
  needs. The compiler statically computes the actually-used set
  and rejects builds where declared ≠ actual. Lockfile records
  capabilities per dependency; capability changes between versions
  surface during `cajeta upgrade` as review prompts. Actions and
  plugins are capability-gated too — an action declares what it
  touches, the build tool sandboxes accordingly.
- **Reproducible builds.** Same source + same lockfile + same
  compiler version → byte-identical artifact. SLSA-style build
  attestation generated by the `publish` action. Cross-org cache
  via repositories serving pre-built artifacts (no Bazel-style
  build cache; see [Repositories](#repositories) for rationale).
- **Workspace / monorepo support as first-class.** Multi-package
  projects share a single lockfile, share dependency resolution
  across members, build incrementally across the workspace graph.
  Tasks at the workspace root are workspace-wide; per-member
  tasks shadow workspace tasks of the same name.
- **Open specifications.** Every interface — manifest schema,
  lockfile format, task/action schema, repository HTTP protocol,
  capability schema, archive format — is published, versioned,
  machine-readable. Third parties can build alternative
  implementations (IDE integrations, CI runners, alternative
  registries, build visualizers) without reverse-engineering.

## Non-goals

- **Turing-complete task or action definitions.** Tasks compose
  actions; actions compose other actions; nothing in the chain is
  an embedded programming environment (Gradle's failure mode).
  No conditionals beyond simple property-driven action skipping,
  no loops, no expression language beyond `${property}`
  substitution. The 5% of projects that need real logic use the
  `exec` action — explicit, sandboxed, no pretense at being
  analyzable.
- **Engine-enforced lifecycle.** The tool does not declare
  built-in phases that projects extend or override. Projects
  define whatever tasks they want; `cajeta <name>` runs the task
  by that name. Maven's lifecycle inverted the priority — the
  framework decided the order, projects bent themselves to fit.
  Cajeta's task-on-task `depends-on` lets projects declare
  ordering when they want it, without imposing one.
- **Embedded build cache competing with the repository model.**
  Bazel / Buck-style content-addressed build caches are a
  different mechanism than repositories — and we picked
  repositories. A local download cache for already-fetched
  artifacts exists; a remote build cache for "I just compiled X,
  let me share the artifact with my coworkers" is explicitly out
  of scope. Coworkers publish to an internal repository if
  artifact sharing is desired.
- **Multi-language build orchestration.** The cajeta build tool
  is a cajeta-language tool. Calling out to a C++ build or
  bundling JavaScript assets is done via the `exec` action, not
  via core build-graph integration. A separate "polyglot"
  build tool would compete with Bazel / Pants — out of scope
  for v1.
- **Hosted registry as part of v1.** The repository protocol is
  specified, but running the public `repo.cajeta.org` registry
  is its own (separate-RFC, separate-team) infrastructure
  project. Local and self-hosted Nexus-equivalent repositories
  are sufficient for v1.

---

## CLI surface

The CLI takes a task name and an optional argument list:

```
cajeta <task>           Run the task named <task> from cajeta.json's
                          "tasks" block
cajeta <task> <args>    Same, with extra args bound as task params
cajeta -- <task>        Disambiguate when <task> collides with a
                          built-in subcommand
```

Bare invocations like `cajeta build`, `cajeta test`, `cajeta run`
each run the like-named task. There is no built-in `build`
subcommand the tool implements — `tasks.build` (shipped by
`cajeta init`) is the implementation, and the project owns it.

### Built-in subcommands (separate from tasks)

A small set of subcommands are built into the tool itself
because they manipulate manifest/lockfile/cache state directly
rather than invoking tasks from the manifest:

```
cajeta init <name>           Scaffold a heap project (writes a starter cajeta.json
                                with a default "tasks" block — see "Default
                                cajeta init manifest")
cajeta tasks                 List task names from cajeta.json's "tasks" block
cajeta task <name> [--show]  Show the resolved action sequence for a task
                                (substitutions applied, dependencies expanded)
cajeta add <dep>             Add a dependency (manifest + lockfile mutation)
cajeta remove <dep>          Remove a dependency
cajeta upgrade [dep]         Re-resolve versions per manifest constraints
cajeta vendor                Copy all transitive deps into ./vendor for offline builds
cajeta install               Install an artifact from a repository into the local download cache
cajeta info                  Print computed dependency tree, capability set, properties, profile
cajeta trust                 Manage the launcher's signature-verification trust store
cajeta workspace             Workspace-wide operations (multi-package projects)
```

These aren't tasks because they're tool-state operations, not
project behavior the user customizes. A project that genuinely
wanted to customize `cajeta add`'s behavior wraps it in a task
of its own.

Every built-in subcommand and every task accepts
`--manifest=<path>` (default `./cajeta.json`), `-v`/`--verbose`,
`--quiet`, `--profile=<name>`, `-P <prop>=<value>`. Flavor flags
(`--release`, `--debug`, `--fast`, ...) propagate as the
`${flavor}` property to whichever `build` action the task
invokes.

---

## Project layout

A cajeta project's on-disk layout comes in four shapes — single
package, workspace (monorepo), melt-only package, and the
workstation-wide bits that sit in `$HOME`.

### Single package

```
my-project/                          # any name; this is the project root
│
├── cajeta.json                      # REQUIRED — the manifest (fixed name)
├── cajeta.lock                      # generated by `cajeta build`; committed to VCS
│
├── src/
│   ├── main/
│   │   ├── cajeta/                  # production .cajeta sources
│   │   └── resources/               # bundled into the published .cja
│   │       ├── templates/
│   │       └── config/
│   └── test/
│       ├── cajeta/                  # test .cajeta sources
│       └── resources/               # bundled only into the test archive
│           ├── fixtures/
│           ├── golden/
│           └── config/
│
├── build/                           # build outputs (configurable; default ./build)
│   ├── exe/<name>                   #   final executable
│   ├── archive/<name>-<version>.cja #   the published artifact
│   ├── coverage/html/               #   cajeta.coverage HTML report
│   ├── docs/                        #   `cajeta doc` output
│   └── logs/                        #   per-action structured logs
│
├── dist/                            # `copy` distribution-action default
│   ├── <name>-<version>.cja
│   └── <name>-<version>.cja.sig
│
└── .cajeta/                         # build STATE — gitignored
    ├── cache/                       #   content-addressed, safe to wipe
    │   ├── artifacts/<sha256>.cja
    │   ├── git/<repo-hash>/<rev>/
    │   ├── ir/<discriminator>.bc
    │   └── metadata/<repo>/<pkg>/
    └── work/                        #   intermediate build outputs
```

### Workspace (monorepo)

```
my-org/                              # workspace root
│
├── cajeta.json                      # workspace manifest — has "workspace" block
├── cajeta.lock                      # SINGLE lockfile across all members
│
├── shared/                          # member location is free-form
│   ├── core/
│   │   ├── cajeta.json              #   member manifest (no "workspace" block)
│   │   └── src/main/cajeta/...
│   ├── util/...
│   └── logging/...
├── apps/
│   ├── api/                         # depends on ../../shared/core via { "path": "..." }
│   ├── client/
│   └── batch/
│
└── .cajeta/                         # WORKSPACE-WIDE build state
    ├── cache/                       #   SHARED across all members (content-addressed)
    │   ├── artifacts/
    │   ├── ir/                      #   cross-member IR hits when source matches
    │   ├── git/
    │   └── metadata/
    └── work/                        #   PER-MEMBER outputs
        ├── shared/core/{obj,exe,log}/
        ├── shared/util/...
        ├── apps/api/...
        └── ...
```

Member source trees mirror the single-package convention
(`src/main/cajeta`, `src/main/resources`, `src/test/cajeta`,
`src/test/resources`). A member built standalone (outside its
workspace) falls back to a member-local `.cajeta/`
automatically.

### Melt-only package

```
platform-melt/
│
├── cajeta.json                      # has "melt" block; NO tasks, NO source
└── cajeta.lock                      # records the melt's own (transitive) melt imports
```

That's the entire tree. A melt is consumed by reference;
publishing emits a `.cja` containing only the manifest.

### Workstation-wide (per user, not per project)

```
~/.cajeta/                           # populated lazily by cajeta tools
│
├── cache/                           # cross-project content-addressed cache
│   ├── artifacts/<sha256>.cja       # fetched once, used by every project
│   ├── git/
│   ├── ir/                          # IR cache shared across projects
│   └── metadata/
│
├── toolchains/                      # cajeta toolchain store
│   ├── official/                    # per-distribution
│   │   ├── 1.0.0/bin/cajeta
│   │   ├── 1.0.3/bin/cajeta
│   │   └── current -> 1.0.3         # workstation-wide default
│   ├── nightly/
│   │   └── 2026-06-01/bin/cajeta
│   └── ...                          # third-party distributions
│
└── trust/keys/                      # signature-verification trust store
    ├── my-org-release-2026.pem      # filename (sans .pem) IS the key-id
    └── upstream-cajeta-org.pem
```

Plus the system-wide layer (managed by the OS package manager,
locked-down read-only):

```
/etc/cajeta/trust/keys/              # Linux/macOS
%ProgramData%\cajeta\trust\keys\     # Windows
```

### Source-tree conventions

The four source roots follow the Maven/Gradle pattern,
parallelized: production and test each have a `cajeta/` source
tree and a `resources/` tree.

| Path | Default | Configurable via |
|---|---|---|
| Production sources | `src/main/cajeta/`     | `settings.build.source-root` |
| Production resources | `src/main/resources/` | `settings.build.resources-root` |
| Test sources | `src/test/cajeta/`     | `settings.build.test-source-root` |
| Test resources | `src/test/resources/`  | `settings.build.test-resources-root` |
| Build output | `build/`               | `settings.build.output-dir` |
| Distribution staging | `dist/`           | (whatever `copy` action's `to` says) |

### Resource lookup at runtime

The `cajeta.lang.Resources` API
(see [Compilation.md "Resources"](Compilation.md#resources))
reads from the archive's bundled resources. Production vs.
test:

- Production code calling `Resources.loadBytes("templates/x.html")`
  reads from `src/main/resources/`.
- Test code calling `Resources.loadBytes("fixtures/sample.json")`
  reads from `src/test/resources/`.
- When the test build runs, `src/test/resources/` is overlaid on
  `src/main/resources/`. Both are visible; **test resources take
  precedence** on path conflict so tests can stub production
  config without touching the real file.
- Production builds (anything but the `test` action) bundle ONLY
  `src/main/resources/`. Test resources never leak into a
  published artifact.

### Cache lookup order

```
1. Project / workspace cache:   ./.cajeta/cache/<key>     ← read first
2. Workstation cache:           ~/.cajeta/cache/<key>     ← read on miss
3. Network fetch:               configured repository      ← only when both miss
```

Every successful fetch writes through to both project AND
workstation caches. Each layer is content-addressed, so write
-through can never produce a conflict — either keys match
(same answer) or they don't (different bucket).

### Fixed vs. configurable paths

| Path | Fixed by spec | Notes |
|---|---|---|
| `cajeta.json`                  | ✓ | Filename fixed; `--manifest=<path>` overrides location only. |
| `cajeta.lock`                  | ✓ | Filename fixed. |
| `.cajeta/` (project / workspace) | ✓ | Build-state directory is always `.cajeta/`. |
| `~/.cajeta/`                   | ✓ | Workstation-wide directory is always under `$HOME/.cajeta/`. |
| `/etc/cajeta/trust/keys/`      | ✓ | System trust store; Windows variant noted above. |
| `src/main/cajeta/`             |   | Convention; configurable. |
| `src/main/resources/`          |   | Convention; configurable. |
| `src/test/cajeta/`             |   | Convention; configurable. |
| `src/test/resources/`          |   | Convention; configurable. |
| `build/`                       |   | Convention; configurable. |
| `dist/`                        |   | Convention; whatever the `copy` action's `to` says. |
| Workspace member directories   |   | Free-form; `workspace.members` lists paths. |

### Committed vs. gitignored

**Committed:**

- `cajeta.json`
- `cajeta.lock`
- Everything under `src/main/`
- Everything under `src/test/` (including `src/test/resources/`)

**Suggested `.gitignore`:**

```
.cajeta/             # build state
build/               # build outputs
dist/                # distribution staging (CI usually doesn't commit)
```

---

## Manifest — `cajeta.json`

JSONC format (strict JSON's data model + `//` and `/* */`
comments + trailing commas). The cajeta toolchain reads it;
third-party tooling that prefers strict JSON can pre-strip
comments via the single-pass preprocessor.

The manifest has six core blocks plus two specialty blocks. A
package is either a regular project, a workspace coordinator, or
a melt — `workspace` and `melt` are mutually exclusive with
each other and with `tasks`/source content.

| Block        | What it contains                                                            |
|--------------|-----------------------------------------------------------------------------|
| `details`    | Package identity: name, version, description, license, authors, lang-version |
| `properties` | `${PROPERTY}` substitution variables (see [Properties](#properties))         |
| `settings`   | Everything tool-related: dependencies, repositories, build flavor, capabilities, profiles, overrides, melts, lint, docs, cache, resources, sandbox |
| `actions`    | User-defined action presets — partial bindings of native or plugin actions, reusable across tasks |
| `plugins`    | Plugin declarations (see [Plugins](#plugins))                                |
| `tasks`      | Task definitions — named sequences of action invocations the user runs via `cajeta <task>` |
| `workspace`  | Multi-package coordinator (see [Workspaces](#workspaces)). Present only on workspace-root manifests; mutually exclusive with `tasks` and `melt`. |
| `melt`       | Curated dependency / property / action / repository exports (see [Melts](#melts)). Present only on melt packages; mutually exclusive with `tasks` and `workspace`. Named for the caramel metaphor — a melt fuses many curated ingredients into one cohesive consumable unit. |

```jsonc
{
    // --- details ----------------------------------------------------
    // Package identity. Used by repositories to address this artifact
    // and by other manifests to depend on it.
    "details": {
        "name":               "com.example.my-service",
        "version":            "0.4.2",
        "description":        "Order processing service",
        "license":            "Apache-2.0",
        "authors":            ["Alice <alice@example.com>"],
        "repository-url":     "https://github.com/example/my-service",
        "cajeta-lang-version": "1.0"
    },

    // --- properties -------------------------------------------------
    // Substitution variables referenced as ${name} anywhere in the
    // manifest. See "Properties".
    "properties": {
        "stack-version":   "1.4.7",
        "metrics-version": "2.0.3",
        "org-nexus":       "https://nexus.company.internal/cajeta",
        "release-prefix":  "cajeta/${details.name}/${details.version}/"
    },

    // --- settings ---------------------------------------------------
    // Everything else the tool needs to know to compile, link,
    // resolve deps, and apply policy.
    "settings": {
        // Capabilities this package requires (security model).
        "capabilities": ["network", "filesystem", "env", "clock", "random"],

        // Direct dependencies.
        "dependencies": {
            "cajeta.io.net.http": "${stack-version}",
            "cajeta.math":        "0.5.0",
            "acme.metrics": {
                "version": "${metrics-version}",
                "from":    "company-nexus"
            },
            "vendor.legacy":   { "path": "./vendor/legacy" }
        },

        // Test-only deps — not propagated to consumers.
        "dev-dependencies": {
            "cajeta.testkit": "1.0.*"
        },

        // Force specific versions transitively (see "Overriding
        // transitive dependencies").
        "overrides": {
            "acme.metrics": "1.2.5",
            "vendor.fork":  { "path": "./vendor/patched-fork" }
        },

        // Repository list, priority-ordered.
        "repositories": [
            { "name": "local-dev",     "type": "filesystem", "path": "/home/me/cajeta-local", "priority": 200 },
            { "name": "company-nexus", "url":  "${org-nexus}",                                "auth": { "type": "bearer", "token-env": "NEXUS_TOKEN" }, "priority": 100 },
            { "name": "central",       "url":  "https://repo.cajeta.org",                     "priority": 0 }
        ],

        // Build configuration. Flavors and overrides see "Build
        // flavors"). Tasks pick their own flavor in their build
        // action; this `settings.build` block carries cross-cutting
        // defaults (target, entry-method, cache config).
        "build": {
            "target":       "host",
            "entry-method": "com.example.Main::main",
            "cache": {
                "max-size": "5GiB",
                "ttl":      "30d"
            },
            // Named flavor compositions (see "Build flavors").
            // Tasks reference these by name from their build action.
            "custom-flavors": {
                "integration":   { "base": "release", "debug-info": "full", "analytics": true },
                "release-debug": { "base": "release", "debug-info": "full" }
            }
        },

        // Toolchain pin (see "Toolchain provisioning"). The running
        // cajeta binary auto-fetches + dispatches to this version if
        // it doesn't match. Independent of cajeta-lang-version
        // (which specifies LANGUAGE compatibility, not exact toolchain).
        "toolchain": {
            "version":      "1.0.3",
            "distribution": "official",
            "fetch":        "auto"
        },

        // Linter configuration.
        "lint": {
            "warn-error": ["unused-import", "deprecated-api"],
            "allow":      ["unused-function-parameter"]
        },

        // Documentation configuration.
        "docs": {
            "theme":           "cajeta-default",
            "member-order":    "alpha",
            "include-private": false
        },

        // Additional files to ship in the archive beyond
        // src/main/resources/. See Compilation.md "Resources".
        "resources": [
            "src/main/resources/**",
            "config/defaults.json"
        ],

        // Capability allowlist for plugin processes (see "Plugins").
        "plugins-allowed-capabilities": ["filesystem"]
    },

    // --- actions ----------------------------------------------------
    // User-defined action presets. Each entry is a partial binding
    // of a native or plugin action; invoking the preset is like
    // invoking the underlying action with the bound params already
    // filled in. Useful for DRY-ing repeated invocations.
    "actions": {
        // Bind upload defaults so each task can just say
        //   { "action": "upload-to-releases", "input": "${art.path}" }
        // without restating bucket / prefix / region every time.
        "upload-to-releases": {
            "extends": "upload",
            "params":  {
                "target": "s3",
                "bucket": "${env.RELEASE_BUCKET}",
                "prefix": "${release-prefix}",
                "region": "us-west-2"
            }
        },

        // Compose multiple native actions into one named verb.
        "ship": {
            "params": {
                "art": { "type": "string", "required": true,
                         "doc": "Path to the built .cja" }
            },
            "actions": [
                { "action": "sign",
                  "input":  "${params.art}",
                  "key-env": "CAJETA_RELEASE_KEY",
                  "key-id":  "${details.name}",
                  "id":      "sig" },
                { "action": "upload-to-releases",
                  "input":  "${params.art}",
                  "also":   "${sig.path}" }
            ]
        }
    },

    // --- plugins ----------------------------------------------------
    // Plugin declarations (see "Plugins"). Plugins ship additional
    // actions invokable from tasks.
    "plugins": {
        "cajeta.coverage": {
            "version": "1.0.*",
            "config":  { "grain": "line", "min": 80,
                         "report": ["html", "sarif", "console"] }
        },
        "acme.license-check": {
            "version": "0.5.0",
            "config":  { "required-header": "config/license-header.txt" }
        }
    },

    // --- tasks ------------------------------------------------------
    // Named sequences of action invocations. `cajeta <name>` runs
    // tasks.<name>. See "Tasks" and "Action pipelining".
    "tasks": {
        "build": {
            "description": "Compile sources to a .cja archive",
            "actions": [
                { "action": "build",
                  "flavor": "${params.flavor:-release}",
                  "id":     "art" }
            ],
            "outputs": { "path": "${art.path}", "sha256": "${art.sha256}" }
        },

        "test": {
            "description": "Build + run tests with coverage",
            "depends-on":  ["build"],
            "actions": [
                { "action": "test", "coverage": true, "id": "tr" }
            ],
            "outputs": { "passed": "${tr.passed}", "failed": "${tr.failed}" }
        },

        "release": {
            "description": "version + build + sign + upload + publish",
            "depends-on":  ["test"],
            "params": {
                "version": { "type": "string", "required": true }
            },
            "actions": [
                { "action": "version", "set": "${params.version}" },
                { "action": "build",   "flavor": "release", "id": "art" },
                { "action": "ship",    "art": "${art.path}" },
                { "action": "publish", "repository": "central" }
            ]
        }
    }
}
```

Field-level conventions:

- **`details.name`** uses reverse-DNS form (`com.example.foo`).
  Matches the package namespace in source; repositories index
  by this name. The leading segments are the **group**
  (organizational namespace, e.g. `com.example`); the last
  segment is the **library** (artifact name, e.g. `foo`).
  Same coordinate scheme as Maven's `groupId:artifactId`,
  flattened into one dotted string. `${details.group}` and
  `${details.library}` are computed properties for the split
  form. Public-name registration follows Maven Central's
  convention: the group's reverse-DNS prefix must be one the
  publisher controls.
- **Versions** use semver-2.0 (`MAJOR.MINOR.PATCH[-prerelease][+build]`).
  Constraints support `*` wildcards, range operators (`>=1.2.0`,
  `<2.0.0`), and combinations.
- **Capabilities** (under `settings.capabilities`) are coarse
  flags from the canonical list (see
  [Capability system](#capability-system)). Any missing
  capability the compiler computes as actually used causes a
  build error naming the call site.
- **Repositories** (under `settings.repositories`) are ordered
  by `priority` (descending). Ties broken by declaration order.
  The build tool walks the list per dependency.
- **Overrides** (under `settings.overrides`) force a specific
  resolution for transitive deps. Applied after MVS; see
  [Overriding transitive dependencies](#overriding-transitive-dependencies).
- **Profiles** in cajeta are per-task literals on the `build`
  action, not a manifest-overlay activation mechanism. Each
  task declares its profile in its own `build` action
  invocation (see [Profiles](#profiles) for the rationale and
  the source-level `@Profile` connection).

The major blocks (`details`, `properties`, `settings`, `actions`,
`plugins`, `tasks`) are all top-level; nothing in the manifest
nests deeper than three levels. This is deliberate — flatter
JSON survives schema versioning better than deep nesting
(adding a sibling under `settings` doesn't move existing
fields, whereas adding a parent layer would).

### Subsections of the manifest

The following sections of this doc each cover one top-level
block in detail:

- [Properties](#properties) — the `properties` block.
- [Settings details follow](#dependency-resolution) below
  ([Dependency resolution](#dependency-resolution),
  [Overrides](#overriding-transitive-dependencies),
  [Repositories](#repositories),
  [Capability system](#capability-system),
  [Build flavors](#build-flavors),
  [Profiles](#profiles)).
- [Action catalog](#action-catalog) — what `actions` block
  entries can extend and compose.
- [Action pipelining](#action-pipelining) — output→input
  threading through `id` references and `${id.field}`.
- [Plugins](#plugins) — the `plugins` block.
- [Default `cajeta init` manifest](#default-cajeta-init-manifest)
  — what `cajeta init` writes by default.

The Tasks structure is covered in the next subsection.

### `tasks` block

A task is a named, declarative sequence of actions; each is an
entry in `manifest.tasks` keyed by task name (the name you'd
pass to `cajeta <name>` on the CLI).

```jsonc
"tasks": {
    "release": {
        "description": "Build, sign, and upload a release",
        "depends-on":  ["test"],
        "params": {
            "version":     { "type": "string", "required": true,
                             "doc": "Release version to publish" },
            "bucket":      { "type": "string", "default": "${env.RELEASE_BUCKET}" },
            "skip-upload": { "type": "bool",   "default": false }
        },
        "actions": [
            { "action": "version", "set": "${params.version}" },
            { "action": "build",   "flavor": "release", "id": "art" },
            { "action": "sign",
              "input":  "${art.path}",
              "key-env": "RELEASE_SIGN_KEY_PEM",
              "key-id":  "release-2026",
              "id":      "sig" },
            { "action": "upload",
              "target": "s3",
              "input":  "${art.path}",
              "bucket": "${params.bucket}",
              "prefix": "cajeta/${details.name}/${params.version}/",
              "also":   "${sig.path}",
              "skip-when": "${params.skip-upload}" }
        ],
        "outputs": {
            "version": "${params.version}"
        }
    }
}
```

Task field reference:

| Field          | Type                          | Required | Notes                                                                  |
|----------------|-------------------------------|----------|------------------------------------------------------------------------|
| `description`  | string                        | no       | Shown in `cajeta tasks` listing.                                        |
| `depends-on`   | array of task names           | no       | Tasks that must complete first; DAG checked at manifest-load time.      |
| `params`       | object {name: param-spec}     | no       | CLI-bindable parameters with `type` + `default` + `doc` + `required`.   |
| `actions`      | array of action invocations   | yes      | The work. Each entry is an action invocation, parallel group, or run-task. |
| `outputs`      | object {name: expression}     | no       | Named values this task exposes to its callers (when invoked via `run-task`). |
| `working-dir`  | string                        | no       | Override the working directory (default: project root).                 |
| `env`          | object {NAME: value}          | no       | Extra env vars for sub-processes spawned by this task.                  |

Each `actions[]` entry is one of:

1. **Action invocation** — object with `action` field naming a
   native action, plugin action, or `actions`-block preset:
   ```jsonc
   { "action": "copy", "from": "build/exe/my-service", "to": "./dist/" }
   ```

2. **Parallel group** — children run concurrently:
   ```jsonc
   { "parallel": [
       { "action": "test", "filter": "Unit*" },
       { "action": "test", "filter": "Integration*" }
   ]}
   ```

3. **Task invocation** — call another task by name; equivalent
   to listing it in `depends-on` but inline at the call site:
   ```jsonc
   { "run-task": "build", "params": { "flavor": "release" } }
   ```

Conditional skipping at the action level uses `when` /
`skip-when`:

```jsonc
{ "action": "upload", "target": "s3", ..., "skip-when": "${params.skip-upload}" }
```

`when` runs the action if the expression is truthy; `skip-when`
runs unless truthy. The expression is one substituted string;
`true`/`false`/`null`/`""` have boolean meaning. No `&&`/`||`
chaining, no nested expressions — if you need real logic, write
a preset in the `actions` block that wraps `exec`, or call a
sub-task.

#### Task dependencies and ordering

`depends-on` declares hard ordering — `tasks.release` with
`depends-on: ["test"]` always runs `test` first. The build tool
walks the task DAG, ignores already-satisfied deps within one
invocation, parallelizes independent branches.

Failure modes:

- An action returning non-zero aborts its task, which aborts
  any dependent tasks queued behind it.
- `--continue-on-error` runs queued dependents anyway; the
  overall exit code still reflects the failure.
- `--keep-going` continues within a task — subsequent actions
  run even if an earlier one fails. The task's exit is the
  worst of any.

### `actions` block (presets)

The manifest's `actions` block holds **action presets** — partial
bindings of an underlying native or plugin action that DRY up
repeated invocations, plus named compositions of multiple
actions.

Two shapes:

**1. Partial binding** — `extends` names the base action;
`params` supplies bound values. Callers fill in whatever isn't
bound.

```jsonc
"actions": {
    "upload-to-releases": {
        "extends": "upload",
        "params": {
            "target": "s3",
            "bucket": "${env.RELEASE_BUCKET}",
            "prefix": "${release-prefix}",
            "region": "us-west-2"
        }
    }
}
```

Callers invoke it like any action, supplying the unbound params:
```jsonc
{ "action": "upload-to-releases", "input": "${art.path}", "also": "${sig.path}" }
```

The caller's params override bound params on a per-field basis;
the bound `region` is replaced if the caller specifies its own.

**2. Composition** — `actions` (an array) names a sequence of
inner action invocations. Outputs are exposed to the caller via
the preset's `outputs` block.

```jsonc
"actions": {
    "ship": {
        "params": {
            "art": { "type": "string", "required": true }
        },
        "actions": [
            { "action": "sign",
              "input":  "${params.art}",
              "key-env": "CAJETA_RELEASE_KEY",
              "key-id":  "${details.name}",
              "id":      "sig" },
            { "action": "upload-to-releases",
              "input":  "${params.art}",
              "also":   "${sig.path}",
              "id":     "up" }
        ],
        "outputs": {
            "url":      "${up.url}",
            "sig-path": "${sig.path}"
        }
    }
}
```

Callers get the outputs via `id`-threading:
```jsonc
{ "action": "ship", "art": "${build.path}", "id": "shipped" },
{ "action": "exec", "command": "notify", "args": ["Released to ${shipped.url}"] }
```

Recursion across presets is a hard error at manifest-load time.

### Why one document instead of many files

A single `cajeta.json` is what the user, IDE, and reviewer all
look at to understand the project. Splitting tasks and actions
into a directory tree was Gradle's path (the `build.gradle` plus
plugins-and-imports-and-applied-files maze); the cost is
real — a non-trivial build requires opening five files to follow
one task. One document with clear top-level sections wins
readability without losing modularity (tasks can call other
tasks, actions can extend other actions — the composition
mechanisms are inside the document rather than across files).

Workspaces (multi-package projects, [Workspaces](#workspaces))
do split: each member has its own `cajeta.json`. A workspace
root's `cajeta.json` has its own `tasks` block that operates at
the workspace level, calling member tasks via `run-task`. Even
there, each manifest is one file.

---

## Properties

Manifests routinely need the same value in many places — a stdlib
version pinned to a single number used by half a dozen
dependencies, a base URL shared by several repositories, a prefix
used by multiple distribution actions. The `properties` block
centralizes these values; any string in the manifest can reference
them with `${PROPERTY}` substitution. Pattern is exactly Maven's
`${property}` and Gradle's `${prop}` — direct prior art, no
surprises.

```jsonc
{
    "properties": {
        "stack-version":   "1.4.7",
        "metrics-version": "2.0.3",
        "org-nexus":       "https://nexus.company.internal/cajeta",
        "release-prefix":  "cajeta/${package.name}/${package.version}/"
    },

    "dependencies": {
        "cajeta.io.net.http":  "${stack-version}",
        "cajeta.lang":         "${stack-version}",
        "cajeta.threading":    "${stack-version}",
        "acme.metrics":        "${metrics-version}"
    },

    "repositories": [
        { "name": "company-nexus", "url": "${org-nexus}", "priority": 100 }
    ],

    "distribute": [
        { "action": "upload-s3", "bucket": "my-releases", "prefix": "${release-prefix}" }
    ]
}
```

The win: bumping `stack-version` once updates every dependency on
the cajeta stdlib that depends on it.

### Substitution rules

- `${NAME}` expands to the value of `properties.NAME`.
- Substitution is purely textual — the resolved string is what
  downstream parsers see. A property holding `"1.4.7"` substituted
  into `"${stack-version}"` produces `"1.4.7"`; substituted into
  `">=${stack-version}"` produces `">=1.4.7"`.
- Substitution is **eager** — properties are resolved when the
  manifest loads, before any subsequent processing. Cyclic
  references are detected and rejected with a clear error citing
  the cycle.
- Properties can reference other properties:
  ```jsonc
  "properties": {
      "stack-major":    "1.4",
      "stack-version":  "${stack-major}.7",                       // value: "1.4.7"
      "release-prefix": "cajeta/${package.name}/v${stack-major}/" // value: "cajeta/<name>/v1.4/"
  }
  ```
  Resolution is by topological order of the cross-reference
  graph.
- Missing property is a hard error citing the reference site. No
  silent empty-string substitution — that turned out badly in
  Make and shell, and we don't repeat it.
- `$$` is the literal `$` escape — needed only when a literal
  `${` would otherwise be misread.

### Built-in properties

These are always available without declaration; user properties
cannot shadow them.

| Property                | Resolves to                                                      |
|-------------------------|------------------------------------------------------------------|
| `${package.name}`       | `package.name` from the manifest                                 |
| `${package.version}`    | `package.version` from the manifest                              |
| `${package.group}`      | Reverse-DNS prefix before the last segment                       |
| `${package.library}`    | Last segment of `package.name`                                   |
| `${flavor}`             | Active build flavor                                              |
| `${target}`             | Build target triple                                              |
| `${env.NAME}`           | Environment variable `NAME`; missing → empty + warning           |
| `${artifact.path}`      | Absolute path to the built `.cja` (in distribution actions only) |
| `${artifact.sha256}`    | Content hash of the built `.cja`                                 |
| `${workspace.root}`     | Workspace root directory; equals project root in non-workspace projects |
| `${cajeta.version}`     | Cajeta toolchain version building this project                   |

A property name colliding with a built-in is a hard error at
manifest load — the developer must rename their property.

### Override precedence

External overrides apply in precedence order (highest wins):

1. **CLI flag.** `cajeta build --property stack-version=1.5.0`
   (short form `-P stack-version=1.5.0`). Useful for one-off
   builds (security patches, A/B comparisons, CI matrices).
2. **Environment variable.** `CAJETA_PROPERTY_STACK_VERSION=1.5.0`
   — uppercase + underscores, matching POSIX conventions. Dots in
   property names map to underscores.
3. **Manifest.** The `properties` block in `cajeta.json`.

The resolved property set is materialized once per build,
recorded in `cajeta.lock` under `"properties"`, and surfaced via
`cajeta info --properties` for inspection. Reproducible builds
depend on the resolved set — a property changed at the CLI for
one build but not another is detectable at lockfile diff time.

### Where properties can be used

Anywhere a JSON string appears in the manifest, with one
exception: keys are not substituted (it would break manifest
indexing). Concretely:

- ✓ dependency version constraints
- ✓ repository URLs, paths, auth references
- ✓ build flavor flag lists, sanitizer name
- ✓ task action parameters (including `profile` literals)
- ✓ distribution action params
- ✓ resource patterns
- ✓ post-build hook commands
- ✓ override target versions / paths
- ✗ property NAMES themselves (the keys of `properties`)
- ✗ dependency name keys (`"cajeta.io.net.http"` is a key, not a value)

The not-in-keys rule is the only material restriction; everything
else is uniform.

---

## Lockfile — `cajeta.lock`

Generated by `cajeta build` / `cajeta upgrade` / `cajeta add`.
Committed to version control. Strict JSON (no comments — it's
machine-only, comments would just create merge conflicts).

```json
{
    "lockfile-version":   1,
    "manifest-checksum":  "sha256:9a4b...",
    "generator":          { "tool": "cajeta", "version": "1.0.0" },
    "resolved-at":        "2026-05-15T17:30:00Z",
    "packages": [
        {
            "name":              "cajeta.io.net.http",
            "version":           "1.2.4",
            "resolved-from":     "central",
            "checksum":          "sha256:8f...",
            "capabilities":      ["network", "filesystem", "clock"],
            "transitive-deps":   ["cajeta.io.net", "cajeta.io", "cajeta.thread"]
        },
        {
            "name":              "cajeta.io.net",
            "version":           "1.0.7",
            "resolved-from":     "central",
            "checksum":          "sha256:3c...",
            "capabilities":      ["network", "clock"],
            "transitive-deps":   []
        },
        ...
    ]
}
```

`manifest-checksum` lets `cajeta build` detect whether the
manifest changed since the lockfile was generated; mismatch
triggers an automatic `cajeta upgrade` (or warns, depending on
config).

`capabilities` per package is the authoritative record of what
that version requires. `cajeta upgrade` compares old-vs-new
capability sets and surfaces additions to the developer:

```
$ cajeta upgrade acme.metrics
acme.metrics 1.0.7 -> 1.1.0
  capabilities changed: + network
  Approve? [y/N]
```

Refusing the prompt rolls the upgrade back. Forcing through (`-y`
or environment override) records the new capability set in the
lockfile; future builds run with the new permissions.

---

## Dependency resolution

Given a manifest, the build tool computes the full transitive
dependency graph by:

1. Reading direct dependencies from `cajeta.json`.
2. For each, querying the manifest's repository list in priority
   order until the requested version is found.
3. Recursing into that artifact's transitive dependencies (each
   `.cja` archive carries its own `cajeta.json` in its manifest
   section).
4. Resolving version conflicts via **minimum-version selection**
   (MVS): when two paths through the graph constrain the same
   package differently, pick the **lowest version that satisfies
   all constraints**. Cargo's algorithm. Predictable; avoids the
   "latest patch broke us" surprise.
5. Recording the resolved graph in `cajeta.lock`.

Conflict resolution prefers explicit pinning over inference:

```
Direct dep:    "cajeta.io.net.http": "1.2.*"
Transitive:    cajeta.io.net.http via dep X requires "1.2.3+"
Resolution:    1.2.3 (lowest 1.2.* satisfying 1.2.3+)
```

Constraint solver runs in O(N log N) for typical graphs; the
worst case (large conflict surface) is exposed via `cajeta info
--resolve-time` for debugging.

---

## Overriding transitive dependencies

MVS picks the lowest version satisfying all constraints — but
sometimes you need to force a specific version across the entire
graph (apply a security patch ahead of upstream, ship a vendored
fork, pin away from a known-bad release). The manifest's
`overrides` block does that:

```jsonc
"overrides": {
    // Pin to an exact version. Every reference in the resolved
    // graph — direct or transitive — uses 1.2.5, regardless of
    // what the intermediate libraries asked for.
    "acme.metrics": "1.2.5",

    // Constrain to a version range. The resolver picks within
    // the range as it would for a direct dep, but the range is
    // applied universally rather than per-import-path.
    "old.lib": ">=2.3.4 <3.0.0",

    // Replace with a local path. Useful for working on a
    // dependency in-tree without round-tripping through a
    // repository.
    "vendor.fork": {
        "path": "./vendor/patched-fork"
    },

    // Replace with a Git-pinned fork. The build tool clones to
    // .cajeta/cache/git/<hash>/ and treats the result as a local
    // package, same as the Git repository mechanism.
    "old.vulnerable": {
        "git":  "https://github.com/our-org/old-vulnerable-fork",
        "rev":  "a1b2c3d"
    }
}
```

Equivalent in concept to Maven's `<dependencyManagement>`,
Gradle's `resolutionStrategy.force`, and Cargo's `[patch]` /
`[replace]` combined. Differences:

- Overrides apply **after** MVS, last-write-wins. Multiple
  overrides for the same package in nested workspaces resolve in
  workspace-root-then-member order.
- An override that drops a major version (`1.x` forced when some
  transitive needs `2.x`) emits a hard error naming the
  conflicting transitive and the path that reaches it. Override
  this anyway by adding `"allow-major-downgrade": true` on the
  override object — the build tool then surfaces a prominent
  warning each build.
- Overrides are recorded in `cajeta.lock` under a top-level
  `"overrides"` array so reviewers see them on every PR that
  touches the lockfile.

Override-vs-direct-dep precedence: a direct dependency in the
top-level `dependencies` block wins over an override of the same
package. Overrides exist for **transitive** dependencies; if
you're picking your own direct version, you don't need an
override.

---

## Melts

A **melt** is a curated bundle of dependency versions, properties,
action presets, and repositories that consumers import as one
unit. Spring users would call this a BOM. Maven users would call
it dependency management. Gradle users would call it a version
catalog. Cajeta calls it a melt because the metaphor matches:
cajeta is caramel; a melt is how the ingredients fuse into one
cohesive consumable thing. The name extends naturally to its
purpose — a single named entity carrying versions, properties,
presets, repos blended into one curated set.

### Anatomy of a melt package

A melt is a regular cajeta package whose purpose is to **export
curated configuration** rather than executable code. It's
distinguished by a top-level `melt` block; its manifest has no
`tasks`, no source, no build output. Publishing a melt produces
a `.cja` containing only the manifest.

```jsonc
// com.example.platform-melt@2024.1.0 — the melt package
{
    "details": {
        "name":        "com.example.platform-melt",
        "version":     "2024.1.0",
        "description": "Curated versions for our internal platform"
    },

    "melt": {
        // Version constraints for packages this melt curates.
        // Consumers that import this melt can declare these deps
        // with version "*" and pick up the constraint here.
        "dependencies": {
            "cajeta.io.net.http":      "1.2.5",
            "cajeta.lang.io":          "2.0.0",
            "cajeta.threading":        "1.4.3",
            "com.example.shared.core": "0.8.2"
        },

        // Shared properties consumers can use after import.
        "properties": {
            "platform-version": "2024.1",
            "release-tag":      "v${platform-version}-stable"
        },

        // Shared action presets consumers can invoke from tasks.
        "actions": {
            "ship-to-prod": {
                "extends": "upload",
                "params":  { "target": "s3", "bucket": "prod-releases" }
            }
        },

        // Repositories to add when this melt is imported. Appended
        // to consumer's list; doesn't replace.
        "repositories": [
            { "name":     "platform-internal",
              "url":      "https://nexus.example.com/cajeta",
              "priority": 150 }
        ],

        // Melts can import other melts (Spring BOM-of-BOM pattern).
        "melts": [
            "cajeta.platform.lang-melt@1.0.0"
        ]
    }

    // No tasks. No source. The package is consumed by reference.
}
```

### What a melt exports — and what it doesn't

| `melt.*` field | Exported? | Why |
|---|---|---|
| `dependencies` | ✓ | The core use case — coordinated version constraints |
| `properties` | ✓ | Same inheritance rule as workspace — inert text substitution |
| `actions` (presets) | ✓ | Inert named verbs; consumer can shadow by redeclaring |
| `repositories` | ✓ | Appended to consumer's resolution list |
| `melts` | ✓ | Transitive melt imports — flatten recursively into the resolved constraint set |
| `plugins` | ✗ | Plugins are active code + security surface; consumers declare explicitly |
| `capabilities` | ✗ | Per-consumer least-privilege; melts can't broaden trust silently |
| `tasks` | ✗ | Tasks are how YOU build; not for a third party to inject |
| `settings.build` flavor / target / sanitizer | ✗ | Per-consumer build choice |

Mirror of the workspace inheritance rule: **inert inherits, active
doesn't.** A consumer importing a melt gets version curation and
convenient presets — never silent plugin activation or capability
broadening.

### Consuming a melt

```jsonc
// com.example.my-app — consumes the platform melt
{
    "details": { "name": "com.example.my-app", "version": "0.1.0" },
    "settings": {
        // Melt imports. Order matters: later melts override earlier
        // on version-constraint conflicts. Each entry is name@Version
        // (concrete pin; melts themselves are version-pinned for
        // reproducibility, not range-resolved).
        "melts": [
            "com.example.platform-melt@2024.1.0",
            "com.example.test-melt@1.0.0"
        ],

        "dependencies": {
            // Version "*" → look up from imported melts.
            "cajeta.io.net.http":      "*",
            "com.example.shared.core": "*",

            // Explicit version → overrides the melt-provided constraint.
            "cajeta.threading":        "1.4.0",

            // Not in any melt → must specify version yourself.
            "vendor.special":          "3.2.1"
        }
    }
}
```

**`"*"` means "use whatever the melt says."** Explicit version
always wins. `"*"` for a dep that no imported melt curates is a
hard error citing the dep name.

### Resolution layers — where melts fit

Each layer narrows the constraint set fed to MVS at the bottom.

```
Highest:  CLI -P / manifest "overrides" block          (final-word forcing)
          ↓
          Member's explicit version in dependencies     (per-package pin)
          ↓
          workspace.shared-dependencies constraint      (workspace curation)
          ↓
          Melt imports (later melt > earlier melt)      (ecosystem curation)
          ↓
          Transitive's stated version constraint
          ↓
Lowest:   MVS-selected lowest acceptable                (resolver default)
```

If a member explicitly pins `1.4.0` while a melt says `1.5.*`, the
explicit pin wins **and** the build emits a warning so the
developer knows they've deviated from the curated set.

### Transitive melt imports

A melt that imports another melt resolves recursively:

1. Consumer's `settings.melts` is processed in order.
2. For each melt, fetch it through the repository machinery, then
   recursively process its `melt.melts`.
3. Flatten into a single ordered constraint set via post-order
   traversal. Later positions override earlier ones.
4. Cycle detection — a melt importing itself transitively fails
   the resolve with a citation.

### Relationship to `workspace.shared-dependencies`

These are the **same idea applied at different scopes:**

| | `workspace.shared-dependencies` | Melt |
|---|---|---|
| Where it's declared | Workspace root manifest | A published `.cja` artifact |
| How consumers see it | Implicit (members inherit by being in the workspace) | Explicit (`settings.melts` import) |
| Scope | Members of one workspace | Any project, anywhere |
| Use case | Internal monorepo coordination | Cross-org / ecosystem curation |

You'd typically have both: a monorepo uses `workspace.shared-dependencies`
for its internal libs (no publish ceremony) and imports external
melts (a stdlib melt, a Spring-equivalent web-stack melt) for
public-ecosystem version curation.

### Lockfile

Melt imports are part of reproducible state. The lockfile records
resolved melts alongside resolved packages:

```jsonc
{
    "lockfile-version":   1,
    "manifest-checksum":  "sha256:...",
    "melts": [
        {
            "name":             "com.example.platform-melt",
            "version":          "2024.1.0",
            "resolved-from":    "central",
            "checksum":         "sha256:...",
            "transitive-melts": ["cajeta.platform.lang-melt@1.0.0"]
        }
    ],
    "packages": [
        {
            "name":         "cajeta.io.net.http",
            "version":      "1.2.5",
            "provided-by":  "com.example.platform-melt@2024.1.0",
            "resolved-from": "central",
            "checksum":     "sha256:..."
        }
    ]
}
```

`provided-by` carries either the melt that supplied the version,
or `"explicit"` when the manifest pinned it directly. Lockfile
diffs in code review then surface "this version moved because of
a melt bump" vs "this version was explicitly bumped."

### CLI

```
cajeta info --melts              # show imported melts + which dep each one provided
cajeta info --melt-tree          # transitive melt graph
cajeta upgrade --melt <name>     # bump one melt, re-resolve dependents
cajeta publish --as-melt         # publish a melt-only package (no source/tasks)
```

### Why `"*"` as the "use melt" sentinel

`"*"` already means "any version" elsewhere in version
constraints. Reusing it for "the melt will pin this" reads
naturally: the consumer is saying "any version — whichever the
melt curates." Alternative spellings considered and rejected:

- `"from-melt"` literal — explicit but verbose.
- Omit the version entirely — Maven-style, but it makes a
  semver-shaped consumer surface (`"cajeta.io.net.http": "*"`)
  inconsistent with the rest of `dependencies`.
- Special syntax (`"@melt"`) — extra notation to learn.

The hard error when no imported melt provides a `"*"`-declared
dep keeps the sentinel from being silently broken.

---

## Repositories

A repository is anything the build tool can ask for an artifact
named `<name>@<version>` and get back a `.cja`. Three concrete
types in v1:

### Filesystem repository

```json
{ "name": "local-dev", "type": "filesystem", "path": "/path/to/repo" }
```

The path contains a tree:

```
/path/to/repo/
├── cajeta.io.net.http/
│   ├── 1.0.0/
│   │   └── cajeta.io.net.http-1.0.0.cja
│   ├── 1.2.4/
│   │   └── cajeta.io.net.http-1.2.4.cja
│   └── versions.json          # { "versions": ["1.0.0", "1.2.4"] }
├── cajeta.math/
│   └── ...
└── index.json                 # { "packages": ["cajeta.io.net.http", "cajeta.math", ...] }
```

Used for dev overrides, vendoring, and CI scenarios that pre-stage
artifacts.

### HTTP repository

Default for public + corporate registries. Wire protocol is a
minimal REST API:

```
GET /index.json
    Returns { "packages": [<name>, ...] }

GET /<name>/versions.json
    Returns { "versions": [<version>, ...], "deprecated": [<version>, ...] }

GET /<name>/<version>/manifest.json
    Returns the package's cajeta.json (the public-facing fields
    only — capabilities, dependencies, etc.). Used for resolution
    without downloading the artifact.

GET /<name>/<version>/<name>-<version>.cja
    Returns the artifact. Should include Content-SHA256 header
    matching what the lockfile would record.

POST /<name>/<version>/<name>-<version>.cja   (auth required)
    Publishes a new artifact. Server validates checksums and
    rejects if a version already exists.
```

Authentication is bearer-token or mutual-TLS, configurable per
repository. Auth tokens come from environment variables (never
embedded in `cajeta.json`).

### Git repository

For private / pre-release deps not yet in a registry:

```json
{
    "vendor.experimental": {
        "git": "https://github.com/vendor/experimental",
        "tag": "v0.3.0",          // or "branch", "rev"
        "subdir": "packages/core"  // optional
    }
}
```

The build tool clones to `.cajeta/cache/git/<hash>/`, finds the
`cajeta.json`, and treats the result as a local package.

### Repository check order

Repositories declared in the manifest are tried in priority order
(descending). The first repository that has the requested
`name@Version` wins. Filesystem repositories with high priority
let developers locally override published versions without
modifying the manifest:

```
$ ls /home/me/cajeta-local/cajeta.io.net.http/1.2.5-dev/
cajeta.io.net.http-1.2.5-dev.cja

$ cajeta build       # uses the local 1.2.5-dev, not the central 1.2.4
```

Once the local override is removed, the next build returns to the
central version.

### Maven-compatible repositories

Existing Maven / Nexus / Artifactory / GitHub Packages / AWS
CodeArtifact / GCP Artifact Registry / Azure Artifacts
repositories follow a stable URL convention
(`<repo>/<group-as-path>/<artifact>/<version>/<artifact>-<version>.<ext>`).
Cajeta can consume them via a compatibility shim:

```jsonc
{
    "name":     "maven-central",
    "type":     "maven-compat",
    "url":      "https://repo.maven.apache.org/maven2",
    "priority": 50
}
```

The shim translates cajeta's `<name>@<version>` requests into
Maven's group-path layout (last `.` segment becomes the artifact
id, preceding segments become the group path), fetches the
`.cja` (Maven Central will only carry cajeta artifacts once
they're published; Maven Central can also serve mirror copies
of cajeta's own central registry).

Practically: any HTTPS-served, path-addressable artifact
repository works without custom code. Cajeta's native HTTP
protocol exists for the richer metadata endpoints (capability
sets, deprecation flags) that Maven Central can't serve.

### Local download cache

Every artifact fetched from a remote repository lands in the
shared local cache at `.cajeta/cache/artifacts/<sha256>.cja`,
keyed by content hash. Repeated builds across projects on the
same machine reuse cached artifacts without re-fetching. The
cache survives `cajeta clean`; only `cajeta clean --deep`
clears it.

Cache layout:

```
.cajeta/cache/
├── artifacts/                  # fetched .cja archives, by content hash
│   └── <sha256>.cja
├── git/                        # cloned Git repositories
│   └── <hash>/
├── ir/                         # incremental build IR cache (see "Incremental builds")
│   └── <discriminator>.bc
└── metadata/                   # cached versions.json / manifest.json responses
    └── <repo-name>/<package>/...
```

A workstation-wide cache directory (`~/.cajeta/cache/`) is also
consulted when the project-local cache misses, before falling
back to the network. Useful for cold-clone CI runs that pre-warm
the workstation cache.

### Repository protocol — v2 enhancements (deferred)

The v1 protocol above (one GET per artifact) is what cajeta's
own central registry ships first. It's intentionally Maven-Central-
shaped so existing CDN infrastructure, proxies, and mirroring tools
work unchanged.

v2 adds opt-in endpoints that fix Maven Central's two structural
pain points: piecemeal transfer (50 deps = 50+ round trips) and
URL-addressed bytes (path collisions, CDN-invalidation thrash,
weak trust roots). Servers advertise v2 support via:

```
GET /.well-known/cajeta-capabilities.json
→ {
    "protocol-versions":  ["v1", "v2"],
    "bundle":             true,
    "content-addressed":  true,
    "transparency-log":   "https://log.cajeta.org/v1",
    "mirrors":            [
      { "url": "https://mirror.eu.cajeta.org", "region": "eu-west" },
      { "url": "https://mirror.us.cajeta.org", "region": "us-east" }
    ]
  }
```

When a v2-capable server is reached, the client prefers v2 paths.
v1 stays available as the fallback / compatibility surface.

#### Bundle endpoint

One round trip for an arbitrary-sized dep graph, with client-cache
awareness so already-fetched bytes don't move:

```
POST /v2/bundle
Content-Type: application/cajeta-bundle-request+json

{
  "have":       ["sha256:abc...", "sha256:def..."],
  "want":       [
    { "name": "cajeta.io.net.http", "version": "1.2.5" },
    { "name": "cajeta.lang",        "version": "1.0.7" }
  ],
  "transitive": true,
  "format":     "tar.zst"
}

→ Content-Type: application/cajeta-bundle
  Streamed tar.zst whose entries are one .cja per requested artifact
  that isn't in `have`, plus a `bundle.json` index at the archive
  root listing what's inside (name, version, sha256) and what was
  omitted (already-cached + reason).
```

Properties:
- Single request handles the entire resolved graph (with
  `transitive: true` the server walks the graph server-side).
- `have` lets the server omit bytes the client already has. A
  build that bumped one dep transfers only that dep + any new
  transitives.
- Streamed response — the server starts writing tar.zst entries
  as it reads them from storage; the client can start unpacking
  before the transfer finishes.
- Idempotent on `(have-set ∪ want-set)`, so common request shapes
  are CDN-cacheable when the request body hashes into a key.

#### Content-addressed storage

v1 is path-addressed (`<name>/<version>/<name>-<version>.cja`),
inheriting Maven Central's cache-invalidation problem: a republish
collides at the same URL.

v2 puts bytes under their sha256, with a small metadata indirection
on top:

```
GET /v2/resolve?name=cajeta.lang&version=1.0.7
→ {
    "sha256":         "sha256:...",
    "size":           1234567,
    "deps":           [ { "name": "...", "version": "..." } ],
    "capabilities":   ["network", "filesystem"],
    "published-at":   "2026-05-15T...",
    "retracted":      false,
    "retracted-reason": null
  }

GET /v2/blob/<sha256>
→ application/cja  (the bytes)
```

Consequences:
- The blob's URL never changes once published. CDNs cache it
  forever; invalidation is no longer a concept.
- Mirrors, Bazel-style remote caches, and corporate proxies key
  on the same sha256 the workstation cache already uses
  (`.cajeta/cache/artifacts/<sha256>.cja`) — no translation layer.
- Retractions stay byte-addressable: the bytes remain reachable
  (downstream builds with the old sha256 in their lockfile keep
  working) but the metadata flips `retracted: true` so new
  resolves emit a warning.

#### Pre-computed common bundles

For widely-used dep sets (the stdlib, popular frameworks) the
registry pre-computes bundles and serves them via plain GETs
that the CDN caches:

```
GET /v2/bundle/well-known/cajeta-stdlib-1.0.7.tar.zst
GET /v2/bundle/well-known/cajeta-platform-2024.1.0.tar.zst
```

Common combinations bypass the bundle compute step entirely. The
keys are derived from a melt's identity, making melts the natural
unit of pre-computation.

#### Differential lockfile fetch

Lockfiles encode the full resolved graph; two builds N days apart
have lockfiles that differ in a small subset. The differential
endpoint takes the old + new lockfile sha256 and returns only the
diff:

```
POST /v2/lockfile-diff
{
  "old-lockfile-sha256": "sha256:...",
  "new-lockfile-sha256": "sha256:..."
}
→ application/cajeta-bundle  (only artifacts in the diff)
```

If the server hasn't snapshotted the old lockfile, the client
falls back to a regular `/v2/bundle` request. Both lockfiles must
be publicly resolvable (i.e., already published or part of a
publish operation); private project lockfiles use the bundle
endpoint with an explicit `have` set instead.

#### Cross-file compression (opt-in)

Individual `.cja` archives are already compressed, so tar.zst of
many of them gets concatenation-level compression only. For
pre-computed common bundles where the CPU cost is paid once, an
opt-in "super-compress" mode decompresses each .cja, concatenates,
and re-compresses as one stream. Cross-file zstd captures shared
symbols / class names / strings — 10-30% smaller in practice on
mono-language ecosystems.

```
POST /v2/bundle
{ ..., "format": "supercompress.zst" }
```

Server-CPU-expensive, so it's gated to pre-computed bundles or
explicit request. The recipient inverts (decompress whole stream,
split, re-compress each .cja to land in the workstation cache).

#### Transparency log

Maven Central trusts per-publisher PGP keys; key compromise
allows undetectable malicious publishes. v2 adds a sigstore-style
append-only log: every publication appends a record that clients
can independently verify.

```
GET /v2/transparency-log/<sha256>
→ {
    "log-index":     12345,
    "log-timestamp": "2026-05-15T...",
    "log-signature": "...",
    "key-id":        "...",
    "issuer":        "github.com/owner/repo"
  }
```

A malicious key works exactly once before the log entry is public,
after which the publisher (or community) can revoke and
investigate. The signing-launcher (see "Signing & launching")
already verifies per-artifact signatures; the transparency check
slots in alongside.

#### Mirror federation

Registries advertise mirrors in `/.well-known/cajeta-capabilities.json`
(see above). The client probes mirror latency and prefers the
closest, falling back to the primary on miss. Companies can host
regional mirrors without ad-hoc HTTP proxy configuration.

#### Namespace verification at publish

Maven Central's namespace squatting (any unclaimed `groupId` was
free for the taking) cost the ecosystem years of cleanup. v2
requires DNS-proof or GitHub-proof of group ownership at publish
time:

- `com.example.foo` — TXT record at `_cajeta-publish.example.com`
  containing the publisher's verification token, OR
- `.github/cajeta-publish.txt` in `github.com/example`'s repo
  root, OR
- For non-DNS namespaces (e.g. `gh.<user>.<project>` for personal
  packages), GitHub OIDC at publish time.

Verified once per publisher; the registry then trusts that
publisher for the entire group prefix.

#### Data-rate impact

For a project with 50 deps, average `.cja` size 200 KB, total
payload ~10 MB, 50 ms RTT, 50 Mbps downlink:

| Approach                                       | Round trips | Wall time |
|------------------------------------------------|-------------|-----------|
| v1 — one GET per artifact (Maven Central shape)| 100+        | ~12s (latency-bound) |
| v1 — same, HTTP/2 multiplexed                  | 1 conn      | ~3s |
| v2 `/bundle` cold (no client cache)            | 1 + resolve | ~2s (bandwidth-bound) |
| v2 `/bundle` warm (one dep bumped)             | 1           | ~0.1s |
| v2 pre-built well-known bundle                 | 1, CDN hit  | ~0.5s |

The warm case is where bundle + content-addressing pay off most:
incremental builds today re-validate every artifact in the graph
even when the answer is "nothing changed."

#### Roadmap

v2 ships as a separate, post-v1 phase. v1 is what initial release
needs; v2 lands once there's enough traffic to justify the
registry-side compute and storage for bundle pre-computation.
Backward compatibility is permanent — a v1-only client and a
v2-only client both keep working against a server that advertises
both.

---

## Capability system

Every package declares the set of capabilities it requires. The
compiler statically verifies the declaration against the
actually-used set. Mismatch is a build error.

### The canonical capability list (v1)

| Capability         | What it gates                                                                          |
|--------------------|---------------------------------------------------------------------------------------|
| `network`          | TCP / UDP sockets, DNS, all of `cajeta.io.net` and `cajeta.io.net.http`.                        |
| `filesystem`       | File I/O — read + write at any path the OS lets the process touch.                    |
| `process`          | Spawning subprocesses, sending signals.                                                |
| `env`              | Reading environment variables, command-line args.                                      |
| `clock`            | Wall-clock time (`System.currentTimeMillis()`, `Instant.now()`).                       |
| `random`           | OS entropy sources (`SecureRandom`, `/dev/urandom`).                                    |
| `threading`        | Spawning OS threads (`Thread`). Fibers always permitted; this is for `cajeta.thread.Thread`. |
| `native-code`      | `@Native` declarations, linking external C libraries.                                  |
| `reflection`       | `cajeta.reflect.*` — introspection, reflective invocation.                            |
| `unsafe-memory`    | Direct memory manipulation, pointer arithmetic, casts past type guards.               |
| `compute`          | Long-running CPU-bound work (ML libraries declare this so embedding hosts can route appropriately). |
| `accelerator`      | GPU / NPU / specialized hardware access.                                                |
| `display`          | UI / windowing (future, when `cajeta.ui` lands).                                       |
| `audio`            | Audio I/O (future).                                                                    |

Future capabilities get added via spec amendments. Each
capability has a stable name; renaming requires a compatibility
shim (old name aliased to new, deprecation warning).

### How the compiler knows what each API needs

Every capability-gated stdlib API carries an `@capability`
annotation:

```cajeta
package cajeta.io.net;

/**
 * TCP socket. Reaching the network requires the `network`
 * capability.
 *
 * @capability network
 */
public final class Socket {

    @capability("network")
    public static Socket connect(String host, int32 port) { ... }

    @capability("network")
    public int64 read(byte[] dest) { ... }

    @capability("network")
    public int64 write(byte[] data) { ... }
}
```

At codegen time, every call site that resolves to a method with
`@capability(...)` contributes that capability to the current
module's required-set. After whole-program analysis the
required-set is the union over reachable methods.

### Build-time enforcement

The build tool computes the required-set; compares to the
manifest's declared set; enforces:

1. **Required ⊆ Declared.** If the program calls into the network
   but `cajeta.json` doesn't declare `network`, the build fails
   with a citation of the first call site:

   ```
   error: capability `network` used but not declared in cajeta.json
       cajeta.io.net.http.HttpClient.send (declared with @capability("network"))
       called from
       com.example.Main.fetchUrl
         at src/main/cajeta/com/example/Main.cajeta:42
   help: add "network" to the "capabilities" array in cajeta.json
   ```

2. **Declared ⊇ Used** — lint warning if the manifest declares
   capabilities that aren't actually used. Useful for noticing
   when refactoring removed the last network call but the
   declaration lingered.

3. **Justification required** for `native-code` and `unsafe-memory`:

   ```jsonc
   "capabilities": [
       "network",
       "filesystem",
       {
           "name":   "unsafe-memory",
           "reason": "Direct pointer-arithmetic for the ring-buffer carrier loop. See docs/UnsafeMemory.md."
       }
   ]
   ```

   The build tool reads the `reason` and records it in the
   lockfile; reviewers see it during PRs.

### Reflection is a known capability hole

`Class.forName(...).getMethod(...).invoke()` lets a program reach
any capability transitively. So `reflection` is effectively a
trust transfer: declaring `reflection` implicitly admits the full
capability set at deploy enforcement. The build emits a
prominent warning:

```
note: package declares `reflection` capability.
  Once reflection is granted, deploy-time sandboxing cannot
  meaningfully restrict the program's capability set — runtime
  reflective dispatch can reach any cajeta API.
  This is by design; documented for awareness.
```

Most programs don't need reflection. The ones that do (DI
containers, serializers, ORMs) declare it openly.

### Interface method capability contracts

An interface declares the **union** of its possible
implementations' capabilities:

```cajeta
package cajeta.io;

public interface Sink {
    /**
     * @capability network filesystem
     */
    void write(byte[] data);
}
```

Callers see the union: `Sink.write()` requires both `network`
and `filesystem` (the worst case across implementations). A
narrower form — `@capability-polymorphic` — would let the
caller pay only for the implementation actually used at runtime,
with a runtime check rejecting impls whose caps exceed; deferred
to a v2 spec amendment.

### Deploy-time enforcement (optional)

The runtime can apply OS-level sandboxing based on the declared
capability set:

- Linux: `seccomp-bpf` filter blocking syscalls outside the
  declared set (`socket()` blocked unless `network`,
  `open(O_RDONLY|O_WRONLY)` blocked unless `filesystem`, etc.)
- macOS: process entitlements profile
- Windows: AppContainer policy

Enabled via `cajeta build --enforce-capabilities=strict`. Default
is `--enforce-capabilities=record-only` — the build tool records
the declared set in the binary's `.cajeta-manifest` section but
doesn't activate any OS-level enforcement; a deployment tool
(systemd unit, container runtime, k8s pod spec) can read the
section and apply policy externally.

---

## Build flavors

A **flavor** is a named bundle of compiler properties. Two
built-ins ship with the toolchain; everything else is either
inline composition or a project-defined custom flavor.

### Built-in flavors

| Flavor    | Property bundle |
|-----------|---------------------------------------------------------------------------------------|
| `release` | `opt=O2 lto=thin strip-symbols=on debug-info=line bounds-check=off`                   |
| `debug`   | `opt=O0 lto=off  strip-symbols=off debug-info=full bounds-check=on`                   |

That's it for built-ins. The rest of what older toolchains call
flavors (`fast`, `minimal`, `instrumented`, `debug-release`, …)
becomes either an inline composition or a custom flavor the
project names — both shown below.

### Property vocabulary

The compiler's property names ARE the vocabulary; no shorthand
macro layer above them. Unknown property keys are a hard error
at manifest-load (catches typos).

| Property | Values |
|---|---|
| `opt`               | `"O0"` / `"O1"` / `"O2"` / `"O3"` / `"Oz"` |
| `lto`               | `"off"` / `"thin"` / `"full"` |
| `debug-info`        | `"off"` / `"line"` / `"full"` |
| `strip-symbols`     | `true` / `false` |
| `bounds-check`      | `"on"` / `"off"` / `"trap"` |
| `null-checks`       | `"on"` / `"off"` / `"trap"` |
| `overflow-checks`   | `"on"` / `"off"` / `"wrapping"` |
| `asan` / `tsan` / `msan` / `ubsan` | `true` / `false` |
| `analytics`         | `true` / `false` (async-profiler hooks) |
| `source-tags`       | `true` / `false` (carry source positions on the drop chain) |

### Two accepted forms — string or map

Wherever a flavor value appears (task action parameter or
custom-flavor definition), it's either a string (bare flavor
name) or a map (composition).

**String — bare named flavor:**

```jsonc
"flavor": "release"
"flavor": "debug"
"flavor": "integration"          // a custom flavor (declared elsewhere in the manifest)
```

**Map — composition:**

```jsonc
"flavor": {
    "base":          "release",      // required when overriding; names the starting flavor
    "debug-info":    "full",
    "strip-symbols": false,
    "analytics":     true
}
```

Semantics:

- **`base`** names the flavor whose property bundle is the
  starting point. Required field when overrides are present.
  Itself may be a built-in or a custom flavor.
- Every other key is a property override; takes precedence over
  the base's value for that property.
- Unknown property keys are a hard error at manifest-load time.
- A custom flavor whose `base` cycles transitively through itself
  is a hard error.

### Custom flavors

A project that uses the same composition repeatedly names it in
`settings.build.custom-flavors`:

```jsonc
"settings": {
    "build": {
        "custom-flavors": {
            "integration": {
                "base":       "release",
                "debug-info": "full",
                "analytics":  true
            },
            "release-debug": {
                "base":       "release",
                "debug-info": "full"
            },
            "asan-debug": {
                "base": "debug",
                "asan": true
            },
            "perf": {
                "base":       "release",
                "debug-info": "full",
                "analytics":  true,
                "lto":        "full"
            }
        }
    }
}
```

Then tasks reference by name:

```jsonc
{ "action": "build", "flavor": "integration", "profile": "integration" }
{ "action": "build", "flavor": "asan-debug",  "profile": "dev" }
```

### Worked examples

**One-off composition inline** — useful when investigating a
release-only bug:

```jsonc
{ "action":  "build",
  "flavor":  { "base": "release", "debug-info": "full" },
  "profile": "release",
  "id":      "art" }
```

**Named composition referenced from a task** — the typical case
when the same composition appears in multiple tasks:

```jsonc
// settings.build.custom-flavors.integration defines the composition
{ "action": "build", "flavor": "integration", "profile": "integration" }
```

### Why map over a list-with-prefix-toggles form

The map shape was chosen over alternatives like
`["release", "+debug-symbols"]` for these reasons:

| | List with `+toggle` shorthand | Map |
|---|---|---|
| Self-documenting at the use site | `+debug-symbols` is opaque | `"debug-info": "full"` is obvious |
| Order semantics | "later overrides earlier" is implicit | None — flat overrides |
| Typo catching | `"+debg-symbols"` would have to be checked against a shorthand vocabulary | Unknown property is a hard error |
| Schema validation / IDE autocomplete | Strings with prefix conventions are hard to schema | Direct JSON schema against the property table |
| Composition with named base | Awkward (`["custom-name", "+toggle"]`) | Natural (`{"base": "custom-name", …}`) |

Map form covers every case the list form could, more rigorously.

---

## Profiles

A **profile** in cajeta is the string the `build` action passes
to the compiler so source-level `@Profile`-annotated components
resolve to the right wiring. Profiles are **per-task literals**
in the manifest — not central state, not a CLI activation flag,
not a sticky workstation default. Different builds (test,
integration, release, …) are different tasks; each task names
its profile in its own `build` action invocation.

```jsonc
"tasks": {
    "test": {
        "actions": [
            { "action":  "build",
              "flavor":  "debug",
              "profile": "test",        // ← per-task literal
              "id":      "art" },
            { "action": "test", "coverage": true }
        ]
    },
    "release": {
        "actions": [
            { "action":  "build",
              "flavor":  "release",
              "profile": "release",     // ← per-task literal
              "id":      "art" },
            { "action": "publish", "repository": "central" }
        ]
    }
}
```

### Flavor and profile are orthogonal

They control different axes of the build, and any combination is
valid:

| Axis | What it controls | Examples |
|---|---|---|
| **`flavor`** | Compiler optimization, debug info, sanitizers — *how* the code is compiled | `release`, `debug`, custom names |
| **`profile`** | Which `@Profile`-gated components get wired into the DI graph — *what* is wired | `dev`, `test`, `integration`, `release`, or any project-defined string |

So `flavor: "debug"` + `profile: "test"` (fast-compile unit
tests with mocked deps) is fine, as is `flavor: "release"` +
`profile: "test"` (verify the optimized binary still passes
the mocked-deps tests). Coverage instrumentation is not a flavor
— it's a separate action from the `cajeta.coverage` plugin.

### Profile names are arbitrary

The compiler accepts any string as a profile name. The
[`@Profile` source annotation](stdlib/AspectModel.md#selecting-a-context-profile)
already says "Profile names are arbitrary strings; there's no
fixed enumeration"; the build-tool side mirrors that. Project
teams pick names that match their actual environments:

```
dev          local development
test         unit tests, mocked dependencies
integration  sandbox / test-account dependencies
staging      pre-production
release      production
canary       limited prod rollout
e2e          end-to-end with real services
perf         performance benchmarks
eu-west / us-east / ap-southeast-2  regional deployments
tenant-acme / tenant-globex          per-tenant configurations
```

The init template ships a few common starter tasks but doesn't
claim the names are canonical — see
[Default `cajeta init` manifest](#default-cajeta-init-manifest).

### Source-level `@Profile`

The compiler gates `@Component` / `@Repository` declarations on
their `@Profile` annotations against the active profile. A
build with `profile: "release"` resolves only profile-neutral
components and components annotated with `@Profile("release")`.

```cajeta
@Component @Profile("dev")         public class LocalStubDatabase  implements Database { ... }
@Component @Profile("test")        public class InMemoryDatabase   implements Database { ... }
@Component @Profile("integration") public class SandboxDatabase    implements Database { ... }
@Component @Profile("release")     public class PostgresDatabase   implements Database { ... }
```

Four implementations of `Database`; each task picks one via the
profile string it passes to the compiler. Same source, four
distinct binaries.

`@TestComponent` continues to handle per-class test overrides
under whichever profile the test task uses (typically `test`);
see [AspectModel.md](stdlib/AspectModel.md#testcomponent).

### Why per-task and not an activated overlay

Earlier drafts of this spec had a `settings.profiles` overlay
block (Maven-style activation that mutates manifest fields when
a profile is "active"). That model went away because:

- **Two concepts collided.** Maven's profile-overlay activation
  and Spring's `@Profile`-gated DI selection happen to share a
  name but solve different problems. Conflating them adds
  precedence rules and surprises ("which active profile is
  driving this resolution?").
- **No central state to track.** With per-task literals, the
  binary's wiring is determined by which task built it. Trace
  any artifact back through `cajeta info` → which task ran →
  which profile literal it passed. No global activation state
  to consult.
- **No precedence chain to learn.** No CLI flag vs env var vs
  sticky file vs manifest-default ordering.
- **Customization is just another task.** Different
  environment? Add a task. Different settings for CI vs dev?
  Different tasks. The task IS the build configuration.

The shape parallels how flavor works: each task picks its
flavor in its own action invocation, not by activating an
"active flavor" globally.

### Overriding the task's profile at the call site

If a developer wants to invoke a task under a different profile
once (e.g., to bisect a profile-specific bug), the task
parameterizes its profile and the developer overrides via the
property mechanism:

```jsonc
"test": {
    "params": {
        "profile": { "type": "string", "default": "test" }
    },
    "actions": [
        { "action": "build",
          "flavor": "debug",
          "profile": "${params.profile}",
          "id":     "art" },
        { "action": "test", "coverage": true }
    ]
}
```

```
cajeta test                              # profile = "test" (the default)
cajeta test -P profile=integration       # profile = "integration" for this invocation only
```

The init template parameterizes the profile this way so the
override path is available without forcing the developer to
edit the manifest.

---

## Incremental builds

`cajeta build` is incremental by default. The unit of cache is a
**per-file IR module** keyed by a content-derived discriminator.
Cache lives under `.cajeta/cache/ir/<discriminator>.bc`.

Compilation flow with cache:

1. For each source file, compute the discriminator:
   ```
   sha256(
       source-file-bytes               ||
       compiler-version                ||
       canonical-flag-set              ||
       transitive-imports-digest
   )
   ```
   The flag-set is canonicalized (sorted, normalized) so flag
   order doesn't bust the cache. The transitive-imports-digest
   is computed recursively — depth-first walk of every type and
   method this file references, hashing each referenced module's
   own discriminator. Cycles are broken by post-order traversal
   with fixed-point on the digest.

2. Look up `.cajeta/cache/ir/<discriminator>.bc`. Cache hit
   means the IR module is byte-identical to what a fresh compile
   would produce — load and emit it directly.

3. Cache miss: compile to IR, write the cached module under the
   computed discriminator, emit it.

4. Link as normal.

This is the generalized form of C++'s precompiled headers:
content-addressed rather than path-addressed, applied to every
source file rather than just headers. Rebuilds across branches
share cache hits as long as the byte content matches; changing
one source file's content only forces a recompile of that file
and its dependents (the dependents detect the change through
the transitive-imports-digest).

### Compared to Gradle's build cache

Gradle's build cache works on task-level inputs / outputs and
keys against the same content-discriminator idea. Cajeta's IR
cache is finer-grained (per source file, not per task) and
narrower (only the IR-emission step; linking is always re-run
since its inputs are the union of every file's IR). The
trade-off: smaller change blast radius (fewer files rebuild
unnecessarily), at the cost of not caching the link step.

### Cache eviction

`.cajeta/cache/ir/` grows monotonically until a configured upper
bound, then evicts LRU. Configure via `cajeta.json`:

```jsonc
"build": {
    "cache": {
        "max-size": "5GiB",       // total cap on cache dir
        "ttl":      "30d"          // evict entries unused for 30+ days
    }
}
```

`cajeta clean` removes `.cajeta/work/` (the incremental build
output dir) but preserves `.cajeta/cache/`. To wipe the cache:
`cajeta clean --deep` (and answer the y/N prompt — fully cold
rebuilds are expensive enough to warrant a confirmation).

### Cross-machine cache sharing

Not in v1. The repository model serves the "share an artifact
across machines" need at the archive grain; sharing IR modules
at the file grain would require a remote-cache protocol Bazel-
style. Deferred — see [Comparative
analysis](#comparative-analysis) for the reasoning.

---

## Workspaces

Multi-package projects share a single manifest at the workspace
root + a per-package manifest in each member directory.

```
my-org/
├── cajeta.json              # workspace manifest
├── cajeta.lock              # one lockfile for the whole workspace
├── packages/
│   ├── api/
│   │   ├── cajeta.json      # member manifest
│   │   └── src/main/cajeta/...
│   ├── core/
│   │   ├── cajeta.json
│   │   └── src/main/cajeta/...
│   └── client/
│       ├── cajeta.json
│       └── src/main/cajeta/...
└── target/
    └── ...
```

Workspace manifest:

```jsonc
{
    "workspace": {
        "members": [
            "packages/api",
            "packages/core",
            "packages/client"
        ],

        // Versions resolved once across the workspace. Conflicts
        // between members' versions are caught at workspace-load
        // time.
        "shared-dependencies": {
            "cajeta.io.net.http": "1.2.*"
        }
    },

    "repositories": [...]
}
```

`cajeta build` from the workspace root builds every member;
`cajeta build -p api` builds just one; cross-package deps are
resolved through the workspace's local-path mechanism so members
see each other's sources directly without round-tripping through
a repository.

---

## Sandboxing and reproducibility

### Build sandbox

`cajeta build` runs each compilation step in a sandbox by default
(opt-out via `--no-sandbox` for debugging):

- **Filesystem** — the compiler sees only the project directory,
  the source tree under `src/main/cajeta`, the dep cache, and
  the build output directory. `/etc`, `/var`, user `$HOME` are
  invisible.
- **Network** — no network access from compilation steps. Build
  is a function of declared inputs.
- **Environment** — a clean environment with only `PATH`, `HOME`
  (a fake), and any `cajeta.json`-declared env vars.

Linux: `bwrap` (bubblewrap) or unprivileged user namespaces.
macOS: `sandbox-exec` profile. Windows: Job objects with
restricted descriptors.

### Reproducible artifacts

Same source + same lockfile + same compiler version → byte-
identical `.cja` output. Build inputs that would otherwise
contribute non-determinism:

- **Build timestamps** — pinned via `SOURCE_DATE_EPOCH` environment
  variable (the Reproducible Builds standard). `cajeta.json` can
  set a project default; CI invokes with whatever is
  appropriate.
- **Path-dependent symbols** — debug info strips absolute paths;
  the compiler's `-fdebug-prefix-map` equivalent rewrites paths
  to repository-relative form.
- **Random seeds** — any seed used during compilation reads from
  a deterministic source seeded by the source content's hash,
  not from OS entropy.

### Build attestation

`cajeta publish` generates a SLSA-level provenance record:

```json
{
    "_type": "https://in-toto.io/Statement/v1",
    "subject": [
        {
            "name": "cajeta.io.net.http-1.2.4.cja",
            "digest": { "sha256": "..." }
        }
    ],
    "predicateType": "https://slsa.dev/provenance/v1",
    "predicate": {
        "buildDefinition": {
            "buildType": "https://cajeta.org/build/v1",
            "externalParameters": {
                "manifest-checksum": "sha256:...",
                "lockfile-checksum": "sha256:..."
            },
            "internalParameters": {
                "compiler-version": "1.0.0",
                "flavor": "release",
                "target": "x86_64-linux-gnu"
            }
        },
        "runDetails": {
            "builder": { "id": "https://github.com/cajeta-org/builder" },
            "metadata": {
                "startedOn": "2026-05-15T...",
                "finishedOn": "2026-05-15T..."
            }
        }
    }
}
```

Optionally signed via cosign / sigstore. Consumers verify the
signature before installing.

---

## Action catalog

Actions are the verbs that tasks invoke. Every action is either
**native** (built into the build tool) or **plugin-provided**
(supplied by a plugin declared in the manifest). User-defined
**action presets** (in the manifest's `actions` block) are
partial bindings or compositions of these underlying actions —
they don't add new verbs, they wrap existing ones.

This section is the catalog of native actions. Plugin-provided
actions are documented by each plugin; see [Plugins](#plugins)
for the discovery/sandboxing model.

### Native action catalog (v1)

Action contracts use `required → optional → outputs` columns.
**Required** params must be supplied at the call site (or bound
in an `actions`-block preset). **Optional** have sensible
defaults. **Outputs** are the named values an action publishes
for downstream actions to consume via
[pipelining](#action-pipelining).

#### Compile / test / quality

| Action     | Required                          | Optional                                                              | Outputs                                                |
|------------|-----------------------------------|-----------------------------------------------------------------------|--------------------------------------------------------|
| `build`    | —                                 | `flavor`, `target`, `modules`, `incremental` (default `true`), `profile`, `entry-method`, `binary`, `output-path`, `emit` (`exploded-ir` / `archived-ir` / `executable`) | `path`, `sha256`, `size`, `format`            |
| `clean`    | —                                 | `paths` (default: `build/` + `.cajeta/work/`), `deep` (also wipes cache) | —                                                    |
| `test`     | —                                 | `filter`, `parallel`, `coverage`, `report`                              | `passed`, `failed`, `crashed`, `report-path`         |
| `lint`     | —                                 | `plugins` (subset), `fail-on-severity`, `format`, `output`              | `findings`, `report-path`                            |
| `doc`      | —                                 | `output` (default: `build/docs/`)                                       | `output-path`                                        |
| `fmt`      | —                                 | `check-only`                                                            | `changed-count`                                      |

##### The `build` action in detail

`build` produces one of three first-class compile artifacts,
selected by `emit`:

| `emit` value | What it is | Default output location |
|---|---|---|
| `exploded-ir`  | Per-source `.bc` (LLVM bitcode) tree | `build/ir/<package>/<class>.bc` |
| `archived-ir`  | Single `.cja` archive (IR + manifest + resources) | `build/archive/<details.name>-<version>.cja` |
| `executable`   | Fully-linked native binary; requires an entry method | `build/exe/<details.name>` (`.exe` on Windows) |

Default `emit` selection (when not explicit):

- An entry method is configured (`entry-method` param, `binary`
  resolved against `settings.build.binaries`, or a fallback to
  `settings.build.entry-method`) → default is `executable`.
- No entry method configured → default is `archived-ir`
  (library).

`exploded-ir` is always explicit — it's a tooling/inspection
output, not a "deliverable."

**Entry-method resolution (when emit is `executable` or an
executable `archived-ir`):**

1. `entry-method` param on the action — direct override
   (e.g. `"entry-method": "com.example.cli.Main::main"`).
2. `binary` param on the action — name lookup in
   `settings.build.binaries`
   (e.g. `"binary": "cli"`).
3. `settings.build.entry-method` — the manifest default.
4. None of the above + `emit: "executable"` → hard error
   ("can't link an executable without a main"). With
   `emit: "archived-ir"` and no entry method, the archive is a
   library.

**Multiple binaries from one source tree** — use
`settings.build.binaries` to register named binaries with their
entry methods; tasks reference by name:

```jsonc
"settings": {
    "build": {
        "binaries": {
            "server":  { "entry-method": "com.example.api.server.Main::main",
                         "description":  "Production HTTP server" },
            "migrate": { "entry-method": "com.example.api.migrate.Main::main",
                         "description":  "Database migration runner" },
            "diag":    { "entry-method": "com.example.api.diag.Main::main",
                         "description":  "On-call diagnostic tool" }
        }
    }
}
```

```jsonc
{ "action": "build", "binary": "server",  "profile": "release", "id": "art" }
{ "action": "build", "binary": "migrate", "profile": "release", "id": "mig" }
```

Three binaries from one source tree, three independent `.cja`
or native exe outputs.

**Outputs by `emit` type:**

| `emit` | `path` points at | `format` value |
|---|---|---|
| `exploded-ir`  | Directory containing the IR tree | `"exploded-ir"` |
| `archived-ir`  | The `.cja` file | `"archived-ir"` |
| `executable`   | The native executable | `"elf"` / `"macho"` / `"pe"` |

For `archived-ir`, the entry-method (if any) is recorded as
metadata on the archive's manifest section; consumers (the
launcher, `cajeta run`) read it from there.

#### Filesystem

| Action     | Required                     | Optional                              | Outputs                       |
|------------|------------------------------|---------------------------------------|-------------------------------|
| `copy`     | `from`, `to`                 | `also` (additional file), `mkdir` (default `true`), `recursive` | `destinations` (string array) |
| `delete`   | `paths` (string or array)    | `if-exists` (default `true`)           | —                             |
| `mkdir`    | `path`                       | `recursive` (default `true`)           | —                             |

#### Artifact + crypto

| Action       | Required                                            | Optional                            | Outputs              |
|--------------|-----------------------------------------------------|-------------------------------------|----------------------|
| `sign`       | `input`, (`key-env` or `key-path`), `key-id`        | —                                   | `path` (.sig file), `sha256` |
| `verify-sig` | `input`, (`pubkey-env` or `pubkey-path`)            | `sig` (default `<input>.sig`)        | `valid` (bool)       |
| `version`    | (one of) `bump: major\|minor\|patch`, `set: <semver>` | `write-to` (default `cajeta.json`) | `version`, `previous` |

#### Packaging

`package` is the universal "transform a `build` output into a
different format" verb. The `build` action produces one of three
first-class compile artifacts (exploded-ir, archived-ir,
executable); `package` repackages those into formats consumers
or distribution systems expect. Format catalog organized by
input type:

##### Inputs: IR (`exploded-ir` or `archived-ir`)

| `format` | Output |
|---|---|
| `obj-tree`   | Per-source `.o` (native object) tree |
| `uber-ir`    | Single linked `.bc` (LLVM bitcode), all source IR merged |
| `static-lib` | Native `.a` archive |
| `shared-lib` | Native `.so` (Linux) / `.dylib` (macOS) / `.dll` (Windows) |

##### Input: `archived-ir` (.cja)

| `format` | Output |
|---|---|
| `uber-archive` | Single `.cja` carrying this project AND all resolved transitive deps |

##### Inputs: `executable` (native binary) — the installer family

| `format` | Output | Platform |
|---|---|---|
| `tarball`     | `.tar.zst` (or `.tar.gz` if requested) | any |
| `zip`         | `.zip`                                  | any |
| `deb`         | Debian `.deb` package                   | Linux (Debian-family) |
| `rpm`         | RPM `.rpm` package                      | Linux (RHEL-family) |
| `msi`         | Windows `.msi` installer                | Windows |
| `app-bundle`  | macOS `.app` bundle                     | macOS |
| `pkg`         | macOS `.pkg` installer                  | macOS |
| `dmg`         | macOS `.dmg` disk image                 | macOS |
| `appimage`    | Linux `.AppImage`                        | Linux |
| `flatpak`     | Linux `.flatpak`                         | Linux |
| `snap`        | Linux `.snap`                            | Linux |
| `container`   | OCI container image                      | cross-platform |

##### v1 scope vs. deferred

Many installer formats require platform-specific tooling
(`dpkg-deb`, `rpmbuild`, `candle/light` for MSI,
`productbuild` for `pkg`, etc.). v1 ships:

- **Always (v1):** `obj-tree`, `uber-ir`, `uber-archive`,
  `static-lib`, `shared-lib`, `tarball`, `zip`, `container`.
- **Deferred to v1.x:** `deb`, `rpm`, `app-bundle`, `pkg`,
  `dmg`, `msi`.
- **Post-v1:** `appimage`, `flatpak`, `snap`, mobile formats.

Each deferred format is a self-contained slice; landing one
doesn't gate any other phase.

##### `package` contract

| Action    | Required        | Optional                                                       | Outputs |
|-----------|-----------------|----------------------------------------------------------------|---------|
| `package` | `input`, `format` | `spec` (metadata file), `output-path`, plus format-specific params | `path`, `format`, `sha256`, `size`, (`manifest`, `registry-url` for `container`) |

Metadata for installer formats supplied two ways: inline params,
or a spec file referenced via `spec`. Use whichever reads better
at the call site.

**Inline metadata example (`deb`):**

```jsonc
{ "action": "package",
  "input":  "${exe.path}",
  "format": "deb",
  "package-name": "my-service",
  "section":      "utils",
  "maintainer":   "Alice <alice@example.com>",
  "description":  "Order processing service",
  "depends":      ["libc6", "libssl3"] }
```

**Spec-file metadata example:**

```jsonc
{ "action": "package",
  "input":  "${exe.path}",
  "format": "deb",
  "spec":   "packaging/debian.json" }
```

**Container example:**

```jsonc
{ "action": "package",
  "input":  "${exe.path}",
  "format": "container",
  "base":   "debian:bookworm-slim",
  "tag":    "my-org/my-service:${details.version}",
  "expose": [8080],
  "env":    { "LOG_LEVEL": "info" },
  "labels": { "git-commit": "${env.GITHUB_SHA}" } }
```

##### Input/format validation

Validation runs at manifest-load when possible: `format:
"obj-tree"` requires an IR input (`exploded-ir` directory or
`archived-ir` `.cja`); `format: "deb"` requires an `executable`
input. Mismatch is a hard error with a citation naming the
expected vs. actual input shape.

##### Worked pipeline — full release flow

```jsonc
"release": {
    "depends-on": ["test", "integration"],
    "actions": [
        // 1. Build the native binary.
        { "action": "build", "binary": "server",
          "emit":   "executable",
          "profile": "release", "id": "exe" },

        // 2. Sign it.
        { "action": "sign", "input": "${exe.path}",
          "key-env": "CAJETA_RELEASE_KEY",
          "key-id":  "${details.name}",
          "id":      "sig" },

        // 3. Repackage for the platforms we ship to (in parallel).
        { "parallel": [
            { "action": "package", "input": "${exe.path}", "format": "deb",
              "spec": "packaging/debian.json", "id": "deb-pkg" },
            { "action": "package", "input": "${exe.path}", "format": "container",
              "base":  "debian:bookworm-slim",
              "tag":   "my-org/server:${details.version}",
              "id":    "img" },
            { "action": "package", "input": "${exe.path}", "format": "tarball",
              "id":    "tgz" }
        ]},

        // 4. Upload each artifact to its destination.
        { "parallel": [
            { "action": "upload", "target": "s3",
              "input":  "${deb-pkg.path}", "bucket": "my-org-releases" },
            { "action": "upload", "target": "http",
              "input":  "${img.path}",
              "url":    "https://registry.internal/v2/server/manifests/${details.version}",
              "method": "PUT" },
            { "action": "upload", "target": "s3",
              "input":  "${tgz.path}", "bucket": "my-org-releases" }
        ]}
    ]
}
```

Same `package` action wearing three different format hats, all
consuming the one executable the build produced.

#### Distribution

A single `upload` action covers every transport via the `target`
discriminator. The protocol-specific params are below.

| Action       | `target`        | Required                                          | Optional                                              | Outputs                  |
|--------------|-----------------|---------------------------------------------------|-------------------------------------------------------|--------------------------|
| `upload`     | `s3`            | `input`, `bucket`, `prefix`                       | `region`, `endpoint` (S3-compatible non-AWS), `acl`, `also` (array of extra paths), `auth` (default: `AWS_*` env) | `url`, `urls` (array if `also`) |
| `upload`     | `azure`         | `input`, `container`, `prefix`                    | `account` (overrides env), `also`, `auth` (default: `AZURE_*` env) | `url`, `urls`            |
| `upload`     | `gcs`           | `input`, `bucket`, `prefix`                       | `also`, `auth` (default: `GOOGLE_APPLICATION_CREDENTIALS`)         | `url`, `urls`            |
| `upload`     | `http`          | `input`, `url`                                    | `method` (default `PUT`), `auth`, `headers`, `also`    | `url`, `status`          |
| `upload`     | `sftp`          | `input`, `host`, `path`                           | `port` (default 22), `user`, `auth` (key-env, key-path, or password-env), `also` | `url`                    |
| `download`   | (n/a)           | `url`, `to`                                       | `sha256` (verifies on receive), `auth`                | `path`, `sha256`         |
| `publish`    | (n/a)           | `repository` (name from manifest)                 | `version` (default: `details.version`)                | `url`                    |

The `also` array contains extra paths to upload alongside the
primary input — typically the `.sig` produced by a preceding
`sign` action. Each gets uploaded under the same `prefix` /
`path`; outputs include URLs for every file (the primary's URL
is in `url`, the full set in `urls`).

#### State / profile

| Action        | Required             | Optional                | Outputs           |
|---------------|----------------------|-------------------------|-------------------|
| `set-profile` | `name`               | —                       | `previous`        |

#### Composition / control

| Action       | Required                       | Optional             | Outputs           |
|--------------|--------------------------------|----------------------|-------------------|
| `run-task`   | `name`                         | `params`             | (the called task's `outputs` block) |
| `parallel`   | (the wrapped sub-array, see [Tasks](#manifest--cajetajson)) | — | —                 |
| `exec`       | `command`, `args` (array)      | `working-dir`, `env`, `timeout` | `stdout`, `stderr`, `exit-code`   |

### Variable substitution in action params

Every string-typed action parameter is subject to
[`${PROPERTY}`](#properties) substitution. The substitution
context for an action includes:

- All manifest-level properties (from `properties` block).
- Built-ins (`${details.name}`, `${details.version}`,
  `${flavor}`, `${env.NAME}`, etc.).
- Task parameters (`${params.<name>}`).
- Outputs from prior actions in the same task
  (`${<id>.<output>}`) — see [Action pipelining](#action-pipelining).

Missing property is a hard error citing the action.

### `exec` — the escape hatch

```jsonc
{ "action":  "exec",
  "command": "scripts/notify-slack.sh",
  "args":    ["Release ${params.version} shipped"],
  "env":     { "WEBHOOK_URL": "${env.SLACK_WEBHOOK}" },
  "timeout": "30s" }
```

The build tool spawns the command, captures stdout/stderr,
honors the timeout, sandboxes the subprocess according to the
task's declared capabilities. Non-zero exit aborts the task
(unless `--continue-on-error` is set). Outputs (`stdout`,
`stderr`, `exit-code`) are captured for downstream actions.

Use sparingly — every declarative alternative gets sandboxing,
retry, structured logging, parallelization, and cross-platform
behavior for free; `exec` opts out of all of that.

### Cache and idempotence

Actions declare their inputs (param values + referenced files)
and outputs. The build tool computes a cache discriminator per
action invocation; if the discriminator matches a prior
invocation's recorded outputs, the action is skipped and its
prior outputs are reused.

Cacheable by default: `build`, `copy`, `sign`, `mkdir`, `fmt`,
`doc`, `lint`. Re-run by default (network or side-effecting):
`upload`, `publish`, `download`, `exec`, `delete`, `version`,
`set-profile`. Each action's caching policy is part of its
contract (the open spec `action-catalog-v1.json` documents this
precisely).

Override:
- `--force` — skip the cache entirely for this run.
- `--force-action=<name>` — force one named action.
- `--no-cache` — disable caching for this run (don't read, but
  do write).

### Why declarative rather than shell

`exec` is the escape hatch. The reasons declarative actions are
preferred for everything else:

| Concern                    | `exec` shell | Declarative action       |
|----------------------------|--------------|--------------------------|
| Sandbox                    | task-level   | per-action               |
| Credential injection       | env-only     | first-class `*-env` fields |
| Retry on transient failure | manual       | built-in (exponential backoff) |
| Parallelization            | no auto      | dep-graph driven         |
| CI log integration         | grep         | structured, per-action   |
| Cross-platform             | shell-dependent | uniform                  |
| Cache participation        | no           | yes (where applicable)   |

---

## Action pipelining

The chain that connects actions is: an action **declares
outputs**, a downstream action **consumes them as inputs** via
`${id.field}` substitution. The build tool checks the connection
at manifest-load time — if an action references `${art.path}`
but no preceding action assigned `id: "art"` to a result
publishing `path`, the manifest fails to load with a citation.

### The threading model

Three pieces:

**1. `id` on the producer.** An action can take an optional
`id` field naming its result. The id is task-scoped — each task
has its own namespace.

```jsonc
{ "action": "build", "flavor": "release", "id": "art" }
```

**2. `${id.<field>}` reference at the consumer.** Any string
parameter on a later action can read fields off the producer's
output object.

```jsonc
{ "action": "sign", "input": "${art.path}", "key-id": "..." }
```

**3. Action contracts.** Each action's `Outputs` column in the
catalog defines the field names it produces. `build` publishes
`path`, `sha256`, `size`; `sign` publishes `path`, `sha256`;
`upload` publishes `url`, possibly `urls` and `status`. The
catalog is the contract; consumers know which fields exist
without reading the producer's implementation.

### Worked example: package → sign → upload

```jsonc
"tasks": {
    "release": {
        "actions": [
            // 1. build — produces ${art.path}, ${art.sha256}, ${art.size}
            { "action": "build",
              "flavor": "release",
              "id":     "art" },

            // 2. sign — consumes ${art.path}; produces ${sig.path}
            { "action":  "sign",
              "input":   "${art.path}",
              "key-env": "CAJETA_RELEASE_KEY",
              "key-id":  "${details.name}",
              "id":      "sig" },

            // 3. upload (s3) — consumes both; produces ${pub.url}, ${pub.urls}
            { "action":  "upload",
              "target":  "s3",
              "input":   "${art.path}",
              "also":    ["${sig.path}"],
              "bucket":  "${env.RELEASE_BUCKET}",
              "prefix":  "cajeta/${details.name}/${details.version}/",
              "id":      "pub" },

            // 4. exec — broadcast the resulting URL
            { "action":  "exec",
              "command": "scripts/notify",
              "args":    ["Released to ${pub.url}"] }
        ]
    }
}
```

The graph is mechanically derivable from the references:
`art → sign`, `art → upload`, `sig → upload`, `upload → exec`.
Independent branches run in parallel where the action's
parallelism policy allows; the build tool generates the
parallel schedule automatically.

### Sftp variant of the upload

Same `upload` action, different `target`:

```jsonc
{ "action":  "upload",
  "target":  "sftp",
  "input":   "${art.path}",
  "also":    ["${sig.path}"],
  "host":    "uploads.internal",
  "path":    "/srv/releases/cajeta/${details.name}/${details.version}/",
  "auth":    { "type": "key", "key-env": "SFTP_KEY_PEM" },
  "id":      "pub" }
```

The outputs are the same shape (`url`, `urls`) so downstream
actions don't care which transport was used. This is the model
for any future upload target — adding `target: "ftps"` or
`target: "swift"` extends the catalog without changing the
pipeline shape.

### HTTP PUT vs POST

The `http` target distinguishes via `method`:

```jsonc
{ "action":  "upload",
  "target":  "http",
  "input":   "${art.path}",
  "url":     "https://artifacts.internal/v1/releases/${details.name}/${details.version}",
  "method":  "PUT",       // or "POST" for endpoints that expect form-style upload
  "auth":    { "type": "bearer", "token-env": "ARTIFACT_TOKEN" },
  "headers": { "X-Source": "cajeta-build", "X-Version": "${details.version}" },
  "id":      "pub" }
```

`POST` with a body of type `multipart/form-data` is supported
by setting `method: "POST"` plus `form-field` (the field name
to use for the file part).

### Output objects across `parallel` and `run-task`

- **Parallel groups** publish each child's outputs under the
  child's `id`. The order in the parallel sub-array doesn't
  matter for visibility; what matters is that an action
  referencing `${id.field}` comes after (in the task's
  flattened ordering) the action that produced it.
- **`run-task`** publishes the called task's `outputs` block
  under the invocation's `id`. The calling task sees them as
  `${invocationId.fieldName}` like any other action.

### Cycles and dangling references

A reference to an undefined `id` is a hard error at manifest-
load time, with a citation of the action that referenced it
and (if any close-named id exists) a fix-it hint.

A cycle in the implicit graph (action A's input references B's
output and B's input references A's output) is a hard error.
The build tool detects cycles before any action runs.

### Why this matters

This is the design's load-bearing piece: a task is not a free-
form list of commands but a structured graph of producers and
consumers. The references through `${id.field}` make the graph
explicit; the action catalog makes the graph type-checked. Pre-
build validation catches "the sign action expected
`${art.path}` but no `build` action with id `art` exists" — the
class of error that's intermittent runtime failures in shell
scripts becomes a load-time error here.

---

## Default `cajeta init` manifest

`cajeta init <name>` scaffolds a new project. It writes a
`cajeta.json` with a starter `tasks` block, a starter `actions`
block (one preset — a `ship` composition), and reasonable
defaults under `settings`. **Every entry is editable.** Delete
what you don't need; rewrite what doesn't fit; add what's
missing. The build tool ships no opinion about which tasks a
project should have — `cajeta init` provides sensible
starters, the project owns them from that point on.

### Profile and flavor: per-task literals

The starter tasks demonstrate the per-task pattern: each build
task hard-codes both its **flavor** (compile/optimize knobs)
and its **profile** (`@Profile`-driven DI wiring) in its
`build` action. The starter profile names below — `dev`,
`test`, `integration`, `release` — are common conventions, not
a canonical set. **Rename them, drop them, or add others** to
match your project's environments (`staging`, `canary`, `e2e`,
`perf`, `eu-west`, `tenant-acme`, anything).

```jsonc
{
    "details": {
        "name":               "<name>",
        "version":            "0.1.0",
        "description":        "<description>",
        "license":            "Apache-2.0",
        "authors":            [],
        "cajeta-lang-version": "1.0"
    },

    "properties": {
        // Add reusable values here.
    },

    "settings": {
        "capabilities": [],
        "dependencies": {
            "cajeta.lang":     "*",
            "cajeta.lang.io":  "*"
        },
        "dev-dependencies": {
            "cajeta.testkit": "*"
        },
        "repositories": [
            { "name": "central", "url": "https://repo.cajeta.org", "priority": 0 }
        ],
        "build": {
            "target":       "host",
            "entry-method": "<name>.Main::main"
        },

        // Named compositions. Add more as needed; reference by name
        // from any `flavor` field below.
        "custom-flavors": {
            "release-debug":  { "base": "release", "debug-info": "full" },
            "integration":    { "base": "release", "debug-info": "full", "analytics": true }
        }
    },

    "actions": {
        "ship": {
            "params": {
                "art": { "type": "string", "required": true }
            },
            "actions": [
                { "action": "sign",
                  "input":  "${params.art}",
                  "key-env": "CAJETA_RELEASE_KEY",
                  "key-id":  "${details.name}",
                  "id":      "sig" },
                { "action": "upload",
                  "target": "s3",
                  "input":  "${params.art}",
                  "also":   ["${sig.path}"],
                  "bucket": "${env.RELEASE_BUCKET}",
                  "prefix": "cajeta/${details.name}/${details.version}/",
                  "id":     "up" }
            ],
            "outputs": { "url": "${up.url}", "sig-path": "${sig.path}" }
        }
    },

    "plugins": { },

    "tasks": {

        // ─── Starter build tasks ────────────────────────────────────
        // Each task carries TWO independent knobs in its build action:
        //   * profile — which @Profile-gated components get wired
        //   * flavor  — how the compiler optimizes / instruments
        // Profile names are arbitrary; the names below are common
        // conventions. Rename, drop, or add tasks to match YOUR
        // environments (staging, canary, e2e, regional, per-tenant).

        // Local development.
        "build": {
            "description": "Local development build",
            "params": {
                "profile": { "type": "string", "default": "dev" }
            },
            "actions": [
                { "action":  "build",
                  "flavor":  "debug",
                  "profile": "${params.profile}",
                  "id":      "art" }
            ],
            "outputs": { "path": "${art.path}", "sha256": "${art.sha256}" }
        },

        // Unit tests with dependencies mocked.
        "test": {
            "description": "Unit tests with dependencies mocked",
            "params": {
                "filter":   { "type": "string", "default": "" },
                "parallel": { "type": "bool",   "default": true },
                "profile":  { "type": "string", "default": "test" }
            },
            "actions": [
                { "action":  "build",
                  "flavor":  "debug",
                  "profile": "${params.profile}",
                  "id":      "art" },
                { "action":   "test",
                  "filter":   "${params.filter}",
                  "parallel": "${params.parallel}",
                  "coverage": true,
                  "id":       "tr" }
            ],
            "outputs": { "passed": "${tr.passed}", "failed": "${tr.failed}" }
        },

        // Integration tests against sandbox / test-account deps.
        "integration": {
            "description": "Integration tests against sandboxed dependencies",
            "params": {
                "profile": { "type": "string", "default": "integration" }
            },
            "actions": [
                { "action":  "build",
                  "flavor":  "integration",
                  "profile": "${params.profile}",
                  "id":      "art" },
                { "action": "test", "filter": "*Integration*", "id": "tr" }
            ],
            "outputs": { "passed": "${tr.passed}", "failed": "${tr.failed}" }
        },

        // Release build → sign → publish to a repository.
        "release": {
            "description": "Release build wiring real production endpoints",
            "depends-on": ["test", "integration"],
            "params": {
                "profile": { "type": "string", "default": "release" },
                "version": { "type": "string",
                             "doc": "Release version (semver). Omit to keep current." }
            },
            "actions": [
                { "action": "version", "set": "${params.version}", "when": "${params.version}" },
                { "action":  "build",
                  "flavor":  "release",
                  "profile": "${params.profile}",
                  "id":      "art" },
                { "action": "ship",    "art":    "${art.path}" },
                { "action": "publish", "repository": "central" }
            ]
        },

        // ─── Auxiliary tasks ────────────────────────────────────────

        "clean":   { "description": "Wipe build outputs",
                     "actions": [ { "action": "clean" } ] },

        "lint":    { "description": "Static analysis",
                     "actions": [ { "action": "lint", "id": "ln" } ],
                     "outputs": { "findings": "${ln.findings}" } },

        "doc":     { "description": "Generate documentation",
                     "actions": [ { "action": "doc", "id": "d" } ],
                     "outputs": { "path": "${d.output-path}" } },

        "fmt":     { "description": "Format source",
                     "params":  { "check-only": { "type": "bool", "default": false } },
                     "actions": [ { "action": "fmt", "check-only": "${params.check-only}" } ] },

        "check":   { "description": "Parse + typecheck only (fast IDE / pre-commit gate)",
                     "actions": [ { "action": "build", "target": "check", "profile": "dev" } ] },

        "run":     { "description": "Build + execute the entry method",
                     "depends-on": ["build"],
                     "actions": [ { "action": "exec",
                                    "command": "${details.name}", "args": [] } ] },

        "install": { "description": "Install into the local download cache",
                     "depends-on": ["release"],
                     "actions": [ { "action": "install", "input": "${release.path}" } ] }
    }
}
```

### What this template demonstrates

- **Profile-as-property pattern.** Each build task names its
  profile literally. No central activation state.
- **Override path.** Each task parameterizes its profile, so
  `cajeta test -P profile=integration` is the one-off-different-profile
  invocation without editing the manifest.
- **Flavor / profile orthogonality.** `build` uses `debug`+`dev`;
  `test` uses `debug`+`test`; `integration` uses a custom
  flavor + `integration`; `release` uses `release`+`release`.
  Any combination is valid; the template just shows common
  pairings.
- **Custom flavor as named composition.** `integration` flavor
  lives in `settings.build.custom-flavors` so multiple tasks
  can reference it by name rather than restating the
  composition map.
- **Dependency gates.** `tasks.release` `depends-on` both
  `test` and `integration` so a release can't ship without
  both passing. The dependency is "tests must succeed first,"
  not "reuse the test build's binary" — `release` builds its
  own binary with the right profile.

### Customizing the template

Four common patterns:

- **Edit in place.** Change `tasks.test`'s profile, add a
  coverage threshold, switch parallelism — the manifest is the
  truth.
- **Rename profiles.** `dev` / `test` / `integration` /
  `release` are conventions, not requirements. If your project
  uses `local` / `unit` / `sandbox` / `prod`, just rename them.
- **Delete.** A project that doesn't ship docs deletes
  `tasks.doc`; `cajeta doc` then errors with "no such task,"
  surfacing the intent.
- **Add.** A project with bespoke flows adds new tasks freely.
  `cajeta tasks` lists them; `cajeta my-custom-task` runs them.
  Common additions: `staging`, `canary`, `e2e`, `perf`,
  regional builds, per-tenant builds.

The build tool ships no opinion about what tasks a project
should have. The template is convenience, not policy.

---

## Archive signing and launcher verification

A signed `.cja` carries an ed25519 signature over the archive
bytes. ArchiveManagement.md §8 specifies the on-disk signature
format (detached `.sig` today, header-flag bit reserved for
embedded) and the `cajeta archive sign` / `cajeta archive verify-sig`
subcommands. This section covers how the **build tool** and
**runtime launcher** interact with signing — the build-time
production of signatures, runtime verification before code
execution, and the trust store.

### Build-time: signing during distribution

The `sign` distribution action is the build-tool-level hook:

```jsonc
{
    "action":  "sign",
    "flavor":  "release",
    "key-env": "CAJETA_SIGN_KEY_PEM",
    "key-id":  "my-org-release-key-2026"
}
```

The `key-id` is recorded in the archive's `.cajeta-manifest`
section. The launcher uses it to look up the expected public key
in the trust store at runtime (see below).

`cajeta publish` is the higher-level command that bundles
build + sign + push to a repository in one operation, using the
manifest's repository-protocol auth. Both paths produce
equivalent `.cja.sig` artifacts.

### Runtime: launcher verification

The launcher (`cajeta run`, or any standalone runner that loads
`.cja` archives) can require a valid signature before executing
the archive's code:

```
cajeta run --verify-signature ./dist/my-service-1.2.3.cja
cajeta run --verify-signature=strict ./dist/my-service-1.2.3.cja
```

Verification modes:

- **`off`** (default for `cajeta run` of a local build) — skip
  verification entirely.
- **`warn`** — verify if a `.sig` is present (sidecar or
  embedded); warn-and-proceed if absent.
- **`strict`** — refuse to load any archive that doesn't carry a
  valid signature against a trusted public key. Refuse to load
  an archive whose `key-id` isn't in the trust store.

Verification flow in `strict` mode:

1. Open the archive, read the `.cajeta-manifest` section to get
   `key-id`. Read the `.cja.sig` (sidecar) or the embedded
   signature region.
2. Look up `key-id` in the trust stores in precedence order
   (see below). If absent → refuse; suggest `cajeta trust add`.
3. Verify the 64-byte ed25519 signature against the public key
   over the archive bytes (with the signature region zeroed for
   the embedded form).
4. On match → proceed to load. On mismatch → refuse with the
   computed-vs-expected digest pair printed for triage.

### Trust store

Public keys the launcher will accept come from three locations,
in precedence order:

1. **`$CAJETA_TRUST_KEYS_DIR`** — additional trust dir specified
   via environment. First-match-wins. Useful for CI runners that
   pre-stage org-specific keys without modifying system state.
2. **`~/.cajeta/trust/keys/`** — per-user trust store. Each file
   is a PEM ed25519 public key; the filename (sans `.pem`) is
   the `key-id` that identifies it. Used for personal trust
   decisions.
3. **`/etc/cajeta/trust/keys/`** (Windows: `%ProgramData%\cajeta\trust\keys\`)
   — system-wide trust store. Managed by the OS package manager
   or org-admin; typically locked-down read-only.

### Subcommand: `cajeta trust`

```
cajeta trust                          # list trusted keys + their fingerprints
cajeta trust list                     # same as bare invocation
cajeta trust add <key-id> <pem-path>  # install a public key
cajeta trust remove <key-id>          # uninstall (user store only by default)
cajeta trust show <key-id>            # print key fingerprint + metadata
cajeta trust verify <archive>         # one-shot verification
```

`cajeta trust add` writes into the per-user store by default;
`--system` writes into the system store (typically requires
elevation). `cajeta trust remove --system` likewise. The CLI is
a thin wrapper around the filesystem layout — shell scripts can
equivalently `cp` files into the trust dirs.

### Deploy-time: forcing signature requirements

Operators can enforce `strict` verification globally regardless
of how the launcher is invoked:

- **systemd unit**: `Environment=CAJETA_REQUIRE_SIGNATURE=strict`
- **Container runtimes** (Docker / Podman): the same env var,
  set per-service.
- **Kubernetes**: env var in the container definition.

When `CAJETA_REQUIRE_SIGNATURE=strict` is set, the launcher
ignores any laxer `--verify-signature` CLI flag. Useful for
locked-down production environments where the security posture
should not be downgradable by a misconfigured launch script.

### What the signature does NOT do

- **It does not authenticate publisher identity.** That's a PKI
  / web-of-trust problem. The trust store is "keys I've decided
  to accept artifacts from"; key distribution and revocation are
  out of scope for cajeta itself. Sigstore / cosign-style
  transparency-log integration is a possible extension (see
  Comparative analysis).
- **It does not enforce capability constraints.** A signed
  artifact is still subject to the capability declaration;
  signing means "this is the artifact the publisher meant to
  ship", not "this artifact is safe to grant any capability".
- **It does not gate dependency loading.** A signed top-level
  archive may transitively load unsigned archives from the local
  download cache. Closing this hole requires signature-checking
  on every load, which is feasible but adds complexity; deferred
  to v2.

---

## Toolchain provisioning

Java showed what goes wrong when builds depend on "Java 17"
without naming which JDK distribution from which vendor.
Different Java vendors (Eclipse Temurin, Amazon Corretto,
Oracle, GraalVM, Azul Zulu) ship the same major version with
different bytecode JITs, GC defaults, native-image support, and
bug patches — and the ecosystem lacked a first-class mechanism
for builds to declare which one they need. The result was
years of "works on my JDK" failures, SDKMAN, jenv, jabba, asdf,
and eventually Gradle's `toolchain { languageVersion = JavaLanguageVersion.of(17) }`
band-aid layered on top.

Cajeta's answer is to pin the toolchain from day one. The
manifest names the toolchain version + distribution; if the
running toolchain doesn't match, the build tool **auto-fetches
the right one and transparently dispatches** into it. The
ergonomic model is `rustup`: developers don't switch toolchains
manually; the toolchain manager does it.

### The pin: `settings.toolchain`

```jsonc
"settings": {
    "toolchain": {
        "version":      "1.0.3",          // exact toolchain version
        "distribution": "official",        // which build/distribution
        "channel":      "stable",          // optional: stable | beta | nightly
        "sha256":       "sha256:...",      // optional: verify the binary
        "fetch":        "auto",             // auto | warn | error | off
        "from":         "https://toolchains.cajeta.org"   // optional registry override
    }
}
```

Field semantics:

| Field | Required | Meaning |
|---|---|---|
| `version` | ✓ | Exact toolchain semver. Independent of `details.cajeta-lang-version`; pinning `1.0.3` is "this exact compiler build," not "any 1.0-compatible compiler." |
| `distribution` |  | Names the build. Defaults to `official` (the cajeta-org reference). Forks or specialty builds claim distinct names (`chronicled`, etc.). |
| `channel` |  | Release channel for floating pins. Useful for "latest 1.0.x stable" without manually bumping `version`. |
| `sha256` |  | Pin the binary's content hash. Strongest reproducibility guarantee; the registry publishes per-platform checksums. |
| `fetch` |  | Behavior when the running toolchain doesn't match. Default `auto`. |
| `from` |  | Toolchain registry URL. Defaults to the cajeta-org registry; companies/CI mirror or self-host. |

**`fetch` modes:**

- **`auto`** (default) — download to `~/.cajeta/toolchains/<dist>/<version>/`, re-exec transparently. The rustup model.
- **`warn`** — print a warning and proceed with the running toolchain. Useful for environments where downloads aren't desired but a hard error would block too much.
- **`error`** — refuse to build until the user installs the right one. Standard for locked-down CI.
- **`off`** — skip the check entirely. Escape hatch for tooling that knows what it's doing.

### Language version vs toolchain version

These are different concerns. Both belong in the manifest.

| | `details.cajeta-lang-version` | `settings.toolchain.version` |
|---|---|---|
| What it pins | Language + stdlib API surface | The specific compiler binary build |
| Granularity | Major.minor (`"1.0"`) | Full semver (`"1.0.3"`) |
| Used for | Source-level compatibility (will this code compile?) | Build-level reproducibility (will this produce the same bytes?) |
| Multiple satisfy? | Yes — toolchain 1.0.0 through 1.0.x all implement language 1.0 | No — one exact build |

A project that wants "compile with the latest 1.0.x bugfix" pins `cajeta-lang-version: "1.0"` and `settings.toolchain.channel: "stable"` (no exact version). A project that wants byte-identical reproducible builds pins both.

### Toolchain store

```
~/.cajeta/toolchains/
├── official/                       # the cajeta-org reference distribution
│   ├── 1.0.0/bin/cajeta
│   ├── 1.0.1/bin/cajeta
│   ├── 1.0.3/bin/cajeta
│   ├── 1.0.3/lib/cajeta-runtime.so
│   ├── 1.0.3/share/                # stdlib archives, docs, share data
│   └── current -> 1.0.3            # workstation-wide default symlink
├── nightly/
│   └── 2026-06-01/bin/cajeta
└── chronicled/                     # hypothetical third-party distribution
    └── 1.0.3-fast-math/bin/cajeta
```

Parallel to `~/.cajeta/cache/` (artifact + IR cache) and
`~/.cajeta/trust/` (signature trust store). Each distribution
holds its own versions; toolchains across distributions are
independent and don't conflict.

### Transparent dispatch — the rustup trick

When `cajeta` on PATH is invoked, here's what happens:

1. Read `settings.toolchain` from the manifest (if there is one).
   No manifest, no override, no env var → run the PATH binary
   directly.
2. Locate the right toolchain in `~/.cajeta/toolchains/<dist>/<version>/`.
3. If found and it isn't the running build, **re-exec into it**
   with the original argv. The user sees no difference; the
   right toolchain runs.
4. If not found:
   - `fetch: auto` → download, verify, install, then re-exec.
   - `fetch: warn` → warn-and-proceed with the running binary.
   - `fetch: error` → refuse with a suggested install command.
   - `fetch: off` → run the running binary without comment.

The PATH `cajeta` becomes a thin dispatcher; the real toolchains
live in the store. Same model `cargo` uses on top of `rustup`.

**Escape hatch:** `CAJETA_NO_DISPATCH=1` skips the re-exec
entirely and runs whatever's on PATH. Useful for tooling that
needs to know exactly which binary is running (debuggers,
profilers, recursive cajeta invocations from a build script that
shouldn't re-dispatch).

### Project-local override file

For cases where editing the manifest is the wrong granularity
(bisecting toolchain versions, CI matrix testing, temporary
local overrides), a one-line file at the project root takes
precedence:

```
.cajeta-toolchain                    # one of these forms:
1.0.3                                # bare version → official:1.0.3
official:1.0.3                       # explicit distribution
nightly:2026-06-01                   # date-stamped nightly
```

Same precedence chain as for properties:
**CLI flag > env var > project-local override file > manifest pin.**

Not committed to VCS.

### CLI

```
cajeta toolchain                         # show the active toolchain for this dir
cajeta toolchain list                    # all installed toolchains
cajeta toolchain install 1.0.3           # fetch official:1.0.3
cajeta toolchain install nightly:2026-06-01
cajeta toolchain install chronicled:1.0.3-fast-math \
                            --from=https://acme.example/cajeta-toolchains
cajeta toolchain remove 1.0.0
cajeta toolchain default 1.0.3           # set workstation-wide default
cajeta toolchain pin 1.0.3               # write settings.toolchain into cajeta.json
cajeta toolchain which                   # print absolute path of the active binary
cajeta toolchain show                    # the manifest's pin + the resolved binary path
```

### Toolchain registry protocol

Separate from artifact repositories (different content,
different protocol). The cajeta-org registry serves the official
distribution; companies / forks host their own.

```
GET  /toolchains/<dist>/index.json
     → { "versions": [...], "channels": { "stable": "1.0.3", "beta": "1.1.0-rc1", ... } }

GET  /toolchains/<dist>/<version>/<platform>/cajeta-<dist>-<version>-<platform>.tar.zst
     → archive containing bin/cajeta, lib/, share/, etc.

GET  /toolchains/<dist>/<version>/<platform>/checksums.json
     → { "cajeta-...-tar.zst": "sha256:..." } — optionally signed
```

Platforms follow standard target triples: `x86_64-linux-gnu`,
`aarch64-darwin`, `x86_64-windows-msvc`, etc.

### Reserved distribution names

| Name | Purpose |
|---|---|
| `official` | The cajeta-org reference build; default when `distribution` is unspecified. |
| `nightly` | Pre-release builds from `main`; for testing language features ahead of stable. |
| `lts` | Long-term-support cuts (v2 concern; reserve the name). |
| `system` | Whatever the OS package manager installed (PATH `cajeta` with no version pin); resolves to the running binary without re-dispatch. |

Third parties claim distinct distribution names. The cajeta-org
registry doesn't gatekeep distribution names that don't conflict
with reserved ones, but communities are encouraged to namespace
distinctively.

### Signature verification of toolchain binaries

Toolchain downloads are verified before install:

1. The registry publishes a checksums file per `<dist>/<version>/<platform>`.
2. The checksums file is signed (cosign / sigstore-equivalent)
   with the distribution's release key.
3. On install, the build tool verifies the signature against the
   appropriate trust-store entry (filename `<dist>-release.pem`)
   and the binary's sha256 against the checksums file.
4. Mismatch → refuse to install; cite the digest pair.

Same trust-store machinery used by archive signing
([Archive signing and launcher verification](#archive-signing-and-launcher-verification)).
A toolchain's release key is just another key-id in
`~/.cajeta/trust/keys/`.

### Lockfile integration

The toolchain pin lands in `cajeta.lock` so drift in the
toolchain itself is detectable:

```jsonc
{
    "lockfile-version":  1,
    "manifest-checksum": "sha256:...",
    "toolchain": {
        "distribution": "official",
        "version":      "1.0.3",
        "sha256":       "sha256:..."     // recorded resolved checksum
    },
    "melts":     [...],
    "packages":  [...]
}
```

Lockfile drift detection now covers three sources: manifest
content, melt versions, **and toolchain version**. A toolchain
bump is visible in PR review.

### IR cache discriminator

The toolchain version + distribution is already part of the IR
cache key (see [Incremental builds](#incremental-builds)). Pinning
the toolchain pins the cache: switching toolchain bumps the
discriminator and forces a fresh compile, as it should.

### Cross-compilation

`settings.toolchain` and `settings.build.target` are
independent. The toolchain knows how to cross-compile to various
target triples; the toolchain version + target combo is what
matters for reproducibility. A single toolchain build serves
multiple targets.

### What this does NOT do

- **It doesn't manage runtime JVMs / JDKs.** Cajeta is not Java
  hosted; there's no separate runtime to provision. The toolchain
  binary is the compiler AND it ships the cajeta runtime.
- **It doesn't manage LLVM.** The toolchain binary statically
  links its LLVM. Different toolchain versions may carry
  different LLVMs (the cajeta toolchain release notes call out
  the LLVM major version); users don't pin LLVM separately.
- **It doesn't manage other languages.** A cajeta toolchain
  manages cajeta. Polyglot projects use the OS or asdf to manage
  their other-language toolchains.

---

## Plugins

`cajeta lint` and `cajeta test` ship built-in checks and
coverage probes. Real projects need more — project-specific
banned APIs, license-header validation, security linters
tracking known-bad patterns, custom test-coverage
configurations, third-party policy enforcement. Plugins are the
extension point.

A plugin is a cajeta package that **ships actions** — named
verbs invokable from tasks, alongside the native action catalog.
Plugins resolve through the standard repository machinery (same
as runtime dependencies) and are declared in the manifest's
`plugins` block.

### What plugins can do, and can't

Deliberate constraints — Maven and Gradle's plugin proliferation
is the failure mode to avoid.

Plugins **cannot**:

- Customize the build graph or task DAG. Plugins ship actions;
  the user wires those actions into tasks. There is no plugin-
  driven side-effect that bypasses the manifest's declared task
  list.
- Modify source files or generated IR.
- Influence dependency resolution.
- Run automatically as "lifecycle hooks" — the task author
  decides when each plugin action runs.

Plugins **can**:

- Provide actions in any catalog category — analysis, validation,
  measurement, reporting, transport, codegen of supplementary
  artifacts, etc. The actions appear in tasks with the plugin's
  namespace prefix (`cajeta.coverage.report`,
  `cajeta.lint.security.scan`, etc.).
- Carry their own capability declarations; the build tool
  sandboxes plugin processes against the consuming project's
  `settings.plugins-allowed-capabilities` allowlist.

### Plugin declaration

Declared in the manifest under `plugins`:

```jsonc
"plugins": {
    "cajeta.lint.security": {
        "version": "1.2.*",
        "config": {
            "banned-imports":  ["unsafe.legacy", "vendor.deprecated"],
            "secret-patterns": ["AKIA[0-9A-Z]{16}", "ghp_[A-Za-z0-9]{36}"]
        }
    },
    "acme.license-check": {
        "version": "0.5.0",
        "config": {
            "required-header": "config/license-header.txt"
        }
    },
    "cajeta.coverage": {
        "version": "1.0.*",
        "config": {
            "grain":   "line",
            "min":     80,
            "exclude": ["**/*_generated.cajeta", "**/Mock*.cajeta"],
            "report":  ["html", "sarif", "console"]
        }
    },
    "acme.policy-gate": {
        "version": "2.1.0",
        "config": {
            "fail-on": ["error", "license-violation"]
        }
    }
}
```

Plugin versions resolve through MVS like any other dependency
and are recorded in `cajeta.lock` under a top-level `"plugins"`
array. A plugin pinned at `1.2.3` will not be silently upgraded
to `1.3.0` mid-stream.

The `config` block is plugin-specific and documented by each
plugin. Common config patterns surface in the plugin's actions
as default param values (e.g. `cajeta.coverage`'s `min: 80`
applies to `cajeta.coverage.report` unless the task overrides
it).

### Actions plugins provide

Each plugin's documentation lists the actions it ships, their
required and optional params, and the outputs they publish —
exactly the same contract as native actions. Tasks invoke them
by name with the plugin's namespace:

```jsonc
"tasks": {
    "test": {
        "actions": [
            // Compile with coverage probes
            { "action": "cajeta.coverage.instrument", "id": "ci" },

            // Run tests against the instrumented build
            { "action": "test", "instrumented-by": "${ci.path}", "id": "tr" },

            // Reduce probe hits and emit reports
            { "action": "cajeta.coverage.report",
              "input":  "${tr.coverage-data}",
              "min":    80,
              "report": ["html", "console"],
              "id":     "cov" }
        ],
        "outputs": { "coverage": "${cov.percent}" }
    },

    "lint": {
        "actions": [
            { "action": "lint" },                              // native lint
            { "action": "cajeta.lint.security.scan" },         // plugin
            { "action": "acme.license-check.verify" }          // plugin
        ]
    }
}
```

Plugin actions participate in [pipelining](#action-pipelining)
identically to native actions; their outputs are documented by
the plugin and consumable via `${id.field}`.

Multiple plugin actions invoked from the same task run according
to the task's structure — sequential by default, parallel when
wrapped in a `parallel` group. Two plugin actions in a parallel
group run concurrently; there's no implicit "all analyzers in
parallel, then all reporters" phase scheduling. The task author
controls ordering.

Findings from analyzer-style plugin actions merge into a unified
report keyed by source location when the task uses the native
`lint` action (which collects findings from any prior plugin
actions that published `findings` outputs). Two plugins flagging
the same line at different severities both appear; consumers
(CI, IDE) decide which to surface.

### Code coverage (canonical first-party plugin)

`cajeta.coverage` ships as a first-party plugin. The user
explicitly asked for "lines of code or areas of code not touched
by tests" — this is the implementation.

```jsonc
"plugins": {
    "cajeta.coverage": {
        "version": "1.0.*",
        "config": {
            "grain":        "line",     // "line" | "branch" | "region"
            "min":          80,          // overall percentage; CI gate
            "min-per-file": 50,          // per-file floor (optional)
            "exclude":      ["**/*_generated.cajeta", "**/Mock*.cajeta"],
            "report":       ["html", "sarif", "console"]
        }
    }
}
```

The plugin ships three actions:

| Action                          | Purpose                                                             |
|---------------------------------|---------------------------------------------------------------------|
| `cajeta.coverage.instrument`    | Build the project with coverage probes enabled.                     |
| `cajeta.coverage.collect`       | Run during/after tests to reduce probe hits into a coverage map.    |
| `cajeta.coverage.report`        | Emit configured reports (html/sarif/lcov/console); enforce `min`.   |

Wired into the default `test` task:

```jsonc
"tasks": {
    "test": {
        "depends-on": ["build"],
        "actions": [
            { "action": "cajeta.coverage.instrument", "id": "ci" },
            { "action": "test",
              "instrumented-by": "${ci.path}",
              "id": "tr" },
            { "action": "cajeta.coverage.collect",
              "input": "${tr.coverage-data}",
              "id": "cov" },
            { "action": "cajeta.coverage.report",
              "input": "${cov.path}",
              "min":   80 }
        ]
    }
}
```

Mechanism:

1. `cajeta.coverage.instrument` invokes the compiler with
   `--instrument=coverage`, which emits probe-points at every
   line / branch / region.
2. The test runner records probe hits during each test.
3. `cajeta.coverage.collect` reduces hits across all tests into
   a per-file / per-line / per-branch / per-region execution
   map.
4. `cajeta.coverage.report` emits one or more of:
   - **`console`** — overall percentage + bottom-N files by
     coverage, printed at end of `cajeta test`.
   - **`html`** — per-file annotated source: green (covered) /
     red (uncovered) / yellow (partial), with a file index and
     a project-wide summary. Output at `build/coverage/html/`.
   - **`sarif`** — machine-readable for CI integration (GitHub
     Code Scanning, etc.).
   - **`lcov`** — Coveralls / Codecov-compatible.

Granularity:

- **`line`** — was each statement executed at least once? The
  default; fastest, lowest probe overhead, easiest to read.
- **`branch`** — for each `if` / `else` / `switch` arm, was each
  taken? Catches "the condition was always true in tests."
- **`region`** — for each basic block as the compiler sees it,
  was it entered? The most granular, catches dead-code patterns
  that `line` and `branch` miss (compiler-generated paths,
  early-return shortcuts).

The `min` threshold is a CI gate — falling below it fails the
build with a citation of which files dragged the number down.
The threshold applies to project source only; deps and test files
are excluded by default. `min-per-file` applies a floor to each
individual file so the aggregate can't hide one badly-tested
file.

`exclude` patterns drop files from both the numerator and the
denominator — generated code, mocks, and explicitly-opted-out
declarations aren't expected to be tested. Cajeta source can
also use `@nocoverage` on a class or method:

```cajeta
@nocoverage("trivial accessor; tested implicitly via every use")
public int32 getCount() { return this.count; }
```

The reason string is mandatory; `cajeta lint` warns if it's
missing or generic ("WIP", "todo", "skip").

### Plugin capabilities and sandboxing

Plugins run during the build but each in its own sandboxed
subprocess. Each plugin's own `cajeta.json` declares its required
capabilities (the same capability machinery used for runtime
code). The build tool checks plugin capabilities against the
consuming project's `plugins-allowed-capabilities` allowlist:

```jsonc
"plugins-allowed-capabilities": ["filesystem", "network"]
```

A plugin declaring a capability not in the allowlist fails the
build before it runs, citing the plugin name and the missing
capability. Default allowlist is `["filesystem"]` — most plugins
need to read source / IR to do their job, but few need network
or process spawn.

First-party plugins (`cajeta.*` namespace, shipped with the
toolchain) inherit a less restrictive default. User-installed
plugins always hit the explicit allowlist — supply-chain attacks
in the build path are exactly the failure mode the capability
system exists to prevent.

### Plugin conflicts

Two plugins claiming the same `plugin.id` field (declared in
their own manifest) conflict and fail the build. There is no
last-write-wins behavior — "the wrong analyzer silently took
over" is too painful to debug.

A plugin shadowing a built-in lint check ID (`unused-import`,
`deprecated-api`, etc.) similarly fails; built-ins can be
disabled via `lint.allow` but not silently replaced.

### `lint` task and plugin actions

The default `lint` task invokes the native `lint` action plus
whichever analyzer-style plugin actions the project has wired
in. The native `lint` action collects findings published by
preceding actions in the task and produces a unified report.

Default template (from `cajeta init`):

```jsonc
"lint": {
    "actions": [
        { "action": "lint", "id": "ln" }
    ],
    "outputs": { "findings": "${ln.findings}" }
}
```

Extending it to include security and license-check plugins:

```jsonc
"lint": {
    "actions": [
        { "action": "cajeta.lint.security.scan", "id": "sec" },
        { "action": "acme.license-check.verify", "id": "lic" },
        { "action": "lint",
          "include-findings": ["${sec.findings}", "${lic.findings}"],
          "id": "ln" }
    ],
    "outputs": { "findings": "${ln.findings}" }
}
```

CI invocations:

```
$ cajeta lint --format=sarif --output=lint.sarif
$ cajeta lint --fail-on-severity=warning
```

The built-in checks (cyclic types, unused imports, deprecated
APIs, capability mismatches, dead code) are implemented as
first-party plugins under the hood — not special-cased — so the
unified report shape works regardless of plugin source.

### Why not Maven/Gradle-shaped plugins

Maven has hundreds of plugins, Gradle thousands. Cajeta's plugin
system is deliberately narrower:

- **No build-graph customization.** Maven/Gradle plugins inject
  themselves into the lifecycle and can rewrite the build.
  Cajeta plugins ship named actions; the task author invokes
  those actions wherever they make sense in their tasks.
- **No DSL surface.** Plugins ship JSON-declared actions. No
  plugin-specific DSL fragment for users to learn per plugin.
- **Capability-gated.** A Maven plugin can do anything the JVM
  can do; a cajeta plugin runs in a sandbox with declared caps.
- **No phase scheduling.** Plugins don't auto-run at "the right
  point in the lifecycle" — there is no lifecycle. The task
  author writes the action invocations explicitly.

The trade: cajeta plugins can't reshape the build itself, which
is why we have first-class tasks, action presets, properties,
profiles, overrides, and capability declarations — the cases
where Maven/Gradle users reach for plugins, cajeta provides as
manifest constructs.

---

## Open specifications

Every interface the build tool exposes is published as a versioned
spec under `cajeta-docs/specs/`. Third parties can implement
against the spec without reverse-engineering the official
toolchain.

| Spec                              | What it defines                                              |
|-----------------------------------|--------------------------------------------------------------|
| `manifest-v1.json` (JSON Schema) | The shape of `cajeta.json`. Drives editor autocomplete, validators. |
| `lockfile-v1.json`                | The shape of `cajeta.lock`. Drives deterministic builds.    |
| `archive-v1.md`                   | The `.cja` archive on-disk format (per Compilation.md).     |
| `repository-protocol-v1.md`       | HTTP API for repositories (endpoints, request/response shapes, auth). |
| `capabilities-v1.json`            | The capability vocabulary + their meanings.                  |
| `extension-api-v1.md`             | How third-party plugins extend `cajeta` (post-build hooks, custom flavors, repository drivers). |

Specifications are versioned independently. A new manifest field
in `manifest-v2.json` is opt-in via a `"schema-version": 2`
declaration at the top of `cajeta.json`; v1-only tools ignore
v2-tagged manifests with a clear error.

The cajeta toolchain ships one reference implementation of each
spec; nothing prevents a third party from shipping their own
build tool that reads `cajeta.json` and produces compatible
artifacts. The official `cajeta` binary is dominant by default,
not exclusive.

---

## Comparative analysis

Cajeta's build tool is informed by a deliberate study of prior
art. This section catalogs the trade-offs of each major
ecosystem, what cajeta adopts, and what cajeta does differently
and why. Read it as the reasoning behind the design — every
choice above is traceable to a lesson from one of these tools.

### Maven (Java, 2004)

**What worked:**
- Repository-based dependency resolution with transitive
  graphs. The model of "declare what you need, the tool fetches
  it transitively" set the standard.
- The group:artifact:version coordinate system —
  organization-prefixed namespace + a stable artifact name +
  semver. Maven Central scales globally on this.
- Public Maven Central as a universal distribution mechanism.
  One global namespace, anyone can publish (with controlled
  namespace ownership).
- The `<dependencyManagement>` block — a way to pin transitive
  versions across the graph without rewriting every direct dep.
- `${property}` substitution centralizing repeated values
  (versions, paths, group names) in a `<properties>` block. The
  single biggest readability win Maven has, and cajeta carries
  it forward verbatim — see [Properties](#properties).

**What didn't:**
- XML pom files. Verbose, human-hostile, change-noisy. Comparing
  two poms in a code review is unpleasant.
- Plugin proliferation — every build concern (compile, test,
  package, resources, codegen, deployment) is a separate plugin
  with its own DSL fragment, its own version, its own
  documentation. Reading a non-trivial pom requires understanding
  10+ plugin schemas. Cajeta's [plugin system](#plugins) keeps
  the extension hook for the cases that genuinely need it
  (analysis, validation, coverage) but constrains plugins to
  four phases with structured findings, denying them the
  build-graph customization that turned Maven plugins into a
  per-project DSL cocktail.
- Slow incremental builds. Maven re-runs lifecycle phases
  liberally rather than tracking what actually changed.
- "Convention over configuration" works for the common case but
  becomes hostile when you need to deviate; the escape valves
  are awkward (parent poms, multiple profiles).

**What cajeta keeps:** repository model (priority-ordered),
the group/library/version coordinate system (cajeta's reverse-
DNS name flattens group:library, see
[Manifest](#manifest--cajetajson) field conventions), public-
registry pattern, the `dependencyManagement`-equivalent (cajeta's
[overrides](#overriding-transitive-dependencies)).

**What cajeta changes:** JSONC over XML; no plugin system in the
declarative path (post-build hooks for escape, declarative
[distribution actions](#distribution-actions) for the common
case); content-addressed incremental cache.

### Gradle (Java/Android, 2007)

**What worked:**
- The Gradle Build Cache made incremental + distributed builds
  fast — a huge improvement over Maven's "rerun the lifecycle"
  model. Tasks are cached on content-derived input keys.
- Hierarchical task definition: the project graph is a DAG of
  tasks, each task declares its inputs / outputs / dependencies.
  The DAG is inspectable, parallelizable, and reproducibly
  cacheable. This was Gradle's real innovation.
- First-class IDE integration through a model-protocol API.
- Configuration caching: even Gradle's expensive build-config
  evaluation phase gets cached on subsequent runs.

**What didn't:**
- Build scripts are Turing-complete (Groovy or Kotlin DSL). A
  non-trivial build is a real program — debug it with a real
  debugger. Reading someone else's `build.gradle` to understand
  the build is high-effort.
- Plugin compatibility hell — DSL changes between Gradle
  versions break third-party plugins; users get caught in
  compatibility matrices. The plugin API surface is so wide
  that holding compatibility is a structural problem, not a
  matter of discipline.
- The Gradle daemon and configuration cache get stale in
  confusing ways. "Stop the Gradle daemon and rerun" is a
  routine debugging step that should never be necessary.
- IDE integration depends on a sync step that's slow and
  occasionally fails opaquely.

**What cajeta keeps:**
- **The task DAG model itself.** Cajeta's `tasks` block IS
  Gradle's task graph, rendered as JSON. Tasks have
  `depends-on` (Gradle's `dependsOn`), tasks produce outputs
  consumable by other tasks (Gradle's task inputs/outputs),
  tasks compose into pipelines. This is Gradle's primary
  contribution and cajeta takes it whole.
- **Content-addressed incremental cache** — see
  [Incremental builds](#incremental-builds). The IR cache is
  Gradle's build-cache lesson at a finer grain.
- **Output threading from one task to the next** — Gradle's
  `task.outputs.files` for declared outputs maps to cajeta's
  `${id.field}` pipelining mechanism.

**What cajeta changes:**
- **No Turing-complete script.** Tasks compose actions; actions
  compose other actions; nothing is an embedded programming
  environment. The `exec` action is the only escape, explicitly
  outside the analyzable surface.
- **No fixed lifecycle phases.** Gradle inherited Maven's
  phase-based "everything fits into the lifecycle or you fight
  the framework" model. Cajeta drops it: there is no
  `validate → compile → test → package → install` order the
  tool enforces. Projects declare their own tasks with their
  own dependencies; `cajeta init` provides a Maven-shaped
  starter set, and projects edit it freely.
- **No plugin DSL surface.** Gradle plugins extend the build
  language itself, which is what creates the
  compatibility-matrix problem. Cajeta plugins ship JSON-
  declared actions — no DSL fragment, no per-plugin syntax,
  no version-coupled language extensions.
- **No daemon.** No configuration-cache state to manage; the
  manifest loads on every invocation. Faster startup ceiling
  but no warm-cache amortization across runs (the IR cache
  amortizes per-file content; the manifest is small enough that
  re-parsing is cheap).
- **Single document.** Gradle's build is `build.gradle` plus
  `settings.gradle` plus `gradle.properties` plus
  `*.gradle.kts` plus version catalogs plus init scripts plus
  applied plugins-from-the-web. Cajeta is one `cajeta.json`
  per package.

### Cargo (Rust, 2014)

**What worked:**
- Single binary owning the whole toolchain (compile, test, doc,
  fmt, dep management, publish). This single-tool ergonomic is
  the strongest argument against the Maven/Gradle plugin
  fragmentation model.
- Lockfile committed to VCS — reproducible builds without a
  heavy apparatus.
- MVS (minimum version selection) — predictable conflict
  resolution, no "latest patch broke us" surprise.
- Subcommand UX (`cargo build`, `cargo test`, etc.) —
  discoverable, consistent flag semantics.
- Workspaces as a first-class feature, not a bolt-on.
- `[patch]` and `[replace]` for transitive dependency overrides.

**What didn't:**
- Monolithic crate granularity — adding one function from a
  large crate pulls in the whole crate. (Cajeta's archive can
  ship at a finer per-module grain — see Compilation.md.)
- No capability model — anything in the dep graph can call
  anything; supply-chain attacks have to be caught at code
  review. This is Cargo's biggest weakness vs cajeta's design.
- First-time fetch of a large dep graph is slow; no equivalent
  of Maven's parallel-fetch.

**What cajeta keeps:** essentially all the structural decisions
above — single binary, lockfile, MVS, subcommand UX, workspaces,
override mechanism. Cargo is the closest prior art to cajeta's
build tool, and the deliberate design point is "Cargo, but
with capabilities, JSON, and a richer distribution surface."

**What cajeta changes:** capability declarations as first-class.
Per-module archive grain (specified in
[Compilation.md](Compilation.md)). JSONC manifest over TOML.

### Bazel / Buck (Google/Meta, 2010+)

**What worked:**
- Hermetic, sandboxed build steps — builds are reproducible by
  construction.
- Content-addressed remote build cache shared across the org —
  large monorepos see huge wall-clock wins.
- Strict dependency graph: every input is declared, including
  transitive headers / files. No accidental dependencies.
- Selective builds — "build only what's affected by this change"
  is mechanically computable from the DAG.

**What didn't:**
- Starlark (Bazel's build language) is its own programming
  environment with a steep learning curve. The cognitive load
  per-developer is high.
- BUILD files are verbose and fragile to refactoring. Renaming
  a target involves updating every reverse-dep's BUILD file.
- Setting up the remote cache + executor infrastructure is a
  significant operational investment; "you need a tools team to
  run Bazel" is real.
- Per-language rules are owned by separate communities; the
  ecosystem of rule sets varies in quality.

**What cajeta keeps:** sandboxed build steps (see
[Sandboxing and reproducibility](#sandboxing-and-reproducibility)),
hermetic execution as the default, reproducible builds.

**What cajeta changes:** repositories — not remote build caches
— serve the "share an artifact across the org" purpose without
the layered-cache complexity. No Starlark; no BUILD files; the
manifest IS the build graph (because the build graph is
mechanically derivable from the manifest + source tree, without
user-authored task definitions). This is a deliberate v1 trade:
we lose the ability to reuse build outputs at the file grain
across machines, we gain a build that doesn't require operator
investment to set up.

### CMake / Meson / Ninja (C/C++, 1990s–2010s)

**What worked:**
- Ninja is fast — minimal-rebuild graph execution that
  outperforms make by an order of magnitude on touch-one-file
  rebuilds. The Ninja file format is a deliberately constrained
  DSL: a graph of `build out: rule deps` rules with no
  conditionals. That constraint is what makes it fast.
- Meson is a meaningful UX improvement over CMake (declarative
  rather than macro-soup).
- **Precompiled headers** — once a header's translation cost is
  paid, every translation unit including it skips it. The
  win is large for C++ where header inclusion is expensive.

**What didn't:**
- CMake's macro-soup syntax is famously hostile. Reading a
  non-trivial CMakeLists is a recreational sport.
- No dependency management. Every C/C++ project rolls its own
  combination of git submodules, vcpkg, Conan, FetchContent —
  none of which integrate cleanly.
- Two-stage generation: CMake → Ninja → exe. Debugging build
  issues requires understanding both layers.
- Per-target flag customization syntax is bizarre even after
  understanding the model.

**What cajeta keeps:** Ninja's minimal-rebuild discipline — only
the changed source's IR module is recompiled. The
[IR cache](#incremental-builds) is cajeta's equivalent of
precompiled headers, generalized to all source files rather
than just headers, and content-addressed rather than
path-addressed. This means rebuilds across branches and across
machines (within the local cache) share cache hits as long as
content matches.

**What cajeta changes:** integrated dependency management
(repositories), no two-stage generation, declarative manifest
that's the only file the user touches.

### npm / pnpm / yarn (JavaScript, 2010+)

**What worked:**
- `npm install <pkg>` is fast and intuitive — the `cajeta add
  <dep>` UX is directly modeled on this.
- pnpm's content-addressed store saves disk relative to npm's
  duplicated `node_modules` tree.
- Vast ecosystem; the registry pattern works at scale.

**What didn't:**
- Micro-package culture (`left-pad`, `is-odd`) makes dep graphs
  enormous and supply-chain attacks viable. The Russian-doll
  transitive structure means a typical app pulls thousands of
  packages.
- **No capability model.** Anything in the dep graph can
  `require('fs')` and read your `.ssh/`. This is the
  load-bearing argument for cajeta's capability declarations.
- npm's lockfile / `package.json` divergence has had multiple
  redesigns; trust eroded.
- Hoisting / phantom dependencies — code can `require` packages
  it didn't declare.

**What cajeta keeps:** the `cajeta add <dep>` UX, the
content-addressed download cache (pnpm's contribution).

**What cajeta changes:** capability declarations and enforcement;
package granularity at the language-module level rather than
the micro-utility level (no `is-odd` equivalents — the stdlib is
expected to cover the building blocks); strict lockfile with
manifest checksum for divergence detection.

### pip / Python packaging (Python, 1998+)

**What worked:**
- Simple repository protocol (HTTP + predictable URLs); PyPI
  scales reasonably.
- Wheels (precompiled distributions) avoid build-from-source-on-
  every-install pain.

**What didn't:**
- Multiple competing tools (pip, conda, poetry, uv, pip-tools,
  pipx, ...) — the dep-management story is fractured, and
  recommended tooling shifts every few years.
- Virtualenv requirement is a global-state workaround. The
  notion that "the system Python environment is shared and
  fragile" leaks into every workflow.
- No lockfile in pip itself for years; arrived in pip-tools and
  others, with various semi-incompatible formats.
- Native dependencies are platform-specific; Python deals with
  this via wheels per (OS, Python version, arch), an N×M×K
  matrix.

**What cajeta keeps:** the simple repository protocol (HTTPS +
predictable URLs).

**What cajeta changes:** one tool (`cajeta`), one lockfile, no
virtualenv equivalent (the project directory IS the
environment), single-cross-platform archive (cross-compilation
handled by the compiler, not separate wheels per arch).

### Nix (functional package manager, 2003+)

**What worked:**
- Hermetic builds via content-addressed inputs.
- Atomic install / rollback; multiple versions coexist on a
  single system without interference.
- Build expression language is a real (declarative) language —
  the closest mainstream attempt at "declarative but
  expressive."

**What didn't:**
- The Nix language is its own thing to learn (Haskell-flavored
  but not Haskell).
- Nixpkgs is large enough that build-from-source for a
  non-trivial config takes hours; binary substitutes solve
  this but bring their own caching layer.
- Operator complexity is real — `/nix/store` is a global system
  install, channel management is its own discipline.

**What cajeta keeps:** content-addressed cache discipline,
hermetic builds.

**What cajeta changes:** no Nix-language equivalent. Cajeta's
manifest is JSONC for users; the build graph it derives is
internal to the tool. The trade vs Nix: less expressiveness,
much lower cognitive load.

### Summary of cajeta's deliberate trade-offs

| Capability cajeta has              | Inspired by             | Variant chosen                                    |
|------------------------------------|-------------------------|---------------------------------------------------|
| Repository-based deps + MVS        | Cargo                   | Same as Cargo                                     |
| Single binary, subcommand UX       | Cargo                   | Same as Cargo                                     |
| Lockfile committed to VCS          | Cargo                   | + manifest-checksum for divergence detection      |
| Workspaces                         | Cargo + Gradle          | Cargo-shaped; one lockfile across members         |
| Transitive overrides               | Maven, Cargo            | Last-write-wins after MVS                         |
| Public registry pattern            | Maven Central, PyPI     | Cajeta-specific protocol; Maven-compat shim       |
| Content-addressed incremental cache | Gradle Build Cache, Bazel | Per-file IR grain, local-only in v1                |
| Sandboxed build steps              | Bazel, Nix              | bwrap/sandbox-exec/Job objects per platform       |
| Reproducible builds                | Bazel, Nix              | `SOURCE_DATE_EPOCH` + debug-prefix-map + seeded RNG |
| Build attestation (SLSA)           | Sigstore ecosystem      | Same as Sigstore; optional cosign integration      |
| Capability declarations            | (no clean precedent)    | Cajeta-specific; the load-bearing security model  |
| Declarative distribution actions   | Maven plugins, GitHub Actions catalog | Fixed v1 catalog + `shell` escape       |

| Capability cajeta deliberately does NOT have | Why                                                                |
|----------------------------------------------|--------------------------------------------------------------------|
| Turing-complete build scripts                | Gradle's failure mode; analysis requires running code              |
| Remote build cache                           | Repository model serves the same need; remote cache adds operator complexity |
| Multi-language build orchestration           | Out of scope for v1; cajeta is a cajeta-only tool                  |
| Hosted public registry (in v1)               | Infrastructure project, separate effort                            |
| Auto-activation of profiles                  | Non-obvious behavior; explicit activation via CLI or env           |
| Cross-toolchain compatibility windows        | Open question, see [Open questions](#open-questions)               |

---

## Implementation sequence

A reasonable order, given the existing compiler:

1. **`cajeta build` over an existing source tree.** Reads
   `cajeta.json`, invokes the compiler with the right flags,
   produces output in `build/`. No dependency resolution yet —
   `cajeta.json` declares dependencies but they have to be
   pre-staged in a local repository. The lifecycle subcommands
   (`build`, `clean`, `init`, `test`) land in this first slice.
2. **Properties + `${PROPERTY}` substitution.** Manifest
   resolution layer; built-ins available from day one. Lands
   here because every subsequent step (lockfile, repositories,
   distribute) can reference resolved properties without
   special-casing.
3. **`cajeta.lock` generation + reading.** Lockfile produced on
   first build; subsequent builds use the locked versions
   verbatim. Records the resolved property set.
4. **Incremental builds + IR cache.** Per-file IR module cache
   keyed by content discriminator; cache hits skip the
   IR-emission step. The single biggest win for day-to-day
   iteration speed; lands early so the rest of the pipeline
   benefits from it.
5. **Filesystem repositories.** The simplest case — point at a
   directory tree, find artifacts by name + version. Validates
   the resolution machinery without a network layer.
6. **MVS dependency resolution.** The constraint solver handles
   transitive deps. Output is the resolved graph; lockfile
   captures it.
7. **Transitive dependency overrides.** The `overrides` block;
   applies after MVS, last-write-wins; recorded in lockfile.
8. **`cajeta add` / `cajeta upgrade` / `cajeta remove`.** Manifest
   + lockfile mutation. Capability change prompts during upgrade.
9. **HTTP repositories.** The wire protocol from
   [Repositories](#repositories). Auth, caching, retry. Public
   `repo.cajeta.org` infrastructure ships separately. Maven-
   compat shim lands alongside.
10. **Build flavors.** Two built-in flavors (`release`, `debug`);
    map-form composition (`{ base, ...overrides }`); custom
    flavors in `settings.build.custom-flavors`. The `build`
    action accepts a `profile` string param and passes it
    through to the compiler as `--profile=<name>`; profile
    itself is a per-task literal, no central activation
    machinery.
11. **Capability annotations + static verification.** Compiler
    tracks `@capability` annotations, computes required-set per
    module, build tool diffs against declared set and produces
    the citation-style error.
12. **Plugin runtime + first-party `analyze`/`validate` plugins.**
    Plugin discovery, lockfile entry, sandboxed subprocess
    execution, structured-findings collection. First-party
    plugins (the built-in lint checks reimplemented on the
    plugin runtime) ship as the validation.
13. **`cajeta.coverage` plugin + test instrumentation.** Compiler
    `--instrument=coverage` flag, line/branch/region probes, test
    runner reduction, HTML/SARIF/console reporters. The
    canonical `measure`-phase plugin; bottom-N coverage gate at
    CI; per-file floor.
14. **Sandboxing.** bwrap on Linux first; macOS / Windows
    sandboxes follow.
15. **Reproducible builds.** `SOURCE_DATE_EPOCH`, debug-prefix-
    map, seeded RNG. Verified by automated rebuild + compare in
    CI.
16. **Workspaces.** Workspace manifest, multi-member resolution,
    cross-package incremental builds.
17. **Distribution actions.** Declarative catalog —
    `copy` / `upload-s3` / `upload-azure` / `upload-gcs` /
    `upload-http` / `shell`. Replaces ad-hoc post-build shell
    hooks for the common cases.
18. **Archive signing + `cajeta trust` + launcher verification.**
    The `sign` action wires into the existing
    `cajeta archive sign` machinery (ArchiveManagement.md §8);
    the trust store and launcher verification mode round out
    the runtime side. Sigstore / cosign-style transparency-log
    integration is an optional extension.
19. **`cajeta publish` + SLSA attestation.** Sigstore integration
    or equivalent. Repository protocol's POST endpoint.
20. **Git repositories.** The clone-and-build-locally path.
21. **Build attestation verification.** Consumer side: `cajeta
    install` verifies signature + provenance.

Deferred (separate efforts, post-v1):
- Hosted public registry (`repo.cajeta.org`) — infrastructure,
  not toolchain.
- Plugin / extension API for third-party post-build steps
  beyond shell commands.
- `cajeta vendor` advanced flows (selective vendoring, vendored
  archives signed).
- IDE protocol — language server, hover info from manifest.

---

## Open questions

- **Public registry governance.** Who owns it, how do
  malicious-package takedowns work, what's the namespace
  reservation policy (prevent typo-squatting). Major non-toolchain
  problem; this spec doesn't decide it.
- **Capability minor-version drift.** A package adds a
  capability between 1.2.3 and 1.2.4 (semver-patch). Pure
  semver says patch can't break compat; capability addition is
  arguably breaking (it changes the security posture).
  Lean: treat capability addition as a minor-version bump
  even if semantically the API is unchanged. Worth a
  decision before the first stdlib release.
- **Cross-toolchain compatibility.** Cajeta toolchain N produces
  archives that toolchain N+1 may or may not be able to consume.
  We need an explicit compatibility-window policy (e.g.
  "archives from N can be consumed by N, N+1, N+2"; older N-2
  generates a warning; N-3 fails). Lean: N±2 window matching
  the cajeta-lang-version field.
- **Repository CDN model.** Public registry will serve gigabytes
  of artifacts. CDN integration (CloudFront / Fastly / static-
  origin patterns) is an operational concern that affects the
  repository protocol (range requests for partial downloads,
  ETag caching). Defer detailed design until the registry
  infrastructure RFC.
- **Multi-version transitive deps.** Cargo allows the same
  package at multiple major versions in the same dep tree (X
  uses `foo@1`, Y uses `foo@2`). Maven doesn't. Lean toward
  Cargo's model — fewer "everything must use the same version"
  conflicts — at the cost of larger lockfiles and binaries that
  carry multiple major versions of a dep.
- **Deprecation lifecycle.** `@Deprecated` in source is one
  signal; deprecating an entire repository version is another.
  Should `cajeta upgrade` skip deprecated versions by default?
  Lean: yes, warn loudly when locking against one.
