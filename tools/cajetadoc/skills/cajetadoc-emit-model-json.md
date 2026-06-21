---
id: cajetadoc-emit-model-json
applies-to: [cajetadoc/emit-model-json]
title: cajetadoc --emit-model-json (dump the declaration model as JSON)
description: Dump cajetadoc's deterministic declaration Model (packages -> types -> members) as pretty-printed JSON to stdout for snapshots and debugging.
---

# `cajetadoc --emit-model-json`

Dumps the **declaration Model** — the structure cajetadoc builds by ingesting cajeta
source (`Ingest` -> `Model`), after doc-comment parsing — as deterministic, pretty-printed
JSON on **stdout**. Use it for golden snapshots and for inspecting what the parser
recovered before any HTML/Markdown rendering. It does **not** write a doc site.

## Invocation

```sh
cajetadoc <source-root> --emit-model-json
# identical via the compiler subcommand:
cajeta doc <source-root> --emit-model-json
```

`<source-root>` is a directory; cajetadoc walks it for `*.cajeta` files. Flags that shape
the model still apply: `--include-private`, `--include-internal`, `--exclude-dir <name>`
(repeatable). `-o/--output` and all `--project-*` site flags are **ignored** in this mode
(it returns before site generation).

## I/O contract

- **stdout**: one JSON document (see shape below), 2-space indented, trailing newline.
- **stderr**: parse diagnostics, one per line, formatted `<prog>: <file>:<line>: <message>`.
  Diagnostics do **not** change the exit code or suppress the JSON — a file that fails to
  parse simply contributes nothing.
- **No site summary line.** The `parsed N file(s)... wrote M page(s)` line printed in
  normal runs is *not* emitted here; emit-model returns immediately after the JSON.

## Exit codes

- `0` — JSON written (always, even when stderr carried parse diagnostics).
- `2` — usage error: missing `<source-root>`, an unknown `-`option, or an unexpected extra
  positional arg. Prints usage to stderr; no JSON.

There is no nonzero "had diagnostics" code; check stderr if you need to detect parse errors.

## JSON shape

```
{ "packages": [ {                       // sorted by package name (determinism)
  "name": "<dotted, \"\" for default>",
  "types": [ {                          // source order within the package
    "kind": "class|interface|enum|view|annotation|unknown",
    "name": "...", "qualifiedName": "pkg.Name",
    "visibility": "public|protected|private|package",
    "extends": [..], "implements": [..], "annotations": [..],
    "members": [ {
      "kind": "field|constructor|destructor|method|enumConstant",
      "name": "...", "visibility": "...",
      "signature": "<one-line display signature>",
      "modifiers": [..],                // sorted (e.g. static, final, async)
      "annotations": [..],
      "params": [ {"name","type","transfer":bool,"variadic":bool} ],
      "throws": [..],
      "doc": null | {"summary":"...","blockTags":[{"name","arg","body"}]}
    } ],
    "nested": [ <Type ...> ],           // same Type shape, recursive
    "doc": null | { ... }
  } ]
} ] }
```

`doc` is the **parsed** `/** */` block (summary + structured block tags), not rendered
Markdown/HTML. `transfer` is the `#` ownership marker on a param; member signatures show a
leading `#` for `#`-returns. Determinism: packages sorted by name, `modifiers` sorted;
everything else is source order, so output is stable for snapshotting.

## Worked example

Against the test fixture:

```sh
$ cajetadoc tools/cajetadoc/test/fixtures/lang --emit-model-json
tools/cajetadoc: Greeter.cajeta:31: mismatched input ';' expecting {'throws', '{'}   # stderr
{                                                                                     # stdout
  "packages": [
    { "name": "cajeta.lang",
      "types": [
        { "kind": "class", "name": "Greeter", "qualifiedName": "cajeta.lang.Greeter",
          "visibility": "public", "extends": ["Object"], "implements": [], "annotations": [],
          "members": [
            { "kind": "constructor", "name": "Greeter", "visibility": "public",
              "signature": "Greeter(String prefix)", "modifiers": [], "annotations": [],
              "params": [ {"name": "prefix", "type": "String", "transfer": false, "variadic": false} ],
              "throws": [],
              "doc": { "summary": "Builds a greeter with the given prefix.",
                       "blockTags": [ {"name": "Param", "arg": "prefix", "body": "..."} ] } },
            { "kind": "method", "name": "greet", "visibility": "public",
              "signature": "#String greet(String name)", ... }
          ],
          "nested": [],
          "doc": { "summary": "A tiny greeter used by cajetadoc fixtures.", "blockTags": [...] } }
      ] }
  ]
}
```

Note: exit code is `0` despite the parse diagnostic on stderr.

## What it does NOT do

- **No visibility bypass.** Private and package/internal declarations are excluded by
  default — `private String prefix;` above is absent. Pass `--include-private` and/or
  `--include-internal` to include them.
- **No file output / no theming.** It never writes the doc site; `-o` and `--project-*`
  flags are inert here. For HTML pages use `cajetadoc <root> -o <dir>` (no
  `--emit-model-json`).
- **No cross-reference or inheritance resolution.** `extends`/`implements`/`throws` are the
  raw declared names as parsed; link resolution and the inheritance graph happen later in
  the pipeline (`Resolve`/`Render`), not in this model.
- **No Markdown/HTML in `doc`.** `doc.summary`/`blockTags[].body` are raw text; rendering is
  a separate stage (`echo '# Hi' | cajetadoc --render-md` is the rendering helper).
