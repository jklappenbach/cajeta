# Executable output path collides with a same-named source package — spec (defect)

Found 2026-08-02 while characterizing `buildtool-dependency-classpath` U1
against released v0.14.0 (`bddcbf42`). Unrelated to dependency resolution;
surfaced there because it is what actually blocks `cajeta-unit`'s tour from
building off its manifest.

## 1. Definition

- 1.1 An `--emit=exe` build writes the executable to
  `<output-dir>/exe/<details.name>` (`BuildAction.cpp:268-269`).
- 1.2 The project's **own** object files are emitted under that same
  `<output-dir>/exe/` as a **directory tree mirroring the package path** —
  package `tour.assertions` becomes `exe/tour/assertions/`.
- 1.3 When `details.name` equals a top-level source package name, 1.1 and 1.2
  name the same path. The compile succeeds; the **link** then fails:

  > `ld.lld: error: cannot open output file build/exe/app: Is a directory`

- 1.4 The asymmetry that causes it: a **dependency's** objects are emitted as
  flat dotted files (`dev.cajeta.unit.Assert.o`), while the **project's own**
  are a directory tree. Only the latter can collide with the exe path.
- 1.5 Reproduced minimally — `details.name` `app` with sources in package `app`
  fails; `details.name` `myapp` with the same package `app` builds and runs.
  The trigger is exact equality with a *top-level* package name, so it is
  reached by the most natural naming a developer can choose.

## 2. Use cases

- 2.1 When a project's `details.name` matches one of its source package names,
  the build either emits the executable somewhere that cannot collide or fails
  with a diagnostic naming the collision — never a linker error about a
  directory.
- 2.2 When `cajeta-unit`'s tour (`details.name` `tour`, sources in package
  `tour`) builds from its manifest, it produces a runnable binary.
- 2.3 When an existing project already builds, its executable path does not
  move — whatever fixes 2.1 must not silently relocate outputs that scripts and
  CI already reference.

## 3. Design notes

- 3.1 Three candidate fixes, in decreasing order of how much they disturb
  existing layouts: emit objects to a sibling directory (`exe/` for the binary,
  `obj/` for objects); flatten the project's own objects to dotted names, which
  makes them consistent with dependency objects and removes the collision
  class entirely; or detect the collision and error early. **The second is
  worth costing first** — the inconsistency in 1.4 looks unintentional, and
  removing it fixes the defect as a side effect rather than working around it.
- 3.2 Whichever is chosen, 2.3 constrains it: `cajeta-six/samples/tour` and
  every CI script that references `build/exe/<name>` must keep working.

## 4. Acceptance

- 4.1 Buildtool test: a project whose `details.name` equals a top-level source
  package name builds, links, and runs.
- 4.2 The negative control still passes — a project whose name differs from its
  packages is unaffected, and its output path is unchanged.
- 4.3 `cajeta-unit`'s tour builds from its manifest with no hand-rolled
  compile step (this is `buildtool-dependency-classpath` §3.1's second half,
  which cannot close until this does).
