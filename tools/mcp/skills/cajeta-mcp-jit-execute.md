---
id: cajeta-mcp-jit-execute
applies-to: [cajeta-mcp/jit_execute]
title: cajeta-mcp jit_execute — compile & JIT-run a source map
description: MCP tool that compiles an in-memory cajeta source tree and JIT-runs an entry method in a fresh process, returning its return value, stdout, and stderr.
---

# jit_execute

Compile an in-memory cajeta source tree and **JIT-run** it, then return the entry
method's result plus captured output. **One fresh `cajeta` process per call — no
state is shared between executes** (no persisted globals, no leftover files; each
call materializes a throwaway temp source tree and deletes it).

## Invocation

A JSON-RPC 2.0 `tools/call`, one object per line on the server's stdin:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"jit_execute","arguments":{
  "files":{"app/Main.cajeta":"package app;\n\npublic class Main {\n    public static int32 run(String[] args) {\n        cajeta.io.Stdout.println(\"hi\");\n        return 7;\n    }\n}\n"},
  "entry":"app.Main.run"
}}}
```

Response (one line on stdout):

```json
{"jsonrpc":"2.0","id":1,"result":{"returnValue":7,"exitStatus":7,"stdout":"hi\n","stderr":"[jit-run] ...\n","cacheHit":false}}
```

## Arguments

- `files` (**required**) — JSON object mapping **relative source path → file
  content**. Must be non-empty. Package subdirs are created as needed; JSON-string
  escapes in the content are decoded before the file is written to disk.
- `entry` (**required**) — fully-qualified `pkg.Class.method`. The method must
  return `int32` (that value becomes the process exit code).

## Result contract

- `returnValue` / `exitStatus` — **the same number**: the entry method's `int32`
  return, which is the JIT process's exit code. (Both fields are emitted; they are
  always equal.)
- `stdout` — the **program's** stdout, clean. jit-run chatter is NOT here.
- `stderr` — captured stderr; **includes the `[jit-run]` chatter** plus any
  program stderr and runtime diagnostics. Parse program output from `stdout`, not
  this.
- `cacheHit` — `true` if served from the on-disk execution cache (identical
  toolchain path + `entry` + every file path/content ⇒ hit; any change ⇒ miss).
  Cache is on by default and survives server restart.

## Errors

- `-32602` (invalid params) — `files` missing/empty, or `entry` missing/not a
  string.
- `-32603` (internal) — temp dir creation failed, or the `cajeta` binary failed
  to launch.

A **compile or runtime failure of the submitted program is NOT a JSON-RPC error** —
it comes back as a normal `result` with a non-zero `returnValue`/`exitStatus` and
diagnostics on `stderr`. Inspect those fields to detect program failure.

## What it does not do

- Does **not** return the compiled `.cja` artifact or base64 — use the `compile`
  tool for that.
- Does **not** persist anything between calls (no shared heap, no surviving temp
  files); two calls never see each other's state.
- Does **not** expose `srcRoot`/`outDir` paths — the temp tree is internal and
  removed after the run.

Mechanism (internal): materialize `files` under a temp `src` root, run
`cajeta jit-run <srcRoot> <entry>` with stdout+stderr captured, then `rm -rf` the
temp tree.
