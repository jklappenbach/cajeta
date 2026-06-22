---
id: cajeta-mcp-overview
applies-to: [cajeta-mcp]
title: cajeta-mcp — driving the cajeta MCP server (build, config, protocol, tools)
description: How to build, configure, and speak JSON-RPC to cajeta-mcp, with a task→tool routing table over its five tools.
---

# cajeta-mcp

A Model Context Protocol server, written in cajeta, that exposes the cajeta
toolchain over **newline-delimited JSON-RPC 2.0 on stdio**. It implements its
tools by shelling out to the `cajeta` compiler. Source:
`tools/mcp/src/main/cajeta/mcp/Server.cajeta`. Full reference:
`docs/specification/mcp/CajetaMcp.md`.

## Task → tool routing

`tools/call` carries `{name, arguments}`. Pick the tool by task:

| I want to… | Tool (`name`) | Required `arguments` | Result fields |
| --- | --- | --- | --- |
| Find skills by canonical name (fuzzy) | `searchSkills` | `{name}` | `{results:[…]}` |
| List the whole skills catalog | `listSkills` | `{}` | `{skills:[…]}` |
| Fetch skill payloads by `cja-skill://` URI | `getSkills` | `{uris:[…]}` | `{skills:[…]}` |
| Compile a source tree to a `.cja` archive | `compile` | `{files, entry}` | `{exitStatus, diagnostics, artifact?, cacheHit}` |
| Compile **and** JIT-run a source tree | `jit_execute` | `{files, entry}` | `{returnValue, exitStatus, stdout, stderr, cacheHit}` |

- `files` — JSON map of relative source path → file content (non-empty object;
  package subdirs are created; JSON-string escapes in content are decoded).
- `entry` — fully-qualified `pkg.Class.method`; the `compile` artifact is named
  `<SimpleClass>.cja` (base64 in `artifact`, present only when `exitStatus == 0`).
- `compile.diagnostics` is the compiler's stderr split into lines.
- `jit_execute` runs **one fresh process per call** — no shared state across calls;
  `stdout` is the program's clean output, `stderr` includes `[jit-run]` chatter.
- The three skill tools are thin wrappers over `cajeta search-skill <name> --json`,
  `cajeta list-skills --json`, and `cajeta get-skills <comma-uris> --json`.

**Does NOT**: persist any in-memory filesystem between calls; offer a `format`/`lint`/
`run-tests`/`doc` tool (only the five above exist); use HTTP or SSE transport (stdio
only); use `Content-Length` framing (newline-delimited only).

## Build & run

```sh
cd tools/mcp
cajeta build     # -> build/cajeta-mcp
cajeta run       # build + run the server (serves JSON-RPC on stdio)
```

The server resolves the `cajeta` compiler it shells out to, highest precedence
first: `--cajeta=PATH` flag → `$CAJETA_BIN` env → `cajeta` on `PATH`.

## Protocol

JSON-RPC 2.0, **one JSON object per line** on stdin, one response object per line on
stdout. **stdout is protocol only**; all diagnostics and subprocess chatter go to
stderr. Lifecycle:

| Method | Result |
| --- | --- |
| `initialize` | `{protocolVersion, capabilities:{tools:{}}, serverInfo:{name,version}, cacheConfig:{…}}` |
| `notifications/initialized` (or `initialized`) | notification — **no response** |
| `tools/list` | the five tool descriptors |
| `tools/call` | routes to a tool (see table) |

`initialize` echoes the resolved cache config under `cacheConfig` — use it to verify
precedence. `protocolVersion` is `2024-11-05`; `serverInfo` is
`{name:"cajeta-mcp", version:"0.1.0"}`.

### Error codes (standard JSON-RPC)

`-32700` parse error · `-32600` invalid request · `-32601` method not found ·
`-32602` invalid params · `-32603` internal. Notifications (no `id`) never get an
error reply. Missing/empty `files` or missing `entry` ⇒ `-32602`; missing
`name`/`uris` on the skill tools ⇒ `-32602`; launch failure of `cajeta` ⇒ `-32603`.

### Example session

```text
-> {"jsonrpc":"2.0","id":1,"method":"initialize"}
<- {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"cajeta-mcp","version":"0.1.0"},"cacheConfig":{"enabled":true,"dir":"/tmp/cajeta-mcp-cache","ttl":3600,"maxBytes":104857600,"maxEntries":256}}}
-> {"jsonrpc":"2.0","method":"notifications/initialized"}
-> {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"jit_execute","arguments":{"files":{"app/Main.cajeta":"package app;\npublic class Main { public static int32 main(String[] args) { return 7; } }\n"},"entry":"app.Main.main"}}}
<- {"jsonrpc":"2.0","id":2,"result":{"returnValue":7,"exitStatus":7,"stdout":"","stderr":"...[jit-run]...","cacheHit":false}}
```

## Configuration (five cache settings)

Resolved at startup, precedence **CLI flag > env var > `--config` file > default**:

| Setting | CLI flag | Env var | Config key (`cache.*`) | Default |
| --- | --- | --- | --- | --- |
| enabled | `--cache=on\|off`, `--no-cache` | `CAJETA_MCP_CACHE_ENABLED` | `enabled` | on |
| directory | `--cache-dir=PATH` | `CAJETA_MCP_CACHE_DIR` | `dir` | `/tmp/cajeta-mcp-cache` |
| TTL (sec) | `--cache-ttl=N` | `CAJETA_MCP_CACHE_TTL` | `ttl` | 3600 |
| max bytes | `--cache-max-bytes=N` | `CAJETA_MCP_CACHE_MAX_BYTES` | `maxBytes` | 104857600 |
| max entries | `--cache-max-entries=N` | `CAJETA_MCP_CACHE_MAX_ENTRIES` | `maxEntries` | 256 |

`--config` is a JSON file with a `cache` block, e.g.
`{ "cache": { "dir": "/var/cache/cajeta-mcp", "ttl": 86400, "maxEntries": 1024 } }`.

The execution cache serves identical `compile`/`jit_execute` calls from disk (key =
`Sha256(toolchain-path, tool, entry, each file path+content)`; survives restart;
TTL + LRU eviction). `cacheHit` in each result reports whether work was skipped. Any
change to source, `entry`, or the wrapped `cajeta` binary is a cache miss.
