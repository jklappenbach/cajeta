# cajeta.coverage — superseded

**This directory is a specification stub, not an implementation.** It holds a
manifest and this README; the source layout the old README described
(`src/main/cajeta/cajeta/coverage/…`, `Instrument.cajeta`, `Collect.cajeta`,
`Report.cajeta`, four report formats) was never written.

Coverage for Cajeta ships as **`dev.cajeta.coverage`**, a build-tool plugin
adapting **`dev.cajeta.coco`** (the `cajeta-coco` repository) to the action
contracts. It differs from what was specified here in ways that matter to
anyone copying config out of this directory:

| Here (never built) | What ships |
|---|---|
| plugin id `cajeta.coverage` | `dev.cajeta.coverage` |
| three actions — `instrument`, `collect`, `report` | **two** — `instrument`, `report`. Instrumenting and running are one step; the probe dump is written by the measured program when the entry method returns. |
| `grain`, `min-per-file`, `report[]` | not implemented |
| — | `src`, `entry`, `out`, `exclude`, `classpath`, `profile`, `min` |
| `@nocoverage("reason")` in source | not implemented. The typed-exclude CLI (`cajeta coverage ignore\|list\|remove`) does exist. |

coco also reports what a percentage cannot: uncovered code split into
statically-unreachable (delete) versus reachable-but-untested (test), per-test
attribution, a CRAP risk ranking, and mutation survivors.

**Read instead:**

- [Guide: 23 — Code coverage](../../../docs/guide/23-code-coverage.md) — the
  configuration reference: every setting, where results land, how to invoke a
  pass, and how it surfaces in the IDE.
- [BuildTool.md § Code coverage](../../../docs/specification/buildtool/BuildTool.md)
  — the plugin in the context of the action catalog, including a "Not yet
  built" list that preserves the design intent recorded here.
- [cajeta-coco's `samples/tour`](https://github.com/jklappenbach/cajeta-coco/blob/main/samples/tour/README.md) — a runnable project with one
  class per finding.

`cajeta.json` in this directory is kept as the record of the original
first-party plugin design.
