# Manifest dependencies → compile classpath — spec (defect/gap)

Found by the tour-quality unit review (findings/unit.md, Defects). Verify
current behavior first — the tour and its README may predate build-tool work.

## 1. Definition

- 1.1 Reported: `cajeta.json` `dependencies` are inert for local project
  builds — cajeta-unit's tour `build.sh` hand-builds the library `.cja` and
  passes `--classpath` manually (`samples/tour/README.md:18-21`).
- 1.2 Expected: `cajeta build` / `cajeta test` resolve manifest dependencies
  (local repository or Olla) and thread the resolved archives onto the
  compile/link classpath without hand plumbing.

## 2. Use cases

- 2.1 As a developer, when my `cajeta.json` declares a dependency that is
  installed locally or published to Olla, then `cajeta build` compiles against
  it with no manual `--classpath`.
- 2.2 As the cajeta-unit tour, when 2.1 holds, then `build.sh`'s hand-built
  classpath plumbing is deleted and the manifest alone drives the build.

## 3. Acceptance

- 3.1 Buildtool test: project with a manifest dependency builds green from the
  manifest alone; tour build scripts simplified accordingly.

---

**CLOSED — verified fixed on cajeta 0.14.0 (8ca5b362), 2026-08-01.** Re-ran this
spec's repro against a freshly built 0.14.0 compiler; the defect no longer
reproduces. Archived per td-project-workflow (spec -> archive, INDEX row dropped).
