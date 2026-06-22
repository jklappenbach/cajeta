---
id: cajeta-mcp-skill-tools
applies-to: [cajeta-mcp/searchSkills, cajeta-mcp/listSkills, cajeta-mcp/getSkills]
title: cajeta-mcp skill-discovery tools (searchSkills / listSkills / getSkills)
description: Thin MCP transport over `cajeta search-skill|list-skills|get-skills --json`; returns the CLI's JSON verbatim.
---

# Skill discovery over cajeta-mcp

These three `tools/call` tools are thin pass-throughs: each shells out to
`cajeta <cmd> … --json`, parses that stdout, and embeds it verbatim under one
result key. They add **no schema, no caching, no transformation** — to know what a
result row looks like, read the CLI's `--json` output, not this server.

| Tool | Arguments | Shells out to | Result key |
| --- | --- | --- | --- |
| `searchSkills` | `{name}` — required string | `cajeta search-skill <name> --json` | `results` |
| `listSkills` | `{}` — none | `cajeta list-skills --json` | `skills` |
| `getSkills` | `{uris:[…]}` — required array | `cajeta get-skills <comma-joined> --json` | `skills` |

- `getSkills` comma-joins the array's **string** elements into one argument
  (non-string elements are silently dropped, no separator emitted for them).
  URIs are passed through unvalidated — bad `cja-skill://…` URIs surface as
  whatever the CLI emits, not as a transport error.
- **Output parity:** the body of `result` is the CLI's parsed `--json` document.
  If the CLI prints nothing (empty stdout), the field is `[]` — an **empty result
  set is success, not an error**.

## Invocation (JSON-RPC 2.0, newline-delimited stdio)

One request object per line on stdin; one response line on stdout. `tools/call`
params are `{name, arguments}`. Build/run the server from `tools/mcp` with
`cajeta build` / `cajeta run` (see the cajeta-mcp overview skill).

```sh
# searchSkills
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"searchSkills","arguments":{"name":"cajeta/io/file/File"}}}' | cajeta run
# -> {"jsonrpc":"2.0","id":1,"result":{"results":[ … rows from `cajeta search-skill … --json` … ]}}

# listSkills (no arguments)
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"listSkills","arguments":{}}}
# -> {"jsonrpc":"2.0","id":2,"result":{"skills":[ … ]}}

# getSkills (uris -> "a,b")
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"getSkills","arguments":{"uris":["cja-skill://cajeta.io@1.0/io-file-File","cja-skill://cajeta.io@1.0/io-file-overview"]}}}
# -> {"jsonrpc":"2.0","id":3,"result":{"skills":[ … ]}}
```

## Errors (JSON-RPC error codes)

- `-32602` invalid params:
  - `searchSkills` — missing `name`, or `name` not a string.
  - `getSkills` — missing `uris`, or `uris` not an array.
  - `listSkills` takes no required args, so it never raises `-32602`.
- `-32603` internal — the wrapped `cajeta` process failed to launch
  (e.g. binary not found). Resolve which `cajeta` is wrapped via
  `--cajeta=PATH` → `$CAJETA_BIN` → `cajeta` on `PATH`.

## Does NOT

- No execution cache — only `compile`/`jit_execute` cache; every skill call
  re-invokes the CLI.
- No create/edit/delete of skills, and no per-row schema validation — these are
  read-only discovery wrappers.
- Does not validate or normalize URIs/names beyond JSON type-checking.
