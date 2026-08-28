# Build output layout — where generated files go, and who knows

STATUS: DRAFT — two decisions open (§3.4, §5.3), marked **DECIDE**.
Plan: `agents/build-output-layout-plan.md`.

## 1. Definition

Cajeta generates three distinct kinds of file and today has no declared
home for any of them. The compiler takes its output directory as a bare
positional argument; the build tool has its own idea of `build/`; and the
two are related only by convention. Nothing validates that the directory
it is handed is not something else — a source tree, for instance.

This spec defines an **output layout**: named roles, default locations,
manifest settings that override them, and the rule that generated files
never land beside sources.

**Why now.** On 2026-08-27 a compile that was handed two source roots
bound the second to the output directory and wrote object files into it,
at exit 0, with no diagnostic. 180 objects (24 MB) landed in
`cajeta-cabra/src/main/cajeta/` and 75 (8 MB) in
`cajeta-llm/src/main/cajeta/`, and both were then committed by a routine
`git add -A`. The arity itself is now rejected (cajeta `c963d19b`), but
that fix only closes one way in. The deeper problem is that "the output
directory" is a positional string with no role, no default, and no
validation, so *any* mistake at that position silently redirects the
build's entire output.

**Non-goals.** A general artifact cache across machines; reproducible-build
path remapping (already covered by `--debug-prefix-map`); packaging or
publication layout (olla's); changing the `.cja` format.

## 2. Use cases

- **2.1 A library build.** `cajeta build` in `cajeta-codec` compiles
  sources and dependency classes, and emits `dev.cajeta.codec-0.8.1.cja`.
  Intermediates must not appear in `src/`, and `clean` must remove them
  without touching anything a human wrote.
- **2.2 An application build.** `cajeta-cabra` emits an executable plus
  the intermediates for its own classes AND for every dependency class it
  pulled in (`dev.cajeta.codec.*`, `dev.cajeta.jinja.*`). Dependency
  intermediates are not this project's source and should be
  distinguishable from its own.
- **2.3 A consumer resolving a dependency.** cabra's `run-tests.sh` looks
  for `../cajeta-codec/build/archive/dev.cajeta.codec-*.cja`. Today that
  path is hard-coded in shell in every consumer. If the artifact location
  is configurable, every consumer must be able to ASK where it is rather
  than guess.
- **2.4 A developer overriding locations.** Out-of-tree builds, a
  scratch/tmpfs intermediates directory, or CI wanting artifacts collected
  in one place.
- **2.5 The mistake that started this.** A wrong path at the output
  position must fail loudly, and must never write into a directory that
  contains sources.

## 3. Roles and defaults

### 3.1 The three roles
| Role | Holds | Default |
|---|---|---|
| **intermediates** | per-class objects, bitcode, staging | `build/obj/` |
| **artifacts** | the deliverable: `.cja`, executable, installers | `build/archive/`, `build/bin/` |
| **cache** | reusable across builds, safe to delete anytime | `.cajeta/cache/` (already correct) |

`build/` is the single root all three default under (cache excepted, which
is already separate and working). One root keeps `clean` honest and keeps
`.gitignore` to one line.

### 3.2 Dependency intermediates
Dependency classes compile into `build/obj/deps/<module>/` rather than
mixing with the project's own `build/obj/`. This is what makes 2.2
legible and lets a future `clean --keep-deps` avoid a full dependency
rebuild.

### 3.3 Manifest settings
Under `settings.output` in `cajeta.json`, all optional, all relative to
the project root unless absolute:

```jsonc
"output": {
    "root":          "build",           // the one knob most projects touch
    "intermediates": "build/obj",
    "artifacts":     "build/archive",
    "binaries":      "build/bin"
}
```
Setting `root` moves the others unless they are set explicitly.

### 3.4 **DECIDE** — compiler flags, or build tool only?
The positional is what failed, so the recommendation is that the COMPILER
grows explicit `--obj-dir` / `--artifact-dir`, the bare positional becomes
deprecated-but-accepted, and the build tool always passes the flags. The
alternative — build-tool settings only, positional unchanged — leaves the
failing interface in place for everyone invoking `cajeta` directly, which
is every script in every repo today.

## 4. Rules

- **4.1 Generated files never land inside a source root.** The compiler
  rejects an output directory that is, contains, or is contained by a
  declared source root. This is the direct guard for §2.5, and it is
  independent of the arity fix — it catches the mistake however it arrives.
- **4.2 Everything generated lives under one root** so `clean` is
  `rm -rf build` and `.gitignore` is `build/`.
- **4.3 The layout is discoverable, not guessed.** A consumer asks the
  build tool where a project's artifact is (§5.3) instead of hard-coding
  `build/archive/*.cja`.
- **4.4 Defaults require no configuration.** A project with no `output`
  block behaves exactly as today's working projects do.
- **4.5 Overrides are validated on load**, not on first write — a bad
  `output.root` fails at manifest parse with the offending value named.

## 5. Discovery for consumers

### 5.1 The problem
Every consumer hard-codes the artifact path in shell:
`ls -t "$repo"/build/archive/$name-*.cja | head -1`. That breaks the
moment §3.3 lets a project move its artifacts, and it already duplicates
version-resolution logic across `cajeta-llm`, `cajeta-cabra`, and others.

### 5.2 Requirement
A project must be able to report where its artifact will be or was
written, without a build. Shape: `cajeta artifact-path [--flavor debug|
release]` printing one absolute path, exit non-zero if the manifest does
not define one.

### 5.3 **DECIDE** — dependency intermediates: shared or per-project?
Per-project under `build/obj/deps/` is simple and always correct, at the
cost of recompiling the same dependency classes in every consumer. A
shared cache keyed by (module, version, flavor, compiler version) is
faster and is the natural extension of `.cajeta/cache/`, but needs
invalidation discipline and a story for concurrent builds. Recommendation:
per-project now (this spec), shared cache as its own later spec — the
correctness win here is the layout, not the speed.

## 6. Repository hygiene (the existing damage)

Separate from the mechanism, the artifacts already committed must go.
Audited 2026-08-27 across every published cajeta repo:

| Repo | Committed artifacts | `build/` ignored |
|---|---|---|
| cajeta-llm | 74 | yes |
| cajeta-recsys | 6 | yes |
| cajeta-timeseries | 6 | yes |
| cajeta-docs | 2 | yes |
| cajeta-cloud | 1 | yes |
| cajeta-agents | 0 | **NO** |
| cajeta-olla | 0 | **NO** |
| cajeta-caramelo | 0 | **no .gitignore at all** |

All other cajeta repos are clean on both counts.

- **6.1** Untracking is safe and unilateral.
- **6.2** Purging HISTORY is not: these are public repos whose `main`
  other clones have already pulled, so a force-push is a per-repo
  decision by the developer, not a default. cajeta-cabra was different —
  it had never been pushed, so its history was rewritten before creation.
