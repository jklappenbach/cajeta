---
id: cajeta-mcp-compiler-tools
applies-to: [cajeta-mcp/compile, cajeta-mcp/jit_execute]
title: cajeta-mcp compiler tools — compile & jit_execute
description: The shared {files,entry} contract, error codes, and on-disk cache behind the compile and jit_execute MCP tools.
---

# cajeta-mcp compiler tools: `compile` and `jit_execute`

The two `tools/call` operations that turn an in-memory source tree into output.
Both take the **same input** and differ only in what they produce.

| Want to | Call | Get back |
| --- | --- | --- |
| Build a source tree to a `.cja` library archive | `compile` | `{exitStatus, diagnostics, artifact?, cacheHit}` — `artifact` is base64 of the `.cja`, present only on `exitStatus==0` |
| Compile **and** run, capturing program I/O | `jit_execute` | `{returnValue, exitStatus, stdout, stderr, cacheHit}` |

Both are invoked as JSON-RPC `tools/call` with `params={name, arguments}` over
newline-delimited stdio (see the cajeta-mcp overview skill for transport/build/run
and full cache config). This skill is the **I/O contract** for the two tools.

## Shared input: `arguments = {files, entry}`

Both `files` and `entry` are **required**.

- `files` — a JSON object mapping **relative source path → file content** (string).
  - Must be a non-empty object. The path is the on-disk relative path; package
    directories (the `/`-separated prefix) are **created automatically** under a
    temp source root. You do **not** pre-create dirs.
  - Content is run through JSON-string-escape decoding before being written, so a
    `\n` in the JSON string becomes a real newline on disk. Send file content as a
    normal JSON string; don't double-escape.
- `entry` — a fully-qualified `pkg.Class.method` string. For `compile` the emitted
  artifact is named after the entry's simple class: `<SimpleClass>.cja`. The entry
  method's `int32` return becomes `returnValue`/`exitStatus` for `jit_execute`.

Mechanism (identical setup): the server `mktemp -d`s a temp root, materializes
`files` under `<tmp>/src`, runs the compiler, reads results, then `rm -rf`s the
temp tree. `jit_execute` uses **one fresh process per call** — no shared state
between executes. Under the hood:
- `compile` → `cajeta --emit=cja <entry> <srcRoot> <outDir>`
- `jit_execute` → `cajeta jit-run <srcRoot> <entry>`

## Errors (`-32602` invalid params)

For **either** tool, you get a JSON-RPC error with code `-32602` when:
- `files` is missing, not an object, or an empty object, **or**
- `entry` is missing or not a string.

A failed compile is **not** an error envelope — it returns a normal result with
non-zero `exitStatus` and the messages in `diagnostics` (compile) / `stderr`
(jit_execute). `-32603` (internal) is returned only if the server cannot create
the temp dir or launch the `cajeta` binary.

## Execution cache (applies to both)

Identical resubmissions are served from an on-disk result cache; `cacheHit` in the
result says whether work was skipped.

- **Key**: `Sha256(toolchain-path, tool, entry, each file path+content)`. Changing
  any file, the `entry`, the `tool`, or the wrapped `cajeta` binary is a miss. The
  cached blob is stored **without** `cacheHit`; the field is spliced in per read
  (`true` on hit, `false` on a fresh write).
- **Persistence**: one file per result under the cache dir, named by hex key —
  **survives a server restart**.
- **TTL / LRU**: entries older than `ttl` (by mtime) are treated as a miss and
  removed; after each write the cache is trimmed to `maxEntries` / `maxBytes`,
  evicting least-recently-used first (a hit touches its entry). All defaults and
  the CLI/env/`--config` precedence are in the **cajeta-mcp overview** skill;
  inspect the live values in the `initialize` result's `cacheConfig`.

## What these tools do NOT do

- No partial / incremental builds, no watch mode, no shared workspace — every call
  is a clean temp tree built from scratch (cache aside).
- No directory pre-creation step to call; no way to pass an existing on-disk path —
  source must arrive inline in `files`.
- `compile` does not run the program; `jit_execute` does not return the `.cja`
  artifact. Pick the tool by what you need back.
- `entry` is not validated for existence by the server — a bad `pkg.Class.method`
  surfaces as a non-zero `exitStatus` from the compiler, not `-32602`.

## Worked invocation (newline-delimited JSON-RPC on stdin)

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"jit_execute","arguments":{"files":{"hello/Main.cajeta":"package hello;\nclass Main {\n  public static int32 main() {\n    return 7;\n  }\n}\n"},"entry":"hello.Main.main"}}}
```

Response (first run; a second identical line returns `"cacheHit":true`):

```json
{"jsonrpc":"2.0","id":1,"result":{"returnValue":7,"exitStatus":7,"stdout":"","stderr":"[jit-run] ...","cacheHit":false}}
```

Swap `"name":"compile"` (same `arguments`) to get
`{"exitStatus":0,"diagnostics":[...],"artifact":"<base64 of Main.cja>","cacheHit":false}`.
