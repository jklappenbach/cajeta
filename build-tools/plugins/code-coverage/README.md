# cajeta.coverage

First-party code coverage plugin for the cajeta build tool. Ships three
actions consumed by the `test` task:

| Action                          | Purpose                                                          |
|---------------------------------|------------------------------------------------------------------|
| `cajeta.coverage.instrument`    | Build the project with probe-points emitted at line/branch/region grain. |
| `cajeta.coverage.collect`       | Reduce probe hits across all test runs into a coverage map.      |
| `cajeta.coverage.report`        | Emit configured reports (html/sarif/lcov/console); gate on `min`. |

The user-facing spec is `docs/BuildTool.md` §"Code coverage";
this directory is the implementation.

## Wiring into a project

Declare the plugin in `cajeta.json`:

```jsonc
"plugins": {
    "cajeta.coverage": {
        "version": "1.0.*",
        "config": {
            "grain":        "line",      // "line" | "branch" | "region"
            "min":          80,           // overall percentage gate
            "min-per-file": 50,           // per-file floor (optional)
            "report":       ["html", "sarif", "console"],
            "exclude": [
                { "kind":    "file",
                  "pattern": "**/*_generated.cajeta",
                  "reason":  "machine-generated; covered via integration" },
                { "kind":    "package",
                  "pattern": "com.example.mock.*",
                  "reason":  "test scaffolding" },
                { "kind":    "symbol",
                  "pattern": "com.example.Foo.getCount",
                  "reason":  "trivial accessor; tested implicitly via every use" }
            ]
        }
    }
}
```

### Exclude entries

Each `exclude` entry is one of three kinds — `file`, `package`, or
`symbol` — paired with a glob `pattern` and a mandatory `reason`.
The reason shows up on every PR diff that touches the exclude list,
so it has to justify itself; the plugin rejects empty / generic
reasons (`wip`, `todo`, `skip`, `fixme`, `tbd`) at config-parse time.

| Kind     | Pattern matches                        | Glob dialect              |
|----------|----------------------------------------|---------------------------|
| `file`   | Source path                            | `*` `?` `**` (path-aware) |
| `package`| Dotted package name (`com.foo.mock.*`) | `*` `?` only              |
| `symbol` | Qualified declaration name             | `*` `?` only              |

Excluded probes are subtracted from **both** numerator and
denominator — opting out doesn't game the percentage upward, it
removes the code from measurement entirely.

Back-compat: bare-string entries (`"exclude": ["**/*.gen"]`) are
treated as `kind=file` with an empty reason. The plugin emits one
warning per such entry on every run, inviting migration to the
typed form. Use `cajeta coverage ignore --kind=file --pattern=...
--reason=...` to add new entries; the CLI shells in directly to a
JSONC-preserving manifest mutator so existing comments and indent
style survive the edit.

Wire into the `test` task:

```jsonc
"tasks": {
    "test": {
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

## Action contracts

### `cajeta.coverage.instrument`

| Direction | Field          | Type     | Notes                                                |
|-----------|----------------|----------|------------------------------------------------------|
| in        | `grain`        | string   | `"line"`, `"branch"`, or `"region"`. Default `"line"`. |
| in        | `flavor`       | string   | Forwarded to the underlying build action.            |
| in        | `profile`      | string   | Forwarded to the underlying build action.            |
| in        | `exclude`      | string[] | Glob patterns the compiler emits without probes.     |
| out       | `path`         | string   | Path to the instrumented `.cja` artifact.            |
| out       | `probe-count`  | int      | Number of probe-points emitted (for telemetry).      |

Mechanism: spawns the cajeta compiler subprocess with
`--instrument=coverage --coverage-grain=<grain>` and the plugin's
own `<exclude>` rewritten as compiler exclude flags. The compiler
emits per-source coverage maps alongside IR.

### `cajeta.coverage.collect`

| Direction | Field      | Type   | Notes                                            |
|-----------|------------|--------|--------------------------------------------------|
| in        | `input`    | string | Path to the probe-data directory (test runner writes here). |
| in        | `output`   | string | Where to write the merged coverage map. Default `build/coverage.map`. |
| out       | `path`     | string | Path to the merged coverage map.                 |
| out       | `files`    | int    | Number of source files reduced.                  |

Mechanism: walks the input directory, reads each per-test probe
counter dump, merges into a per-file → per-probe hit table. Output
is one self-contained binary file the report action consumes.

### `cajeta.coverage.report`

| Direction | Field            | Type     | Notes                                           |
|-----------|------------------|----------|-------------------------------------------------|
| in        | `input`          | string   | Path to the merged coverage map.                |
| in        | `min`            | int      | Overall percentage gate. Action fails when actual < min. |
| in        | `min-per-file`   | int      | Per-file floor. Optional.                       |
| in        | `report`         | string[] | Any subset of `["html","sarif","lcov","console"]`. |
| in        | `output-dir`     | string   | Where reports go. Default `build/coverage/`.    |
| out       | `percent`        | string   | Overall percent (formatted, e.g. `"83.5"`).     |
| out       | `html-path`      | string   | Path to HTML index (empty if not requested).    |
| out       | `sarif-path`     | string   | Path to SARIF file (empty if not requested).    |
| out       | `lcov-path`      | string   | Path to lcov.info (empty if not requested).     |
| out       | `findings`       | json     | Structured findings: one per uncovered line/branch. |

The action fails the task when the overall percentage is below
`min` or any file falls below `min-per-file`; the error message
cites the bottom-N files so the developer doesn't have to dig.

## Source layout

```
src/main/cajeta/cajeta/coverage/
├── Instrument.cajeta            — entry symbol for cajeta.coverage.instrument
├── Collect.cajeta               — entry symbol for cajeta.coverage.collect
├── Report.cajeta                — entry symbol for cajeta.coverage.report
├── CoverageMap.cajeta           — merged map model (per-file → per-probe hits)
├── ProbeData.cajeta             — raw per-test dump format
├── Grain.cajeta                 — line/branch/region enum
├── Exclude.cajeta               — typed exclude entry record
├── ExcludeKind.cajeta           — file/package/symbol enum
├── ExcludeParser.cajeta         — config-block reader (typed + back-compat string form)
├── ExcludeMatcher.cajeta        — per-kind match predicates + Glob helper
├── ExcludeConfigException.cajeta
├── format/
│   ├── ConsoleFormat.cajeta     — overall % + bottom-N
│   ├── HtmlFormat.cajeta        — annotated per-file source
│   ├── SarifFormat.cajeta       — SARIF 2.1.0 JSON
│   └── LcovFormat.cajeta        — lcov.info tracefile
└── internal/
    ├── BottomN.cajeta           — partial-sort over raw CoverageMap (legacy)
    ├── FilteredCounts.cajeta    — post-exclude per-file aggregates + bottom-N
    ├── FloatFormat.cajeta       — deterministic 1-decimal %
    ├── PositionLookup.cajeta    — probe-id → SourcePosition + package + symbol
    ├── SourcePosition.cajeta
    ├── CoverageMapHeader.cajeta — header-only sidecar read (instrument telemetry)
    ├── CoverageMapReader.cajeta — merged-map binary reader
    ├── CoverageMapWriter.cajeta — merged-map binary writer
    ├── ProbeDataIO.cajeta       — per-test text dump reader + merger
    ├── CoverageMapFormatException.cajeta
    └── ProbeDataFormatException.cajeta
```

## Notes for plugin authors

- The plugin imports `cajeta.buildtool.plugin` which defines the
  `Action`, `ActionContext`, `ActionResult`, and `Finding` types.
  Every plugin's entry symbol has the shape
  `static ActionResult run(ActionContext ctx, Json params)`.
- Capabilities the plugin actually exercises must appear in
  `settings.capabilities`. The build tool checks per-call too — a
  capability you didn't declare but use causes a runtime trap, not
  a quiet ignore. Don't strip capabilities for "we don't need it
  in this version"; the spec says capability changes between plugin
  versions are explicit signal to the consumer.
