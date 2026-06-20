# Skill Discovery — Specification

> Status: **draft, in progress**. Authored with the **design** skill. Sections marked
> _(open)_ are still under discussion and not yet ratified.

## 1. Definition

### 1.1 Purpose
Skill Discovery is a capability built into cajeta that lets a coding agent (or a
developer) find and retrieve **skills** — written aids for implementing against a
given cajeta symbol or dependency — keyed by the **canonical name** of a class,
method, package, or library.

### 1.2 Problem it solves
When writing cajeta code against a class, method, package, or library, an agent has no
built-in way to discover authored guidance for *how to use that thing correctly*. Skill
Discovery provides a deterministic lookup from a canonical name to the skills that aid
its implementation, and a separate retrieval of a skill's payload — so the agent queries
for relevant skills before writing code, and fetches a payload only when it doesn't
already hold it.

### 1.3 Scope
Two operations:
1. **Lookup** — given a canonical name, return the URIs of skills written to aid
   implementation against that name.
2. **Fetch** — given a skill URI, return the skill's payload.

The agent's workflow: query Lookup for the names it's about to work with; for each
returned URI it does not already hold, call Fetch. A held URI is reused without
re-fetching.

### 1.4 Non-goals
- _(open — to be enumerated; e.g. authoring/editing skills, ranking/relevance scoring,
  natural-language search.)_

### 1.5 Resolved design decisions
- **1.5.1 Surface.** Skill Discovery is exposed as **build-tool subcommands** of the
  `cajeta` CLI — `cajeta skill lookup <canonical-name>` and
  `cajeta skill fetch <uri>` — backed by a shared C++ core under
  `src/cajeta/buildtool/`. Rationale: the two operations are naturally one-shot
  (run → print → exit), which a CLI models directly; an MCP stdio server would avoid a
  socket but still impose a persistent session + JSON-RPC handshake the operations don't
  need. The CLI is scriptable by agents (shell out), usable by humans, and sits beside
  the existing build-tool command surface (`BuildToolCommands.cpp`). A thin **MCP
  adapter** can later wrap the same C++ core to gain native tool-discovery in MCP hosts
  without a rewrite.
- **1.5.2 Discoverability.** Because a subcommand is not auto-advertised the way an MCP
  tool is, the agent is told to run `cajeta skill lookup` before implementing via a line
  in the project's `CLAUDE.md`.

## 2. Storage & URI resolution

### 2.1 Storage — skills ship inside the `.cja`
Skills are **members of the package archive** (`.cja`), alongside the compiled `.bc`
members. They are built and published as part of the package, travel with the dependency,
and are therefore available **offline** once the package is resolved — Fetch never needs
the network. Updating a skill means republishing the package version (skills are versioned
exactly as the code they describe).

### 2.2 URI — a logical, resolvable identifier
Every skill has a **URI** that is its stable identity. The URI is *logical*, not a
network location: it names a package coordinate, a version, and a skill id, and is
**resolved against the locally-resolved archive** (via the lockfile) to the in-archive
member. Proposed form _(open — exact scheme TBD)_:

```
cja-skill://<package>@<version>/<skill-id>
e.g.  cja-skill://cajeta/io@1.4.2/file-open
```

- **Lookup** returns a list of these URIs for a canonical name.
- **Fetch** takes a URI, resolves `<package>@<version>` through the lockfile to a local
  `.cja`, and returns the named skill member's payload.
- The URI is stable across machines for the same resolved version, so a held URI is a
  valid cache key — the agent skips Fetch when it already holds the payload for a URI.

### 2.3 Open points
- Exact URI scheme/grammar (host-less `cja-skill://` vs. another form).
- In-archive member layout (e.g. `skills/<id>.<ext>` + a `skills/index`).
- How version is pinned in a returned URI — the resolved (lockfile) version vs. the
  declared range.

## 3. Canonical-name model & match semantics _(open)_

## 4. Skill payload format _(open)_

## 5. Use cases _(open — enumerated per feature subsection once 2–4 are settled)_

## 6. North-star: warm compilation daemon + MCP doorway _(vision, NOT committed scope)_

> This section records strategic direction discovered while scoping skill discovery.
> It is **not** committed work and does not bind §§1–5. Its only binding consequence
> is the §1.5.1 decision to keep the skill-discovery core **transport-agnostic**, which
> this vision validates. Grounded in a deep-research pass (2026-06-20) over the MCP spec,
> the official MCP security best-practices, and the LSP-over-MCP project ecosystem.

### 6.1 The thesis
The consumer of an ML/compute toolchain is increasingly an **AI agent**, not a human at
a terminal. If the agent is the user, MCP is not a delivery channel bolted onto a CLI —
it is the interface. The north star is **a warm cajeta compilation/execution daemon,
reached by agents via MCP**: a model authors cajeta code, compiles it to GPU/SPIR-V
(cloud) or WASM/WebGPU (web), runs it, inspects results, and iterates — inside one
persistent session. The defensible value (the "moat") is the **warm daemon + the
GPU/SPIR-V codegen** (cooperative matrix / tensor cores, ray query, integer dot product,
per `docs/gpu/`); **MCP is the standard agent-facing doorway**, not the room itself.

### 6.2 Substrate check (what's real vs. planned)
- **Real:** the GPU/SPIR-V codegen path (requires LLVM 23), `Sandbox.cpp`,
  `cajeta-cloud-objectstore`, the repository protocol, the build tool.
- **Planned, not built:** the ML API surface — `CajetaTorch.md` is an explicit design
  spec with no `cajeta.torch` code and no backing `Tensor` (only `Matrix` ships).
- Implication: the engine (codegen) and plumbing exist; the ML library and the daemon/MCP
  layer do not. **Sequencing matters — MCP over a thin substrate impresses no one.**

### 6.3 Workflows MCP unlocks that a one-shot CLI structurally cannot
Validated by research as the canonical MCP-over-CLI differential; each maps onto the
ML inner loop (author → compile → run → inspect → adjust):
- **Server-push** — `resources/subscribe` → `notifications/resources/updated` and
  `list_changed`: stream live diagnostics, build status, profiling traces, and a **loss
  curve as a run progresses**. A run-and-exit CLI cannot push.
- **LSP-over-MCP semantics** — expose cajeta's compiler intelligence (definitions,
  references, hover/type, rename, diagnostics, code actions) as MCP tools so the agent
  reasons semantically, not as text. This is the dominant, proven real-world pattern
  (mcp-language-server, karellen-lsp-mcp, mcpls, agent-lsp).
- **Speculative edit-verify** — preview an edit in memory, compute the diagnostic delta,
  apply only if clean — without touching disk.
- **Warm shared index** — one daemon reuses LLVM/index/JIT/device context across agent
  sessions, amortizing cold-start cost.
- **Native tool discovery** — agents (e.g. Claude Code) auto-discover MCP tools at
  session start; CLI subcommands must be advertised out-of-band.

### 6.4 Two hard corrections (design around these)
1. **Do not depend on MCP server-initiated Sampling.** The 2026-07-28 MCP release
   candidate makes the protocol **stateless at the core** and **deprecates Sampling,
   Roots, and Logging**. If the compiler should consult a model mid-codegen (e.g. pick a
   cooperative-matrix tiling), do it via a **direct LLM API call**, not MCP Sampling.
2. **Cross-call state is application-level, not protocol-level.** The persistence that
   makes this valuable lives in **the daemon** (refcounted registry, tool-returned
   handles), not in MCP sessions. This *narrows* the protocol-level CLI-vs-MCP gap and
   reaffirms the engine — not the protocol — as the moat. (Confirm resource
   subscriptions / `list_changed` survive into the final 2026 stateless-core release
   before relying on server-push.)

### 6.5 Cloud/web is the expensive, serious half
The cloud/web surface forces **streamable-HTTP**, which carries the full security budget:
Origin validation (DNS rebinding), localhost-only binding, auth, TLS, session-hijack
risk, single point of failure — Bitsight found ~1,000 unauthenticated MCP servers already
exposed online with fully enumerable tools. Multi-tenant compile-and-run of
agent-authored code is a sandboxing problem well beyond `Sandbox.cpp`. Treat this as a
**tracked cost line**, distinct from the cheap local-stdio dev loop.

### 6.6 Consequence for committed scope
- **Skill discovery stays stateless and CLI-first** (§1.5.1). `lookup`/`fetch` map cleanly
  to one-shot calls and gain nothing from persistence; the stateless-protocol RC removes
  most of the would-be MCP advantage. The transport-agnostic core lets it become one MCP
  tool later for free.
- **Split the services.** Skill discovery = stateless (CLI + trivial MCP tool adapter).
  Language-intelligence / build / diagnostics / run = the warm daemon where MCP
  (and LSP-over-MCP) earns its keep. Do **not** fuse them into one service.
- The only thing the committed work must preserve for this north star is the
  **transport-agnostic seam** already decided in §1.5.
