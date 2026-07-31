# compiler-mcp — Specification

> Status: **approved 2026-07-31.** Authored with the **design** skill. Plan:
> `agents/compiler-mcp-plan.md`. An
> internal MCP stdio server built into the `cajeta` compiler binary as a **C++
> subcommand** (`cajeta compiler-mcp`), serving skill search and retrieval only —
> plus the **minimal expert skill catalog**: the smallest set of skills an agent
> needs to write cajeta as an expert, derived from the compiler, the language,
> and the stdlib. Extends `specs/archive/skill-discovery-spec.md` (storage/URI/
> search semantics), `specs/archive/skill-authoring-spec.md` (content levels),
> and `specs/archive/cajeta-mcp-spec.md` §2–§3 (protocol and tool shapes).

## 1. Definition

### 1.1 Purpose
A `cajeta compiler-mcp` subcommand that speaks MCP (JSON-RPC 2.0) over **stdio**
and exposes exactly three tools — `searchSkills`, `listSkills`, `getSkills` — by
calling the in-binary skill-discovery cores (`SkillSearch`/`SkillGet`/
`SkillIndex`) directly. No child processes, no separate install: wherever the
compiler is, the skills server is, and the embedded skill corpus it serves is
version-locked to that compiler.

### 1.2 Why (vs the external `tools/mcp` server)
The external `cajeta-mcp` (tools/mcp, cajeta-written) is the right home for
compile/jit-execute, the compilation cache, and HTTP transport, but for
skills-over-stdio it is a double hop: an extra binary to build and locate, which
then shells out to `cajeta search-skill --json` per call. Skill serving is
stateless, read-only, and entirely in-binary already (embedded corpus + lockfile
archives) — the compiler can answer an MCP host natively with one process and
zero setup. This is the "thin MCP adapter over the transport-agnostic core" the
discovery spec reserved (§1.5.1 there).

### 1.3 Scope
- A `compiler-mcp` verb in the driver dispatch (`src/main.cpp`): MCP handshake,
  `tools/list`, `tools/call` for the three skill tools, over stdio. C++, beside
  the existing skill CLI in `src/cajeta/buildtool/`.
- **Server instructions** in the `initialize` result that direct the agent to
  search before writing cajeta code (§2.1).
- **Corpus extension**: two new embedded skill domains — `cajeta.language` and
  `cajeta.toolchain` — plus a stdlib root router, embedded alongside the stdlib
  skills (§4).
- **The minimal expert skill catalog** (§5–§7): the enumerated set of skills to
  author, each justified against the minimality criterion.

### 1.4 Non-goals
- `compile` / `jit_execute` tools — those stay in `tools/mcp`
  (process-per-execute isolation does not belong inside the compiler binary).
- HTTP transport, multi-client serving, auth; MCP Resources/Prompts/Sampling.
- New search semantics — matching, ranking, URIs, and version resolution are the
  discovery spec's, unchanged.
- Exhaustive documentation. The catalog is a floor, not a reference manual; the
  guide (`docs/guide/`) and cajetadoc remain the full story.

### 1.5 Relationship to prior specs
- **skill-discovery-spec** — owns storage, URIs, fuzzy search, version
  resolution. This spec adds two embedded domains (§4) and a transport.
- **skill-authoring-spec** — owns per-level content requirements. Every catalog
  skill (§5–§7) is written to that spec's level rules and quality bar (§10 there).
- **cajeta-mcp-spec (archived, delivered)** — owns the external server. Tool
  names, argument shapes, and result shapes here are **parity-identical** to its
  skill tools, so an MCP host can point at either server without client changes.

## 2. Transport & lifecycle

### 2.1 Requirements
- **2.1.1** `cajeta compiler-mcp` reads JSON-RPC 2.0 requests on stdin and
  writes responses on stdout (MCP stdio framing); diagnostics go to stderr only
  — stdout is the protocol channel and carries nothing else.
- **2.1.2** `initialize` declares the `tools` capability, server name
  `compiler-mcp`, version = the compiler version, and **instructions** telling
  the agent to `searchSkills` before writing or editing cajeta code, `getSkills`
  for returned URIs it does not hold, and to follow skill guidance over prior
  assumptions (same text as the external server's instructions, kept in sync).
- **2.1.3** `tools/list` enumerates the three tools with JSON-Schema inputs;
  `tools/call` dispatches; unknown methods/tools and malformed arguments yield
  well-formed JSON-RPC errors and the server stays up.
- **2.1.4** The discovery context is built once at startup exactly as the CLI
  builds it: embedded corpora always seeded (stdlib + §4 domains), lockfile
  archives added when a project is present; a missing `cajeta.lock` is not an
  error. The server runs from any working directory and answers from the
  embedded corpus at minimum.
- **2.1.5** The server exits cleanly when stdin closes. It holds no mutable
  state, so shutdown persists nothing.

### 2.2 Use cases
- **2.2.1** As an agent host, when I register `cajeta compiler-mcp` in my MCP
  config and start a session, `initialize` succeeds with instructions and the
  three tools are discoverable — with no project, lockfile, or network.
- **2.2.2** As an agent, when I send a malformed `tools/call`, I get a JSON-RPC
  error and the next request still works.
- **2.2.3** As an operator, when the host process exits and stdin closes, the
  server terminates without orphaning anything.

## 3. Skill tools

### 3.1 Requirements
- **3.1.1** `searchSkills { name, version?, from?, exact? }` → ranked matches,
  each carrying `uri`, `matchedName`, and `title` — the core's results, shaped as
  the external server shapes them.
- **3.1.2** `listSkills { scope?, version?, from? }` → `uri` + `title` (+
  `applies-to`) rows.
- **3.1.3** `getSkills { uris: [...] }` → each URI's Markdown payload, or a
  per-URI error for unknown URIs (batch semantics, partial success allowed).
- **3.1.4** All three call the C++ cores **in-process** — no shell-out, no logic
  duplicated from the CLI. The same query gives result-identical output to
  `cajeta search-skill` / `list-skills` / `get-skills --json` (parity is a test).
- **3.1.5** An empty search result is a normal empty result, not an error.

### 3.2 Use cases
- **3.2.1** As an agent about to use `cajeta.io` `File`, I call
  `searchSkills {name:"cajeta/io/file/Fiel"}` and get the `io-file-File` skill
  URI (typo-tolerant), then `getSkills` for its payload.
- **3.2.2** As an agent starting a cajeta task cold, I call
  `searchSkills {name:"cajeta.language"}` and receive the language overview plus
  its topic skills (§3.2 hierarchy of the discovery spec).
- **3.2.3** As an agent, the same `searchSkills` call against `compiler-mcp` and
  `tools/mcp` returns the same URIs.

## 4. Corpus extension — language & toolchain domains

### 4.1 Two new embedded pseudo-libraries
The discovery spec's canonical-name model (library/package/class/method) has no
home for skills about the **language itself** or the **toolchain workflow**. This
spec ratifies two pseudo-libraries, served exactly like the embedded stdlib
corpus (discovery §2.5), versioned with the compiler:

- **`cajeta.language`** — the language: syntax, semantics, memory model.
  Topics bind as package-kind names: `cajeta/language/ownership`,
  `cajeta/language/templates`, … No grammar change — pure naming convention.
- **`cajeta.toolchain`** — driving the toolchain: project layout, build, run,
  test. The existing `tools/driver/skills/*.md` (5 skills, currently authored but
  **not embedded and not searchable**) are absorbed into this domain, gaining
  `cajeta/toolchain/...` bindings; their `cajeta-driver` bindings remain valid
  aliases.

URIs follow the standard form: `cja-skill://cajeta.language@<ver>/<id>`.
**Binding convention** (segment matching is literal — `.` and `/` are distinct
prefixes): the domain overview binds both the dotted library coordinate
(`cajeta.toolchain`) and the slash root (`cajeta/toolchain`); topic skills bind
slash package-kind names (`cajeta/toolchain/jit-run`). List **scope** therefore
uses the slash form.

### 4.2 Authoring home & embed pipeline
- All corpus-extension skills live under **`runtime/skills/<domain>/*.md`** —
  `runtime/skills/language/`, `runtime/skills/toolchain/`, and
  `runtime/skills/stdlib/` (the §7 router). The five existing driver skills
  relocate from `tools/driver/skills/` to `runtime/skills/toolchain/`
  unchanged; one home, one glob.
- `src/CMakeLists.txt`'s skillembed glob extends to `runtime/skills/*/`; each
  `<domain>` directory maps to pseudo-library `cajeta.<domain>`. The corpus
  stays one zstd blob decompressed on first access (`EmbeddedStdlibSkills`).
- Frontmatter, levels, and quality bar are the authoring spec's, unchanged.

### 4.3 Use cases
- **4.3.1** As an agent, `searchSkills {name:"ownership"}` resolves
  `cajeta/language/ownership` by fuzzy title/name match with no project present.
- **4.3.2** As an agent, `listSkills {scope:"cajeta/toolchain"}` enumerates the
  driver/build/run/test skills.
- **4.3.3** As an author, I edit a language skill Markdown file and rebuild; the
  compiler serves the new payload — no packaging step beyond the build.

## 5. The minimal expert skill catalog — criterion and language domain

### 5.1 Minimality criterion
The reader is an agent already expert in Java/C++/Rust. A skill earns a place in
the catalog **only if** its absence would make that agent:

- **(a) crash or corrupt** — code that compiles but dies (ownership, lifecycle,
  capture, transfer);
- **(b) fail to compile blind** — rejected for a reason the error text alone
  does not teach;
- **(c) silently miscompile intent** — code that runs but does the wrong thing;
- **(d) dead-end** — hunt a capability that does not exist or lives elsewhere.

Anything the agent would infer correctly from Java intuition is excluded — at
most a routing row or hazard line in an overview. Each catalog entry below cites
the failure class(es) it prevents. Content mandates reference the authoring
spec's level requirements; hazards are sourced from the guide, the tour-quality
findings, and the language-gotcha record — the spec mandates the categories, not
a frozen bug list.

### 5.2 `cajeta.language` skills (nine)
- **5.2.1 `language-overview`** — applies-to `cajeta.language`; library level.
  The delta map: Java surface over a value-semantics, no-GC core. Keyword deltas
  and reserved words (`spawn`, `scope`, `annotation`, …); the routing table into
  5.2.2–5.2.9, `cajeta.toolchain`, and the stdlib router (§7); negative rows for
  what cajeta is **not** (no erasure generics — say "templates"; no
  try-with-resources; no GC). Prevents (d); the entry point for everything else.
- **5.2.2 `language-types-and-allocation`** — `cajeta/language/types`.
  Primitives and literal typing (literal→overload binding rules), casts, the
  type kinds (`class`/`interface`/`enum`/`record`/`view`/`annotation`/`@Kernel`),
  one-type-either-storage: stack default vs `heap`, and when to pick which.
  Prevents (b), (c).
- **5.2.3 `language-ownership`** — `cajeta/language/ownership`. The highest-value
  skill: borrow-by-default, `#` transfer and every position it appears in (call
  site, return, field, closure), drop at scope exit, reading borrow-checker
  errors, slices and the `shared` state, arrays own elements by move,
  collections do **not** own elements. Prevents (a) — the crash class that
  motivates skills existing at all.
- **5.2.4 `language-classes`** — `cajeta/language/classes`, with alias bindings
  `cajeta/language/operators` and `cajeta/language/inheritance` (bindings are
  cheap — one payload, several searchable names). Construction and `heap`,
  single and multiple inheritance, diamond ancestor sharing, template mixins,
  interfaces vs multiple inheritance, operator overloading (static and derived
  forms, the `==`/`hash()` pairing rule), records. Prevents (b), (c).
- **5.2.5 `language-templates`** — `cajeta/language/templates`. Monomorphization
  — `T` is a real type (`T.class` exists; type info survives), numeric
  pseudo-bounds, wildcards and capture conversion; never reason from erasure.
  Prevents (b), (d).
- **5.2.6 `language-lambdas`** — `cajeta/language/lambdas`. Capture semantics:
  primitives by value, heap values by borrow, `#` transfers into the closure —
  and the lifetime consequences of each. Prevents (a).
- **5.2.7 `language-concurrency`** — `cajeta/language/concurrency`. The
  structured-concurrency keywords, the fiber model, what may cross a `spawn`
  boundary and with what ownership; routes to the `cajeta.concurrent` skills for
  the library surface. Prevents (a).
- **5.2.8 `language-errors`** — `cajeta/language/errors`. Advisory-checked
  exceptions, recoverable vs unrecoverable at the top of the hierarchy, and **no
  try-with-resources** — drop-on-scope is the resource pattern; routes to
  `error-overview` for the stdlib hierarchy. Prevents (c), (d).
- **5.2.9 `language-annotations`** — `cajeta/language/annotations`. Declaring
  annotation types with the `annotation` keyword (not `@interface`), the
  synthesis family (`@Builder`, `@Value`, `@Logged`, …), and routing to the
  `cajeta.aot` skills for DI (components, factories, profiles, aspects).
  Prevents (b), (d).

### 5.3 Use cases
- **5.3.1** As an agent writing my first cajeta file, I read `language-overview`
  and `language-ownership` and produce code whose call sites transfer with `#`
  where required — no use-after-free on first compile-run.
- **5.3.2** As an agent hitting a borrow-checker rejection, `language-ownership`
  tells me what the error means and the idiomatic fix, without reading compiler
  source.
- **5.3.3** As an agent porting Java, `language-overview`'s negative rows stop me
  from writing try-with-resources or reasoning about erasure.

## 6. Catalog — toolchain domain

### 6.1 Requirements
- **6.1.1** The five existing driver skills (overview, compile, jit-run, tasks,
  skill-discovery) join the embedded corpus as-is (§4.1).
- **6.1.2** Gap-fill audit against the criterion, adding **at most two** skills:
  - **`toolchain-project`** — `cajeta/toolchain/project`: `cajeta.json`, source
    layout (`src/main/cajeta/...`), dependencies/classpath, build outputs, and
    the edit→build→run loop (`cajeta build`, jit-run vs `--emit=exe`).
    Prevents (d).
  - **`toolchain-testing`** — `cajeta/toolchain/testing`: writing and running
    tests with cajeta-unit (coordinate, wiring, run command). Prevents (d).
  If an existing driver skill already carries a mandated fact, it is linked, not
  duplicated (authoring §2.1.4).

### 6.2 Use cases
- **6.2.1** As an agent asked to "create a cajeta project and run it," I follow
  `toolchain-project` from empty directory to executed program without trial-
  and-error on layout or flags.
- **6.2.2** As an agent, `searchSkills {name:"jit-run"}` now resolves the driver
  skill that was previously unsearchable.

## 7. Catalog — stdlib root router

### 7.1 Requirements
- **7.1.1** One skill, **`stdlib-overview`**, bound to pseudo-library
  `cajeta.stdlib`: a task→package routing table over the stdlib (I/O, net,
  collections, concurrency, time, math/tensor, codec, hash, process, reflect,
  xpu/gfx, wire, search, ifx), each row pointing at that package's existing
  overview skill; negative rows for what the stdlib does not provide. Prevents
  (d). The per-package corpus (~130 skills) is already the depth — this is the
  missing top.
- **7.1.2** No stdlib package content is restated here (authoring §2.1.4).

### 7.2 Explicit exclusions (and why they are not in the minimum)
- **7.2.1** Control flow, strings, streams basics — Java intuition holds;
  stdlib `lang-string`/`lang-stream` skills already cover the library surface.
- **7.2.2** Reflection — `reflect-*` skills exist; no language-level delta big
  enough to earn a topic skill (routing row in `stdlib-overview` suffices).
- **7.2.3** Differentiation, xpu/gfx kernels, ML/nucleo — expert *domains*, not
  prerequisites for writing cajeta; their stdlib skills carry them.
- **7.2.4** Debugging, IDE, cajetadoc, cvm — not needed to *write* cajeta;
  toolchain overview routes to them.
- **7.2.5** Title stores (`#=`) and other unlanded features — the catalog
  documents shipped surface only; each lands with its feature.

### 7.3 Use case
- **7.3.1** As an agent needing "parse JSON from a TCP stream," I read
  `stdlib-overview`, route to `codec-json-overview` and `io-net-overview`, and
  get depth from the existing corpus — three reads, no spelunking.

## 8. Testing
- **8.1** Handler-level protocol tests (no live process): `initialize` /
  `tools/list` / `tools/call` request objects in, response objects asserted —
  including error paths (unknown tool, malformed args, bad URI).
- **8.2** Parity tests: for a fixed query set, MCP tool results equal the CLI
  `--json` results.
- **8.3** An end-to-end stdio smoke test: launch `cajeta compiler-mcp`, drive a
  real handshake + one call per tool through pipes.
- **8.4** Corpus tests: every catalog id (§5–§7) resolves via `getSkills`; every
  catalog `applies-to` is reachable via `searchSkills`; frontmatter validates.
- **8.5** Catalog acceptance: each authored skill passes the authoring spec §10
  checklist in review; each entry's cited failure class is demonstrated by at
  least one concrete example in its body.

## 9. Relationship to existing work
- `tools/mcp` keeps compile/jit-execute, the compilation cache, and HTTP; its
  skill tools remain (same cores underneath, via the CLI) but `cajeta
  compiler-mcp` is the recommended stdio skills endpoint for agent hosts.
- The discovery cores, embed pipeline, and CLI subcommands are unchanged except
  for the widened embed glob (§4.2).
- Skill content authored here becomes the corpus every transport serves — CLI,
  `tools/mcp`, and compiler-mcp alike.
