# compiler-mcp

**The Cajeta compiler is an MCP server.** `cajeta compiler-mcp` speaks Model
Context Protocol (JSON-RPC 2.0) over stdio and serves skill discovery — the
hand-written implementation guidance an agent retrieves *before* it writes
Cajeta code. There is no second binary to install and no network to reach:
wherever the compiler is, the skills server is, and the corpus it serves is
version-locked to that compiler.

For what a skill *is* — the format, the authoring rules, how libraries ship
theirs, and how matching works — see [Skills](Skills.md). For the external,
Cajeta-written server that adds `compile` and `jit_execute`, see
[CajetaMcp](CajetaMcp.md).

Spec: `specs/archive/compiler-mcp-spec.md`.
Source: `src/cajeta/buildtool/mcp/CompilerMcpServer.{h,cpp}`, dispatched from
`src/main.cpp`; the skill cores it calls live in `src/cajeta/buildtool/skill/`.

## Why it lives in the compiler

Skill serving is stateless, read-only, and already entirely in-binary — the
embedded corpus plus whatever archives the project's lockfile resolves. Routing
that through a separate process would mean an extra binary to build, locate, and
keep in sync, which would then shell out to `cajeta search-skill --json` once per
call. Serving it natively means:

- **Zero setup.** Installing the toolchain installs the agent integration.
- **No version skew.** The skills describe the compiler that serves them.
- **No project required.** It answers from any working directory, with no
  `cajeta.json`, no `cajeta.lock`, and no dependencies resolved.

## Run it

```sh
cajeta compiler-mcp
```

One JSON-RPC object per line on stdin, one response object per line on stdout.
**stdout is the protocol channel and carries nothing else** — diagnostics go to
stderr. The server holds no mutable state and exits cleanly when stdin closes.

Register it with an agent host (`.mcp.json` in this repo does exactly this):

```json
{
  "mcpServers": {
    "cajeta-skills": {
      "type": "stdio",
      "command": "cajeta",
      "args": ["compiler-mcp"]
    }
  }
}
```

## Lifecycle

| Method | Result |
|---|---|
| `initialize` | `{protocolVersion, capabilities:{tools:{}}, serverInfo:{name:"compiler-mcp", version}, instructions}` |
| `notifications/initialized` | (notification — no response) |
| `tools/list` | the three tool descriptors, with JSON-Schema inputs |
| `tools/call` | dispatches to a tool |

`serverInfo.version` is the compiler version. Unknown methods, unknown tools, and
malformed arguments yield well-formed JSON-RPC errors (`-32700` parse, `-32600`
invalid request, `-32601` method not found, `-32602` invalid params) and **the
server stays up** — the next request still works.

### Instructions

The `initialize` result carries server instructions that tell the agent how to
use the tools, verbatim:

> This server provides skills: authoritative, hand-written implementation guides
> for cajeta libraries. Before you write or edit cajeta code that uses a library,
> package, class, or method, first call `searchSkills` with its name (fuzzy and
> typo-tolerant). If a relevant skill is returned, call `getSkills` to fetch its
> payload and follow that guidance over your prior assumptions. Use `listSkills`
> to browse what is available, and follow a skill's references to related skills.

The text is kept identical to the external server's instructions, so an agent
behaves the same against either.

### Search context

The discovery context is built **once at startup**, exactly as the CLI builds it:

1. The embedded corpora are **always** seeded — the stdlib packages plus the
   `cajeta.language`, `cajeta.toolchain`, and `cajeta.stdlib` domains.
2. If a `cajeta.lock` is present in the working directory, its resolved packages
   are added: each package's `.cja` is located in the artifact cache and its
   `skills/index.json` read.

A missing lockfile is **not** an error — it means an empty resolved set plus the
always-present embedded corpus. A package that is not cached, or that ships no
skills, is skipped; a corrupt index is an error.

## Tools

### `searchSkills`

```
{ name, version?, from?, exact? }  →  { results: [{uri, matchedName, tier, distance}] }
```

`name` is a canonical name — library (`cajeta.io`), package (`cajeta/io/file`),
class (`cajeta/io/file/File`), or method (`cajeta/io/File.open`). Matching is
fuzzy and typo-tolerant, and hierarchical: a query returns exact matches, the
skills bound to *descendants* of the name, and the nearest *ancestor overview*.
`tier` is `exact` | `descendant` | `ancestorOverview`; `distance` is the edit
distance (0 = exact); `matchedName` reports what the query actually resolved to.
`exact: true` disables fuzzy matching. `version` / `from` pin version resolution
in multi-version builds. An empty result is a normal empty result, not an error.

```jsonc
→ {"jsonrpc":"2.0","id":1,"method":"tools/call",
   "params":{"name":"searchSkills","arguments":{"name":"cajeta/language/ownership"}}}
← {"id":1,"jsonrpc":"2.0","result":{"results":[
     {"uri":"cja-skill://cajeta.language@1.0/language-ownership",
      "matchedName":"cajeta/language/ownership","tier":"exact","distance":0},
     {"uri":"cja-skill://cajeta.language@1.0/language-overview",
      "matchedName":"cajeta/language/ownership","tier":"ancestorOverview","distance":0}]}}
```

### `listSkills`

```
{ scope?, version?, from? }  →  { skills: [{uri, names, title}] }
```

Browse rather than resolve: it filters, it does not guess (no fuzzy matching).
`scope` is a library or package subtree in slash form (e.g. `cajeta/toolchain`);
omit it to enumerate everything resolved. Ordering is deterministic.

### `getSkills`

```
{ uris: [ "cja-skill://<library>@<version>/<id>", … ] }  →  { skills: [{uri, ok, payload|error}] }
```

Batched, with partial success: an unknown URI yields a per-URI error while the
rest still return their Markdown payload. Because a URI pins the *resolved*
version, `getSkills` is always exact — it resolves to exactly one archive — and
it never touches the network.

## CLI parity

The three tools call the C++ cores in-process; the `cajeta search-skill`,
`cajeta list-skills`, and `cajeta get-skills` subcommands call the same cores
through the same formatting helpers (`src/cajeta/buildtool/skill/SkillCli.h`).
The same query gives result-identical output to the CLI's `--json` form — parity
is a test, not a convention.

```sh
cajeta search-skill cajeta/collection/ArrayList --json
cajeta list-skills cajeta/language --json
cajeta get-skills cja-skill://cajeta.language@1.0/language-ownership --json
```

## Boundaries

`compile` and `jit_execute` deliberately stay in the external
[`cajeta-mcp`](CajetaMcp.md) server — process-per-execute isolation does not
belong inside the compiler binary. This server also does not implement HTTP
transport, multi-client serving, auth, or MCP Resources/Prompts/Sampling. Search
semantics — matching, ranking, URIs, version resolution — belong to
[Skills](Skills.md) and are unchanged by the transport.
