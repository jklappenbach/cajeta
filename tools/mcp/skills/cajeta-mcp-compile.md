---
id: cajeta-mcp-compile
applies-to: [cajeta-mcp/compile]
title: cajeta-mcp compile tool — compile a source map to a .cja artifact
description: MCP tools/call "compile" — runs cajeta --emit=cja and returns exitStatus, diagnostics, base64 artifact, cacheHit.
---

# `compile` (cajeta-mcp tool)

Compiles an in-memory cajeta source tree to a `.cja` library archive. Invoke it as an
MCP `tools/call` over the server's newline-delimited JSON-RPC 2.0 stdio transport (one
request object per line on stdin, one response per line on stdout). See the cajeta-mcp
overview skill for transport, build/run, and config; this skill is the `compile` I/O
contract only.

## Request

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"compile","arguments":{
  "files":{"demo/Hello.cajeta":"package demo;\npublic final class Hello {\n    public static int32 run() { return 0; }\n}\n"},
  "entry":"demo.Hello.run"}}}
```

- `files` (required) — non-empty JSON map of **relative source path → file content**.
  Package subdirectories are created as needed; JSON-string escapes in the content are
  decoded before the file is written to disk.
- `entry` (required) — fully-qualified `pkg.Class.method`. The emitted artifact is named
  after the entry's **simple class** name: `demo.Hello.run` → `Hello.cja`.

## Response (success)

```json
{"jsonrpc":"2.0","id":1,"result":{
  "exitStatus":0,
  "diagnostics":[],
  "artifact":"<base64 of Hello.cja>",
  "cacheHit":false}}
```

- `exitStatus` — the `cajeta` compiler's exit code (`0` = success).
- `diagnostics` — array of stderr lines (each non-empty line of compiler stderr).
- `artifact` — base64 of the emitted `<SimpleClass>.cja`, **present only when
  `exitStatus == 0`**. Decode it to get the archive bytes; the server frees its temp tree
  after responding, so this base64 is your only copy.
- `cacheHit` — `true` if served from the on-disk execution cache (identical
  toolchain+entry+files), `false` if freshly compiled.

## Response (compile error — a normal result, NOT a transport error)

A compile failure returns a successful JSON-RPC `result` with non-zero `exitStatus` and
populated `diagnostics`; **`artifact` is absent**. Do not treat this as an RPC error —
there is no `error` envelope. Read `diagnostics` for the cause.

```json
{"jsonrpc":"2.0","id":1,"result":{
  "exitStatus":1,
  "diagnostics":["demo/Bad.cajeta:2: parse error: ...","..."],
  "cacheHit":false}}
```

## JSON-RPC errors (the `error` envelope — distinct from a compile error)

Returned only for bad calls, not bad source:

- `-32602` invalid params — `files` missing/not an object, `files` empty `{}`, or `entry`
  missing/not a string.
- `-32603` internal — server failed to create the temp dir or launch the `cajeta` binary.

## Mechanism / what it does NOT do

Materializes `files` under a temp source root, runs
`cajeta --emit=cja <entry> <srcRoot> <outDir>`, reads back `<SimpleClass>.cja`, base64s
it, and deletes the temp tree.

- Does NOT JIT-run or execute the code — use the `jit_execute` tool for that.
- Does NOT return the artifact on failure, and does NOT persist it server-side; capture
  the base64 from the response.
- Does NOT take absolute paths, a pre-existing source dir, or compiler flags other than
  the fixed `--emit=cja`; the only inputs are `files` + `entry`.
- The cache key includes the wrapped `cajeta` binary path, the tool, the entry, and every
  file path+content — any change is a cache miss.
