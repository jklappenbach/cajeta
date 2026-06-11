# cajetadoc

The cajeta documentation generator — JavaDoc in capability, **plus Markdown**,
**plus** a themeable, React-adoptable HTML output. Turns the `/** … */` doc
comments in cajeta source into a hierarchical, browsable website organised by
package.

Authoritative spec: [`docs/Documentation.md`](../../docs/Documentation.md).
Build plan / status: [`plans/docs/cajetadoc-tool-plan.md`](../../plans/docs/cajetadoc-tool-plan.md).

## Two ways to run it, one engine

The doc engine is `cajetadoc_core` (an OBJECT library). The same code backs:

- **`cajeta doc <args>`** — a subcommand of the main compiler (the canonical,
  spec-aligned UX; this is also what `cajeta publish` will call to bundle docs).
  Forwarder: `src/cajeta/cli/DocCommand.cpp` → `cajetadoc::runCli`, dispatched in
  `src/main.cpp`.
- **`cajetadoc <args>`** — a standalone binary (lean: antlr-only, no LLVM; fast
  to build/iterate, usable in CI without the full compiler).

Both share one CLI (`cajetadoc::runCli`) and emit byte-identical output. The
engine links into the cajeta **executable only**, never `cajeta_lib`, so adding
`cajeta doc` costs the compiler no recompilation.

## How it works

cajetadoc **reuses the real cajeta front end**: it compiles the checked-in
antlr-generated lexer/parser (`generated/`) directly and links only the antlr4
runtime — no LLVM — so it builds independently of the compiler's link graph.
Doc comments are recovered from the hidden token channel by source position (the
grammar lexes every `/* */` to `channel(HIDDEN)`; there is no separate DOC
token), then paired with the declaration they immediately precede.

Pipeline: **ingest** (`Ingest`) → **declaration model** (`Model`) →
**doc-comment parse** (`DocComment`) → **Markdown render** (`Markdown`) →
**HTML pages + themeable CSS** (`Render` / `Stylesheet`).

## Build

Built as part of the top-level CMake build (opt-out with
`-DCAJETA_BUILD_CAJETADOC=OFF`):

```sh
cmake -S . -B build
cmake --build build --target cajetadoc cajetadoc_test
```

## Run

```sh
# as a cajeta subcommand (canonical):
cajeta doc runtime/src/cajeta --exclude-dir xpu -o build/docs

# or the standalone binary (identical behaviour):
cajetadoc runtime/src/cajeta --exclude-dir xpu -o build/docs

# dump the declaration model as JSON (for snapshots / debugging)
cajetadoc runtime/src/cajeta --emit-model-json

# render a Markdown fragment to HTML (debug / golden-blessing helper)
echo '# Hi' | cajetadoc --render-md
```

Options: `-o/--output`, `--emit-model-json`, `--include-private`,
`--include-internal`, `--exclude-dir <name>` (repeatable).

## Test

```sh
./build/tools/cajetadoc/cajetadoc_test        # or: ctest -L cajetadoc
```

Golden fixtures live under `test/fixtures/` (source) and `test/golden/`
(expected output). To re-bless the inline-Markdown golden after an intentional
renderer change:

```sh
printf 'Hello **world**.\n\n- a\n- b\n\n`code` and a [link](http://x).\n' \
  | ./build/tools/cajetadoc/cajetadoc --render-md > test/golden/inline_snippet.html
```

## Status

First implementation pass — see the *Implementation status* note and the
checkboxes in [`plans/docs/cajetadoc-tool-plan.md`](../../plans/docs/cajetadoc-tool-plan.md). Done:
ingestion + model (§2), doc-comment parsing (§3), Markdown subset (§4, partial),
hierarchical page generation + themeable CSS (§10/§11, partial), structured-tag
badges + block-tag widgets (§6/§8, partial), deterministic model JSON, gtest
shard. Open: cross-reference resolution (§5), inheritance graph (§9), search
(§12), the doc-lint gate (§13), incremental/watch (§14), `.car` ingestion, and
the cajeta code-fence highlighter.
