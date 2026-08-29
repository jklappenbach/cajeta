# Build output layout — where generated files go, and who knows

STATUS: ACTIVE — both decisions made by Julian 2026-08-27: §3.4 build tool
only (the compiler's positionals are unchanged), §5.3 dependency
intermediates per-project. Plan: `agents/build-output-layout-plan.md`.

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

### 3.4 DECIDED (Julian, 2026-08-27) — build tool only
Output destinations are the BUILD TOOL's concern. The compiler keeps its
three positionals unchanged; no `--obj-dir` / `--artifact-dir` is added.
`settings.output` is read by the build tool, which resolves the layout and
passes the already-correct output directory down.

The recommendation had been to add compiler flags, on the grounds that the
positional is the interface that failed. Overruled, and the consequence is
explicit: **every script that invokes `cajeta` directly keeps the bare
positional**, so the layout gives them nothing. That makes §4.1 the whole
of their protection rather than a belt-and-braces addition — it is the one
mechanism that guards a hand-written invocation, and it is why §4.1 sits
in unit 1 with the hygiene work instead of alongside the layout.

Corollary for §5.2: discovery matters MORE under this decision, not less.
A direct invoker cannot be told where things go by a manifest it never
reads, so `cajeta artifact-path` is how a script learns the location
instead of hard-coding it.

## 4. Rules

- **4.1 Generated files never land inside a source tree.** The compiler
  rejects an output directory that **contains cajeta sources** (searched
  recursively, bounded). Direct guard for §2.5, independent of the arity
  fix — it catches the mistake however it arrives, and under §3.4 it is
  the ONLY protection a direct `cajeta` invocation has.

  REVISED while writing the tests (2026-08-27). The first wording was
  "is, contains, or is contained by the source root", which over-fires on
  an ordinary correct invocation: source root `.` with output `./build`
  has the output contained by the source root, yet nothing is polluted —
  sources live in `./src`, artifacts in `./build`. Keying on *contains
  sources* catches every real case (the output dir being the source root,
  or any directory of `.cajeta` files) and cannot fire on a build
  directory, which by definition holds none. The search must be RECURSIVE:
  a source root holds package directories, not loose files, so an
  immediate-children scan sees only directories and waves it through.
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

### 5.3 DECIDED (Julian, 2026-08-27) — per-project
Dependency intermediates live under `build/obj/deps/<module>/`, inside the
consuming project. Always correct, no invalidation protocol, no concurrent-
build story needed, and `clean` stays `rm -rf build`.

The cost is accepted and stated: the same dependency classes recompile in
every consumer, so a machine building codec, jinja, llm and cabra compiles
`dev.cajeta.codec.*` four times. A shared cache keyed by (module, version,
flavor, compiler version) — the natural extension of `.cajeta/cache/` —
remains available as its own later spec if that cost becomes the thing
worth fixing. It is a SPEED optimization; this spec is about correctness,
and correctness should not wait on an invalidation design.

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

  DECIDED (Julian, 2026-08-28) — **purge all of them**. Done for the five
  affected repos and force-pushed. 275 artifact objects were still in
  history after every tip was clean, because untracking removes a file
  from the next commit and never from the ones already written: an audit
  of tips reports clean while the weight remains. The rewrite was gated
  on the tip TREE hash being identical before and after — history
  changes, content does not — which held on all five.
