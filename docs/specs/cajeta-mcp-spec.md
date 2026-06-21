# cajeta MCP Server — Specification

> Status: **draft, pending approval.** Authored with the **design** skill. Branch:
> `feature/mcd-lift`. Exposes the cajeta toolchain to AI coding agents over the Model
> Context Protocol (MCP), wrapping the already-built, transport-agnostic skill-discovery
> cores (`docs/specs/skill-discovery-spec.md`) and adding compile + JIT-execute tools with
> a compilation cache.

## 1. Definition

### 1.1 Purpose
A persistent **MCP server** (`cajeta mcp`) that lets an AI coding agent drive the cajeta
toolchain as a long-lived session rather than a series of stateless CLI shell-outs. It
surfaces two capability groups as MCP **tools**: (a) **skill discovery** — find and fetch
the implementation-guidance skills shipped in resolved dependencies; and (b) **compiler
access** — compile submitted cajeta source and JIT-execute an entry point, with a
**compilation cache** so repeated runs of the same source skip recompilation.

### 1.2 Why
A stateless `cajeta <subcommand>` re-pays full startup + parse + typecheck + codegen on
every invocation and carries no memory between calls. An agent in an edit→compile→run loop
submits *nearly the same* source repeatedly; a persistent server can cache the expensive
compiled artifact keyed by the exact source bytes and answer a re-run from cache. The
skill-discovery cores were deliberately built transport-agnostic (skill-discovery spec
§D.7.3) precisely so an MCP adapter could wrap them without a rewrite — this spec is that
adapter, plus the compiler surface that makes the server worth running.

### 1.3 Scope
- A new `cajeta mcp` subcommand speaking MCP (JSON-RPC 2.0) over **stdio**.
- Five MCP tools: `search_skill`, `list_skills`, `get_skills`, `compile`, `jit_execute`.
- A **source-archive** input model: code is submitted as a compressed (zstd) bundle.
- A content-addressed **compilation cache** of compiled artifacts, bounded by total
  bytes, entry count, and TTL, configured via CLI / environment / config file.
- **Process-per-execute** isolation: each `jit_execute` runs in a fresh child process
  loading the cached artifact, so no state leaks between runs and a crash in submitted
  code cannot take down the server.

### 1.4 Non-goals
- **Streamable-HTTP transport**, remote/multi-client serving, and authentication — stdio,
  single local agent, v1. (Noted as a future transport.)
- MCP **Resources / Prompts / Sampling / progress notifications** — v1 is tools-only.
- Exposing the **build tool** (dependency resolution, tasks, publish), the package
  manager, or repository protocol — only compile + jit-execute in v1.
- **Sandbox hardening** beyond process isolation (bwrap/seccomp/capability policy,
  resource limits, filesystem jails). Process-per-execute is the v1 isolation floor;
  hardening the child is a deployment concern and an explicit fast-follow.
- **Incremental / partial recompilation.** The cache is whole-archive: a single byte
  change in the submitted source is a cache miss and a full recompile.
- **Persisting a live JIT** or any cross-run execution state (see §5.3).

## 2. Transport & lifecycle

### 2.1 Requirements
- `cajeta mcp` starts a server that reads JSON-RPC 2.0 requests on stdin and writes
  responses on stdout (one message per line / framed per the MCP stdio convention),
  logging diagnostics to stderr only (never stdout, which is the protocol channel).
- It implements the MCP handshake: `initialize` (declaring `tools` capability and server
  name/version), `tools/list` (enumerating the five tools with JSON-Schema input specs),
  and `tools/call` (dispatching to a tool). Unknown methods return a JSON-RPC error, not a
  crash.
- The server runs until stdin closes or it receives the shutdown notification, then exits
  cleanly (flushing the cache's on-disk state).
- A malformed request, an unknown tool, or a tool that throws yields a well-formed
  JSON-RPC error response; the server stays up.

### 2.2 Use cases
- **2.2.1** As an agent, when I launch `cajeta mcp` and send `initialize`, then I get a
  result advertising the `tools` capability and the server identity, and the session is
  ready for `tools/call`.
- **2.2.2** As an agent, when I send `tools/list`, then I receive all five tools with a
  name, description, and JSON-Schema for their inputs.
- **2.2.3** As an agent, when I send a `tools/call` for an unknown tool or with malformed
  arguments, then I get a JSON-RPC error result and the server remains responsive to the
  next request.
- **2.2.4** As an operator, when stdin closes, then the server shuts down cleanly and
  persists its cache index.

## 3. Skill-discovery tools

### 3.1 Requirements
- `search_skill` wraps `searchSkills`: input `{ name, version?, from?, exact? }`; output
  the ranked `uri\tmatchedName` rows the CLI already produces, as structured results.
- `list_skills` wraps `listSkills`: input `{ scope?, version?, from? }`; output the
  `uri\ttitle` rows.
- `get_skills` wraps `getSkills`: input `{ uris: [..] }` (or a comma-delimited string);
  output each URI's payload (or a per-URI error).
- All three load their `SkillSearchContext` from the project the server was started in
  (lockfile + artifact cache), identically to the CLI subcommands — the tools are pure
  transport over the existing cores, holding no business logic.
- Behavior parity with the CLI: the same query produces the same results.

### 3.2 Use cases
- **3.2.1** As an agent, `search_skill {name:"cajeta/io/Fiel.open"}` returns the
  `file-open` skill URI (typo-tolerant), matching `cajeta search-skill`.
- **3.2.2** As an agent, `get_skills {uris:["cja-skill://…/file-open"]}` returns the skill
  document payload.
- **3.2.3** As an agent, `list_skills {}` enumerates available skills for the project.
- **3.2.4** As an agent, a `search_skill` with no matches returns an empty result set (not
  an error).

## 4. Compiler tools — `compile` and `jit_execute`

### 4.1 Requirements
- **Source input is a compressed archive.** Both tools take
  `{ archive: <base64 zstd source bundle>, options?: {...} }`; `jit_execute` additionally
  takes `{ entry: "pkg.Class.method" }`. The bundle carries the source tree (relative
  path → bytes); decoding + decompression happens server-side. Base64 is required because
  MCP tool arguments are JSON; zstd keeps the on-wire payload small.
- `compile` parses + type-checks + codegens the submitted source and returns
  **structured diagnostics** (severity, file, line/col, message) plus an overall
  success/failure and the resulting artifact's identity (its cache key / hash). It does
  **not** run anything.
- `jit_execute` ensures the source is compiled (cache hit or fresh compile, §6), then runs
  the named entry in a fresh process (§5) and returns `{ returnValue, stdout, stderr,
  diagnostics, exitStatus, cacheHit }`.
- A compile error is a normal structured result (success=false + diagnostics), **not** a
  transport error. A malformed/corrupt archive is a tool error.
- The entry method must resolve in the submitted source; an unknown entry is a structured
  failure citing the missing method.

### 4.2 Use cases
- **4.2.1** As an agent, `compile {archive:…}` of well-formed source returns success with
  an empty diagnostics list and the artifact hash.
- **4.2.2** As an agent, `compile {archive:…}` of source with a type error returns
  success=false and a diagnostic naming the file, line, and message — the server stays up.
- **4.2.3** As an agent, `jit_execute {archive:…, entry:"test.D.run"}` returns the int
  value `run()` produced and any stdout it printed.
- **4.2.4** As an agent, submitting a corrupt or non-archive `archive` returns a tool
  error distinct from a compile failure.
- **4.2.5** As an agent, `jit_execute` with an `entry` that doesn't exist in the source
  returns a structured failure (no crash).

## 5. Execution model & isolation

### 5.1 Requirements
- Each `jit_execute` runs the entry in a **fresh child process** that loads the cached
  compiled artifact and runs it (reusing the existing `jit-run` execution path). The
  parent server process never executes submitted code in-process.
- The child's stdout/stderr/return value/exit status are captured and returned; the parent
  is unaffected by what the child does.
- A child that crashes, aborts, infinite-loops past a timeout, or corrupts its own memory
  does **not** affect the server or other requests — it is reaped and reported as a failed
  execution.
- No execution state (globals, `static` locals, class-static-initializer effects, runtime
  allocator/registry state) survives from one `jit_execute` to the next — guaranteed
  structurally by the fresh process, not by in-place reset.

### 5.2 Use cases
- **5.2.1** As an agent, two `jit_execute` calls of source with a mutable global both
  observe the global at its initial value — the first run's mutation does not leak into the
  second (state purity).
- **5.2.2** As an agent, a `jit_execute` whose entry calls `abort()` / segfaults returns a
  failed-execution result, and a subsequent `jit_execute` still succeeds (crash isolation).
- **5.2.3** As an agent, a `jit_execute` that exceeds the execution timeout is terminated
  and reported as a timeout, with the server still responsive.

### 5.3 Rationale (informative)
A reused JIT instance retains module globals/statics, runs class static initializers only
once, and accumulates runtime/allocator state; scrubbing that in place is not air-tight and
is a known hazard in this codebase (the JIT test harness already carries `StdlibReuseCache`
reuse-hazard logic for exactly this). Separating **cache** (compiled artifact, persists)
from **execution** (fresh process, ephemeral) makes purity and crash-isolation structural.

## 6. Compilation cache

### 6.1 Requirements
- The cache is **content-addressed**: key = `sha256(decoded archive bytes)` combined with
  the normalized compile options (and toolchain identity discriminator). Identical source
  + options ⇒ same key ⇒ hit.
- A hit on `jit_execute` (and `compile`) **skips parse + typecheck + codegen**, loading the
  stored compiled artifact instead. The result reports `cacheHit: true`.
- Stored artifacts are bounded by three independently-configurable limits (§7): **total
  size in bytes**, **maximum entry count**, and **TTL** per entry. Eviction is LRU for the
  size/count bounds; TTL expiry is swept lazily on access and/or on a periodic basis.
- The cache reuses the existing on-disk artifact/IR cache machinery
  (`IrCache`/`ArtifactCache`) rather than introducing a parallel store.
- The cache survives across requests within a server session (its whole purpose) and its
  index is persisted on clean shutdown.

### 6.2 Use cases
- **6.2.1** As an agent, a second `jit_execute` of byte-identical source returns
  `cacheHit: true` and is materially faster (no recompile).
- **6.2.2** As an agent, a one-byte change to the source is a miss (`cacheHit: false`) and
  recompiles.
- **6.2.3** As an operator, when stored artifacts exceed the byte or entry bound, the
  least-recently-used entries are evicted; a subsequent run of an evicted source recompiles.
- **6.2.4** As an operator, an entry older than the TTL is treated as absent and recompiled.

## 7. Configuration

### 7.1 Requirements
- Three cache parameters are configurable: `cache-max-bytes`, `cache-max-entries`,
  `cache-ttl` (seconds/duration). Each has a built-in default so the server runs with no
  configuration.
- Precedence, highest wins: **CLI flag → environment variable → config file → built-in
  default**, resolved per-parameter (a CLI flag for one param does not suppress an env var
  for another).
- CLI: `--cache-max-bytes=`, `--cache-max-entries=`, `--cache-ttl=` on `cajeta mcp`.
  Environment: `CAJETA_MCP_CACHE_MAX_BYTES`, `CAJETA_MCP_CACHE_MAX_ENTRIES`,
  `CAJETA_MCP_CACHE_TTL`. Config file: a `cache` block in **JSON or YAML** (reusing the
  in-tree `JsonC` and `Yaml` facilities; format chosen by file extension —
  `.json`/`.jsonc` vs `.yaml`/`.yml`), at a default path overridable by `--config=`.
- An invalid value (non-numeric, negative) is a startup error citing the offending source
  and parameter; the server does not start with a bad config.

### 7.2 Use cases
- **7.2.1** As an operator, with no flags/env/file, the server starts on built-in defaults.
- **7.2.2** As an operator, `--cache-max-bytes=N` overrides the env var and the config file
  for that parameter, while the other two still read from env/file/default.
- **7.2.3** As an operator, `CAJETA_MCP_CACHE_TTL=…` overrides the config file but is
  overridden by `--cache-ttl=…`.
- **7.2.4** As an operator, a malformed value in any source fails startup with a message
  naming the source (CLI/env/file) and the parameter.

## 8. Testing
- **Config resolution** (§7) and **cache eviction/TTL** (§6) are pure logic — unit-tested
  directly (precedence matrix; LRU + byte/count bounds + TTL expiry), no server needed.
- **Tool request/response shaping** (§2–§4) is tested at the JSON-RPC handler level:
  feed an `initialize` / `tools/list` / `tools/call` request object, assert the response
  object — no live stdio process required.
- **Skill tools** (§3) reuse the skill-discovery cores already covered by 53 suites; the
  MCP tests assert the adapter forwards and shapes results (parity with the CLI).
- **Compile / execute / isolation** (§4, §5) are validated end-to-end through the real
  toolchain on a small submitted archive: a successful run, a compile-error result, a
  cache-hit second run, a state-purity pair (§5.2.1), and a crash-isolation pair (§5.2.2).

## 9. Relationship to existing work
- Skill tools are a thin transport over `SkillSearch`/`SkillGet`/`SkillCli` — no logic
  duplicated (skill-discovery spec §D.7.3 anticipated this).
- Compile/execute reuse `Compiler`, the JIT host (`CajetaJitHost`/`jit-run`), and the
  on-disk caches (`IrCache`/`ArtifactCache`).
- This server is the transport-agnostic-core payoff: when HTTP transport, Resources, or
  build-tool tools are wanted later, they extend this surface without disturbing the cores.
- **`cajeta.process` has landed** (`docs/specs/cajeta-process-spec.md`,
  `docs/specification/process/Process.md`): one-shot `Command.run()` and streaming
  `Command.start()` over `__cajeta_proc_*` bridges. This unblocks the **cajeta-written**
  server path — a server in cajeta can now orchestrate the toolchain subcommands and run
  each `jit_execute` as a process-per-execute child entirely in cajeta. The C++-vs-cajeta
  implementation-language choice (still open) is no longer gated on a missing subprocess
  primitive.
