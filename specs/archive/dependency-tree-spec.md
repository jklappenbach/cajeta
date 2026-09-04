# Dependency tree — spec

> Status: **closed** — 2026-09-04, all five units done. Plan: `agents/archive/dependency-tree-plan.md`.
> JSON output schema: `specs/schemas/deps-output.schema.json`.

## 1. Definition

### 1.1 Purpose
Add a build-tool verb, `cajeta deps`, that prints the dependencies a project
builds against: the direct dependencies, and under each one the dependencies
it brings in, to any depth. Every listed entry names its parent. Output is
plain text, JSON, or CSV. The walk detects cycles and reports them instead
of looping.

### 1.2 Problem
The resolver already discovers every parent → child edge. After it picks a
version it reads the package's manifest sidecar and pushes the children's
constraints back into the solver (`Resolver.cpp:790-806`,
`MvsState::childDeps`). At the API boundary it flattens to one
`ResolvedDependency` per name and the edges are lost (`Resolver.cpp:856-868`).
Nothing in the tool prints them. `BuildTool.md:221` promises that
`cajeta info` prints "the computed dependency tree"; `infoCommand` prints
block counts, properties, lockfile drift, timings and melts, never a tree.
The lockfile is flat as well (`ResolvedPackageEntry` has no parent field),
and `cajeta build` never writes one.

So a developer asking "why is `dev.cajeta.logging 0.7.0` on my classpath,
and who asked for it" has no answer short of reading sidecars under
`~/.olla` by hand.

### 1.3 Scope
- One manifest: `./cajeta.json` or `--manifest=<path>`, its
  `settings.dependencies`.
- Resolution is the resolver `build` uses (`resolveProjectDependencies`):
  overrides, melts, declared repositories, and the implicit `~/.olla` store
  at highest priority. The tree never disagrees with what `build` links.
- The resolver gains a graph-returning entry point. The flat result and
  every existing caller are unchanged.
- Three renderers: text, JSON, CSV. Cycle detection in the walk.

### 1.4 Non-goals
- **`dev-dependencies`.** Every archetype writes the block; no C++ parses it
  (`grep dev-dependencies src/` is empty). Listing what the build does not
  resolve would be false. Separate spec.
- **Path and git dependencies.** The resolver drops entries whose constraint
  is empty (`Resolver.cpp:801`), so they are not built against and are not
  listed. Fixing the resolver is a separate change.
- **Built-in stdlib packages (`cajeta.*`).** Stripped before resolution;
  part of the toolchain, not the graph.
- **Workspace aggregation.** `deps` runs on one member's manifest. A
  `workspace deps` verb can compose it later.
- **A strict, JPMS-style acyclic rule for published artifacts.** `build` follows
  Maven and Gradle instead: it refuses a cycle among workspace members and
  warns on one that arrives through published artifacts (§8).
- **`cajeta vendor`.** Named as planned in `BuildTool.md:244`; it will
  consume the same graph, but is not this spec.
- **Offline mode.** Resolution fetches exactly as `build` does. If a manifest
  sidecar must come from a remote, the network is used and `~/.olla` is
  written through.

### 1.5 Terms
- **Node** — one package, identified by name. MVS picks one version per
  name, so a name maps to exactly one version in a tree.
- **Edge** — a parent declares a child with a constraint. The same child can
  have many parents, each with its own constraint.
- **Root** — the project itself. Its children are the direct dependencies.

## 2. The resolver surfaces the graph

The edges exist in `MvsState::childDeps` and the root set in `directRoot`.
Surfacing them is an additive API.

- **2.1** When resolution completes, the caller can obtain, alongside the
  flat package list, the direct dependencies and, for every resolved
  package, the dependencies its manifest declared: child name and the
  constraint as written.
- **2.2** When the existing flat entry point is called, its result is what it
  was: same entries, same topological order. `BuildAction`, `PackageAction`,
  `Upgrader`, the plugin runtime, the kernel session and the JIT host do not
  change.
- **2.3** When a package's repository provides no manifest sidecar, the
  package is a leaf as today, and the graph records that its children are
  unknown rather than empty.
- **2.4** When the graph is queried for a package, its resolved version,
  supplying repository and checksum are the ones the flat list reports.
- **2.5** When a package is reached from several parents, it is one node with
  several incoming edges. Each edge keeps the constraint its own parent
  wrote.

## 3. The walk

The tree is a depth-first walk of the graph from the root.

- **3.1** When a child is listed, its parent is the node it was reached from,
  never the first node that happened to mention it.
- **3.2** When a package is reached whose subtree has already been printed,
  it is listed under this parent as well, marked **repeated**, and its
  children are not printed again. With `--no-dedupe` its children are
  printed again in full.
- **3.3** When `--depth=N` is given, nodes more than N levels below the root
  are not printed, and a node at level N that has children is marked
  **truncated**. `--depth=1` prints the direct dependencies only;
  `--depth=0` prints the root alone. Default is unlimited.
- **3.4** When a package has no manifest sidecar (2.3), it is marked
  **opaque**, so an empty child list is not read as "no dependencies".
- **3.5** At every level, children are printed in name order, so two runs on
  the same inputs produce identical output in every format.
- **3.6** When the manifest declares no dependencies, the output is the root
  alone: one text line, a JSON root with an empty list, a CSV header with
  no rows. Exit code 0.

## 4. Cycles

The graph can contain cycles: the registry does not forbid a package that
depends, at any distance, on a package that depends on it, and the solver
tolerates the loop. A naive walk over `childDeps` never terminates.

- **4.1** When a child is reached that is already on the path from the root
  to the current node, the edge is listed under its parent, marked
  **cycle**, and is not followed.
- **4.2** When a dependency's name equals the project's own name, that is a
  cycle through the root and is handled by 4.1.
- **4.3** When a package depends on itself, that is a cycle of length one and
  is handled by 4.1.
- **4.4** Every cycle found is reported on stderr once, as the path that
  closes it: `dependency cycle detected: a -> b -> c -> a`. This is the melt
  detector's message shape (`Melt.cpp:140-147`).
- **4.5** When at least one cycle was found, the exit code is 1. The full
  output is still written to stdout first, so the tree that contains the
  cycle can be read.
- **4.6** When no cycle exists, the walk visits every reachable node and
  prints the same tree with or without cycle detection. An acyclic graph is
  the control: detection has no visible effect on it.
- **4.7** The walk terminates on every graph, including one where every
  package depends on every other.

## 5. Output formats

`--format=text|json|csv`, default `text`. `--json` is accepted as shorthand
for `--format=json`, matching `cajeta tasks --json`. Output goes to stdout;
diagnostics go to stderr.

### 5.1 Text
An indented tree. The root line is `<name> <version>`; each child line is
the same, prefixed with box-drawing guides. A status follows the version in
parentheses.

```
myapp 1.0.0
├── dev.cajeta.codec 0.8.1
│   └── dev.cajeta.logging 0.7.0
├── dev.cajeta.jinja 0.1.0
└── dev.cajeta.logging 0.7.0 (*)
```

- **5.1.1** When the status is repeated, the marker is `(*)`; cycle is
  `(cycle)`; opaque is `(no manifest)`; truncated is `(...)`.
- **5.1.2** When `--ascii` is given, the guides are `|--`, `` `-- `` and
  `|   `, for terminals and logs that cannot show box-drawing characters.
- **5.1.3** The text form shows name, version and status only. The requested
  constraint, repository and checksum are data, and live in JSON and CSV.

### 5.2 JSON
One document. The root is a node of the same shape as every other node, so a
consumer walks it with one function. `dependencies` on the root is the
direct-dependency list.

```json
{
  "name": "myapp",
  "version": "1.0.0",
  "manifest": "/abs/path/cajeta.json",
  "dependencies": [
    {
      "name": "dev.cajeta.codec",
      "version": "0.8.1",
      "requested": "0.8.*",
      "repository": "central",
      "checksum": "sha256:…",
      "dependencies": [
        {
          "name": "dev.cajeta.logging",
          "version": "0.7.0",
          "requested": ">=0.6.0",
          "repository": "central",
          "checksum": "sha256:…",
          "dependencies": []
        }
      ]
    },
    {
      "name": "dev.cajeta.logging",
      "version": "0.7.0",
      "requested": "0.7.0",
      "repository": "central",
      "checksum": "sha256:…",
      "status": "repeated",
      "dependencies": []
    }
  ],
  "cycles": []
}
```

- **5.2.1** Every node has `name`, `version` and `dependencies` (always an
  array, possibly empty). Every non-root node also has `requested`,
  `repository` and `checksum`. `repository` is the repository name from the
  manifest, or `olla` when the implicit local store supplied the artifact.
- **5.2.2** `status` is present only when the node is `repeated`, `cycle`,
  `opaque` or `truncated`. Absence means the children listed are the
  children there are.
- **5.2.3** The root carries `manifest` (absolute path) and `cycles`: an
  array of paths, each an array of names whose first and last entries are
  equal, in the order found. Empty when there are none.
- **5.2.4** The document is pretty-printed with two-space indentation and
  validates against `specs/schemas/deps-output.schema.json`.
- **5.2.5** The renderer is pure (no I/O), so the shape is golden-tested the
  way `renderTasksJson` is (`Task.h:130-140`).

### 5.3 CSV
One row per listed edge, so the parent is a column and a diamond appears
once per parent.

```
parent,name,version,requested,repository,checksum,depth,status
myapp,dev.cajeta.codec,0.8.1,0.8.*,central,sha256:…,1,
dev.cajeta.codec,dev.cajeta.logging,0.7.0,>=0.6.0,central,sha256:…,2,
myapp,dev.cajeta.logging,0.7.0,0.7.0,central,sha256:…,1,repeated
```

- **5.3.1** The header row is always written. `depth` is 1 for direct
  dependencies. `status` is empty or one of the four words.
- **5.3.2** A field containing a comma, a double quote or a newline is quoted
  and inner quotes are doubled (RFC 4180). A range constraint such as
  `>=1.0, <2.0` contains a comma and is the case that matters.
- **5.3.3** Rows appear in walk order (depth-first, children by name), so the
  file reads as a pre-order traversal.

### 5.4 Errors and exit codes
- **5.4.1** When no manifest is found, the verb reports it the way
  `cajeta info` does and exits 1.
- **5.4.2** When resolution fails (unsatisfiable constraint, unreachable
  repository, checksum mismatch), the resolver's error goes to stderr,
  nothing is written to stdout, and the exit code is 1.
- **5.4.3** When `--format` names an unknown format, `--depth` is not a
  non-negative integer, or an unknown flag is given, usage goes to stderr
  and the exit code is 2, matching the other verbs.
- **5.4.4** `cajeta deps --help` prints usage to stdout and exits 0.
- **5.4.5** Exit codes: 0 success and acyclic; 1 cycle found or resolution
  failed; 2 usage.

## 6. Registration

The tool lists its verbs in four places that already drift
(`main.cpp:76-81` advertises `pin`, `show`, `members`, `which` and `verify`,
none of which dispatch).

- **6.1** `cajeta deps` is claimed in `dispatchBuildTool` and in the built-in
  exclusion list of `looksLikeTaskInvocation`, so a manifest task named
  `deps` cannot shadow it.
- **6.2** `cajeta tasks --json` lists
  `{"name": "deps", "description": "Print the dependency tree"}` in
  `builtins`. The `info` entry's description is frozen by
  `TasksJsonTests.cpp:44` and stays as it is; the buildtool-widget contract
  tolerates added entries.
- **6.3** `cajeta --help` lists `deps` next to `info`.
- **6.4** `docs/specification/buildtool/BuildTool.md` lists `deps` in the
  built-in block and gains a section describing the three formats, the
  status words and the exit codes; the `info` line stops promising a
  dependency tree. `docs/guide/03-your-first-project.md` adds it to the
  command table.

## 7. Decisions and open questions

Decisions taken in this draft, for confirmation:

| # | Decision | Alternative considered |
|---|---|---|
| D1 | New verb `deps`, not a flag on `info` | `info --tree`; `info` already has ten flags, and the tree is a different document |
| D2 | `--format=<f>`, with `--json` as an alias | `--json`/`--csv` booleans only; a named format is what a third format needs, and `renderCoverageReports(format)` already sets the precedent |
| D3 | JSON root is the project node; the direct list is `root.dependencies` | a bare top-level array; loses `cycles` and `manifest`, and makes the root a special case for walkers |
| D4 | A repeated subtree is expanded once and marked at later parents, in all formats | expand everywhere by default (`--no-dedupe` provides it) |
| D5 | Cycles: mark, report, exit 1, output still printed | fail before printing; hides the tree the user needs in order to fix it |
| D6 | Box-drawing guides by default, `--ascii` opt-in | ASCII default; Windows consoles are the reason the flag exists |
| D7 | Children sorted by name | manifest declaration order; `llvm::json::Object` does not preserve it, so it is not available |
| D8 | Node identity is the name | `name@version`; MVS picks one version per name, so the two coincide until multi-version transitives land (`build-tool-plan.md:1745`) |

Decided 2026-09-03 (Julian: "follow Java's conventions"):
- **O1** `build` and cycles. Java allows cycles between classes and packages,
  forbids them between JPMS modules, and its build tools refuse them within
  one build while tolerating them across published artifacts. cajeta takes
  the build-tool position: §8.

Open:
- **O2** Should a JSON node carry the artifact path? Left out: it is
  machine-specific, and derivable from `checksum` through the cache layout.

## 8. Cycles in `build`

Maven refuses a cyclic reactor and Gradle a circular project dependency;
both tolerate a cycle among artifacts pulled from a repository, because the
resolved classpath is flat and the loop is invisible to the compiler. The
project owner cannot fix a cycle in someone else's manifest, so refusing
would only block them. cajeta does the same.

- **8.1** When workspace members depend on each other cyclically, the
  workspace build is refused, as it is today
  (`Workspace.cpp:337-397`, `topologicallySortMembers`). No change.
- **8.2** When a project's resolved graph contains a cycle through published
  artifacts, `build` prints `warning: dependency cycle detected: a -> b -> a`
  on stderr, once per cycle, and proceeds.
- **8.3** When it proceeds, the build result is what it would be without the
  warning: the same classpath, the same artifact. The warning changes
  nothing but stderr.
- **8.4** When the graph is acyclic, `build` prints no such warning. This is
  the control for 8.2.
- **8.5** The cycles `build` warns about are exactly the cycles `deps`
  reports for the same manifest: one detector, two callers.
