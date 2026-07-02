# Cajeta IntelliJ IDEA Plugin

First-party IntelliJ IDEA plugin for the Cajeta language. Covers
the v0.1 scope defined in [`Plan.md`](Plan.md):

- **Syntax tier** — file type, ANTLR-driven lexer/parser, PSI,
  syntax highlighting, brace matching, commenter, structure view.
- **Linting tier** — real-time syntax errors plus `cajetac` lint
  warnings surfaced as squigglies and Problems-tool entries.
  Ships in **degraded mode** (regex over stderr) until the
  compiler-side `--lint --diag-format=json --stdin` work lands;
  see Plan.md § Follow-up.
- **Markdown comments** — comment regions render as rich HTML
  (full CommonMark + GFM via the bundled `org.jetbrains:markdown`
  library); caret enters → that comment reverts to raw source for
  editing; caret leaves → re-renders. Obsidian live-preview shape.
  Rendering is implemented in `markdown/MarkdownFoldRenderer.kt`
  (block comments) and `markdown/InlineMarkdownRenderer.kt`
  (trailing `//`), behind the `markdown/engines/` engine seam.
  Deferred (cosmetic): a per-comment gutter-icon toggle and a
  menu toggle-action — a global Settings → Cajeta checkbox covers
  the same need today.

- **Build tool window** — build-family tasks (`compile`, `package`,
  `validate`, `install`, `deploy`, `test`, and non-runnable user
  tasks) run in the IDE's native **Build** tool window: streaming
  console, success/failure/cancelled status, Stop/Restart, and
  compiler diagnostics as problems (navigable when the message
  carries a path). `run` and debuggable tasks stay on the **Run**
  window (they execute your program). Toggle off via *Settings →
  Cajeta → "Run build tasks in the Build tool window"* to route
  everything back to the Run window. See
  `docs/specs/idea-build-toolwindow-spec.md`.

## Build

Requires JDK 21+ on `PATH`. From this directory:

```sh
./gradlew buildPlugin
```

Produces `build/distributions/cajeta-idea-0.1.0.zip`.

The build copies `../../antlr4/CajetaLexer.g4` and
`../../antlr4/CajetaParser.g4` into `src/main/antlr/` (gitignored),
generates Java lexer/parser into `build/generated-src/`, then
compiles Kotlin against that.

## Run in a sandbox IDE

```sh
./gradlew runIde
```

Downloads (cached after first run) IntelliJ IDEA Community
2024.2.4 and launches a clean sandbox with the plugin loaded.

## Install into your IntelliJ

1. Build the distributable per above.
2. IntelliJ → Settings → Plugins → ⚙ → Install Plugin from
   Disk… → select `build/distributions/cajeta-idea-0.1.0.zip`.
3. Restart.
4. Settings → Languages & Frameworks → Cajeta → set the path to
   your `cajetac` binary (default
   `/home/julian/code/cpp/cajeta/build/src/cajeta`).

## Verification samples

Open `~/code/cpp/cajeta/samples/Tour/` as an IntelliJ project.
Recommended files:

- `OwnershipDemo.cajeta` — exercises the parser and structure
  view; deliberate `;` deletion verifies error recovery.
- `AspectsDiDemo.cajeta` — has bulleted `//` markdown comments
  for the comment-folding feature.

See [`Plan.md`](Plan.md) § Verification checklist for the full
per-tier acceptance criteria.

## Layout

```
src/main/
├── kotlin/dev/cajeta/idea/
│   ├── CajetaLanguage.kt        Language singleton
│   ├── CajetaFileType.kt        .cajeta file type
│   ├── CajetaIcons.kt           File icon
│   ├── parser/                  ANTLR adaptor wiring
│   │   ├── CajetaParserDefinition.kt
│   │   ├── CajetaErrorStrategy.kt  (anchor-sync recovery)
│   │   ├── CajetaTokenTypes.kt
│   │   ├── CajetaElementTypes.kt
│   │   └── CajetaPsiFile.kt
│   ├── highlighting/            Lexer-based syntax coloring
│   ├── editor/                  Brace matcher, commenter, structure view
│   ├── lint/                    ExternalAnnotator + cajetac runner
│   ├── settings/                Application settings + Configurable
│   ├── markdown/                Comment folding + caret-following toggle
│   │   └── engines/             Pluggable markdown renderers
│   ├── debugger/                Phase 2 — DAP client + XDebugger integration:
│   │                           run config, line/conditional/exception
│   │                           breakpoints, fibers view, variables + edit,
│   │                           memory-facet visualization, drop breakpoints
│   ├── harness/                 Typing-simulator test fixtures
│   └── wizard/                  New-project wizard + scaffold
└── resources/
    ├── META-INF/plugin.xml
    └── icons/cajetaFile.svg
```
