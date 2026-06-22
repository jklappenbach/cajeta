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
Discovery provides a deterministic **search** from a canonical name to the skills that aid
its implementation, and a separate **get** of a skill's payload — so the agent searches
for relevant skills before writing code, and gets a payload only when it doesn't
already hold it.

### 1.3 Scope
Three operations (CLI verb · core API method):
1. **Search** (`cajeta search-skill` · `searchSkills`) — given a (possibly misspelled)
   canonical name, return the URIs of skills written to aid implementation against it.
2. **List** (`cajeta list-skills` · `listSkills`) — enumerate the skills available in the
   resolved dependency set, optionally scoped to a library or package (browse, not resolve).
3. **Get** (`cajeta get-skills` · `getSkills`) — given **one or more** skill URIs, return their
   payloads (batch; CLI takes a comma-delimited URI list).

The agent's workflow: **Search** for the names it's about to work with (or **List** to
browse what a dependency offers); for each returned URI it does not already hold, **Get**
the payload. A held URI is reused without getting it again.

### 1.4 Non-goals
- **Semantic / natural-language search** — matching by *meaning*. Fuzzy *spelling*-tolerant
  search over names and titles (§3.5) is in scope; intent/NL/embedding search is not.
- **Authoring/editing tooling** — skills are hand-authored Markdown (§4.2); no generator.
- **A persistent service / MCP server** — the committed surface is one-shot CLI over a
  transport-agnostic core; the MCP/daemon direction is the §6 north-star, not committed.

### 1.5 Resolved design decisions
- **1.5.1 Surface.** Skill Discovery is exposed as **build-tool subcommands** of the
  `cajeta` CLI — `cajeta search-skill <canonical-name>`, `cajeta list-skills [<scope>]`, and
  `cajeta get-skills <uri>[,<uri>…]` — backed by a shared C++ core under
  `src/cajeta/buildtool/`. Rationale: the operations are naturally one-shot
  (run → print → exit), which a CLI models directly; an MCP stdio server would avoid a
  socket but still impose a persistent session + JSON-RPC handshake the operations don't
  need. The CLI is scriptable by agents (shell out), usable by humans, and sits beside
  the existing build-tool command surface (`BuildToolCommands.cpp`). A thin **MCP
  adapter** can later wrap the same C++ core to gain native tool-discovery in MCP hosts
  without a rewrite.
- **1.5.2 Discoverability.** Because a subcommand is not auto-advertised the way an MCP
  tool is, the agent is told to run `cajeta search-skill` before implementing via a line
  in the project's `CLAUDE.md`.

## 2. Storage & URI resolution

### 2.1 Storage — skills ship inside the `.cja`
Skills are **members of the package archive** (`.cja`), alongside the compiled `.bc`
members. They are built and published as part of the package, travel with the dependency,
and are therefore available **offline** once the package is resolved — Get never needs
the network. Updating a skill means republishing the package version (skills are versioned
exactly as the code they describe). Skill members are stored **compressed** (§2.4).

### 2.2 URI — a logical, resolvable identifier
Every skill has a **URI** that is its stable identity. The URI is *logical*, not a
network location: it names a **library** coordinate, a version, and a skill id, and is
**resolved against the locally-resolved archive** (via the lockfile) to the in-archive
member. Form _(**ratified**; grammar settled at plan unit D.4, implemented in
`src/cajeta/buildtool/skill/SkillUri`)_:

```
cja-skill://<library>@<version>/<skill-id>
e.g.  cja-skill://cajeta.io@1.4.2/file-open
```

- **Search** returns a list of these URIs for a canonical name.
- **Get** takes one or more URIs (comma-delimited on the CLI), resolves each
  `<library>@<version>` through the lockfile to a local `.cja` archive, and returns each
  named skill member's payload.
- The URI is stable across machines for the same resolved version, so a held URI is a
  valid cache key — the agent skips Get when it already holds the payload for a URI.

### 2.3 Resolved (formerly open)
These were settled during planning/implementation (plan units D.1–D.4):
- **URI scheme/grammar** — host-less `cja-skill://<library>@<version>/<skill-id>`
  (`src/cajeta/buildtool/skill/SkillUri`).
- **In-archive member layout** — skill bodies are `skills/<id>.md` members, plus a
  per-package `skills/index.json` whose schema is canonical-name → `[id]` and
  `id → {title, member-path}` (plan §D.2, `skill/SkillIndex`). The index carries **no
  per-skill version** (§3.4).
- **Version pinning** — a returned URI pins the **resolved (lockfile) version**, not a
  declared range, so it is a stable cross-machine identity (§2.2, §3.4).

### 2.4 Compression (protocol)
Skill payloads and the per-package `skills/index.json` are stored **compressed**, and
decompressed only when read to build the index or return a payload — so the on-disk /
in-binary footprint stays small while access stays transparent.

- **In a `.cja`** this is automatic: the `CAJETA01` archive zstd-compresses every entry's
  data block (and the manifest), framed `uint64 uncompressed_length || zstd_bytes`
  (default level 3). Skill members ride that path with no special handling — the packager
  adds raw bytes, the archive compresses on write and decompresses on read.
- **In the embedded stdlib corpus** (§2.5) the same scheme applies: the corpus is
  zstd-compressed at compiler-build time and decompressed on first access to build the
  index / serve payloads.
- **Implementation note:** compiler and build-tool internals are C++ and compress/
  decompress with **libzstd** (`zstd.h`, the same dependency `CajetaArchive` uses). The
  cajeta-language `cajeta.wire` decompressor is for cajeta *programs*, not compiler
  internals.

### 2.5 Always-available stdlib skills
The cajeta standard library is **not** a resolved `.cja` dependency — its source is
embedded in the compiler binary (`cmake/EmbedStdlib.cmake`). Its skills must therefore be
available in **every** project, with **no lockfile and no dependencies present**.

- Stdlib skills are authored under `runtime/src/cajeta/<pkg>/skills/<id>.md`, one library
  per top-level package (`cajeta.<pkg>`), and **embedded into the compiler** as a
  zstd-compressed corpus (§2.4) at build time, tagged with the stdlib version.
- The discovery context is **always seeded** with the embedded stdlib archives *before*
  any lockfile packages, so `search-skill` / `list-skills` / `get-skills` return stdlib
  skills even outside a project. A missing `cajeta.lock` is **not an error** — it means an
  empty resolved set plus the always-present stdlib.
- Embedded stdlib skills resolve through the same
  `cja-skill://cajeta.<pkg>@<stdlib-version>/<id>` URIs; `get` reads the embedded
  (decompressed) payload.

## 3. Canonical-name model & match semantics

> **Ratified** (2026-06-20).

### 3.1 Name kinds
A canonical name is one of four kinds, mirroring the user's original framing:
- **library** — the published dependency unit / coordinate (e.g. `cajeta.io`).
- **package** — a namespace node within a library (slash-path, e.g. `cajeta/torch/nn`).
- **class** — a type (e.g. `cajeta/torch/nn/Linear`).
- **method** — a member of a class (e.g. `cajeta/torch/nn/Linear.forward`).

**Terminology (ratified).** A **library** is the dependency *concept* — the named,
versioned unit you declare and resolve — and it is *published as a `.cja` **archive***
(the on-disk `CAJETA01` file). "library" names the concept; "archive" names the file;
they are not synonyms. A **package** is a namespace node *within* a library. No coined
term ("cube"/"jar") is introduced.

### 3.2 Match semantics — hierarchical, prefix-inclusive
`searchSkills(name, [version])` returns the union of (see §3.4 for the `version` key,
§3.5 for fuzzy matching):
1. skills bound **exactly** to `name`;
2. skills bound to any **descendant** of `name` (so a package query surfaces its
   classes' and methods' skills);
3. the **nearest ancestor overview** skill, if one exists (so a method query also
   surfaces the class/package overview).

Results are deduplicated by URI and ranked closest-match-first (exact → descendant →
ancestor). One query surfaces the neighborhood before the agent writes code.

### 3.3 First-party scoping
Because skills ship inside one library's `.cja` (§2.1), a library carries skills only for
the canonical names **it defines**. A search is assembled **across all archives in the
resolved dependency set** (via the lockfile) **plus the always-present embedded stdlib
archives (§2.5)**, each contributing skills for its own symbols. There is no mechanism to
attach skills to names you do not own.

### 3.4 Version resolution in multi-version builds (diamond dependencies)
A build may resolve **multiple versions of the same library** side-by-side (e.g. `foo@1.2`
and `foo@2.0`), so a bare canonical name maps to a *set* — one matching skill per resolved
version. Versioning is handled at the **request and URI** levels, **not** in the skill
entry (§4):

- **Skill entry carries no version** — a skill is identified within its archive by `id`.
- **URI carries the version** (`cja-skill://<library>@<version>/<skill-id>`), so **Get is
  always exact** — it resolves to exactly one archive.
- **`version` is an optional key on Search**, scoping the owning library (the library is
  implied by the name's namespace, so one selector suffices):
  - **version given** → results restricted to that resolved version's archive.
  - **version omitted, one version resolved** → that version.
  - **version omitted, multiple versions resolved** → matches for **every** resolved
    version, each URI version-tagged; the caller disambiguates. (No silent pick.)
- Returned URIs always carry the version, so the Search → Get handoff stays exact.

**Consumer-scoped resolution (committed).** `searchSkills(name, from: <module>)` lets the
resolver infer the correct version from the **asking module's** resolved deps, so the
caller need not know it — in a diamond, different consumers correctly resolve different
versions. The explicit `version` key above still works and overrides `from`; with neither,
the §3.4 default applies (one version → that one; multiple → all, tagged). CLI:
`--from <module>`. _(Exact module-identifier form — source path vs. package node — settled
at plan unit D.5.)_

### 3.5 Fuzzy matching (typo-tolerant search)
**Search is fuzzy, not exact** — a misspelled or slightly-off query must still resolve the
intended skills (e.g. `cajeta/io/Fiel.open` → `cajeta/io/File.open`). This is a core
capability, not a nicety. Design:

- **Surface — names *and* titles.** Fuzzy matching runs over **both** the canonical-name
  keys **and** each skill's `title` (§4.1), so a query that approximates either the name or
  the title resolves the skill. This is still spelling-tolerant matching, **not**
  semantic/meaning search.
- **Segment-aware matching (names).** A canonical name is tokenized on its structural
  boundaries (`/` between packages, `.` for members), and each segment is matched
  fuzzily. A typo in one segment does not force a rematch of the whole string, and segment
  structure prevents nonsense cross-segment matches. Titles are matched token-wise.
- **Distance metric.** **Damerau–Levenshtein** edit distance (handles
  insert/delete/substitute **and adjacent transposition** — the most common typo class),
  with a **trigram-similarity** prefilter so the matcher does not score every key/title
  (candidate generation must stay sub-linear as the resolved skill set grows).
- **Threshold.** Only candidates within a bounded, length-scaled distance qualify (e.g.
  ≤1 edit for short segments, proportionally more for longer ones); below threshold →
  no match (no garbage). Threshold is tunable via a CLI flag and a sane default.
- **Ranking.** Results are ranked by **(match quality, then §3.2 hierarchy proximity)** —
  an exact match outranks a fuzzy one; a **name** match outranks a **title** match at equal
  distance; closer edits outrank farther ones. The §3.2 hierarchical/prefix-inclusive
  expansion is applied around the matched name(s).
- **Exact override.** An `--exact` mode disables fuzzy matching for callers that want
  strict resolution.
- **Determinism.** Matching is fully deterministic (stable scoring + stable tie-breaking),
  so the same query yields the same ranked results across runs and machines.
- **Transparency.** Because a typo resolves to a *different* name, Search results surface
  the **matched canonical name** alongside each URI, so the caller sees what its query
  resolved to.

_(Open point: confirm the matcher family — Damerau–Levenshtein + trigram prefilter — and
the default distance threshold, at plan unit D.4a.)_

### 3.6 List (enumeration / browse)
`listSkills([scope], [version])` enumerates the skills available in the resolved
dependency set — for browsing what's offered, as opposed to **Search**, which resolves a
specific (possibly misspelled) name. It is **not fuzzy**: it filters, it doesn't guess.

- **Scope** — optional. Omitted → all resolved skills. A **library** or **package** name
  restricts to that subtree (prefix-inclusive, per §3.2's descendant rule).
- **Version** — the same key as Search (§3.4): omit → all resolved versions (tagged);
  give one → that version. `from: <module>` also applies.
- **Result** — each entry carries its **URI**, the bound canonical name(s) (`applies-to`),
  and `title`, so a caller can browse and then **Get** the ones it wants.
- Reads the same per-archive indexes as Search; no network.

## 4. Skill payload format

> **Ratified** (2026-06-20).

### 4.1 Format — Markdown body + YAML frontmatter
A skill is a Markdown document with a YAML frontmatter header:
- **Frontmatter (machine-facing)** — drives Search and indexing:
  - `id` — unique skill id within the library.
  - `applies-to` — list of canonical-name bindings (§3.1) this skill aids.
  - `title` / `description` — short summary (used in indexes and listings).
- **Body (agent/human-facing)** — the implementation guidance, in Markdown.

There is **no per-skill `version`** field: the skill is versioned with its library — the
URI's `<version>` (the resolved library version) pins the payload, so the same URI always
yields the same bytes (§3.4).

```markdown
---
id: file-open
applies-to: [cajeta/io/File, cajeta/io/File.open]
title: Opening files
description: How to open and dispose File handles correctly.
---
# Opening files
...
```

### 4.2 Authoring & packaging
Skills are **authored by hand** in the package source tree (proposed convention: a
`skills/` directory in the package). At package build they are discovered, validated, and
compiled into the in-archive skill index (§2.3). _(Open point: confirm hand-authored vs.
tool-generated — chosen format favors hand-authoring and clean git diffs.)_

## 5. Use cases

> Enumerated against §§1–4. Each is actor + trigger + outcome.

- **5.1 Search before implementing.** As an agent, when I am about to use a class or
  method, I run `cajeta search-skill <name>` and receive the skill URIs for that symbol
  and its neighborhood (§3.2), so I can retrieve guidance before writing code.
- **5.2 Cache skip.** As an agent, when Search returns a URI whose payload I already
  hold, I skip Get and reuse the held payload (URIs are stable cache keys, §2.2).
- **5.3 Get payloads.** As an agent, given one or more skill URIs, I run
  `cajeta get-skills <uri>[,<uri>…]` and receive their Markdown payloads in one call,
  resolved from the local `.cja`s.
- **5.4 Hierarchical discovery.** As an agent, when I search a package or library name, I
  receive the overview skill plus the skills bound to its classes and methods (§3.2).
- **5.5 Author ships skills.** As a package author, I place skill files in my package
  source; on build they are validated and packaged into the `.cja`, so consumers can
  discover them (§4.2).
- **5.6 First-party scoping.** As a consumer, a search across my dependency graph returns
  each dependency's own skills for the symbols it defines (§3.3).
- **5.7 Stable version pinning.** As an agent, a returned URI pins the **resolved**
  version, so it identifies the same payload across machines and over time (§2.2).
- **5.8 Offline get.** As an agent, once dependencies are resolved, Get reads the
  payload from the local archive with no network access (§2.1).
- **5.9 Typo-tolerant search.** As an agent, when I misspell a canonical name **or a skill
  title**, Search still resolves the intended skills and shows me the matched name (§3.5).
- **5.10 Browse a dependency's skills.** As an agent, I run `cajeta list-skills <library>` to
  enumerate the skills a dependency offers (URI + title), then Get the ones I want (§3.6).
- **5.11 Consumer-scoped version.** As an agent editing a module in a diamond build, I pass
  `--from <module>` so Search/List return skills for the version *that module* resolves,
  without my having to know it (§3.4).

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
- **Skill discovery stays stateless and CLI-first** (§1.5.1). `search-skill`/`list-skills`/`get-skills` map cleanly
  to one-shot calls and gain nothing from persistence; the stateless-protocol RC removes
  most of the would-be MCP advantage. The transport-agnostic core lets it become one MCP
  tool later for free.
- **Split the services.** Skill discovery = stateless (CLI + trivial MCP tool adapter).
  Language-intelligence / build / diagnostics / run = the warm daemon where MCP
  (and LSP-over-MCP) earns its keep. Do **not** fuse them into one service.
- The only thing the committed work must preserve for this north star is the
  **transport-agnostic seam** already decided in §1.5.
