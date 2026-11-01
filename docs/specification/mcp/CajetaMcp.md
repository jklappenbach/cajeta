# cajeta-mcp

cajeta-mcp is an MCP (Model Context Protocol) server for the cajeta toolchain,
written in cajeta. It speaks JSON-RPC 2.0 over stdio, one JSON object per line,
and implements its tools by shelling out to the `cajeta` compiler via
`cajeta.process`. It dogfoods the language, since the server itself is a cajeta
program under `tools/mcp/`, built with the cajeta build tool.

The spec is `specs/archive/cajeta-mcp-spec.md` and the plan is
`agents/cajeta-mcp-plan.md`. The source is
`tools/mcp/src/main/cajeta/mcp/Server.cajeta`. The tests are in
`test/mcp/CajetaMcpServerTests.cpp`, a C++ gtest harness that builds the server
once and drives it over stdio. Every behavior below mirrors a passing test.

## Build & run

```sh
cd tools/mcp
cajeta build            # -> build/cajeta-mcp
cajeta run              # build + run the server (stdio JSON-RPC)
```

The server locates the `cajeta` compiler it wraps through `--cajeta=PATH`, then
`$CAJETA_BIN`, then `cajeta` on `PATH`, in that order of precedence.

## Protocol

The protocol is JSON-RPC 2.0, newline-delimited, with one request object per
line on stdin and one response object per line on stdout. There is no
`Content-Length` framing. Diagnostics and subprocess chatter never touch stdout.

| Method | Result |
| --- | --- |
| `initialize` | `{protocolVersion, capabilities:{tools:{}}, serverInfo:{name,version}, cacheConfig:{…}}` |
| `notifications/initialized` | (notification — no response) |
| `tools/list` | the five tool descriptors |
| `tools/call` | routes to a tool (below) |

Error envelopes use standard codes: `-32700` parse error, `-32600` invalid
request, `-32601` method not found, `-32602` invalid params, `-32603` internal.

## Tools

`tools/call` params are `{name, arguments}`.

### Skill discovery — `searchSkills` / `listSkills` / `getSkills`

These are thin wrappers over `cajeta <cmd> … --json`:

| Tool | Arguments | Result |
| --- | --- | --- |
| `searchSkills` | `{name}` (required) | `{results: [...]}` — `cajeta search-skill <name> --json` |
| `listSkills` | `{}` | `{skills: [...]}` — `cajeta list-skills --json` |
| `getSkills` | `{uris: [...]}` (required) | `{skills: [...]}` — `cajeta get-skills <comma-uris> --json` |

A missing `name` or `uris` yields `-32602`.

### `compile`

Compiles an in-memory source tree to a `.cja` library archive.

- **Arguments**: `{files, entry}` (both required).
  - `files` is a JSON map of relative source path to file content, and it must be
    a non-empty object. Package directories are created as needed, and JSON-string
    escapes in the content are decoded before the file is written.
  - `entry` is a fully-qualified `pkg.Class.method`. The emitted artifact is named
    after the entry's class, as `<SimpleClass>.cja`.
- **Result**: `{exitStatus, diagnostics, artifact?, cacheHit}`.
  - `exitStatus` is the compiler's exit code, where 0 is success.
  - `diagnostics` is an array of stderr lines.
  - `artifact` is the base64 of the emitted `.cja`, present only on success.
  - `cacheHit` says whether this result came from the execution cache.
- A missing or empty `files`, or a missing `entry`, yields `-32602`.

The mechanism is to materialize `files` under a temp source root, run
`cajeta --emit=cja <entry> <srcRoot> <outDir>`, read back the artifact, and clean
up.

### `jit_execute`

Compiles and JIT-runs an in-memory source tree in one fresh process per call, so
there is no shared state across executes.

- **Arguments**: `{files, entry}` (same shape as `compile`).
- **Result**: `{returnValue, exitStatus, stdout, stderr, cacheHit}`.
  - `returnValue` / `exitStatus` is the entry method's `int32` return, which
    becomes the process exit code.
  - `stdout` is the program's stdout. It is clean, because jit-run chatter is on
    `stderr`.
  - `stderr` is the captured stderr, which includes the `[jit-run]` chatter.
- A missing or empty `files`, or a missing `entry`, yields `-32602`.

The mechanism is to materialize `files`, run `cajeta jit-run <srcRoot> <entry>`,
and clean up.

## Execution cache

Repeated submissions are served from an on-disk result cache, so identical
`compile` / `jit_execute` calls skip recompilation.

- **Key**: `Sha256(toolchain-path, tool, entry, each file path+content)`. Any
  change to the source, entry, or the wrapped `cajeta` binary is a miss.
- **Storage**: one file per entry under the cache directory, named by the hex key,
  containing the result object. It is on disk, so it survives a server restart.
- **TTL**: an entry older than `ttl` seconds by file mtime is treated as a miss
  and removed.
- **LRU caps**: after each write, the cache is trimmed to at most `maxEntries`
  files and `maxBytes` total, evicting least-recently-used first. A cache hit
  touches its entry to mark it recent.
- `cacheHit` in each result tells callers whether work was skipped.

## Configuration

Configuration is resolved at startup with precedence CLI flag > environment >
`--config` file > default:

| Setting | CLI flag | Env var | Config key (`cache.*`) | Default |
| --- | --- | --- | --- | --- |
| enabled | `--cache=on|off`, `--no-cache` | `CAJETA_MCP_CACHE_ENABLED` | `enabled` | on |
| directory | `--cache-dir=PATH` | `CAJETA_MCP_CACHE_DIR` | `dir` | `/tmp/cajeta-mcp-cache` |
| TTL (seconds) | `--cache-ttl=N` | `CAJETA_MCP_CACHE_TTL` | `ttl` | 3600 |
| max bytes | `--cache-max-bytes=N` | `CAJETA_MCP_CACHE_MAX_BYTES` | `maxBytes` | 104857600 |
| max entries | `--cache-max-entries=N` | `CAJETA_MCP_CACHE_MAX_ENTRIES` | `maxEntries` | 256 |

The `--config` file is JSON with a `cache` block, for example:

```json
{ "cache": { "dir": "/var/cache/cajeta-mcp", "ttl": 86400, "maxEntries": 1024 } }
```

The resolved configuration is echoed back in the `initialize` result under
`cacheConfig`, which is useful for verifying precedence.

## Implementation notes

The server is a single cajeta class, `mcp.Server`. Because the cajeta standard
library currently has no directory-creation, directory-listing, `stat`, time, or
`getenv` API, the server shells out via `cajeta.process` for those. It uses
`mktemp -d` / `mkdir -p` / `rm -rf` for the temp source trees, `printenv` for
environment config, and `sh -c` scripts for the cache probe (existence + TTL +
LRU touch + read) and eviction (`ls -1t` newest-first). Several cajeta quirks
were worked around in the source and are documented inline. The JSON reader
keeps string escapes verbatim, the `File.*` path intrinsics need NUL-terminated
paths and do not load struct-field arguments, and `JsonWriter.writeString` does
not escape control bytes.
