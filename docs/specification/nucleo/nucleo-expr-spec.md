# Núcleo Expression Engine — Specification

> Status: draft for review (2026-06-23). The **lazy expression graph + fusion engine** —
> núcleo's compute core (`dev.cajeta.nucleo.expr`). Layer-1b. Companion analysis:
> `python-stack-analysis.md` §2.2 (one-engine-many-skins); siblings `nucleo-column-spec.md`
> (the columnar buffer it computes over), `nucleo-autograd-spec.md` (differentiation of an
> expression), `nucleo-frame-spec.md` (the dataframe surface), `records-spec.md` (schemas).
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §10, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
The **expression engine** is the single piece of machinery that builds a computation **lazily**
as a graph, **optimizes** the whole chain before running it, and **lowers** it to CPU or GPU.
It is the language-level answer to NumPy's temporary-materialization problem — the reason
numexpr, Cython, and XLA-tracing all exist (analysis §1.3). One engine serves **both** tensor
expressions (`(t - t.mean()) / t.std()`) **and** dataframe pipelines (`filter → groupby → agg`),
because a non-null numeric column and a 1-D tensor buffer are the same bytes (analysis §2.3,
`nucleo-column-spec.md`). This is the "build one engine, two APIs" thesis made into a component.

### 1.2 Scope
- A **lazy expression IR** — operations compose into a graph instead of executing eagerly.
- An **optimizer** — a pass set that rewrites the graph before execution: operator **fusion**
  (collapse an elementwise chain into one kernel with no materialized temporaries), and the
  relational rewrites **predicate pushdown** and **projection pushdown**.
- A **lowering** step — emit the optimized graph as one or more kernels targeting CPU or GPU
  (via `cajeta.xpu`).
- The **eager-vs-lazy boundary** — the rule for when an expression *forces* (materializes).
- The seam by which a **differentiable** expression hands its graph to autograd
  (`nucleo-autograd-spec.md`).

### 1.3 Non-goals
- **The columnar buffer itself** — physical layout, validity bitmaps, Arrow interchange live in
  `nucleo-column-spec.md`. This spec *consumes* columns/tensors; it does not define them.
- **The dataframe surface** (`col.price > 0`, `groupBy`, `agg`, `collect`) — that ergonomics
  and schema-typing live in `nucleo-frame-spec.md`. This spec defines the *engine* those
  operations plan and run on.
- **The autograd rule-set** (VJP/JVP rules, the MIR-pass driver, the eager tape) —
  `nucleo-autograd-spec.md`. This spec defines only the *handoff seam* (§7).
- **A cost-based query optimizer / statistics-driven join reordering.** v1 is rule-based
  rewrites (fusion + pushdown); cost-based planning is a deferred question (§10).
- **Cross-machine / distributed execution.** Single-process, CPU-or-GPU.

### 1.4 Relationship to existing constructs
- The engine is built on the shipped substrate: `cajeta.math.Tensor` and `Storage<T>` (a single
  contiguous, C-order, dense buffer — already the non-null-column case), `cajeta.xpu` (the
  device/lowering target), monomorphized templates (so an expression over `Tensor<float32>` and
  one over `Tensor<float64>` are distinct, fully-typed graphs), and operator overloading (so
  `t - t.mean()` and `col.price > 0.0` *build graph nodes* rather than computing eagerly).
- The target-experience snippets it realizes: `var normalized = (t - t.mean()) / t.std();` as
  one fused kernel (target-experience §1) and the lazy `filter → groupBy → agg → sort →
  collect()` chain that "plans the whole chain before executing" (target-experience §3).
- It is a **compiler-resident** concern where fusion-to-IR is involved: the same mid-level-IR
  placement the autodiff and `@Jit` decisions require (`language-foundations.md` §1.3), so a
  fused expression and a differentiated one share the stage where structure is still legible.

> **TBD (plan-time):** [X1] Is the lazy graph a *runtime* data structure (built by overloaded
> operators, optimized and lowered at runtime — the Polars/XLA-trace model), a *compile-time*
> expression-template construct fused at codegen (the zero-cost expression-templates model the
> analysis §1.3 names), or **both** (compile-time for statically-shaped tensor expressions,
> runtime for shape-dynamic dataframe pipelines)? This is the load-bearing representation fork.

## 2. The lazy expression graph

An expression is **built, not run**. Composing operations yields a graph node; the computation
is deferred until the result is forced (§5).

```cajeta
// Tensor expression — builds a graph, runs nothing yet.
var expr = (t - t.mean()) / t.std();

// Dataframe pipeline — builds a graph, runs nothing until collect().
var plan = ticks.filter(col.price > 0.0).groupBy(col.venue).agg(...);
```

**Use cases**
- **2.1** As a developer, when I write `(t - t.mean()) / t.std()`, then each operator builds a
  node in an expression graph rather than computing a value, and no buffer is materialized at
  this point.
- **2.2** As a developer, when I write a dataframe predicate `col.price > 0.0`, then the
  comparison builds a predicate node in the same graph IR a tensor expression uses (one IR,
  two surfaces — analysis §2.2).
- **2.3** As a developer, when I compose a chain (`.filter(...).groupBy(...).agg(...)`), then the
  chain accumulates into a single graph whose root is the final operation, with earlier
  operations as its inputs.
- **2.4** As a developer, when I reference the same sub-expression twice
  (`var c = a + b; var r = c * c;`), then it is a shared node (a DAG), not duplicated work.
- **2.5** As a developer, when I inspect or print an unforced expression, then I can see its
  graph/plan (for debugging) without triggering execution.

> **TBD (plan-time):** [X2] The expression IR representation — node kinds (elementwise op,
> reduction, broadcast, contraction, relational filter/project/groupby/join/sort/window), how
> dtype and (statically-known) shape are carried on a node, and whether tensor and relational
> nodes share one node type or are two node families over one graph container.

## 3. Optimization — fusion (the tensor answer)

The whole chain is optimized **before** execution. The headline tensor rewrite is **operator
fusion**: an elementwise chain collapses into **one** kernel that streams element-by-element,
materializing **no** intermediate temporaries — the native answer to NumPy's `a*b + c*d`
allocating a temporary per operator.

**Use cases**
- **3.1** As a developer, when I execute a fused elementwise expression
  (`(t - t.mean()) / t.std()`), then it runs as **one** kernel over the buffer and allocates **no**
  intermediate result buffers (only the final output), regardless of how many operators the
  expression has.
- **3.2** As a developer, when an elementwise chain reads each input element once and writes each
  output element once, then fusion guarantees a single pass (no re-read of intermediates from
  memory), turning a memory-bound NumPy chain into a compute-bound fused kernel.
- **3.3** As a núcleo author, when a reduction sits inside an elementwise chain
  (`t - t.mean()` — `mean` reduces, the subtraction broadcasts), then the optimizer fuses what is
  legally fusible (the broadcasted elementwise tail) and schedules the reduction as its own stage,
  rather than failing to optimize the whole expression.
- **3.4** As a developer, when an intermediate sub-expression is used by **two** consumers
  (a shared DAG node), then the optimizer may materialize it once and share it rather than
  re-fusing it into each consumer (a fuse-vs-materialize tradeoff, not blind fusion).
- **3.5** As a developer, when I write a numerically identical expression in two different operator
  orders, then fusion must not change observable results beyond floating-point reassociation that
  the language already permits elsewhere (fusion is an optimization, not a semantics change).

> **TBD (plan-time):** [X3] The optimizer pass set and its ordering — at minimum: elementwise
> fusion, broadcast propagation, common-sub-expression sharing, dead-node elimination, constant
> folding, the fuse-vs-materialize heuristic (§3.4). Whether reductions/contractions participate
> in fusion or only bound fusion regions. Whether passes are fixed-point or single-shot.

## 4. Optimization — pushdown (the relational answer)

The **same** optimizer, over the **same** graph, performs the relational rewrites a query planner
does — because a dataframe pipeline *is* an expression graph (analysis §3.5: Polars/DuckDB plan
the chain before executing). The two headline rewrites are **predicate pushdown** (apply filters
as early as possible, ideally at scan time so unmatched rows are never read) and **projection
pushdown** (read only the columns the pipeline actually consumes).

**Use cases**
- **4.1** As a developer, when I write `scanParquet(...).filter(col.price > 0).groupBy(...)`, then
  the plan is built and optimized **before** any row is processed; nothing executes until the
  result is forced (`.collect()` — §5).
- **4.2** As a developer, when a `filter` sits above a scan, then **predicate pushdown** moves the
  predicate down to the scan so rows failing it are skipped (not read, not materialized, not
  carried through the pipeline) — observably less work for a more selective predicate (use case
  shown in target-experience §3).
- **4.3** As a developer, when my pipeline references only a subset of a wide table's columns, then
  **projection pushdown** restricts the scan to those columns so unreferenced column buffers are
  never read.
- **4.4** As a developer, when predicate pushdown reaches a scan over a source with statistics
  (e.g. zone-maps / per-chunk min-max, the index-interface backlog in `nucleo-frame-spec.md` §9), then whole chunks provably
  outside the predicate may be skipped entirely. *(The skip mechanism is the index subsystem's;
  the engine's job is to *expose* the pushed predicate to it.)*
- **4.5** As a developer, when a filter→groupby→sort chain is optimized, then the optimizer may
  reorder/combine stages where it is provably equivalent (e.g. filter before groupby), without
  changing the result set.

> **TBD (plan-time):** [X4] How far the relational rewrite set goes in v1 — predicate +
> projection pushdown are committed; filter reordering, slice/limit pushdown, and groupby/join
> rewrites are candidates. **Cost-based** planning (join reordering by statistics) is explicitly a
> deferred question, not v1.

## 5. The eager-vs-lazy boundary (forcing)

An expression is lazy until something **forces** it. The forcing rule must be predictable: a
developer needs to know, looking at code, where computation actually happens.

**Use cases**
- **5.1** As a developer, when I build an expression and never force it, then no kernel runs and no
  result buffer is allocated (laziness is observable — building is free).
- **5.2** As a dataframe developer, when I call an **explicit** terminal (`.collect()`), then the
  whole accumulated plan optimizes and executes at that single point (target-experience §3:
  "nothing ran until here").
- **5.3** As a tensor developer, when I assign a tensor expression to a typed tensor variable, read
  a scalar out of it, pass it to a non-engine function, or print its values, then it **forces** at
  that boundary (the result is needed as concrete bytes). *(Whether tensor assignment alone forces,
  or stays lazy until a true consumer, is [X5].)*
- **5.4** As a developer, when an expression is forced, then its result is materialized **once**;
  forcing the same handle again returns the cached result rather than recomputing (forcing is
  idempotent on a value).
- **5.5** As a developer, when forcing fails (e.g. a shape mismatch the type system could not catch,
  or a device-unavailable lowering), then the failure is reported at the force point with the
  expression context, fail-loud (analysis: completeness via fail-loud).

> **TBD (plan-time):** [X5] The exact force triggers for the **tensor** surface — does the engine
> follow the dataframe's *explicit-terminal* model (`.collect()`/`.eval()` everywhere, maximally
> predictable), an *implicit-on-consume* model (force when a concrete value escapes the engine —
> the NumPy *feel*), or a hybrid (implicit for tensors where a typed `Tensor` result is the natural
> escape, explicit for dataframes)? Interacts with [X1].

## 6. Lowering to CPU or GPU

The optimized graph is **lowered** — emitted as executable kernels — to either CPU or GPU. GPU
lowering goes through `cajeta.xpu` (the shipped device model, analysis §3.1/§4.1). The same
optimized graph must be lowerable to either target; device choice does not change which expression
the developer wrote.

**Use cases**
- **6.1** As a developer, when I force an expression whose inputs are host buffers, then it lowers
  to a CPU kernel (the default target).
- **6.2** As a developer, when I force an expression whose inputs are device buffers (or I request
  the GPU target), then the **same** optimized graph lowers to a GPU kernel via `cajeta.xpu`, with
  fusion preserved (one fused device kernel, not one dispatch per operator — the per-op-dispatch
  death the analysis §4.4 warns about).
- **6.3** As a developer, when an expression mixes host and device inputs, then this is surfaced as
  an error consistent with núcleo's "device lives in the type, host/device mix is a compile error"
  stance (target-experience §2), not a silent copy.
- **6.4** As a núcleo author, when an expression lowers, then the fusion decisions from §3 carry
  through to the chosen backend — fusion is a property of the optimized graph, decided before the
  target is picked, so CPU and GPU both get the fused form.
- **6.5** As a developer, when a graph contains an operation a target cannot fuse (or cannot run),
  then the engine either splits it into separately-lowered stages or fails loud — never silently
  drops or mis-lowers it.

> **TBD (plan-time):** [X6] Where lowering physically happens — fully compiler-resident
> (the expression is fused into IR at codegen, the `@Jit`/mid-level-IR placement of
> `language-foundations.md` §1.3, so the fused kernel is ordinary emitted IR), runtime
> code-generation from a runtime graph, or a split keyed to [X1]. And how the CPU↔GPU target is
> selected (input residence, an explicit `.to(device)` / `.gpu()` on the expression, or both).

## 7. One engine over columns and tensors (the shared core)

The load-bearing claim: a tensor expression and a non-null-column expression are **the same
engine** because they are the same bytes (analysis §2.2/§2.3). This section pins the *requirement*
that they share, not merely resemble, the machinery.

**Use cases**
- **7.1** As a núcleo author, when I apply an elementwise expression to a non-null numeric column
  and to a 1-D tensor of the same dtype, then **the same** graph nodes, optimizer passes, and
  lowered kernels are used for both — no per-surface duplicate engine (analysis §2.2).
- **7.2** As a núcleo author, when a dataframe `agg` computes `(col.price * col.size).sum()`, then
  the inner `col.price * col.size` is an elementwise tensor-style fusion over the column buffers
  and the `.sum()` is a reduction node — i.e. the relational and tensor IRs interoperate in **one**
  graph (the target-experience §3 vwap expression).
- **7.3** As a developer, when a column carries a validity bitmap (nullable), then the engine
  computes over the values **and** propagates validity through the expression
  (null-aware semantics); when the column is non-null (bitmap absent), the engine runs the
  raw-buffer fast path identical to a tensor (the invariant pays off — `nucleo-column-spec.md`).
- **7.4** As a graphics author (flagship), when a splat scene's columns are read both as Arrow
  columns (filter/slice) and as tensor buffers (the fused differentiable kernel) within one
  pipeline, then no conversion/marshalling layer sits between the two views — they are one
  engine over one set of bytes (analysis §4.6, target-experience §4).

> **TBD (plan-time):** [X7] Null/validity propagation semantics in fused kernels — does the fused
> kernel branchlessly compute-then-mask (compute over garbage where invalid, AND the bitmaps), or
> does validity gate computation? And how a reduction treats nulls (skip vs. propagate) by default.
> Resolved jointly with `nucleo-column-spec.md`.

## 8. Differentiable expressions (autograd seam)

An expression graph is exactly the structure autograd differentiates. This spec owns the
**handoff**: an expression must be able to present its graph to the autograd engine so a backward
pass can be built over it (`nucleo-autograd-spec.md`). The differentiation rules and drivers live
there; the requirement here is that the expression IR is *legible to* them at the right stage.

**Use cases**
- **8.1** As a developer, when I differentiate an expression (`Grad(f)` / `@Grad`, per
  `language-foundations.md` §1.7), then autograd consumes the **same** expression graph this engine
  builds — there is no second, parallel tracing mechanism for differentiation.
- **8.2** As a núcleo author, when the backward pass is emitted, then it is ordinary expression IR
  that **re-enters this engine** — so the backward fuses, shares sub-expressions, and lowers to
  CPU/GPU exactly like the forward (analysis §4.4: "the backward is emitted as ordinary IR that
  then fuses, DCEs, and remats").
- **8.3** As a núcleo author, when fusion would discard an intermediate that the backward pass
  needs (an activation), then the materialize-vs-rematerialize decision is coordinated with the
  checkpoint policy (`nucleo-autograd-spec.md` §9, `language-foundations.md` §1.7), not made blind
  to differentiation.
- **8.4** As a developer, when an expression is *not* being differentiated, then it carries no
  autograd overhead — differentiability is opt-in at the transform site, not a tax on every
  expression (no global grad state — the torch mistake the analysis §3.2 drops).

> **TBD (plan-time):** [X8] The exact seam shape — does autograd walk this engine's IR directly
> (shared in-memory representation), or does the engine expose a stable graph-visitor/handoff
> interface autograd consumes? Ties to whether both are mid-level-IR-resident ([X1], [X6]) — if so,
> the seam is "same IR, different pass," the cleanest form.

## 9. Acceptance criteria (spec-level)
- A multi-operator elementwise tensor expression executes as **one** kernel and allocates **no**
  intermediate buffers (only the final result) — demonstrable by allocation count.
- A dataframe `filter → groupby → agg` chain builds a plan that **optimizes before executing**, and
  forces only at an explicit terminal.
- **Predicate pushdown** and **projection pushdown** are observable: a more selective filter / a
  narrower projection does measurably less work (rows/columns read).
- The **same** optimized graph lowers to **both** CPU and GPU (via `cajeta.xpu`), preserving
  fusion.
- An elementwise expression over a non-null column and over a tensor of the same dtype uses the
  **same** engine (one code path, demonstrable).
- The eager-vs-lazy boundary is **predictable**: building never executes; forcing happens only at
  the defined trigger points and is reported fail-loud on failure.
- A differentiable expression's backward pass re-enters the engine and is itself fused/lowered (the
  autograd handoff seam exists), with no autograd overhead on non-differentiated expressions.

## 10. Open questions (resolve at plan time)
- **[X1]** Expression IR residence: runtime graph vs. compile-time expression templates vs. both
  (tensor compile-time / dataframe runtime) — the load-bearing representation fork (§1.4).
- **[X2]** The expression IR node model — node kinds, dtype/shape carriage, one node family vs.
  two over a shared container (§2).
- **[X3]** The optimizer pass set and ordering for fusion (incl. the fuse-vs-materialize heuristic
  and whether reductions/contractions fuse) (§3).
- **[X4]** The relational rewrite set beyond the committed predicate+projection pushdown; whether
  any cost-based planning lands in v1 (§4).
- **[X5]** Tensor-surface force triggers — explicit-terminal vs. implicit-on-consume vs. hybrid
  (§5).
- **[X6]** Where lowering happens (compiler-resident vs. runtime codegen) and how the CPU/GPU
  target is selected (§6).
- **[X7]** Null/validity propagation semantics in fused kernels and null-handling in reductions —
  jointly with `nucleo-column-spec.md` (§7).
- **[X8]** The autograd handoff seam shape — shared IR vs. a graph-visitor interface — jointly with
  `nucleo-autograd-spec.md` (§8).
- Whether window/rolling and join operations are in the v1 graph IR or deferred to a later frame
  milestone (touches §2, §4; resolved with `nucleo-frame-spec.md`).
