# `.cja` Archive Source View — IR-as-Cajeta & Author-Declared Sources — Spec

> Status: **DRAFT for review** (design skill). Scopes an IDE-plugin capability to
> **browse a `.cja` archive, open a class, and view its code** — reconstructed
> from Cajeta IR where the archive carries it, and/or fetched from
> author-declared source URLs. Opens with a **feasibility analysis** (the
> "what would it take to present IR as Cajeta" question), then specs the
> mechanisms.
>
> Companions: [`docs/BuildTool.md`](../docs/specification/buildtool/BuildTool.md) (archive/build model),
> `docs/specs/archive-v1.md` (the `.cja` on-disk format — authoritative),
> [`cajeta-ir-spec.md`](archive/cajeta-ir-spec.md) / [`cajeta-ir-phase-b-spec.md`](cajeta-ir-phase-b-spec.md)
> (CIR), `manifest-v1.json` (manifest schema this extends). Plan lives at
> `agents/cja-source-view-plan.md` once approved.

---

## 1. Definition

### 1.1 Purpose
Let a developer open a published `.cja` dependency in IntelliJ — the way Java
devs open a `.jar` to read decompiled or source-attached classes — and **read
the code** of the classes it contains. Two complementary sources of that code:
1. **IR-as-Cajeta** — render the archive's bundled **Cajeta IR (CIR)** back into
   readable Cajeta. Always available offline when the archive ships IR; produces
   semantically faithful but *reconstructed* source.
2. **Author-declared original source** — library authors publish, in the
   manifest/archive, a pointer to the original source tree (e.g. a public Git
   repo URL + the tagged-release commit hash). The IDE fetches and shows the
   **original** source, byte-for-byte.

### 1.2 Scope (v1)
- A `.cja` archive **browser**: open a `.cja` (as a dependency or a file),
  navigate its packages/classes, open a class to a read-only editor.
- An **IR-as-Cajeta** renderer turning bundled CIR into readable Cajeta
  (best-effort, clearly marked as reconstructed).
- A manifest **`sources` block** (new) by which authors declare a source-tree
  location (VCS kind, URL, commit/tag); baked into the published `.cja`.
- A **source-resolution precedence** (original-via-URL → IR-render → signature
  stub) with per-class indication of which was used.
- Fetch + cache of author-declared source (Git checkout at the pinned commit /
  archive download), with offline/failure degradation to IR-render.
- Reuse of the existing syntax tier (highlighting, structure view, folding) for
  the displayed Cajeta, since it is ordinary `.cajeta` content.

### 1.3 Problem
A `.cja` dependency is today an opaque binary. A developer who wants to
understand a library's behavior — step into it while debugging, read an API's
implementation, confirm what a method does — has no in-IDE way to see its code.
Java solved this with sources jars and decompilers; Cajeta has neither surfaced
in the IDE. Two facts make a good solution possible: archives can ship CIR
(`emit: archived-ir`, `docs/BuildTool.md` §action catalog), and CIR preserves
the language semantics needed to render readable Cajeta.

### 1.4 Constraints & dependencies
- **Archive format.** Reading a `.cja` follows `docs/specs/archive-v1.md`
  (on-disk format). The plugin reads archives; it never writes them.
- **IR availability.** IR-as-Cajeta only applies when the archive was built with
  `emit: archived-ir` (or `exploded-ir`). An `executable`-only `.cja` has no IR;
  for it, only the author-declared-source path applies.
- **CIR contract.** Rendering targets CIR as specified in `cajeta-ir-spec.md`
  (typed SSA/CFG preserving generics, closures, ownership, drop, storage class).
  The renderer depends on a stable, documented CIR serialization in the archive
  — a build-tool/compiler-side prerequisite where one does not yet exist (§7).
- **Network.** Author-declared-source fetch needs network + a reachable VCS
  host; everything degrades gracefully offline to IR-render or stub (§6, §9.3).
- **Behavioral cores plain JVM** (archive index parse, CIR→Cajeta render model,
  source-URL resolution) — unit-testable without a platform fixture, per the
  debugger/widget precedent.

### 1.5 Non-goals (v1)
- A general-purpose decompiler for arbitrary LLVM IR or machine code — only CIR
  (which retains the needed semantics) is rendered.
- Editing classes inside an archive — the view is strictly read-only.
- Round-trip fidelity / recompilation of rendered IR-as-Cajeta — it is a
  *reading* aid, not a source-of-truth the user re-builds.
- Hosting or proxying source — the IDE fetches from the author's declared
  location; it does not republish source.

---

## 2. Feasibility analysis — "what would it take to present IR as Cajeta"

This section is the exploration the capability was scoped around; its verdict
drives the precedence in §6.

### 2.1 What CIR makes easy
CIR is **far higher-level than LLVM IR** (`cajeta-ir-spec.md` §1): it preserves
unmonomorphized generics, closures as first-class values, ownership/borrow,
explicit drop points, and value-vs-heap storage class — plus full types. So,
unlike a classic decompiler working from erased machine code, an IR-as-Cajeta
renderer already has the *semantics and types* it needs. Declarations, method
signatures, field layout, generic parameters, and ownership annotations
reconstruct **faithfully**.

### 2.2 What is genuinely hard
- **SSA/CFG → structured control flow.** CIR is SSA over a control-flow graph;
  surface Cajeta is structured (`if/else`, `while`, `for`, `switch`).
  Reconstructing readable structured code from basic blocks + φ-nodes is a
  control-flow-**structuring** problem (loop/conditional recovery). Tractable
  (well-trodden in decompilation) but the bulk of the work.
- **Names.** SSA values are synthetic; original local/param names survive only
  if CIR carries them as debug metadata. Where absent, the renderer emits
  stable synthetic names (`v0`, `tmp1`) — readable, not original.
- **De-sugaring.** Lambdas, streams, operator overloads, and `for`-comprehension
  sugar may render in a lowered-but-equivalent form, not the author's surface
  spelling.

### 2.3 Verdict
**Feasible, as a reconstructed view** — semantically faithful Cajeta with
recovered structure, faithful signatures/types/ownership, and synthetic names
where debug names are absent. It is **not** guaranteed to equal the original
source. Therefore IR-as-Cajeta is the **always-available offline fallback**, and
**author-declared original source (§5) is the preferred path** when the author
provides it. This is the §6 precedence. (If, on deeper CIR review, structuring
proves materially harder than estimated, v1 may ship the archive browser +
signature stubs + author-source path first, and land the IR renderer as a fast
follow — noted in the plan's unit ordering.)

### 2.4 Use cases
- **2.4.1** As a developer, when I open an IR-bearing `.cja` class with no
  author source declared, then I see reconstructed Cajeta with a clear
  "reconstructed from IR" banner and synthetic names where originals were
  unavailable.
- **2.4.2** As a developer, when original names survive in CIR debug metadata,
  then the rendered code uses them rather than synthetic placeholders.

---

## 3. The `.cja` archive browser

### 3.1 Requirements
Open a `.cja` — both as a resolved **dependency** (surfaced under an "External
Libraries"-style node for the project's `cajeta.lock` deps) and as a **file**
(double-click a `.cja` in the project view) — and present its contents as a tree
of packages → classes, per `docs/specs/archive-v1.md`. Selecting a class opens
it read-only in an editor showing its code via the §6 resolution. Archive
metadata (name, version, flavor, whether IR is present, declared sources) is
viewable.

### 3.2 Use cases
- **3.2.1** As a developer, when I expand External Libraries, then each `.cja`
  dependency from `cajeta.lock` appears, expandable into its packages/classes.
- **3.2.2** As a developer, when I double-click a `.cja` file, then it opens as a
  navigable archive (not raw bytes).
- **3.2.3** As a developer, when I open a class node, then its code opens
  read-only with full Cajeta highlighting/structure view (reusing the syntax
  tier), and an indicator of the source used (original / reconstructed / stub).
- **3.2.4** As a developer, when an archive carries no IR and no declared source,
  then classes still open to a **signature stub** (declarations + types from the
  archive index, bodies elided) rather than failing.

---

## 4. IR-as-Cajeta rendering

### 4.1 Requirements
A renderer mapping a class's CIR (read from the archive per the CIR
serialization contract, §7) to readable Cajeta text: declarations, signatures,
generics, ownership annotations faithful; control flow structured; names
original-where-available else synthetic; lowered sugar acceptable. Output is
`.cajeta`-typed read-only content. The render core is plain JVM and unit-tested
on CIR fixtures → expected Cajeta. A per-class cache keyed by archive + class +
renderer version.

### 4.2 Use cases
- **4.2.1** As a developer, when I open an IR-bearing class, then I read
  structured Cajeta (recovered `if`/loops), not a basic-block dump.
- **4.2.2** As a developer, when I open the same class twice, then the second
  open is served from cache (no re-render).
- **4.2.3** As a developer, when a class's CIR uses an unsupported construct,
  then the renderer degrades that region to an annotated lowered form rather
  than failing the whole class.

---

## 5. Author-declared source attachment

### 5.1 Requirements
Extend the manifest (`manifest-v1.json`, under `details` or a new top-level
`sources`) with an author-declared source-location block, baked into the
published `.cja`:

```jsonc
"sources": {
  "vcs": "git",                                  // git (v1); others later
  "url": "https://github.com/acme/widgets",      // public, fetchable
  "commit": "9f3c1e0…",                          // pinned; preferred
  "tag": "v1.2.5",                               // recorded for provenance
  "path": "src"                                  // optional subdir root
}
```

- **5.1.1** The block is **optional**; its presence enables original-source view.
- **5.1.2** `commit` (immutable SHA) is authoritative; `tag` is provenance/
  display. The IDE fetches the tree at the pinned commit so the source matches
  the published artifact's revision.
- **5.1.3** The published `.cja` carries this metadata (per `archive-v1.md`) so a
  consumer needs only the archive to know where to fetch source.
- **5.1.4** The IDE fetches (shallow Git fetch/checkout at the commit, or archive
  download), maps an archive class to its source file by package/path, and
  caches the checkout under a workspace cache keyed by url+commit.
- **5.1.5** A mismatch (declared source's class set diverges from the archive's)
  is surfaced as a non-fatal warning; the IDE still shows what it can and may
  fall back to IR-render per class.

### 5.2 Use cases
- **5.2.1** As a library author, when I add a `sources` block with a repo URL and
  release commit and publish, then consumers' IDEs can show my original source.
- **5.2.2** As a developer, when I open a class from a `.cja` whose author
  declared sources, then the IDE fetches the pinned revision and shows the
  **original** source (highlighted, navigable), labeled "original source".
- **5.2.3** As a developer, when I am offline or the host is unreachable, then
  the IDE falls back to IR-render (or stub) and notes that original source was
  unavailable, without blocking.
- **5.2.4** As a developer, when the declared commit no longer exists upstream,
  then the IDE reports it and falls back, rather than silently showing a wrong
  revision.

---

## 6. Source-resolution precedence

### 6.1 Requirements
Per class, resolve the displayed code in order, with the chosen source shown in
the editor's banner and the archive tree:
1. **Original source** via author-declared `sources` (§5), if declared and
   fetchable at the pinned commit.
2. **IR-as-Cajeta** render (§4), if the archive carries CIR.
3. **Signature stub** (§3.1) from the archive index.
A user override may force IR-render even when original source is available
(e.g. to inspect what the optimizer sees).

### 6.2 Use cases
- **6.2.1** As a developer, when both original source and IR are available, then
  I see original source by default and can switch to the IR-as-Cajeta view.
- **6.2.2** As a developer, when only IR is available, then I see the
  reconstructed view labeled as such.
- **6.2.3** As a developer, when neither is available, then I see the signature
  stub, never an error page.
- **6.2.4** As a developer debugging, when I step into a `.cja` dependency, then
  the debugger opens the same resolved view for the stopped frame's class so
  breakpoints/positions line up (best-effort; exact only with original source).

---

## 7. Compiler / build-tool prerequisites (cross-repo)

- **7.1 CIR-in-archive serialization contract.** A documented, versioned
  serialization of CIR within the `.cja` (`archive-v1.md` + `cajeta-ir-spec.md`)
  that the renderer reads. Required for §4; if absent today, it is the first
  prerequisite unit.
- **7.2 Manifest `sources` block.** Schema addition to `manifest-v1.json` +
  loader (`src/cajeta/buildtool/Manifest.cpp`) + bake-into-archive on publish.
  Required for §5.
- **7.3 Optional CIR debug-names.** Carrying original local/param names in CIR
  debug metadata improves §4 fidelity (synthetic otherwise); a quality
  enhancement, not a blocker.

---

## 8. Settings

### 8.1 Requirements
Cajeta settings additions: enable/disable author-source fetch (default on),
source-cache location + size cap, default per-class view (original vs IR when
both exist), and a "trust hosts" allowlist for source fetch. Persisted via
`CajetaSettings`.

### 8.2 Use cases
- **8.2.1** As a developer, when I disable source fetch, then archives render via
  IR/stub only and the IDE makes no network calls for sources.
- **8.2.2** As a developer, when I set the default view to IR, then IR-bearing
  classes open in the reconstructed view even when original source exists.

---

## 9. Non-functional requirements

- **9.1 Performance.** Archive indexing, IR render, and source fetch run off the
  EDT; class open is incremental; renders/fetches cache; large archives index
  lazily.
- **9.2 Cross-platform.** Linux/macOS/Windows; Git fetch via the platform's VCS
  integration or a bundled client path; no hard dependency on a system `git`
  beyond what the platform already requires.
- **9.3 Graceful degradation.** Every failure mode (no IR, no source, offline,
  unreachable host, missing commit, malformed archive, unsupported CIR) falls
  back along the §6 precedence and surfaces a non-fatal indicator — never an
  error dialog or hang.
- **9.4 Security/trust.** Source fetch only from author-declared hosts, subject
  to the §8 allowlist; fetched source is read-only and never executed; the
  pinned commit guards against tag-moved-under-us substitution.
- **9.5 Testability.** Archive index parse, CIR→Cajeta render, source-URL
  resolution, and precedence selection are plain-JVM cores with direct unit
  tests; platform editor/VCS/integration tested against real fixtures, skipping
  cleanly when a binary/host is absent.
