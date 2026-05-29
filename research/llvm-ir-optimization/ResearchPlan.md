# LLVM IR Optimization — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Cajeta compiles to native code through an LLVM 22 backend, so the quality of the LLVM IR the compiler emits — and how well it is shaped to feed LLVM's optimization pipeline — directly determines runtime performance on every target. This index collects the primary sources for the LLVM new pass manager and pipeline, the canonical middle-end passes (mem2reg/SROA, GVN, inlining, LICM and loop opts), auto-vectorization (loop + SLP), polyhedral optimization (Polly), MLIR/progressive lowering, PGO and (Thin)LTO, alias analysis, superoptimization (Souper), and GPU codegen (NVPTX/AMDGPU). The aim is to ground Cajeta's codegen and standard-library design choices in how LLVM actually optimizes IR, with particular attention to how Rust-style borrow-checking can be lowered into LLVM `noalias`/`readonly`/`captures` metadata to unlock alias-analysis-dependent optimizations, and how first-class GPU offload should target the LLVM GPU backends.

## Research Index

### LLVM IR foundations & the SSA representation

- **What:** The LLVM IR design — a typed, SSA-based, language-independent low-level representation — and the dominance-frontier algorithm that underlies SSA construction.
- **Why for Cajeta:** Cajeta's front-end must emit well-formed LLVM IR; understanding SSA, `getelementptr`, the type system, and how high-level constructs map to IR is the prerequisite for everything else. Cajeta likely builds its own SSA / `alloca`+`mem2reg` strategy at codegen time.
- **Key papers / sources:**
  - [LLVM: A Compilation Framework for Lifelong Program Analysis & Transformation](https://llvm.org/pubs/2004-01-30-CGO-LLVM.html) — Lattner & Adve, CGO 2004. The foundational LLVM paper: SSA IR, lifelong (compile/link/runtime) optimization. (verified)
  - [Efficiently Computing Static Single Assignment Form and the Control Dependence Graph](https://dl.acm.org/doi/10.1145/115372.115320) — Cytron, Ferrante, Rosen, Wegman, Zadeck, ACM TOPLAS 13(4) 1991. Dominance frontiers; the classic SSA construction algorithm behind mem2reg. (landing page verified; PDF at cs.utexas.edu returned binary, details corroborated by search)
  - [LLVM Language Reference Manual](https://llvm.org/docs/LangRef.html) — LLVM project docs. Authoritative spec for IR instructions, types, metadata, attributes. (verified; LLVM 23.0.0git docs)
- **Algorithms to capture:** Dominance-frontier SSA construction; `mem2reg` (promote `alloca` to registers); pruned/semi-pruned SSA.
- **Implementation notes:** Cajeta should emit local variables as `alloca` in the entry block and rely on `mem2reg`/SROA rather than building SSA by hand. Attach borrow-checker-derived facts as IR attributes/metadata (see Alias Analysis).

### The new pass manager & pass pipeline

- **What:** LLVM's new pass manager (NPM), `PassBuilder`, the `-O0..-O3/-Os/-Oz` per-module and ThinLTO pre/post-link pipelines, and extension points for injecting custom passes.
- **Why for Cajeta:** Cajeta's driver must construct an optimization pipeline programmatically via `PassBuilder::buildPerModuleDefaultPipeline()` (or build a tuned custom pipeline) and may want to register Cajeta-specific passes at known extension points.
- **Key papers / sources:**
  - [Using the New Pass Manager — LLVM docs](https://llvm.org/docs/NewPassManager.html) — LLVM project. PassBuilder, analysis managers, pipeline parsing, extension-point callbacks. (verified)
  - [Writing an LLVM Pass (new PM) — LLVM docs](https://llvm.org/docs/WritingAnLLVMNewPMPass.html) — LLVM project. How to author and register a new-PM pass. (verified via search listing)
  - [LLVM's Analysis and Transform Passes — LLVM docs](https://llvm.org/docs/Passes.html) — LLVM project. Catalog of every analysis and transform pass with one-line descriptions. (verified via search listing)
- **Algorithms to capture:** Per-module default pipeline composition; CGSCC (call-graph SCC) inliner ordering; analysis-manager invalidation.
- **Implementation notes:** Use NPM (middle-end is NPM; backend codegen still uses legacy PM). Register Cajeta passes via `registerPipelineStartEPCallback` / `PipelineParsingCallback`. Cache `TargetMachine` so target-specific passes are injected.

### Core scalar/middle-end passes (mem2reg, SROA, GVN, inlining)

- **What:** The workhorse cleanup/redundancy passes: `mem2reg`, SROA (scalar replacement of aggregates), GVN (global value numbering) and PRE (partial redundancy elimination), InstCombine, SCCP, and the inliner.
- **Why for Cajeta:** These passes recover performance from naive front-end IR. GVN/PRE in particular benefit enormously from precise alias info, which Cajeta's borrow checker can supply.
- **Key papers / sources:**
  - [An Efficient SSA-Based Algorithm for Complete Global Value Numbering](https://link.springer.com/chapter/10.1007/978-3-540-76637-7_22) — Nie & Cheng, APLAS 2007. SSA-based complete GVN. (search-verified landing page)
  - [Value-Based Partial Redundancy Elimination](https://link.springer.com/chapter/10.1007/978-3-540-24723-4_12) — VanDrunen & Hosking, CC 2004. Hybrid GVN+PRE; conceptual basis of LLVM's GVN-PRE. (search-verified landing page)
  - [LLVM's Analysis and Transform Passes — LLVM docs](https://llvm.org/docs/Passes.html) — descriptions of `sroa`, `gvn`, `instcombine`, `sccp`, `inline`. (verified via search listing)
- **Algorithms to capture:** mem2reg (alloca promotion); SROA (split/scalarize aggregates); hash-based GVN + value-based PRE; SCCP (sparse conditional constant propagation); bottom-up CGSCC inlining with cost model.
- **Implementation notes:** Emit aggregates (structs, fixed arrays) in a SROA-friendly way. Cajeta move semantics / RAII destructors should be lowered so the inliner and SROA can see through them; avoid opaque calls that block GVN.

### Loop optimizations & LICM

- **What:** Loop-invariant code motion (LICM), loop rotation, unrolling, unswitching, indvar simplification, loop interchange/distribution, and the loop-pass infrastructure built on LoopInfo + ScalarEvolution.
- **Why for Cajeta:** Loops dominate numeric/array workloads (and GPU kernels). Good loop-form IR (rotated, with canonical induction vars) is also a precondition for vectorization and Polly.
- **Key papers / sources:**
  - [LLVM's Analysis and Transform Passes — LLVM docs](https://llvm.org/docs/Passes.html) — `licm`, `loop-rotate`, `loop-unroll`, `loop-unswitch`, `indvars`. (verified via search listing)
  - [LLVM Loop Terminology (and Canonical Forms) — LLVM docs](https://llvm.org/docs/LoopTerminology.html) — LLVM project. Canonical loop form, LCSSA, loop-simplify form, rotated loops. (verified)
- **Algorithms to capture:** LICM (hoist/sink invariants using alias info); loop rotation; LCSSA construction; ScalarEvolution-based induction-variable analysis; loop unrolling cost model.
- **Implementation notes:** Cajeta loops (range-for, iterators) should lower to canonical counted loops with a single induction variable so ScalarEvolution can analyze trip counts; this gates LICM, vectorization, and Polly.

### Auto-vectorization (Loop & SLP vectorizers)

- **What:** LLVM's two vectorizers — the Loop Vectorizer (widens loop iterations, with runtime alias checks, if-conversion, reductions, gather/scatter, epilogue handling) and the SLP Vectorizer (superword-level parallelism, bottom-up across basic blocks).
- **Why for Cajeta:** Free SIMD speedups on CPU targets; the standard library's numeric/array types should be shaped to vectorize.
- **Key papers / sources:**
  - [Auto-Vectorization in LLVM — LLVM docs](https://llvm.org/docs/Vectorizers.html) — LLVM project. Full feature list for Loop + SLP vectorizers; both enabled by default. (verified)
  - [Exploiting Superword Level Parallelism with Multimedia Instruction Sets](https://groups.csail.mit.edu/cag/slp/SLP-PLDI-2000.pdf) — Samuel Larsen & Saman Amarasinghe (MIT Laboratory for Computer Science), PLDI 2000. Origin of SLP vectorization. (verified; free PDF mirror at MIT CSAIL — ACM DOI: 10.1145/349299.349320)
- **Algorithms to capture:** Loop vectorization with VPlan cost model + vectorization/interleave factor selection; runtime memory-aliasing checks; if-conversion; SLP bottom-up tree building.
- **Implementation notes:** Borrow-checking lets Cajeta emit `noalias` on parameters, eliminating expensive runtime aliasing-check fallbacks. Provide `#pragma`-equivalent vectorization hints in the language; ensure standard-library containers expose contiguous, alignment-annotated storage.

### Polyhedral optimization (Polly)

- **What:** Polly raises LLVM IR loop nests into an integer-polyhedral model (via isl) to perform loop tiling, fusion, interchange, and to expose SIMD/OpenMP parallelism, then regenerates LLVM IR.
- **Why for Cajeta:** High-value for dense linear-algebra / tensor kernels and as a precursor to GPU offload; demonstrates the "raise to a higher abstraction, optimize, lower back" pattern Cajeta may emulate.
- **Key papers / sources:**
  - [Polly — Polyhedral optimization in LLVM](https://perso.ens-lyon.fr/christophe.alias/impact2011/impact-07.pdf) — Grosser, Zheng, Aloor, Simbürger, Größlinger, Pouchet, IMPACT 2011. SCoP detection, isl-based scheduling, code generation. (PDF fetched; binary-only extraction, details corroborated by search)
  - [Polly project site & publications](https://polly.llvm.org/publications.html) — LLVM/Polly project. Index of follow-on Polly papers. (search-verified)
- **Algorithms to capture:** SCoP (Static Control Part) detection; polyhedral dependence analysis; isl scheduling (Pluto-style tiling/fusion); polyhedral code generation back to LLVM IR.
- **Implementation notes:** Affine, side-effect-free Cajeta loop nests over contiguous arrays are SCoP candidates. Borrow-checker guarantees of non-aliasing make SCoP detection far more reliable than in C/C++.

### MLIR & progressive lowering

- **What:** MLIR — a multi-level IR with user-defined dialects, enabling progressive lowering from high-level/domain dialects down through structured/affine dialects to the LLVM and GPU dialects.
- **Why for Cajeta:** A candidate architecture for Cajeta's own IR layering — represent borrow/ownership and GPU offload at a high dialect, then progressively lower to LLVM IR. Directly relevant to first-class GPU integration.
- **Key papers / sources:**
  - [MLIR: A Compiler Infrastructure for the End of Moore's Law](https://arxiv.org/abs/2002.11054) — Lattner, Amini, Bondhugula, Cohen, Davis, Pienaar, Riddle, Shpeisman, Vasilache, Zinenko, 2020 (later CGO 2021). Dialects, operations, regions, progressive lowering. (verified)
  - [MLIR Rationale — MLIR docs](https://mlir.llvm.org/docs/Rationale/Rationale/) — MLIR project. Design rationale for the dialect/op model. (search-verified)
  - [Progressive Raising in Multi-Level IR](https://grosser.science/static/7d02fb58ecc49e4d2097d11bc9e8152a/chelini-2021-abstraction-raising.pdf) — Chelini et al., CGO 2021. Raising from low-level to higher-level dialects. (search-verified PDF link)
- **Algorithms to capture:** Dialect conversion / progressive lowering; pattern-based rewriting (DRR); affine and linalg dialect transformations; LLVM/GPU/NVVM/ROCDL dialect lowering.
- **Implementation notes:** Evaluate whether Cajeta should introduce an MLIR layer (`cajeta` dialect → `linalg`/`affine`/`gpu` → `llvm`) instead of emitting LLVM IR directly, especially for GPU kernels. Major architectural decision; weigh build-system and dependency cost.

### Alias analysis

- **What:** LLVM's alias-analysis (AA) infrastructure — BasicAA, TBAA, ScopedNoAlias, globals-mod-ref, CFL-AA, SCEV-AA — and MemorySSA. AA answers May/Must/No-alias and Mod/Ref queries that gate GVN, LICM, DSE, and vectorization.
- **Why for Cajeta:** This is where Cajeta's borrow checker pays off the most: ownership/borrow facts can be lowered to `noalias`, `readonly`, `readnone`, `captures(none)` attributes and `!alias.scope`/`!noalias` metadata, giving LLVM precision unattainable in C/C++.
- **Key papers / sources:**
  - [LLVM Alias Analysis Infrastructure — LLVM docs](https://llvm.org/docs/AliasAnalysis.html) — LLVM project. AA query model, chaining of AA implementations, SCEV-AA. (verified)
  - [ScopedNoAlias / alias.scope metadata in LangRef](https://llvm.org/docs/LangRef.html#noalias-and-alias-scope-metadata) — LLVM project. How `noalias`/`alias.scope` metadata is encoded. (verified via LangRef; LLVM 23.0.0git)
  - [MemorySSA — LLVM docs](https://llvm.org/docs/MemorySSA.html) — LLVM project. SSA-like form for memory used by modern AA-dependent passes. (verified; documents MemoryDef/MemoryPhi/MemoryUse and the MemorySSAWalker)
- **Algorithms to capture:** Andersen-style subset vs. Steensgaard unification pointer analysis (classification context); type-based AA (TBAA); scoped no-alias; SCEV-based AA; MemorySSA walker.
- **Implementation notes:** Define a deterministic lowering from Cajeta borrow regions to `noalias` parameter attributes and per-scope `!noalias`/`!alias.scope` metadata. Emit TBAA metadata from Cajeta's type system. This is arguably Cajeta's single biggest optimization lever.

### Profile-guided optimization (PGO) & (Thin)LTO

- **What:** Instrumentation- and sample-based PGO feeding the inliner/layout passes, plus LTO and ThinLTO for scalable cross-module optimization via lightweight module summaries.
- **Why for Cajeta:** Whole-program inlining/devirtualization and hot/cold layout for release builds; ThinLTO keeps this scalable for large Cajeta projects and parallel/incremental builds.
- **Key papers / sources:**
  - [ThinLTO: Scalable and Incremental LTO](https://research.google/pubs/thinlto-scalable-and-incremental-lto/) — Johnson, Amini, Li, CGO 2017. Function-summary-based, parallel, incremental LTO. (PDF mirror at storage.googleapis.com; search-verified)
  - [ThinLTO: Scalable and Incremental LTO — LLVM Project Blog](https://blog.llvm.org/2016/06/thinlto-scalable-and-incremental-lto.html) — Teresa Johnson, 2016. Accessible design overview. (search-verified)
  - [LLVM Profile-Guided Optimization docs / Clang PGO](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization) — LLVM/Clang project. Instrumentation vs. sample PGO workflow. (verified; "Clang Compiler User's Manual", Profile Guided Optimization section)
  - [From Profiling to Optimization: Unveiling the Profile Guided Optimization](https://arxiv.org/abs/2507.16649) — Liu, Huang, Gao, Shi, Liu, Sun, Ji, arXiv 2507.16649, submitted 22 Jul 2025. Recent survey of PGO. (verified)
- **Algorithms to capture:** Edge/block instrumentation profiling; sample-based profile inference; ThinLTO module summaries + cross-module import; profile-guided inlining and hot/cold splitting; function/block layout.
- **Implementation notes:** Cajeta build tooling should expose a two-phase PGO workflow and default release builds to ThinLTO. Ensure debug-info/discriminators are emitted so sample PGO can map back to Cajeta source.

### Superoptimization (Souper)

- **What:** Souper, an SMT-solver-based synthesizing superoptimizer over a functional, control-flow-free subset of LLVM IR; used to discover missing peephole optimizations.
- **Why for Cajeta:** Offline tool to harden Cajeta's own front-end-emitted IR patterns and to discover peepholes; conceptual basis for verified rewrite rules in Cajeta's optimizer.
- **Key papers / sources:**
  - [Souper: A Synthesizing Superoptimizer](https://arxiv.org/abs/1711.04422) — Sasnauskas, Chen, Collingbourne, Ketema, Lup, Taneja, Regehr, 2017. SMT-driven synthesis of optimizations; produced a 4.4% smaller Clang. (verified)
  - [google/souper on GitHub](https://github.com/google/souper) — Reference implementation. (search-verified)
- **Algorithms to capture:** Dataflow-graph extraction to Souper IR; counterexample-guided inductive synthesis (CEGIS) of replacements; SMT (Z3) verification of equivalence.
- **Implementation notes:** Run Souper over representative Cajeta-emitted IR to find front-end patterns that defeat InstCombine; feed findings back as front-end fixes or custom canonicalization passes.

### Platform-specific codegen & GPU backends (NVPTX / AMDGPU)

- **What:** LLVM target backends for GPU offload — NVPTX (→ PTX, `ptx_kernel` calling convention, address spaces) and AMDGPU (→ GCN/RDNA ISA, code objects), plus Clang's offloading design (host+device compilation, bitcode embedding, LTO for AMDGPU/SPIR-V).
- **Why for Cajeta:** Directly serves Cajeta's first-class NVIDIA + AMD GPU ambitions; defines the IR conventions Cajeta GPU kernels must follow.
- **Key papers / sources:**
  - [User Guide for NVPTX Back-end — LLVM docs](https://llvm.org/docs/NVPTXUsage.html) — LLVM project. Accepted IR subset, `ptx_kernel` CC, address-space mapping, intrinsics. (search-verified)
  - [User Guide for AMDGPU Backend — LLVM docs](https://llvm.org/docs/AMDGPUUsage.html) — LLVM project. Target IDs (GFX families), address spaces, memory model, code-object metadata. (verified)
  - [Offloading Design & Internals — Clang docs](https://clang.llvm.org/docs/OffloadingDesign.html) — Clang project. Host/device split, device image creation, offload LTO. (search-verified)
- **Algorithms to capture:** Address-space inference/coercion; kernel calling-convention lowering; SelectionDAG/GlobalISel instruction selection; register allocation and scheduling for wide GPU register files; bitcode-link + device-LTO offload flow.
- **Implementation notes:** Cajeta GPU kernels need explicit/inferred address spaces (global/local/private/constant) and kernel CC. Reuse the device runtime/offload packaging that Clang uses; consider an MLIR `gpu` dialect path (see MLIR section) for portability across NVPTX and AMDGPU.

## PDF / paper backlog

- [x] Souper: A Synthesizing Superoptimizer — https://arxiv.org/pdf/1711.04422 (verified abstract) — papers/souper-2017-synthesizing-superoptimizer.pdf
- [x] MLIR: A Compiler Infrastructure for the End of Moore's Law — https://arxiv.org/pdf/2002.11054 (verified) — papers/lattner-2020-mlir.pdf
- [x] Polly — Polyhedral optimization in LLVM (IMPACT 2011) — https://perso.ens-lyon.fr/christophe.alias/impact2011/impact-07.pdf — papers/grosser-2011-polly.pdf
- [ ] LLVM: A Compilation Framework for Lifelong Program Analysis & Transformation (CGO 2004) — https://llvm.org/pubs/2004-01-30-CGO-LLVM.html (verified) — (html-only landing page, not a PDF; not downloaded)
- [x] Efficiently Computing SSA Form and the Control Dependence Graph (TOPLAS 1991) — https://www.cs.utexas.edu/~pingali/CS380C/2010/papers/ssaCytron.pdf — papers/cytron-1991-ssa-computation.pdf
- [x] ThinLTO: Scalable and Incremental LTO (CGO 2017) — https://storage.googleapis.com/gweb-research2023-media/pubtools/pdf/af0a39422b19fbbe063479f5d3a71d9278677314.pdf — papers/johnson-2017-thinlto.pdf
- [ ] Value-Based Partial Redundancy Elimination (CC 2004) — https://link.springer.com/chapter/10.1007/978-3-540-24723-4_12 — (paywalled SpringerLink landing page, not downloaded)
- [ ] An Efficient SSA-Based Algorithm for Complete GVN (APLAS 2007) — https://link.springer.com/chapter/10.1007/978-3-540-76637-7_22 — (paywalled SpringerLink landing page, not downloaded)
- [x] Exploiting Superword Level Parallelism (PLDI 2000) — https://groups.csail.mit.edu/cag/slp/SLP-PLDI-2000.pdf (free MIT CSAIL mirror; ACM DOI 10.1145/349299.349320 is paywalled) — papers/larsen-2000-slp.pdf
- [x] Progressive Raising in Multi-Level IR (CGO 2021) — https://grosser.science/static/7d02fb58ecc49e4d2097d11bc9e8152a/chelini-2021-abstraction-raising.pdf — papers/chelini-2021-progressive-raising.pdf
- [ ] LLVM docs to snapshot: NewPassManager, Passes, Vectorizers, AliasAnalysis, LangRef, MemorySSA, LoopTerminology, NVPTXUsage, AMDGPUUsage, Clang OffloadingDesign. — (html-only docs pages, not PDFs; not downloaded)

## Open questions

- Should Cajeta emit LLVM IR directly, or introduce an MLIR dialect stack (`cajeta` → `linalg`/`affine`/`gpu` → `llvm`) — especially for GPU offload portability across NVPTX and AMDGPU? Cost/benefit vs. build complexity.
- What is the precise, sound lowering from Cajeta borrow/ownership regions to `noalias` / `readonly` / `captures(none)` attributes and `!noalias`/`!alias.scope` metadata, and how is it validated (Souper / Alive2)?
- How should Cajeta's type system generate TBAA metadata without violating LLVM's strict-aliasing assumptions for safe code?
- Default optimization pipeline: adopt `buildPerModuleDefaultPipeline(-O2/-O3)` verbatim, or insert Cajeta canonicalization passes at pipeline-start extension points? Where do Cajeta-specific passes belong relative to the inliner and vectorizers?
- Should release builds default to ThinLTO + (instrumentation or sample) PGO, and what profile/debug-info must the front-end emit to support sample PGO mapping back to Cajeta source?
- For GPU kernels: explicit address-space annotations in the language vs. compiler address-space inference — which gives better AMDGPU/NVPTX codegen?
- Is LLVM "22" the right pinned target (LLVM trunk is 23.0.0git as of this research), and which release branch should Cajeta track for API stability of `PassBuilder`/NPM?
- Validate remaining items: the GVN/PRE Springer papers' exact author/year (SpringerLink landing pages are paywalled; metadata corroborated by search — Nie & Cheng, APLAS 2007; VanDrunen & Hosking, CC 2004). LangRef, MemorySSA, LoopTerminology docs and the SLP PLDI 2000 paper are now verified. The Cytron, Polly, SLP and other PDFs are downloaded under papers/ and can be re-extracted for detailed notes.
