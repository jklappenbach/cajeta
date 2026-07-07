# docs-refactor — teaching docs, work-spec organization, and tour coverage

## 1. Definition

### 1.1 Purpose
Restructure how this repository teaches the Cajeta language and how it organizes
engineering work artifacts. Today teaching documents, the language reference, work
specs, and machine-readable schemas are intermixed under `docs/`; plans lack any
lifecycle; and the language tour neither builds cleanly (until recently) nor covers
the shipped stdlib surface. This spec defines the target state for all four.

### 1.2 Scope
1. A book-style teaching guide at `docs/guide/` with a getting-started track and a
   language/stdlib ladder, every chapter linked to runnable tour code. The guide is
   a linear walk through the language and stdlib.
2. A hierarchical stdlib reference at `docs/stdlib/`: one directory per package,
   one document per main component class. Referenced from the guide; never merged
   into it.
3. Migration of work specs out of `docs/specs/` to root-level `specs/` with an
   active-work INDEX, a `schemas/` home for JSON artifacts, and an `archive/`
   lifecycle for completed work (plans likewise archive under `agents/archive/`;
   plans whose work already shipped archive during migration).
4. Tour hardening and expansion to cover every shipped stdlib package that exposes
   entry-point APIs, at the corresponding tour package.
5. Repository documentation hygiene: slim root `README.md`, refresh `Features.md`
   status, deduplicate the docs site checkout, fix broken cross-links.

### 1.3 Constraints
1. **Markdown-first authoring.** All documentation is authored as markdown in the
   repo; the site builder imports the markdown into HTML/React. No site-native
   authoring.
2. **Only shipped features are demoed.** The tour must compile and run; packages
   that are designed-only (e.g. `nucleo.*`) are tracked as pending tour work, never
   demoed speculatively.
3. **The tour is the example corpus.** Guide chapters link to tour sources rather
   than duplicating large examples inline; small inline snippets are fine.
4. **Verified against code.** Specs and design docs supply the content, but every
   documented claim is checked against the source or the running binary. Shipped
   and deferred behavior are never presented together as fact.
5. **Prose style.** Brief, plain, direct. No filler, no flowery phrasing, no
   "not just X" constructions.
6. **Workflow conventions come from twilight v0.6.0** (`td-project-workflow.md` at
   the repo root): specs at `specs/[name]-spec.md`, INDEX + archive lifecycle,
   plans at `agents/[name]-plan.md`.

### 1.4 Non-goals
1. Implementing unshipped stdlib (nucleo, deferred packaging formats, etc.).
2. Building or restyling the Astro docs site itself (it imports the markdown; site
   work is separate).
3. Fixing compiler bugs uncovered by the tour (e.g. the stack-drop double-free) —
   those are separate specs; this spec only depends on them (§6.8).
4. Rewriting `docs/specification/` (the language reference) — it keeps its name,
   location, and content except where chapters need cross-links.

## 2. Teaching guide (`docs/guide/`)

### 2.1 Requirements
1. Numbered, ordered chapters in four parts (Part I Getting started, Part II The
   language, Part III The standard library, Part IV Specialized), following the
   classic language-book template.
2. A `docs/guide/README.md` index listing all chapters with one-line descriptions.
3. Every chapter that discusses a runnable feature ends with (or inlines) links to
   the specific tour file(s) demonstrating it, as relative repo links. When a
   chapter refers to a class, it links the tour demo showing that class in use
   (same rule as the stdlib reference, §3.1.4).
4. The root `README.md` becomes a landing page: pitch, quickstart, and links to
   both the guide and the stdlib reference (§3); detailed content moves into guide
   chapters.
5. Existing loose teaching docs under `docs/*.md` — and stray directories
   (`docs/history/`, `docs/gpu/`, `docs/cajeta/`, `docs/buildtool/`) — are
   classified: absorbed into guide chapters, moved to `docs/specification/` if
   reference/design material (e.g. `Debugging.md` is a DAP/LSP design spec),
   relocated if misplaced (code under `docs/cajeta/` → `samples/`), or retired.
6. The guide is a linear walk. Part III chapters introduce each stdlib area and
   link to `docs/stdlib/` (§3) for API detail; method surfaces are not duplicated
   in the guide.

### 2.2 Chapter outline (Part I — Getting started)
1. `00` Introduction — what Cajeta is, hello world teaser.
2. `01` Installation — per-platform via `cvm`; install/build from GitHub source.
3. `02` Kick the tires — verifying the install; introduction to the main `cajeta`
   subcommands.
4. `03` Your first project — `cajeta.json`, standard layout, compiling; subsections:
   executables, uber-archives, libraries (`.cja`, `static-lib`, `shared-lib`), and
   the full artifact-kind enumeration (`obj-tree`, `uber-ir`, packaging family).
5. `04` Running your application — `cajeta run`, the capabilities security model.
6. `05` Debugging — shared concepts (breakpoints, `cajeta dap`, fiber pane,
   drop-chain inspection, ownership annotations, capability-violation breakpoints),
   then per-environment sections: gdb, IntelliJ, VS Code.

### 2.3 Chapter outline (Part II — The language)
1. `06` Keywords · `07` Comments (markdown in comments) · `08` Native types
   (including microfloats) · `09` Type kinds (`class`, `interface`, `enum`,
   `record`, `view`, `structure`, `annotation`, `@Kernel`) · `10` Allocation
   (`stack`/`heap`) · `11` Ownership & borrowing (`=` vs `#`, drops) · `12` Control
   flow · `13` Strings & formatting · `14` Templates & wildcards · `15` Lambdas &
   captures · `16` Operator overloading & `@AutoHash` · `17` Inheritance (single,
   multiple, interfaces) · `18` Annotations & synthesis · `19` DI & aspects ·
   `20` Error handling · `21` Reflection.

### 2.4 Chapter outline (Parts III–IV)
1. Part III: one chapter per stdlib area, mirroring tour packages — collections &
   streams, concurrency, file I/O, networking (TCP/UDP → HTTP → WS → DNS/URI/TLS),
   time, hashing, codecs, wire & views, math (intrinsic value types and the
   `cajeta.math` package), process, search.
2. Part IV: GPU programming (`@Kernel`/xpu), graphics (gfx/ifx), embedded targets,
   the MCP server tool, toolchain deep-dives (incremental compilation, compiler
   modes, lint, profiling).

### 2.5 Use cases
1. As a **newcomer**, when I land on the repo README, I can install Cajeta and run
   hello world by following Part I without reading any other document.
2. As a **learner**, when I finish a guide chapter, I can open the linked tour file
   and run the exact demo of what I just read.
3. As a **doc author**, when I add a guide chapter, I add it to the guide index and
   link its tour demo — there is one obvious place for each topic.
4. As the **site builder**, when the site is rebuilt, it imports the guide markdown
   from `docs/` unchanged — no manual conversion step.

## 3. Stdlib reference (`docs/stdlib/`)

### 3.1 Requirements
1. A hierarchical reference: one directory per stdlib package (package path minus
   the constant `cajeta.` root, e.g. `cajeta.io.net.http` →
   `docs/stdlib/io/net/http/`), one document per main component class —
   `docs/stdlib/collection/ArrayList.md`.
2. "Main component class" = a class with entry points and public methods meant for
   users to call. Internal helpers get no document. **Coverage rule: every worthy
   class without a reference document gets one created** — the shipped-surface
   audit's entry-point table is the initial inventory, and the rule applies to
   anything it missed.
3. **`@EntryPoint` doc tag.** Before reference authoring and tour coverage, an
   annotation pass adds the `@EntryPoint` doc tag to every entry-point method in
   `runtime/src/cajeta/` doc comments. The tag joins the cajeta-specific table in
   `docs/Documentation.md` and is recognized by `cajeta doc` (badge + an
   entry-points index, like `@FiberSafe`). The tags are the keys: reference docs
   document the tagged methods, tour coverage targets them, and
   `cajeta doc --emit-model-json` makes both checkable. Worthiness questions
   resolve in the source annotation, not in doc prose.
4. Each document covers the class's purpose, its `@EntryPoint`-tagged methods, and
   user-facing public methods, verified against `runtime/src/cajeta/` (§1.3.4).
5. Reference documents link to the tour demo(s) exercising the class, where one
   exists.
6. `docs/stdlib/README.md` indexes packages and classes.
7. The guide stays a linear walk; it links here for API detail instead of
   duplicating method surfaces. The reference never absorbs tutorial content.

### 3.2 Use cases
1. As a **learner**, when a guide chapter mentions a class, I follow its link to
   the reference document for the full method surface.
2. As a **user**, when I know the package, I browse `docs/stdlib/<package>/` and
   find one document per public class.
3. As a **stdlib author**, when I ship a class with public entry points, the
   coverage rule tells me a reference document is required and where it goes.

## 4. Work-spec and plan organization

### 4.1 Requirements
1. Root-level `specs/` holds all engineering work specs (`specs/[name]-spec.md`);
   `docs/specs/` is emptied and removed.
2. `specs/schemas/` holds machine-readable artifacts currently mixed into
   `docs/specs/` (`*-v1.json`, `schema-versioning.md`, protocol docs that define
   wire contracts).
3. `specs/INDEX.md` lists active work only: spec ↔ plan ↔ status (`draft` /
   `active` / `blocked`).
4. Completed work archives: spec → `specs/archive/`, plan → `agents/archive/`,
   INDEX row dropped.
5. During migration, every existing spec is classified: **done** (plan fully
   checked or work verifiably shipped) → archive; **active** → INDEX row;
   **spec-only** (no plan yet) → INDEX row with status `draft`. The classification
   table (spec → evidence → verdict) is reviewed by the developer before any file
   moves; moves use `git mv`.
6. Focus state is one stack per repo, `agents/focus.md` (twilight 0.6.0): entries
   `<plan>:<outline-id>` or `[explore: <what>]`, recording departures from plan
   order only. Old-style focus files (`agents/*-focus.md`, `agents/state/`) are
   removed (done 2026-07-03).
6. All inbound references to moved files (docs, CLAUDE.md files, scripts, plugin
   docs, `tools/cvm/README.md`'s broken `plans/installer/installer-plan.md` link)
   are updated or fixed.

### 4.2 Use cases
1. As a **developer**, when I want to know what work is in flight, I read
   `specs/INDEX.md` and see every active spec, its plan, and its status.
2. As a **developer**, when a plan's last unit is checked, the implement skill
   archives the pair and the INDEX shrinks — finished work stops cluttering the
   active view.
3. As a **doc reader**, when I browse `docs/`, I see only user-facing documentation
   — no work specs, no JSON schemas.
4. As a **new clone**, when the design skill scaffolds a project, it creates the
   same `specs/` + `agents/` shape this repo uses (twilight v0.5.0 parity).

## 5. Tour hardening and coverage

### 5.1 Requirements
1. **Coverage rule:** every shipped stdlib package that exposes entry-point APIs
   (keyed by `@EntryPoint` tags, §3.1.3) has a demo in the corresponding tour
   package. Every demo is registered in `Tour.cajeta` (or, for
   environment-dependent subsystems, in a documented separate entry point like
   the xpu tour).
2. The tour builds with zero errors and zero warnings, and runs to completion with
   exit code 0 (dependent on §5.6 for the MI drop fix).
3. New demo packages for shipped-but-untoured packages: `process`, `search`
   (distance/fuzzy/ngram), networking upper layers (`http`, `ws`, `dns`, `uri`;
   `tls` if environment permits), `cajeta.math` proper (tensor, geometry, linalg,
   fft, random, stats, poly, npio), `ifx` (Null-backend headless), xpu extensions
   (ray query/BVH, SDF, cooperative matrix, mesh).
4. Extensions to existing demos: collections (`Cache`, `Collectors`, `Sort`),
   codec (`Base64`, `Csv`), wire (`Schema`/`SchemaEncoder`, compression), hash
   (`Sha1`, `SipHash`), file I/O (`Path`, `Watcher`), net (`UdpSocket`,
   `Server`/`ServerBuilder`), gfx (swapchain, render graph).
5. Core-language gap demos: interfaces (fat-pointer dispatch), `@Encoding` binary
   serialization, static nested classes, static fields, `Optional<T>`,
   `Pair<K,V>`, stream `collect`/Collectors. (try-with-resources removed by
   design — the drop chain covers it; no demo.)
6. The main tour output mentions the separate xpu tour so GPU coverage is
   discoverable.
7. Designed-only packages are listed as pending tour work in the tour README, not
   demoed.
8. **Environment policy:** the main tour runs anywhere — no network beyond
   loopback, no GPU, no system deps beyond the base OS. Hardware- or
   library-dependent demos (xpu, gfx, ifx, tls) live in separate entry points
   that skip gracefully when the environment lacks support. The tour manifest's
   capability list grows only as demos require, each capability documented in
   `cajeta.json`. Work sequencing lives in the plan, not this spec.

### 5.2 Use cases
1. As a **learner**, when I run `./run.sh`, every registered demo executes and the
   process exits 0.
2. As a **stdlib author**, when I ship a new package, the coverage rule tells me
   exactly where its demo belongs and the tour README's pending list is where it
   waits until then.
3. As a **guide reader**, when a chapter links a tour file, that file exists, is
   registered, and demonstrates the chapter's topic.

## 6. Repository documentation hygiene

### 6.1 Root README
As a **visitor**, when I open the repo, the README gives me the pitch, a
quickstart, and links to both the guide (`docs/guide/`) and the stdlib reference
(`docs/stdlib/`) — not 31KB of mixed content.

### 6.2 Features.md
AMENDED 2026-07-06: `docs/` is site-bound user documentation only — never
state tracking. Features.md is a status register (a workflow artifact), so it
moves to `specs/Features.md` instead of `docs/` (move executed; the ~20
"tracked in Features.md" references across `docs/specification/` rewritten to
plain-text `specs/Features.md` pointers — site-bound markdown must not link
outside `docs/`). The status column still gets refreshed against the source
tree (e.g. HashSet, LinkedList, Heap, Collectors are shipped, not
"designed"); rows link to guide chapters and tour demos where they exist.

### 6.3 Docs site checkout
Exactly one Astro site checkout remains: `cajeta-docs-site/` (customized, sources
the repo `docs/` tree). `site/` is a leftover template copy and is removed.

### 6.4 Stdlib docstrings
Doc-comment examples compile as written; specifically, no `#Type local = …`
examples remain (completed 2026-07-03 across 34 files; guarded going forward by a
lint or review rule captured in the plan).

### 6.5 Skills parity
The repo's `td-project-workflow.md`, `CLAUDE.md` import, and directory shape match
twilight v0.6.0 (memory, plugin, and focus-stack consolidation completed
2026-07-03; the repo directory migration is §4).

### 6.6 Help-text completeness
`cajeta --help` lists only `archive`, `ide`, `jit-run`, `dap`; the build-tool
family (`init`, `build`, `test`, `install`, `publish`, dependency and skill
commands, manifest-task fallthrough) is undiscoverable. As a **newcomer**, when I
run `cajeta --help`, I see the build-tool command family that guide chapter `02`
documents. (Grounded command inventory, archetype list — `basic`, `library`,
`workspace`, `multi-binary`, `melt` — and the shipped-vs-deferred package-format
split live in the plan for chapters `02`–`03`.)

### 6.7 Broken `plans/` references
`tools/cvm/README.md:9` and `docs/buildtool/LibraryProjectType.md:5` link to a
vanished `plans/` directory; both are fixed or re-pointed during migration, and
`docs/buildtool/LibraryProjectType.md` (spec material) moves to
`docs/specification/buildtool/`.

### 6.8 External dependency — stack-drop double-free
Cleared 2026-07-03: stack-drop-classref shipped (archived at
`specs/archive/stack-drop-classref-spec.md`); the tour builds and runs to exit 0.
§5.2.1 is unblocked.
