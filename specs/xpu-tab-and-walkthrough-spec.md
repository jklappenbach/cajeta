# xpu-tab-and-walkthrough — one XPU tab from device to language model, and an end-to-end walkthrough of one model (spec)

> Status: **draft**, filed 2026-10-25 at Julian's request ("one thing I want
> on the focus stack"), while the benchmark-reference work runs. Fable lane:
> the writing and the diagrams; Opus lane: the tree move, redirects and the
> site's nested-section rendering.

## 1. Definition

### 1.1 The problem
The site has **Kernels** and **Language models** as top-level tabs beside
Guide, Specification and Stdlib, and nothing between them says how the
pieces stack. A reader who wants to know what happens when a model runs has
to assemble it from `docs/kernels/llm.md` (families, selection, adaptation),
`docs/llm/inside.md` (the forward path, the scheduler, the cache) and
`docs/specification/xpu/CajetaXPU.md` (the contract), and none of them walks
one concrete model from the first instruction to the last token, or says
where the NVIDIA and AMD paths differ and where the optimizer acts.

### 1.2 The layering, stated once
The compute stack reads bottom-up, and the tab reads top-down the same way:

| Layer | Package or library | What it owns |
|---|---|---|
| **XPU** | `cajeta.xpu` | the kernel model, the device, buffers, streams, timers; the kernel adaptor (compile-time adaptation: wave, tiers, occupancy, templates, gates), the tile manifest and the tile scheduler, the shape filter and autotune store |
| **Math** | `cajeta.math` | `Tensor`, linear algebra, FFT, the dtype system, `.npy` |
| **Nucleo** | `cajeta.nucleo` | columns and tables, the autograd tape, `Fuse`, `Grad`, `Vmap`, `Jit` |
| **ML** | `dev.cajeta.ml`, `dev.cajeta.xgboost` | estimators, layers, trainers, boosted trees |
| **LLM** | `dev.cajeta.llm` | the decoder-only engine: load, prefill, decode, sampling, serving |

**Kernels are XPU, not nucleo.** The kernels of `cajeta-llm` and
`cajeta-xgboost` are `@Kernel` code written against `cajeta.xpu`; nucleo's
tensors and fusion sit on top of the same layer, and the LLM engine uses
`cajeta.math.Tensor` and `cajeta.xpu` directly without nucleo. The kernel
material therefore is the XPU tab's own content, what `docs/kernels/` holds
today plus the scheduler and optimization material that lives only in the
specification; Math, Nucleo, ML and LLM are its children.

### 1.3 Scope
1. **One tab: XPU.** `docs/xpu/` whose own pages are the kernel material and
   whose children are `math/`, `nucleo/`, `ml/`, `llm/`; the Kernels and
   Language models tabs go away; the old URLs redirect (§3).
2. **The walkthrough**: one document, in that tab, that follows one model
   end to end (§4).
3. **Section landing pages** for Math, Nucleo and ML that exist today only
   as stdlib reference pages or in the libraries' own repos (§5).

### 1.4 Non-goals
- Rewriting `CajetaXPU.md`, `docs/kernels/llm.md` or `docs/llm/*.md`. They
  move and are linked; the walkthrough cites them.
- Moving the stdlib reference pages for `cajeta.math`, `cajeta.nucleo` and
  `cajeta.xpu`; they stay under Stdlib and are linked from the new landings.
- Performance work the walkthrough reveals.

---

## 2. The tab

### 2.1 Tree (Julian, 2026-10-25: "xpu is top, then math, nucleo, and ml are children")
The XPU tab's own pages are the kernel material; there is no `kernels/`
child. Math, Nucleo and ML are children, and LLM is the fourth child as first
listed.
```
docs/xpu/
├── README.md                ← One kernel, any device (today's docs/kernels/README.md), opening with the stack map
├── walkthrough.md           ← §4: one model, start to last token, across every layer
├── scheduler.md             ← NEW: the tile manifest and the tile scheduler, from the specs
├── optimization.md          ← NEW: shape filter, autotune store, what is measured when
├── kernels-llm.md           ← docs/kernels/llm.md
├── kernels-xgboost.md       ← docs/kernels/xgboost.md
├── catalog-llm.md  catalog-xgboost.md
├── math/README.md           ← NEW landing: Tensor, dtypes, linalg, fft, npio; links the stdlib pages
├── nucleo/README.md         ← NEW landing: columns, tables, tape, fusion, transforms
├── ml/README.md             ← NEW landing: dev.cajeta.ml and dev.cajeta.xgboost, from their repos' docs
└── llm/                     ← docs/llm/* moved
```

### 2.2 Use cases
- **2.2.1** As a reader, when I open the site, then the tabs are Home,
  Guide, Specification, Stdlib, XPU, Benchmarks, and XPU opens on the map
  that names the five layers in order with one sentence each.
- **2.2.2** As a reader on any XPU page, then the sidebar shows the tab's own
  pages first (the map, the walkthrough, scheduler, optimization, the kernel
  pages and catalogs) and then the children in stack order (Math, Nucleo,
  ML, LLM), not alphabetically.
- **2.2.3** As a holder of an old link (`/kernels/`, `/kernels/llm/`,
  `/kernels/catalog-llm/`, `/llm/`, `/llm/measuring/`), then it redirects
  to the new page; cajeta-llm's README links keep working.
- **2.2.4** As the kernel catalog generators (`cajeta-llm/tools/catalog`,
  `cajeta-xgboost/tools/catalog`), then their output path argument moves
  and their tests still compare against the committed page.

## 3. Redirects
The site is an assets-only Cloudflare Worker. Redirects go in
`site/public/_redirects` (Cloudflare's static redirect file) with one line
per moved page and a wildcard for each moved directory; the build test
asserts every old URL in the file maps to a page that exists.

## 4. The walkthrough

### 4.1 The vehicle
**Llama-3.1-8B-Instruct at Q4_K_M**, the model every number in
`docs/llm/measuring.md` is read against, with the mixture-of-experts case
(Qwen3-Coder-30B-A3B) as a side box where it diverges. A 512-token prompt and
128 generated tokens, four sequences, the shape the benchmarks use.

### 4.2 The steps, in order, each with the same four questions
Every step answers: **what happens** (the logic), **what code runs** (the
classes and kernels by name, linked to source and catalog), **NVIDIA against
AMD** (where the paths split: backend selection, the kernel tier the compiler
picked, the driver, the memory model), and **how it optimizes** (what the
adaptor decided at compile time, what the scheduler decides at bind, what
autotune measured and remembered, and what the profiler shows).

1. **Execution start.** The process, the backend selection order
   (CUDA → HIP → Vulkan → CPU), what the device profile says and how it is
   read, the first device touch.
2. **Load.** GGUF (and safetensors) into `Tensor`s, the quantized weight
   layouts the kernels read (Q4_K's super-blocks), what stays on host, what
   is prepared for the device (prepared weights), the K/V block store sized
   from the context, the tune store opened.
3. **Prefill.** The prompt through `CausalLM`: embedding, the per-layer
   route (q/k/v projection, attention, the gate/up/down MLP), which GEMM
   family serves a 512-token batch (the tile GEMMs, the cooperative-matrix
   tiers, the id GEMMs for experts), how the route is chosen for this
   weight format and batch shape on this device, the attention kernel, the
   softmax, the sampler's first token; then the same step on AMD (HIP, and
   Vulkan with RADV) naming the tier differences the manifest records.
4. **Decode.** One token at a time: the wave mat-vec families (one row per
   wave, the llama.cpp mmvq counterpart), the K/V cache append and read,
   the batched decode at four sequences and the pipelined decode, the
   scheduler's continuous batching, what changes at 8k and 32k of context;
   NVIDIA against AMD again.
5. **What the optimizer did.** A table: for each kernel the walkthrough
   named, the adaptor's compile-time decision (wave width, tier, occupancy),
   the bind-time launch configuration, the autotune store's entry after the
   first prefill, and the census standing on each backend.
6. **What the profiler shows.** One captured `.pftrace` of this run, read
   step by step against the text.

### 4.3 Diagrams
Inline SVG, in the site's tokens, both themes (artifact-diagramming rules):
the five-layer stack with the walkthrough's path drawn through it; the
prefill route through one transformer layer with the kernel names on the
edges; the decode loop with the cache; a NVIDIA/AMD split diagram of
backend selection and tier choice; the scheduler's batch timeline for four
sequences. Each diagram shows the mechanism, not a box-and-arrow summary.

### 4.4 Use cases
- **4.4.1** As a reader who has never opened the sources, when I finish the
  walkthrough, then I can say in order what runs from `exec` to the last
  token for this model on this GPU, name the kernel families that did the
  work, and say what differs on the other vendor's GPU.
- **4.4.2** As an engineer on cajeta-llm, when a route or kernel changes,
  then the walkthrough names the section and the table row that must change,
  and a test fails when a named kernel is no longer in the catalog.
- **4.4.3** As a reader of a number on the benchmarks site, when I follow
  its link into the walkthrough, then I land on the step that produced it.

## 5. The three landing pages
Math, Nucleo and ML each get a landing of a few hundred words: what the
layer is for, the types that matter, how it uses the layer below and
serves the one above, and a link list into the stdlib reference (Math,
Nucleo) or the library repo's guide (ML). Written, not generated.

## 6. Acceptance
1. The tab order is Home, Guide, Specification, Stdlib, XPU, Benchmarks;
   every old URL redirects; `npm run build` and `npm test` are green.
2. The walkthrough is reviewed by Julian against the engine as it runs
   today; every kernel it names appears in the catalog, enforced by a test.
3. The catalog generators write to the new path and their tests pass.

## 7. Open questions
1. **Tab label**: "XPU" (the package name) or "Compute". Recommendation: XPU.
2. **Where ML's documentation lives**: the landing links to cajeta-ml's
   repo docs (recommended for v1) or the guide moves in.
3. **Which run the profiler section reads**: the first quiet-box run of
   unit 22, so the walkthrough's trace and the benchmark numbers are the
   same execution (recommended).
