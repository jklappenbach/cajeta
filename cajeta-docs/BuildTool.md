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
4. [Manifest — `cajeta.json`](#manifest--cajetajson)
5. [Properties](#properties)
6. [Lockfile — `cajeta.lock`](#lockfile--cajetalock)
7. [Dependency resolution](#dependency-resolution)
8. [Overriding transitive dependencies](#overriding-transitive-dependencies)
9. [Repositories](#repositories)
10. [Capability system](#capability-system)
11. [Build flavors](#build-flavors)
12. [Profiles](#profiles)
13. [Incremental builds](#incremental-builds)
14. [Workspaces](#workspaces)
15. [Sandboxing and reproducibility](#sandboxing-and-reproducibility)
16. [Action catalog](#action-catalog)
17. [Action pipelining](#action-pipelining)
18. [Default `cajeta init` manifest](#default-cajeta-init-manifest)
19. [Archive signing and launcher verification](#archive-signing-and-launcher-verification)
20. [Plugins](#plugins)
21. [Open specifications](#open-specifications)
22. [Comparative analysis](#comparative-analysis)
23. [Implementation sequence](#implementation-sequence)
24. [Open questions](#open-questions)

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
  priority order until each `name@version` is found. Standard
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
cajeta init <name>           Scaffold a new project (writes a starter cajeta.json
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
cajeta profile               Show, set, or activate a manifest-defined profile
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

## Manifest — `cajeta.json`

JSONC format (strict JSON's data model + `//` and `/* */`
comments + trailing commas). The cajeta toolchain reads it;
third-party tooling that prefers strict JSON can pre-strip
comments via the single-pass preprocessor.

The manifest has six top-level blocks:

| Block        | What it contains                                                            |
|--------------|-----------------------------------------------------------------------------|
| `details`    | Package identity: name, version, description, license, authors, lang-version |
| `properties` | `${PROPERTY}` substitution variables (see [Properties](#properties))         |
| `settings`   | Everything tool-related: dependencies, repositories, build flavor, capabilities, profiles, overrides, lint, docs, cache, resources, sandbox |
| `actions`    | User-defined action presets — partial bindings of native or plugin actions, reusable across tasks |
| `plugins`    | Plugin declarations (see [Plugins](#plugins))                                |
| `tasks`      | Task definitions — named sequences of action invocations the user runs via `cajeta <task>` |

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
        // flavors". Active flavor is overridden by --flavor / profile.
        "build": {
            "flavor":       "release",
            "target":       "host",
            "entry-method": "com.example.Main::main",
            "sanitizer":    "none",
            "cache": {
                "max-size": "5GiB",
                "ttl":      "30d"
            }
        },

        // Profile overlays (see "Profiles").
        "profiles": {
            "ci":      { "settings": { "build": { "flavor": "instrumented" } } },
            "release": { "settings": { "build": { "flavor": "release" } } }
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
- **Profiles** (under `settings.profiles`) are named overlays
  applied on top of the base manifest fields. Activate via
  `cajeta build --profile=<name>` or `cajeta profile activate <name>`.
  See [Profiles](#profiles).

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
| `profile`      | string                        | no       | Profile to activate for the duration of this task only.                 |
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
| `${profile}`            | Active profile name, or empty string                             |
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
3. **Profile.** A profile's `properties` block overrides
   manifest-level properties while the profile is active.
4. **Manifest.** The `properties` block in `cajeta.json`.

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
- ✓ profile overlay fields
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
`name@version` wins. Filesystem repositories with high priority
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

Named bundles of build settings. Project's manifest picks a
default; CLI flag overrides per invocation.

| Flavor          | Compiler flags                                                            |
|-----------------|---------------------------------------------------------------------------|
| `release`       | `-O2 --lto=thin --strip-symbols --debug-info=line`                        |
| `debug`         | `-O0 --debug-info=full --bounds=on`                                       |
| `debug-release` | `-O2 --debug-info=full --bounds=on` (release perf + debug info)           |
| `fast`          | `-O3 --lto=thin --debug-info=off` (max perf, no debug info)               |
| `minimal`       | `-Oz --lto=full --strip-symbols --debug-info=off` (min size)              |
| `instrumented`  | `-O0 --debug-info=full --sanitizer=address --bounds=on` (asan + bounds)   |

Projects can define custom flavors in `cajeta.json`:

```jsonc
"build": {
    "flavor": "release",
    "custom-flavors": {
        "tracing": {
            "extends": "release",
            "flags": ["--debug-info=full", "--asyncProfilerHooks=on"]
        }
    }
}
```

Then `cajeta build --flavor=tracing` materializes the resolved
flag set.

---

## Profiles

A **profile** is a named overlay applied on top of the manifest
when activated. Profiles let one project ship under multiple
contexts (local dev, CI, staging, production) without forking the
manifest, without conditional shell logic, and without the
Turing-complete-build-script trap.

Distinct from build flavors: flavors are compiler-flag bundles
(`-O2`, `--sanitizer=address`); profiles overlay arbitrary
manifest fields (which flavor to use, which repositories, which
distribution actions, which overrides, which capabilities).

Profiles live under `profiles` in `cajeta.json`:

```jsonc
"profiles": {
    // CI profile: instrumented build, mirror repo first, no
    // distribution (CI verifies; release profile ships).
    "ci": {
        "build":  { "flavor": "instrumented" },
        "repositories": [
            { "name": "ci-mirror", "url": "https://ci.internal/cajeta", "priority": 250 }
        ]
    },

    // Local development: debug build, allow extra capabilities
    // for ad-hoc instrumentation.
    "dev": {
        "build":        { "flavor": "debug" },
        "capabilities": ["network", "filesystem", "env", "clock", "reflection"]
    },

    // Production release: signed, uploaded to S3.
    "release": {
        "build": { "flavor": "release" },
        "distribute": [
            { "action": "sign",      "key-env": "RELEASE_SIGN_KEY_PEM", "key-id": "release-2026" },
            { "action": "upload-s3", "bucket":  "my-org-releases",
              "prefix": "${package.name}/${package.version}/" }
        ]
    }
}
```

### Overlay semantics

When a profile is active, its fields are merged onto the base
manifest using JSON-Merge-Patch semantics (RFC 7396):

- **Objects** merge recursively. `build.flavor` in the profile
  replaces `build.flavor` in the base; other `build.*` fields
  carry over.
- **Arrays** replace wholesale by default. To append rather than
  replace, prefix the field name with `+` in the profile
  (`"+repositories"`, `"+distribute"`).
- **Scalars** replace.
- **null** in a profile deletes the corresponding base field.

Example: the `ci` profile's `repositories` array replaces the
base list; the `release` profile's `+distribute` (if written as
`"+distribute"`) appends to the base `distribute` list rather
than replacing it.

### Activation

Profiles activate by name. Precedence order:

1. **CLI flag.** `cajeta build --profile=release` activates
   exactly that profile for the invocation.
2. **Environment variable.** `CAJETA_PROFILE=ci cajeta build`
   activates for the process.
3. **Stored default.** `cajeta profile activate <name>` writes
   the active profile to `.cajeta/profile` (a one-line file
   committed locally, NOT to VCS). Sticky across invocations.
4. **Manifest default.** `"profiles.default"` in the manifest, if
   set, applies when nothing else activates a profile.
5. **None.** Base manifest, no overlay.

Multiple profile activation: `--profile=a,b,c` applies them in
order, each merging onto the cumulative result. Used for
composing orthogonal concerns (`--profile=ci,instrumented`).

### Subcommand: `cajeta profile`

```
cajeta profile               # show active profile + available profiles
cajeta profile list          # list profile names from cajeta.json
cajeta profile show <name>   # print the merged manifest under profile <name>
cajeta profile activate <name>   # set the sticky default
cajeta profile deactivate    # clear the sticky default
cajeta profile diff <a> <b>  # show field-level differences between profiles
```

`cajeta profile show <name>` is the canonical way to verify what
a profile resolves to without actually running a build. Useful in
PR review when a profile overlay changes.

### Activation conditions (deferred)

Maven supports auto-activation based on OS, JDK version, env var
presence, etc. (`<activation>` block). Cajeta v1 keeps profiles
explicit-activation-only — auto-activation adds non-obvious
behavior that's hard to debug. The escape valve is the
environment variable: a wrapper script or CI configuration can
set `CAJETA_PROFILE` based on whatever conditions matter.

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
| `build`    | —                                 | `flavor`, `target`, `modules`, `incremental` (default `true`)          | `path` (built `.cja`), `sha256`, `size`               |
| `clean`    | —                                 | `paths` (default: `build/` + `.cajeta/work/`), `deep` (also wipes cache) | —                                                    |
| `test`     | —                                 | `filter`, `parallel`, `coverage`, `report`                              | `passed`, `failed`, `crashed`, `report-path`         |
| `lint`     | —                                 | `plugins` (subset), `fail-on-severity`, `format`, `output`              | `findings`, `report-path`                            |
| `doc`      | —                                 | `output` (default: `build/docs/`)                                       | `output-path`                                        |
| `fmt`      | —                                 | `check-only`                                                            | `changed-count`                                      |

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
`cajeta.json` with a starter `tasks` block implementing the
Maven-shaped lifecycle as discrete tasks, plus a starter
`actions` block for the most common preset (a `ship` composition).
**Every entry is editable.** Delete what you don't need; rewrite
what doesn't fit; add what's missing. The build tool ships no
opinion about which tasks exist — `cajeta init` provides
sensible defaults, the project owns them from that point on.

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
            "flavor":       "release",
            "target":       "host",
            "entry-method": "<name>.Main::main"
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

        "build": {
            "description": "Compile sources to a .cja archive",
            "params": {
                "flavor": { "type": "string", "default": "release" },
                "target": { "type": "string", "default": "host" }
            },
            "actions": [
                { "action": "build",
                  "flavor": "${params.flavor}",
                  "target": "${params.target}",
                  "id":     "art" }
            ],
            "outputs": { "path": "${art.path}", "sha256": "${art.sha256}" }
        },

        "clean": {
            "description": "Wipe build outputs",
            "actions": [ { "action": "clean" } ]
        },

        "test": {
            "description": "Build + run tests with coverage",
            "depends-on": ["build"],
            "params": {
                "filter":   { "type": "string", "default": "" },
                "parallel": { "type": "bool",   "default": true }
            },
            "actions": [
                { "action":   "test",
                  "filter":   "${params.filter}",
                  "parallel": "${params.parallel}",
                  "coverage": true,
                  "id":       "tr" }
            ],
            "outputs": { "passed": "${tr.passed}", "failed": "${tr.failed}" }
        },

        "run": {
            "description": "Build + execute the entry method",
            "depends-on": ["build"],
            "actions": [
                { "action": "exec",
                  "command": "${details.name}",
                  "args":    [] }
            ]
        },

        "lint": {
            "description": "Static analysis (built-ins + analyze-phase plugins)",
            "actions": [ { "action": "lint", "id": "ln" } ],
            "outputs": { "findings": "${ln.findings}" }
        },

        "doc": {
            "description": "Generate documentation",
            "actions": [ { "action": "doc", "id": "d" } ],
            "outputs": { "path": "${d.output-path}" }
        },

        "fmt": {
            "description": "Format source",
            "params": {
                "check-only": { "type": "bool", "default": false }
            },
            "actions": [
                { "action": "fmt", "check-only": "${params.check-only}" }
            ]
        },

        "check": {
            "description": "Parse + typecheck only (fast IDE / pre-commit gate)",
            "actions": [
                { "action": "build", "incremental": true, "target": "check" }
            ]
        },

        "package": {
            "description": "Build the deployable artifact",
            "depends-on": ["test"],
            "actions": [
                { "action": "build", "flavor": "release", "id": "art" }
            ],
            "outputs": { "path": "${art.path}" }
        },

        "install": {
            "description": "Install into the local download cache",
            "depends-on": ["package"],
            "actions": [
                { "action": "install", "input": "${package.path}" }
            ]
        },

        "publish": {
            "description": "Build + sign + push to a repository",
            "depends-on": ["test"],
            "actions": [
                { "action": "build", "flavor": "release", "id": "art" },
                { "action": "ship",  "art":    "${art.path}" },
                { "action": "publish", "repository": "central" }
            ]
        },

        "release": {
            "description": "Bump version + publish",
            "depends-on": ["test"],
            "params": {
                "version": { "type": "string", "required": true,
                             "doc": "Release version (semver)" }
            },
            "actions": [
                { "action": "version", "set": "${params.version}" },
                { "run-task": "publish" }
            ]
        }
    }
}
```

### Customizing the template

Three common patterns:

- **Edit in place.** Change `tasks.test` to invoke a different
  test runner, add coverage thresholds, switch parallelism —
  the manifest is the truth.
- **Delete.** A project that doesn't ship docs deletes
  `tasks.doc`; `cajeta doc` then errors with "no such task,"
  surfacing the intent.
- **Add.** A project with bespoke flows adds new tasks freely.
  `cajeta tasks` lists them; `cajeta my-custom-task` runs them.

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
10. **Profiles + `cajeta profile`.** Manifest overlay activation
    (CLI flag, env var, sticky default). Profiles let one
    project ship under multiple contexts without forking the
    manifest.
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
- **Deprecation lifecycle.** `@deprecated` in source is one
  signal; deprecating an entire repository version is another.
  Should `cajeta upgrade` skip deprecated versions by default?
  Lean: yes, warn loudly when locking against one.
