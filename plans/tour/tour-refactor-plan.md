# Tour refactor plan

Source of the work: `plans/current-focus.md` → **Tour**. The tour
(`samples/Tour/`) is a working, comprehensive language walkthrough (47 demo
`.cajeta` files + `Tour.cajeta` driver, built native via `build-bin.sh` or to a
`.cja` via `build-uber.sh`; a separate `xpu/` sub-tour). This plan turns that
flat sample into a **proper cajeta project** organized by package, dogfooding
the build tool.

Every unit of work is a checkbox `- [ ]`; `- [x]` when shipped.

## Progress

- [x] **Lowercase `tour`** — `samples/Tour/` → `samples/tour/` (done outside git
      by Julian; files re-tracked under the new path).
- [x] **Refactor into a proper cajeta project with a build file** — adopted the
      standard layout (`cajeta.json` at root, sources under
      `src/main/cajeta/tour/`), modeled on `samples/buildtool/basic`. Root
      `build.sh`/`run.sh` (+ `.cmd`) are thin wrappers that drive the **build
      tool** (`cajeta build` / `cajeta run`); the hand-rolled
      `build-bin.sh`/`build-uber.sh`/`build-execute-*` scripts were removed.
      Verified: `cajeta build` → `build/tour` (ELF), `cajeta run` executes all
      37 demo sections. Required a compiler fix — the `--emit=exe` cc-fallback
      now passes `-Wl,--gc-sections` (`src/cajeta/compile/Compiler.cpp`) so the
      build tool's `build` task links without `lld`.

## Goals (from focus list)

- [ ] **Organize into a directory layout reflecting major packages.** All 47
      demos are still one flat `tour` package under `src/main/cajeta/tour/`
      (matching the flat `basic` exemplar). Optional follow-up: split into
      sub-packages — `collections/`, `lang/`, `time/`, `concurrent/`, `net/`,
      `views/`, `xpu/` (already separate) — and have the `demos[]` driver walk
      them in package order. See D1.
- [ ] **Add `time` demos** — `cajeta.time` walkthrough (the time plan is
      `plans/time/cajeta-time-plan.md`). Mirror the package once it has a stable
      surface.
- [ ] **Add `concurrent` demos** — scheduler / `Tasks` / `Channel` / cooperative
      cancel. (`AsyncDemo`/`ParallelStreamsDemo` partially cover this — fold them
      into a `concurrent/` package and fill gaps.)
- [ ] **Add `net` demos** — once `cajeta.net` lands (`plans/net/cajeta-net-plan.md`);
      the net plan already reserves "Tour examples" (NET tour items). Keep these
      two lists in sync — this is the consumer side of that deliverable.
- [ ] **Refactor the tour into a proper cajeta project with a build file.**
      Add a `cajeta.json` manifest (build-tool spec: `cajeta-docs/BuildTool.md`)
      so the tour builds via `cajeta build` instead of the hand-rolled
      `build-bin.sh`/`build-uber.sh` shell scripts. This makes the tour the
      flagship **dogfood** of the build tool.
- [ ] **Lowercase `tour`.** Rename `samples/Tour/` → `samples/tour/` (the package
      is already `tour`; only the directory is capitalized). Update READMEs,
      build scripts, the xpu sub-tour path, and any CI / docs-sync references.
- [ ] **Add a reflection demo.** Showcase the reflection surface (whatever the
      language exposes — type introspection, annotations at runtime, field/method
      enumeration). New `ReflectionDemo.cajeta` in the appropriate package.

## Sequence

1. [ ] Inventory: map each of the 47 demos to its target package (one-time
       categorization; record the mapping in the tour README).
2. [ ] Introduce package subdirectories under `src/tour/` and move files;
       update the `demos[]` initializer in `Tour.cajeta` and any cross-file
       references. Verify `build-bin.sh` still produces a passing `build/tour`.
3. [ ] Author `cajeta.json` for the tour; verify `cajeta build` reproduces the
       native binary and the `.cja` archive. Keep the shell scripts until the
       manifest path is proven, then retire them.
4. [ ] Lowercase the directory; fix all references; re-run both build paths.
5. [ ] Add `ReflectionDemo`; wire into `demos[]`.
6. [ ] Backfill `time` / `concurrent` / `net` packages as those subsystems stabilize
       (net is gated on `plans/net`).

## Open decisions (need Julian)

- **D1. Directory grouping granularity.** By stdlib package (`collections/`,
  `lang/`, `time/` …) vs by concept (`memory/`, `generics/`, `control-flow/`)?
  The focus note says "major packages" → leaning package-based. Confirm the
  package list and which demos land where (the inventory step produces a
  concrete proposal to approve).
- **D2. Retire the shell build scripts or keep both?** Once `cajeta.json` works,
  do we delete `build-bin.sh`/`build-uber.sh`, or keep them as a no-build-tool
  fallback for bootstrapping?
- **D3. Reflection scope.** What reflection surface is actually shippable today
  vs aspirational? (Determines whether `ReflectionDemo` is real or a stub with a
  TODO.) Needs a read of the current reflection capabilities before authoring.

## Acceptance

- [ ] `samples/tour/` (lowercase) builds via `cajeta build` to both a native
      binary and a `.cja`, with demos grouped into package directories.
- [ ] `demos[]` runs every demo including `ReflectionDemo`; output unchanged for
      existing demos.
- [ ] READMEs, xpu sub-tour, and docs-sync references updated to the new layout
      and name.
