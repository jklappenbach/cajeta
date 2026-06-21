---
id: cajetadoc-overview
applies-to: [cajetadoc]
title: cajetadoc — invocation, modes, and task routing
description: What cajetadoc is, how to run it (cajeta doc vs standalone), its three modes, options, and a task→mode routing table.
---

# cajetadoc — overview

cajetadoc generates browsable documentation from the `/** … */` doc comments in
cajeta source: a hierarchical, package-organised website (HTML + themeable CSS),
JavaDoc-like but also emitting Markdown. It reuses the real cajeta antlr front
end (no LLVM), so it parses source — it does **not** typecheck or compile.

## Task → mode routing

| You want to… | Invoke | Notes |
|---|---|---|
| Generate the doc website | `cajetadoc <source-root> [-o <dir>] [opts]` | Default mode. Writes HTML pages + CSS to the output dir. |
| Dump the parsed declaration model as JSON (snapshots/debugging) | `cajetadoc <source-root> --emit-model-json` | Prints JSON to **stdout**, writes no site. |
| Render a Markdown fragment to HTML (debug / golden-bless) | `cajetadoc --render-md` | Reads Markdown from **stdin**, writes HTML to stdout. No source-root needed. |
| Same as any of the above, inside the compiler | `cajeta doc <same args>` | Byte-identical output to the standalone binary. |

Not provided by cajetadoc: typechecking/compilation, cross-reference resolution,
inheritance graphs, full-text search, a doc-lint gate, incremental/watch mode,
and `.car` archive ingestion (these are unimplemented as of this pass — do not
rely on them).

## Two ways to run it, one engine

Both entry points share one CLI (`cajetadoc::runCli`) and emit identical output:

- **`cajeta doc <args>`** — subcommand of the main compiler (canonical UX; what
  `cajeta publish` will call). Requires the compiler built with
  `-DCAJETA_BUILD_CAJETADOC=ON` (the default); otherwise it errors with exit 2.
- **`cajetadoc <args>`** — standalone binary (antlr-only, no LLVM; fast, CI-friendly).

`cajeta doc <root> …` just drops the leading `cajeta` and forwards to `runCli`,
so the args after `doc` are exactly the standalone args.

## Build

Built by the top-level CMake build (opt out with `-DCAJETA_BUILD_CAJETADOC=OFF`):

```sh
cmake -S . -B build
cmake --build build --target cajetadoc cajetadoc_test
```

## Example

```sh
# Generate the runtime docs, skipping the xpu dir, into build/docs:
./build/tools/cajetadoc/cajetadoc runtime/src/cajeta --exclude-dir xpu -o build/docs

# Equivalent via the compiler subcommand:
cajeta doc runtime/src/cajeta --exclude-dir xpu -o build/docs
```

On success the site mode prints to stderr, e.g.
`cajetadoc: parsed 12 file(s), 7 type(s); wrote 9 page(s) to build/docs`.

## Options (defaults & precedence)

| Option | Default | Effect |
|---|---|---|
| `-o, --output <dir>` | `build/docs` | Output directory for the site. |
| `--emit-model-json` | off | Print model JSON to stdout and exit (skips site write). |
| `--render-md` | off | Render stdin Markdown → stdout HTML and exit. |
| `--include-private` | off | Include private declarations. |
| `--include-internal` | off | Include package-private/internal declarations. |
| `--exclude-dir <name>` | none | Skip a directory by name. **Repeatable.** |
| `--project-title <s>` | `Cajeta` | Site header title. |
| `--project-version <s>` | none | Header version, shown as `v<s>`. |
| `--date-published <s>` | none | Header publish date. |
| `--project-license <s>` | none | Header license type. |
| `--project-icon <url>` | built-in placeholder | Header icon. |
| `-h, --help` | — | Print usage and exit 0. |

**Mode precedence:** `--render-md` short-circuits immediately when parsed (ignores
source-root and every other flag). Otherwise `<source-root>` is ingested first;
then `--emit-model-json` dumps JSON instead of generating the site. Exactly one
positional `<source-root>` is accepted; a second positional is an error.

## Exit codes

- `0` — success (site generated, model emitted, Markdown rendered, or `--help`).
- `1` — site-generation error (after ingest, e.g. output write failure). Message
  to stderr.
- `2` — usage error: missing `<source-root>`, unknown option, unexpected extra
  positional, or `cajeta doc` when cajetadoc wasn't built in.

Parse diagnostics (per `file:line: message`) are written to stderr but do **not**
fail the run — ingestion is best-effort and continues.
