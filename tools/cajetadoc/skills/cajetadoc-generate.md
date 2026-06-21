---
id: cajetadoc-generate
applies-to: [cajetadoc/generate]
title: Generate a package-hierarchical HTML doc site with cajetadoc
description: Run cajetadoc <source-root> to walk a .cajeta tree and emit a themeable HTML site + CSS.
---

# cajetadoc — generate the doc site (default mode)

Point the tool at a cajeta source root; it recursively parses every `.cajeta`
file, recovers `/** … */` doc comments from the hidden token channel, and writes
a static HTML site whose directory tree mirrors the package tree, plus one
themeable stylesheet.

```sh
cajetadoc runtime/src/cajeta --exclude-dir xpu -o build/docs
```

Identical behaviour via the compiler subcommand `cajeta doc <same args>` (one
shared CLI, byte-identical output). Use the standalone `cajetadoc` binary in CI
(antlr-only, no LLVM).

## Invocation

```
cajetadoc <source-root> [-o <out-dir>] [options]
```

`<source-root>` is a directory; it is walked recursively. Files with extension
`.cajeta` are ingested, everything else is ignored. Exactly one positional
source root is allowed.

## Inputs → output contract

- **Input**: the `.cajeta` files under `<source-root>`. Package structure comes
  from the declared package names in the source, not from the on-disk layout.
- **Output dir**: `-o/--output <dir>`, default `build/docs`. Created if missing
  (including parents).
- **Layout written**:
  - `<out>/cajetadoc.css` — the built-in themeable stylesheet (always written).
  - `<out>/index.html` — project overview, lists all packages.
  - `<out>/<pkg>/<path>/index.html` — per-package index (package name `a.b.c`
    becomes directory `a/b/c`). Empty packages are skipped.
  - `<out>/<pkg>/<path>/<Type>.html` — one page per type.
  - `<out>/<pkg>/<path>/<Type>.<Nested>.html` — nested types.

On success the tool prints to **stderr**: `parsed N file(s), M type(s); wrote P
page(s) to <out>`.

## Options

- `-o, --output <dir>` — output directory (default `build/docs`).
- `--include-private` — include `private` declarations (default: excluded).
- `--include-internal` — include package-private/internal declarations.
- `--exclude-dir <name>` — skip any directory with this base name; **repeatable**.
- Header chrome (all optional, fill the fixed top bar): `--project-title <s>`
  (default `Cajeta`), `--project-version <s>` (shown as `v<s>`),
  `--date-published <s>`, `--project-license <s>`, `--project-icon <url>`
  (default: built-in inline SVG placeholder). Empty fields are omitted.

## Other modes (not the default site build)

- `--emit-model-json` — print the declaration model as JSON to **stdout** and
  exit; writes no site. For snapshots/debugging.
- `--render-md` — read Markdown from **stdin**, write rendered HTML to stdout,
  exit. Golden-bless helper; ignores the source root.
- `-h, --help` — usage to stderr, exit 0.

## Exit codes

- `0` — success (also `--help`, `--emit-model-json`, `--render-md`).
- `1` — site generation failure (cannot create output dir / cannot write
  stylesheet); message to stderr.
- `2` — usage error: missing source root, unknown `-`option, or a second
  positional argument.

## Diagnostics

Per-file parse problems are printed to stderr as
`cajetadoc: <file>:<line>: <message>` and do **not** change the exit code — the
site is still generated from whatever parsed.

## What it does NOT do

- A **missing/nonexistent source root is not a usage error**: it emits a
  `source root does not exist` diagnostic, builds an empty model, still writes
  `index.html` + `cajetadoc.css`, and exits `0`. Check the printed
  `parsed 0 file(s)` line, not the exit code, to detect an empty run.
- Does not read `.car`/`.cja` archives — source `.cajeta` files only.
- Does not start a server or watch/incrementally rebuild; it is a one-shot
  full generation that overwrites pages in place (stale files from removed types
  are not pruned).
- No config file or env vars: configuration is CLI flags only.
- Not yet implemented (don't expect them in output): cross-reference resolution,
  inheritance graph, client-side search, the doc-lint gate, the cajeta
  code-fence highlighter.

## Worked example

```sh
cajetadoc runtime/src/cajeta \
  --exclude-dir xpu --exclude-dir test \
  --project-title "Cajeta Runtime" --project-version 0.5.1 \
  --project-license Apache-2.0 \
  -o build/docs
# stderr: cajetadoc: parsed 412 file(s), 196 type(s); wrote 240 page(s) to build/docs
# open build/docs/index.html
```
