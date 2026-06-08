# cajeta.lint.security

First-party security-lint plugin for the cajeta build tool. Ships
one action — `cajeta.lint.security.scan` — that walks the project
source tree and emits findings for:

| Check               | What it flags                                           |
|---------------------|---------------------------------------------------------|
| Banned imports      | `import` statements matching the configured glob list.  |
| Secret patterns     | Lines matching any of the configured regex patterns.    |

The user-facing spec is `docs/BuildTool.md` §"Plugins" /
"lint task"; this directory is the implementation.

## Wiring into a project

```jsonc
"plugins": {
    "cajeta.lint.security": {
        "version": "1.0.*",
        "config": {
            "banned-imports": [
                { "pattern": "unsafe.legacy.*",
                  "reason":  "supply-chain risk; use cajeta.io.net instead" },
                { "pattern": "vendor.deprecated.*",
                  "reason":  "vendor end-of-life 2025-12; migrate" }
            ],
            "secret-patterns": [
                { "id":       "aws-access-key",
                  "pattern":  "AKIA[0-9A-Z]{16}",
                  "severity": "error" },
                { "id":       "github-pat",
                  "pattern":  "ghp_[A-Za-z0-9]{36}",
                  "severity": "error" },
                { "id":       "private-key-header",
                  "pattern":  "-----BEGIN [A-Z ]*PRIVATE KEY-----",
                  "severity": "error" }
            ],
            "exclude-files": [
                "**/*_test_fixture.cajeta",
                "test/secrets/**"
            ]
        }
    }
}
```

Wire into the `lint` task (replaces the default template):

```jsonc
"tasks": {
    "lint": {
        "actions": [
            { "action": "cajeta.lint.security.scan", "id": "sec" },
            { "action": "lint",
              "include-findings": ["${sec.findings}"],
              "id": "ln" }
        ],
        "outputs": { "findings": "${ln.findings}" }
    }
}
```

## Action contract

### `cajeta.lint.security.scan`

| Direction | Field             | Type     | Notes                                         |
|-----------|-------------------|----------|-----------------------------------------------|
| in        | `source-roots`    | string[] | Source roots to walk. Default: `["src/main"]`. |
| in        | `banned-imports`  | object[] | `{pattern, reason}`. Pattern is glob; reason is mandatory. |
| in        | `secret-patterns` | object[] | `{id, pattern, severity}`. Pattern is regex; severity ∈ {error, warning, info}. |
| in        | `exclude-files`   | string[] | Glob patterns. Excluded files aren't scanned. |
| out       | `findings`        | json     | One Finding per hit. Severity defaults to warning for banned imports, per-pattern for secrets. |
| out       | `files-scanned`   | int      | Number of source files visited.               |
| out       | `findings-count`  | int      | Total findings emitted.                       |

## Config shape

### `banned-imports`

Each entry is `{ "pattern": "<glob>", "reason": "<text>" }`.

- `pattern` matches against the dotted import path. Glob dialect:
  `*` matches any segment-content, `?` matches one char, no `**`
  (imports are dotted names, not file paths). `com.foo.*` matches
  `com.foo.bar` and `com.foo.bar.baz` (suffix-anchored).
- `reason` is mandatory and shows up in the finding's message so
  reviewers see *why* the import is banned without digging into
  the manifest.

### `secret-patterns`

Each entry is `{ "id": "<slug>", "pattern": "<regex>", "severity": "<level>" }`.

- `id` is a short slug used as the finding's rule ID — keep stable
  across runs so CI's "new findings since last build" diff stays
  meaningful.
- `pattern` is a regex. v1 supports the standard ASCII regex
  subset (character classes, quantifiers, anchors, groups, `\d`,
  `\w`, `\s`); see `internal/Regex.cajeta` for the precise
  dialect.
- `severity` ∈ `error` / `warning` / `info`. The default `lint`
  task fails on `--fail-on-severity=warning` and above.

### `exclude-files`

Same glob dialect as the coverage plugin's file-kind excludes
(`*` `?` `**`, `/`-aware). Excluded files aren't read or scanned.

## Source layout

```
src/main/cajeta/cajeta/lint/security/
├── Scan.cajeta                    — entry symbol for cajeta.lint.security.scan
├── BannedImports.cajeta           — banned-imports config + matcher
├── SecretPatterns.cajeta          — secret-patterns config + matcher
├── BannedImportEntry.cajeta       — typed config entry
├── SecretPatternEntry.cajeta      — typed config entry
└── internal/
    ├── SourceWalker.cajeta        — walk *.cajeta files under source roots
    ├── ImportExtractor.cajeta     — pull `import X;` lines out of source
    ├── LineScanner.cajeta         — per-line scan for secret patterns
    └── Regex.cajeta               — minimal regex engine
```

## Notes for security operators

- The plugin runs locally — no network, no telemetry. Findings stay
  on the machine running the build.
- Findings are advisory; threshold gating (fail the build) happens
  at the `lint` task level via `--fail-on-severity=...`, not in
  this plugin. The plugin's job is to *find* things, not to *decide*
  whether to fail.
- Secret patterns are intentionally regex (not a fixed list of
  vendor formats) so operators can add their own internal token
  formats without waiting for a plugin update.
- False positives on secret patterns hurt — the operator who
  silenced the scanner because it cried wolf is the operator who
  misses the real leak. Pin patterns precisely; use `exclude-files`
  to suppress test fixtures rather than loosening patterns
  globally.
