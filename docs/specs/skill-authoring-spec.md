# Skill Authoring — Specification

> Status: **draft, in progress**. Authored with the **design** skill. Defines *what
> goes into* a skill at each level so that skills reliably help a coding agent pick up
> unfamiliar cajeta code and work with it correctly. Companion to
> `docs/specs/skill-discovery-spec.md` (which defines how skills are stored, named, and
> retrieved); this spec defines their **content**.

## 1. Definition

### 1.1 Purpose
A skill is a written aid, keyed to a canonical name (library / package / class /
method), that an agent reads *before or while* writing code against that symbol. This
spec defines, per level, the information a skill must carry so the agent can (a) decide
whether it's looking at the right code, (b) find the right entry point fast, (c) use it
correctly the first time, and (d) avoid the non-obvious, hard-to-recover errors.

### 1.2 The guiding principle — write for the agent's failure modes
Skills are optimized for an LLM agent reading under time pressure, not for human
reference browsing. That changes what matters:

- **Front-load routing and decisions.** The agent's expensive failure is reading the
  wrong N files. Lead with "for task X, use Y."
- **Make ownership and lifecycle explicit and early.** In cajeta these cause the worst,
  least-recoverable errors (use-after-free, double-free, leaked handles). A skill that
  omits "who frees this" is a skill that lets the agent crash.
- **State what the code does *not* do.** "There is no `mkdir` here" saves the agent from
  hunting a method that doesn't exist — as valuable as documenting what exists.
- **Show, don't enumerate.** One real, idiomatic, ideally test-backed example beats a
  list of signatures.
- **One fact, one level.** Higher levels *route*; lower levels *detail*. A fact
  duplicated across levels rots; each fact lives at exactly one level and is linked, not
  copied.

### 1.3 Scope
Defines the required and recommended content of skills at five levels — **library,
package, component, class, method** — plus an adaptation for **applications** (tools).
Defines per-level **selection criteria** (when a skill at that level is warranted, so a
library is not flooded with thin skills) and a **review checklist**.

Non-goals: storage/URI/resolution (see skill-discovery-spec), the search index, and the
authoring toolchain. File format conventions are summarized in §8 but owned elsewhere.

### 1.4 Levels mirror canonical-name kinds
The levels map onto the canonical-name kinds of the discovery spec (§3.1 there):
**library → package → class → method**, with **component** inserted between package and
class as a named group of cooperating classes (§6). A skill's `applies-to` frontmatter
lists the canonical name(s) it serves, which is what binds content to a level.

## 2. Cross-cutting requirements (every skill, every level)

### 2.1 Required of all skills
- **2.1.1 Frontmatter**: `id`, `applies-to` (canonical name(s)), `title`, `description`
  (one line, used by search ranking).
- **2.1.2 A worked example** in the body — real cajeta, idiomatic, with the `import`
  lines, preferably mirroring a passing test.
- **2.1.3 Ownership/lifecycle stated wherever a value crosses a boundary** — `#`
  transfer at call sites, owned-vs-borrowed returns, who calls `close()`/who frees.
- **2.1.4 Links, not copies** — reference related skills (by `applies-to`/URI) instead
  of restating their content.
- **2.1.5 Conciseness** — lead with the answer; no API dumps; no restating signatures
  the reader can get from the next level down.

### 2.2 Use cases
- **2.2.1 Right-thing check.** As an agent, when I open a skill I can tell within the
  first lines whether this is the code for my task (purpose + "does/doesn't do").
- **2.2.2 No duplication drift.** As an author, each fact has one home, so updating the
  code means editing one skill, not hunting copies.

## 3. Library-level skill — *orientation & routing*

A library skill is the first thing read about a whole library (e.g. `cajeta.io`,
`cajeta.process`). It is an **index and a map, not an API reference**.

### 3.1 Required content
- **3.1.1 What it's for** — 1–2 sentences: purpose and the core abstraction, so the
  agent can confirm/deny relevance immediately.
- **3.1.2 Task → entry-point routing table** — the highest-value element. Rows of
  "want to do X → start with `Class`/`fn`." Include **negative rows**: "X is *not*
  provided here (do Z instead)."
- **3.1.3 Cross-cutting invariants** that hold library-wide: ownership/`#` conventions,
  who-frees-what, lifecycle/disposal model (drop-on-scope? explicit `close`?),
  error-handling style (exceptions vs `Optional` vs sentinels), null conventions,
  threading/fiber rules.
- **3.1.4 One canonical end-to-end example** — the shortest idiomatic path through the
  library, with imports.
- **3.1.5 Hazard list** — the non-obvious, library-wide traps (e.g. "the JSON reader
  keeps string escapes verbatim"; "`spawn` is a reserved word — the method is `start`").
- **3.1.6 Disambiguation** of overlapping options — 1–2-line "use X when…, Y when…"
  (e.g. `run()` one-shot vs `start()` streaming).
- **3.1.7 Setup/preconditions** — dependency coordinate + version, required
  capability/permission, platform caveats (e.g. POSIX-only, needs LLVM 23).
- **3.1.8 Downward pointers** — the package/component/class skills for depth.

### 3.2 Out of scope here
Full method signatures, exhaustive class lists, per-class detail — those belong to the
class/method levels.

### 3.3 Selection
**Always exactly one** library skill per library.

### 3.4 Use cases
- **3.4.1 Jump to the entry point.** As an agent facing a 100-class library, I read the
  routing table and go straight to the right class without spelunking.
- **3.4.2 Learn the rules once.** As an agent, I read the cross-cutting invariants once
  and apply them across every class in the library (instead of rediscovering ownership
  rules per class, often by crashing).
- **3.4.3 Avoid the dead end.** As an agent, a negative routing row tells me a capability
  is absent so I choose an alternative immediately.

## 4. Package-level skill — *the neighborhood map*

A package is a namespace node within a library (e.g. `cajeta/io/file`). The package
skill is narrower than the library map, broader than a class.

### 4.1 Required content
- **4.1.1 Responsibility** — the slice of the library this package owns.
- **4.1.2 Inventory map** — the classes/components in the package and each one's role,
  **grouped into components** (§6) when there are many. Separate **entry-point types**
  (what you instantiate/call) from **support types** (values, enums, exceptions).
- **4.1.3 Intra-package collaboration** — how the classes work together (e.g.
  `File.openRead` returns a `FileReader`; `FileReader` raises `EndOfFileException`).
- **4.1.4 Package-scoped task recipes** not already covered library-wide.
- **4.1.5 Package-specific invariants** not stated at the library level.
- **4.1.6 Pointers** to component/class skills.

### 4.2 Selection
One per package that contains more than a single class, or whose role isn't obvious from
the library map. A trivial single-class package may fold into its class skill.

### 4.3 Use cases
- **4.3.1 Find the access point in a crowd.** As an agent in a big package, I see which
  types are entry points vs support, so I don't try to instantiate a value/exception type.
- **4.3.2 Understand who-calls-whom.** As an agent, the collaboration notes show me the
  flow between classes before I read any one of them.

## 5. *(reserved)*

## 6. Component-level skill — *collaboration between classes*

A **component** is a cohesive group of classes within a package that are used together
(e.g. the JSON DOM = `JsonValue` + `JsonObject` + `JsonArray`; JSON streaming =
`JsonReader` + `JsonWriter`). Component skills capture the thing single-class skills
structurally miss: **how the classes cooperate**.

### 6.1 Required content
- **6.1.1 Members and roles** — the classes in the component and what each contributes.
- **6.1.2 Collaboration / object graph** — who creates whom, who owns whom, how data
  flows between them.
- **6.1.3 The cross-class call sequence** — the ordered, multi-class workflow (e.g.
  `JsonReader.readValue()` → a `JsonValue` DOM → navigate via `asObject()/get()`).
- **6.1.4 Ownership across the component boundary** — e.g. "`JsonReader` takes and frees
  its input array"; "`JsonValue` strings are borrowed views over the DOM — copy to keep."
- **6.1.5 When to use this component** vs another in the same package.
- **6.1.6 A multi-class worked example.**

### 6.2 Selection
Warranted when a package has **two or more distinct cohesive groups**, or when correct
use requires choreographing several classes. When classes are tightly coupled, a single
component skill may **replace** their individual class skills.

### 6.3 Use cases
- **6.3.1 Wire classes together correctly.** As an agent, I follow the documented call
  sequence across classes instead of guessing the order and ownership.
- **6.3.2 Pick the right component.** As an agent, I choose DOM vs streaming based on the
  "when to use" guidance rather than reading both.

## 7. Class-level skill — *how to use this type*

For a single type. This is where signatures and method-level detail begin to live.

### 7.1 Required content
- **7.1.1 Purpose + placement** — one line on what it is and which component/package.
- **7.1.2 Access-point flag** — is this a "start here" entry point, or a support/value/
  exception type? State it explicitly.
- **7.1.3 Construction & ownership** — how to obtain an instance (constructor, `heap
  Foo(...)`, factory) and the ownership of constructor args (`#` transfer). Or "you do
  not construct this — you receive it from `X`."
- **7.1.4 The methods that matter** — the small subset used most, with signatures and
  **return ownership/null semantics** (owned vs borrowed; nullable). Not an exhaustive
  list.
- **7.1.5 Lifecycle** — must it be closed/disposed? Drop semantics? (Note explicitly
  when there is **no** drop-on-scope.)
- **7.1.6 State & concurrency** — mutable? reusable (e.g. can a `Command` be `run()`
  twice)? thread/fiber-safe?
- **7.1.7 Invariants & preconditions** — valid states, required call order.
- **7.1.8 Sharp edges** specific to this class.
- **7.1.9 Errors raised** — exception types / sentinels and when.
- **7.1.10 A minimal usage snippet.**

### 7.2 Selection
A class skill is warranted when the class is a **main access point**, OR has
**non-obvious construction/ownership/lifecycle**, OR is easy to misuse. Pure value/data
types with obvious shape may be covered by their package/component skill instead.

### 7.3 Use cases
- **7.3.1 Use it correctly first try.** As an agent, I learn construction, the key
  methods, return ownership, and disposal in one place and write correct code without
  reading the source.
- **7.3.2 Avoid the reuse trap.** As an agent, the state/concurrency note tells me
  whether I can reuse or share the instance.

## 8. Method-level skill — *the sharp, the complex, the protocol-bearing*

For an individual method. The **exception, not the rule** — reserved for methods that
genuinely need their own page.

### 8.1 Required content
- **8.1.1 Signature & semantics** — what it does; meaning of the return (incl. ownership
  / null / sentinel like `-1`).
- **8.1.2 Parameters** — meaning, ownership (`#`), valid ranges, units.
- **8.1.3 Pre/postconditions & required call order** — what must happen first (e.g.
  `captureStdout()` before `run()`).
- **8.1.4 The call sequence it participates in** — the surrounding protocol.
- **8.1.5 Failure modes** — exceptions/error codes/sentinels and the conditions.
- **8.1.6 Side effects** — mutates receiver? touches the filesystem? spawns a process?
- **8.1.7 Gotchas** — including toolchain quirks the agent can't infer (e.g.
  "`File.writeAllBytes` won't create parent dirs"; "pass length as a local, not a struct
  field").
- **8.1.8 A tight example.**
- **8.1.9 Cost/perf** notes when they affect choices.

### 8.2 Selection
Warranted only when a method is **multi-step/protocol-bearing**, has **subtle ownership
or failure semantics**, or carries a **known sharp edge**. Ordinary methods are covered
by their class skill.

### 8.3 Use cases
- **8.3.1 Follow the protocol.** As an agent, I call the method in the correct sequence
  with the right preconditions.
- **8.3.2 Dodge the known trap.** As an agent, the gotcha note steers me around a
  documented failure I would otherwise hit.

## 9. Application (tool) skills

An application/tool (e.g. `cajeta-mcp`, `cajetadoc`, the driver) is consumed by *invoking
it*, not by calling its classes. The same levels adapt:

- **9.1 Application overview** (≈ library) — what the tool does, how to invoke/build/run
  it, configuration (flags/env/file + precedence), and a task→command routing table.
- **9.2 Operation/command groups** (≈ package/component) — cohesive groups of
  subcommands or operations and how they relate.
- **9.3 Individual command** (≈ class/method) — invocation, inputs/outputs (incl. exact
  request/response shapes for a server like the MCP), exit codes, errors, and a worked
  invocation example.
- **9.4 Selection** — always one overview; a command skill per non-trivial command.
- **9.5 Use case.** As an agent, when I need to drive the tool, I read the overview's
  routing table, pick the command, and read its command skill for the exact I/O contract.

## 10. Quality bar / review checklist

A skill at any level is review-ready when:

- **10.1** It leads with the answer (routing/decision/purpose), not preamble.
- **10.2** It carries a real, idiomatic, ideally test-backed example with imports.
- **10.3** Ownership/lifecycle is explicit wherever a value crosses a boundary.
- **10.4** It states what the code does **not** do where that prevents a dead end.
- **10.5** It contains no fact that belongs to a different level (no duplication).
- **10.6** Frontmatter `applies-to` matches the level and the real canonical names.
- **10.7** It is as short as it can be while covering its level's required content.

## 11. Open points
- **11.1** Whether component skills should *replace* or *coexist with* the class skills
  of tightly-coupled members (§6.2) — pick a default during planning.
- **11.2** Exact `id` naming convention across levels (e.g. `io-file-overview`,
  `io-file-File`, `io-file-File-writeAllBytes`).
- **11.3** Whether application command skills reuse the `cja-skill://` URI scheme or a
  tool-scoped variant.
