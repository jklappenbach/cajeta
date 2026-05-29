# Machine Learning / Numerical Computing — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Cajeta aims to be a viable host language for numerical and ML workloads, not just a systems language with a math library bolted on. The standard library needs first-class n-dimensional arrays/tensors, a dataframe/columnar layer, classical statistics and ML, and a linear-algebra core — all expressible under borrow checking and lowerable through LLVM 22 (and, eventually, MLIR-style tensor dialects) onto CPU SIMD and NVIDIA/AMD GPUs. This plan indexes the canonical algorithms and data structures for each area, prioritizing primary sources, and frames each in terms of Cajeta-specific concerns: ownership/aliasing of large buffers, monomorphization over element types, codegen of vectorized/tiled kernels, and a clean CPU↔GPU offload boundary. SPELA (the project owner's single-forward-pass, per-layer local-loss training algorithm) is treated as a first-class design input for the on-device training story.

## Research Index

### Columnar data & dataframes

- **What:** In-memory columnar layout (contiguous typed buffers + validity bitmaps + offset arrays) and the dataframe query engines built on it (Arrow, Polars).
- **Why for Cajeta:** A `DataFrame`/`Series` API is the entry point for data-science users; a borrow-checked, SIMD-friendly columnar buffer is also the natural backing store for statistics and feature pipelines, and an Arrow-compatible layout gives zero-copy interop with the wider ecosystem (Parquet, DuckDB, PyArrow).
- **Key papers / sources:**
  - [Arrow Columnar Format](https://arrow.apache.org/docs/format/Columnar.html) — Apache Arrow project docs. Language-agnostic spec: physical buffer layouts for fixed/variable/nested types, validity bitmaps, offsets; designed for SIMD and zero-copy IPC.
  - [Array programming with NumPy](https://arxiv.org/abs/2006.10256) — Harris, Millman, van der Walt, et al., Nature 585:357 (2020). The strides+shape+dtype array model underpinning columnar/tensor buffers. (verified)
  - [pola-rs/polars](https://github.com/pola-rs/polars) — Polars, an analytical query engine for DataFrames written in Rust (with Python bindings) on Arrow-style memory; reference for a borrow-checked columnar engine with lazy query optimization.
- **Algorithms to capture:** validity-bitmap null tracking, offset-buffer variable-length encoding, dictionary encoding, columnar SIMD kernels (filter/take/gather), lazy expression graph + predicate/projection pushdown, run-end encoding.
- **Implementation notes:** Model a `Buffer<T>` as a single owning allocation; `Series`/columns are borrowed views (slices) into it — this maps cleanly onto borrow checking but needs a sound shared-immutable-view story for zero-copy. Adopt the Arrow physical layout verbatim for FFI/zero-copy. Filter/take kernels want `@llvm.masked.*` and compress-store, same codegen path as the SIMD sort work in `../sorting/`.

### Streaming / online statistics & sketches

- **What:** Single-pass, bounded-memory summaries: running mean/variance, approximate quantiles, cardinality, frequency, and sampling.
- **Why for Cajeta:** Streaming aggregates over `Series`/iterators and "big data" rollups need sublinear-memory data structures with mergeable state — ideal as small, value-type std-lib structs that compose over Cajeta's iterator/collection traits.
- **Key papers / sources:**
  - [Note on a Method for Calculating Corrected Sums of Squares and Products](https://www.tandfonline.com/doi/abs/10.1080/00401706.1962.10490022) — B. P. Welford, Technometrics 4(3):419–420 (1962). Numerically stable online mean/variance recurrence.
  - [Computing Extremely Accurate Quantiles Using t-Digests](https://arxiv.org/abs/1902.04023) — Ted Dunning, Otmar Ertl, arXiv 2019. Mergeable rank/quantile sketch with high accuracy at distribution tails.
  - [HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm](https://dmtcs.episciences.org/3545) — Philippe Flajolet, Éric Fusy, Olivier Gandouet, Frédéric Meunier, DMTCS Proc. AofA 2007. Cardinality estimation in ~1.5 KB with ~2% error.
  - [An Improved Data Stream Summary: The Count-Min Sketch and its Applications](https://dimacs.rutgers.edu/~graham/pubs/papers/cm-full.pdf) — Graham Cormode, S. Muthukrishnan, J. Algorithms 55(1):58–75 (2005). Sublinear frequency estimation with provable error bounds.
  - [Random Sampling with a Reservoir](https://www.cs.umd.edu/~samir/498/vitter.pdf) — Jeffrey S. Vitter, ACM TOMS 11(1):37–57 (1985). Algorithm R / Algorithm Z reservoir sampling in one pass.
- **Algorithms to capture:** Welford / Chan parallel-variance merge, t-digest, HyperLogLog (+ HLL++), Count-Min Sketch, reservoir sampling (Algorithm R/Z), bootstrap/resampling.
- **Implementation notes:** Each sketch should be a small `Copy`/value struct with a `merge()` for parallel reduction (associativity matters for deterministic results across thread counts). No heap churn — fixed-size arrays only — which is borrow-checker-friendly. Hash choice (for CMS/HLL) should be a pluggable trait but default to a fast non-crypto hash.

### Regression & classical ML

- **What:** Linear models with regularization (OLS/ridge/lasso/elastic-net, GLMs), tree ensembles, SVMs, clustering.
- **Why for Cajeta:** The "scikit-learn surface" — the most-used non-deep ML. These are CPU-bound, cache-sensitive, and embarrassingly amenable to SIMD/multicore, i.e. exactly where a systems language with good codegen should shine.
- **Key papers / sources:**
  - [Least Angle Regression](https://arxiv.org/abs/math/0406456) — Efron, Hastie, Johnstone, Tibshirani, Ann. Statist. 32:407–451 (2004). LARS; computes the full lasso path in roughly the cost of one OLS fit. (verified URL exists; details from search)
  - [Regularization Paths for Generalized Linear Models via Coordinate Descent](https://www.jstatsoft.org/article/view/v033i01) — Jerome H. Friedman, Trevor Hastie, Rob Tibshirani, J. Stat. Soft. 33(1) (2010). Cyclic coordinate descent (glmnet) for lasso/elastic-net GLM paths.
  - [XGBoost: A Scalable Tree Boosting System](https://arxiv.org/abs/1603.02754) — Tianqi Chen, Carlos Guestrin, KDD 2016. Regularized GBDT, sparsity-aware split finding, weighted quantile sketch. (verified)
  - [LightGBM: A Highly Efficient Gradient Boosting Decision Tree](https://papers.nips.cc/paper/2017/hash/6449f44a102fde848669bdd9eb6b76fa-Abstract.html) — Guolin Ke, Qi Meng, Thomas Finley, Taifeng Wang, Wei Chen, Weidong Ma, Qiwei Ye, Tie-Yan Liu, NeurIPS 2017. Histogram-based GBDT with GOSS + EFB; leaf-wise growth.
  - [CatBoost: unbiased boosting with categorical features](https://arxiv.org/abs/1706.09516) — Liudmila Prokhorenkova, Gleb Gusev, Aleksandr Vorobev, Anna Veronika Dorogush, Andrey Gulin, NeurIPS 2018. Ordered boosting + ordered target statistics to remove prediction-shift bias.
  - [k-means++: The Advantages of Careful Seeding](https://theory.stanford.edu/~sergei/papers/kMeansPP-soda.pdf) — David Arthur, Sergei Vassilvitskii, SODA 2007. D²-weighted seeding, O(log k)-competitive. (verified URL pattern; details from search)
- **Algorithms to capture:** OLS via QR/Cholesky normal equations, ridge (closed form), lasso/elastic-net via cyclic coordinate descent, LARS, IRLS for GLMs, histogram-based GBDT (split finding, GOSS, EFB, ordered boosting), random forest, SMO for SVM, Lloyd's k-means + k-means++ seeding, mini-batch k-means.
- **Implementation notes:** Tree builders need cache-friendly histogram bins (column-major feature buffers) and parallel reductions over rows — pair with the columnar layer. Coordinate descent is a tight scalar loop; verify LLVM vectorizes the inner dot/soft-threshold. Tree training mutates per-node partitions of a row-index array — the same "disjoint mutable sub-slice" borrow pattern flagged in the sorting plan; a verified `split_at_mut` primitive covers both.

### Dimensionality reduction

- **What:** Linear (PCA via SVD) and nonlinear/manifold (t-SNE, UMAP) projections.
- **Why for Cajeta:** Standard preprocessing and visualization; PCA also exercises the SVD/eigensolver core, and t-SNE/UMAP exercise nearest-neighbor + sparse-graph + gradient machinery that reuses other std-lib pieces.
- **Key papers / sources:**
  - [Finding Structure with Randomness: Probabilistic Algorithms for Constructing Approximate Matrix Decompositions](https://arxiv.org/abs/0909.4061) — Halko, Martinsson, Tropp, SIAM Review 53(2):217–288 (2011). Randomized range-finder + small dense SVD; the practical algorithm for truncated PCA/SVD on large data. (verified)
  - [Visualizing Data using t-SNE](https://www.jmlr.org/papers/v9/vandermaaten08a.html) — Laurens van der Maaten, Geoffrey Hinton, JMLR 9:2579–2605 (2008). Student-t low-dim similarities minimizing KL divergence.
  - [UMAP: Uniform Manifold Approximation and Projection for Dimension Reduction](https://arxiv.org/abs/1802.03426) — Leland McInnes, John Healy, James Melville, arXiv 2018. Fuzzy-simplicial-set manifold embedding; faster, preserves more global structure than t-SNE.
- **Algorithms to capture:** PCA (covariance eig / SVD), randomized SVD (range finder + power iteration), truncated/partial SVD, Barnes-Hut t-SNE, UMAP (fuzzy simplicial set construction + SGD layout), approximate kNN (used by both).
- **Implementation notes:** Randomized SVD is the workhorse and reduces to GEMM + a small dense QR/SVD — build it on the BLAS/LAPACK core below so it offloads to GPU GEMM for free. t-SNE/UMAP need an approximate-NN index and a sparse graph type; coordinate these with the sparse-matrix formats below.

### Dense & sparse linear algebra

- **What:** BLAS/LAPACK-style dense kernels, sparse matrix formats, direct factorizations, and Krylov iterative solvers.
- **Why for Cajeta:** This is the foundation everything else stands on (regression, PCA, GNNs, physics). Cajeta needs a native `Matrix`/`Tensor` core with a BLAS-shaped API that can either call vendor BLAS or emit its own tiled kernels, plus sparse types for graphs and PDEs.
- **Key papers / sources:**
  - [An Updated Set of Basic Linear Algebra Subprograms (BLAS)](https://dl.acm.org/doi/10.1145/567806.567807) — L. Susan Blackford, James Demmel, Jack Dongarra, Iain Duff, Sven Hammarling, Greg Henry, et al., ACM TOMS 28(2):135–151 (2002). The BLAS Level-1/2/3 API contract Cajeta's dense API should mirror. (free PDF: https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=50982)
  - [LAPACK Users' Guide, 3rd ed.](https://www.netlib.org/lapack/lug/) — E. Anderson, Z. Bai, C. Bischof, S. Blackford, J. Demmel, J. Dongarra, et al., SIAM (1999). Blocked LU/QR/Cholesky/SVD/eig algorithms.
  - [Templates for the Solution of Linear Systems: Building Blocks for Iterative Methods](https://www.netlib.org/templates/templates.html) — Richard Barrett, Michael Berry, Tony F. Chan, James Demmel, June M. Donato, Jack Dongarra, Victor Eijkhout, Roldan Pozo, Charles Romine, Henk Van der Vorst, SIAM (1994). CG, GMRES, BiCGSTAB, preconditioning — algorithmic templates.
  - [Anatomy of High-Performance Matrix Multiplication](https://www.cs.utexas.edu/~flame/pubs/GotoTOMS.pdf) — Kazushige Goto, Robert A. van de Geijn, ACM TOMS 34(3) (2008). The GotoBLAS/BLIS GEMM blocking + packing strategy.
  - [Gaussian Elimination is not Optimal](https://link.springer.com/article/10.1007/BF02165411) — Volker Strassen, Numer. Math. 13:354–356 (1969). Sub-cubic matmul.
- **Algorithms to capture:** GEMM (Goto/BLIS blocking + packing), LU/QR (Householder)/Cholesky/SVD/eig, sparse formats CSR/CSC/COO/BSR, SpMV, CG, GMRES (Arnoldi), BiCGSTAB, Jacobi/ILU preconditioners, Strassen, mixed-precision iterative refinement.
- **Implementation notes:** Define a BLAS-shaped trait surface so the backend is swappable (own kernels vs vendor BLAS/cuBLAS/rocBLAS). GEMM packing buffers are fixed-size, alias-free tiles — borrow-checker-friendly, but verify LLVM 22 emits FMA + correct vector widths for the micro-kernel (or fall back to intrinsics). Sparse types must enforce structural invariants (sorted indices, row-pointer monotonicity) at construction so kernels can assume them. GPU offload boundary identical to the sort plan: a `DeviceMatrix`/`DeviceBuffer` whose lifetime gates the kernel launch.

### Neural networks: autodiff & optimizers

- **What:** Reverse-mode automatic differentiation / computational graphs, and modern first-order optimizers.
- **Why for Cajeta:** A native deep-learning capability needs an autodiff engine and an optimizer suite. Building the graph/tape under borrow checking, and lowering it to fused kernels, is a core language design question (and where MLIR-style IR becomes relevant).
- **Key papers / sources:**
  - [Automatic Differentiation in Machine Learning: a Survey](https://arxiv.org/abs/1502.05767) — Baydin, Pearlmutter, Radul, Siskind, JMLR 18 (2018). Authoritative survey: forward vs reverse mode, dual numbers, taping. (verified)
  - [Adam: A Method for Stochastic Optimization](https://arxiv.org/abs/1412.6980) — Diederik P. Kingma, Jimmy Ba, arXiv 2014 / ICLR 2015. Adaptive first/second-moment optimizer.
  - [Decoupled Weight Decay Regularization](https://arxiv.org/abs/1711.05101) — Ilya Loshchilov, Frank Hutter, ICLR 2019. AdamW: decouple weight decay from the adaptive step. (verified)
- **Algorithms to capture:** reverse-mode AD (tape/Wengert list), forward-mode via dual numbers, checkpointing/rematerialization, SGD+momentum, Adam/AdamW, RMSProp, Adagrad, learning-rate schedules (cosine/warmup).
- **Implementation notes:** The tape holds references to intermediate activations — lifetime/ownership of those buffers is the central borrow-checking problem; consider an arena/region allocator for a graph whose lifetime is the backward pass. Dual-number forward mode is fully static and type-driven (great fit for generics/operator overloading). Optimizers are simple element-wise update kernels — trivially vectorizable and GPU-offloadable; express them once over a generic `Parameter` view.

### Transformers, attention & normalization

- **What:** The transformer architecture, scaled dot-product/multi-head attention, IO-aware attention kernels, and normalization layers.
- **Why for Cajeta:** The dominant modern architecture. FlashAttention specifically is a tiling/IO-locality kernel — exactly the kind of fused, memory-hierarchy-aware codegen Cajeta wants to express natively (and a strong test of the GPU/MLIR story).
- **Key papers / sources:**
  - [Attention Is All You Need](https://arxiv.org/abs/1706.03762) — Vaswani et al., NeurIPS 2017. The transformer; multi-head scaled dot-product attention. (verified)
  - [FlashAttention: Fast and Memory-Efficient Exact Attention with IO-Awareness](https://arxiv.org/abs/2205.14135) — Dao, Fu, Ermon, Rudra, Ré, NeurIPS 2022. Tiled, recomputation-based attention minimizing HBM traffic; up to 3x faster, linear memory. (verified)
  - [FlashAttention-3: Fast and Accurate Attention with Asynchrony and Low-precision](https://arxiv.org/abs/2407.08608) — Jay Shah, Ganesh Bikshandi, Ying Zhang, Vijay Thakkar, Pradeep Ramani, Tri Dao, 2024. Async + FP8 attention on Hopper.
  - [Layer Normalization](https://arxiv.org/abs/1607.06450) — Jimmy Lei Ba, Jamie Ryan Kiros, Geoffrey E. Hinton, arXiv 2016. Per-sample feature normalization.
  - [Root Mean Square Layer Normalization](https://openreview.net/pdf?id=SygkZ3MTJE) — Biao Zhang, Rico Sennrich, NeurIPS 2019. RMSNorm: drop mean-centering, scale by RMS.
  - [Batch Normalization: Accelerating Deep Network Training by Reducing Internal Covariate Shift](https://arxiv.org/abs/1502.03167) — Sergey Ioffe, Christian Szegedy, ICML 2015.
- **Algorithms to capture:** scaled dot-product attention, multi-head attention, FlashAttention tiling + online softmax, KV-cache, LayerNorm/RMSNorm/BatchNorm forward+backward, rotary position embeddings.
- **Implementation notes:** FlashAttention is the canonical fused kernel — its online-softmax tiling is the test case for whether Cajeta can express tile/SRAM-resident loops that LLVM/MLIR lowers to efficient GPU code rather than naive HBM round-trips. Normalization layers are reduction + element-wise; the online-variance (Welford) reduction reuses the streaming-stats code.

### On-device / local-loss training (SPELA)

- **What:** Backprop-free training where each layer updates from a local loss in a single forward pass (no global gradient, no stored activations).
- **Why for Cajeta:** This is the project owner's research thread (canonical impl in `ml/spela-training/`) and the strongest fit for Cajeta's on-device/Jetson ambitions: no backward tape means dramatically smaller memory/compute and a much simpler ownership story than reverse-mode AD.
- **Key papers / sources:**
  - [Learning Using a Single Forward Pass](https://arxiv.org/abs/2402.09769) — Aditya Somasundaram, Pushkal Mishra, Ayon Borthakur, arXiv 2024 (rev. 2025). The SPELA paper: local loss functions, neural priors (embedded vectors), no weight transport, no update locking, full local Hebbian learning, single forward pass without storing activations, one weight update per sample. (verified)
  - [The Forward-Forward Algorithm: Some Preliminary Investigations](https://arxiv.org/abs/2212.13345) — Geoffrey Hinton, arXiv 2022. Replaces backprop with two forward passes (positive/negative data); the conceptual predecessor SPELA improves on with a single pass.
  - [Mono-Forward: Backpropagation-Free Algorithm for Efficient Neural Network Training Harnessing Local Errors](https://arxiv.org/abs/2501.09238) — James Gong, Bruce Li, Waleed Abdulla, arXiv 2025. Another local-error, backprop-free training scheme; comparison point.
- **Algorithms to capture:** SPELA single-forward-pass local-loss update, per-layer cosine/symmetric-vector loss, Hebbian-style weight update, Forward-Forward positive/negative goodness, local-error layer-wise training.
- **Implementation notes:** This is where Cajeta could be genuinely differentiated: with no backward pass there is no tape, so activations need not outlive the layer's forward call — ownership stays strictly local and the arena/region concern in the autodiff section disappears. Updates are per-layer element-wise kernels (CPU SIMD + GPU offloadable). Mirror the reference API in `ml/spela-training/src/`; keep the layer/optimizer abstraction shared with the backprop path so models can switch training algorithms.

### Tensor IR, fusion & quantization

- **What:** N-dimensional tensor semantics (strides/broadcasting/einsum), operator fusion, a tensor IR (relevant to LLVM/MLIR), and low-precision/quantized inference.
- **Why for Cajeta:** Determines how the tensor std lib lowers to code. An MLIR-style multi-level IR is the bridge between a high-level `Tensor` API and per-target (CPU/NVIDIA/AMD) codegen; quantization is essential for the on-device inference story alongside SPELA.
- **Key papers / sources:**
  - [MLIR: A Compiler Infrastructure for the End of Moore's Law](https://arxiv.org/abs/2002.11054) — Lattner, Amini, Bondhugula, Cohen, et al., arXiv 2020. Multi-level, dialect-based, SSA IR with rich tensor modeling and progressive lowering; pairs with LLVM. (verified)
  - [TVM: An Automated End-to-End Optimizing Compiler for Deep Learning](https://www.usenix.org/conference/osdi18/presentation/chen) — Tianqi Chen, Thierry Moreau, Ziheng Jiang, Lianmin Zheng, Eddie Yan, Haichen Shen, et al., OSDI 2018. Tensor expression + scheduling + autotuning; operator fusion and codegen across backends. (PDF: https://www.usenix.org/system/files/osdi18-chen.pdf)
  - [Integer Quantization for Deep Learning Inference: Principles and Empirical Evaluation](https://arxiv.org/abs/2004.09602) — Hao Wu, Patrick Judd, Xiaojie Zhang, Mikhail Isaev, Paulius Micikevicius (NVIDIA), arXiv 2020. INT8 quantization principles.
  - [LLM.int8(): 8-bit Matrix Multiplication for Transformers at Scale](https://arxiv.org/abs/2208.07339) — Tim Dettmers, Mike Lewis, Younes Belkada, Luke Zettlemoyer, NeurIPS 2022. Outlier-aware mixed INT8/FP16 matmul.
  - [GPTQ: Accurate Post-Training Quantization for Generative Pre-trained Transformers](https://arxiv.org/abs/2210.17323) — Elias Frantar, Saleh Ashkboos, Torsten Hoefler, Dan Alistarh, arXiv 2022 / ICLR 2023. 3–4 bit second-order PTQ.
- **Algorithms to capture:** strided/broadcasting tensor layout, einsum/tensor contraction, operator fusion (elementwise chains, GEMM+bias+activation), loop tiling/scheduling/autotuning, per-tensor/per-channel INT8 quantization, GPTQ, LLM.int8() outlier handling.
- **Implementation notes:** Decide early whether Cajeta lowers tensors straight to LLVM IR or introduces an MLIR-style dialect for the high→low progressive lowering (fusion, tiling, GPU dialect, then LLVM). The latter is the principled route for cross-vendor GPU and autotuning but is a major build. Broadcasting/einsum must be expressible in the type system (shape generics) for compile-time checking; strides + views are the same borrowed-view ownership model as the columnar layer. Quantized kernels need packed sub-byte storage types and saturating integer arithmetic intrinsics.

## PDF / paper backlog

- [x] Learning Using a Single Forward Pass (SPELA) — https://arxiv.org/abs/2402.09769 — papers/somasundaram-2024-spela-single-forward-pass.pdf
- [x] The Forward-Forward Algorithm — https://arxiv.org/abs/2212.13345 — papers/hinton-2022-forward-forward.pdf
- [x] FlashAttention — https://arxiv.org/abs/2205.14135 — papers/dao-2022-flashattention.pdf
- [x] FlashAttention-3 — https://arxiv.org/abs/2407.08608 — papers/shah-2024-flashattention-3.pdf
- [x] Attention Is All You Need — https://arxiv.org/abs/1706.03762 — papers/vaswani-2017-attention.pdf
- [x] Automatic Differentiation in ML: a Survey — https://arxiv.org/abs/1502.05767 — papers/baydin-2018-autodiff-survey.pdf
- [x] Adam: A Method for Stochastic Optimization — https://arxiv.org/abs/1412.6980 — papers/kingma-2015-adam.pdf
- [x] Decoupled Weight Decay Regularization (AdamW) — https://arxiv.org/abs/1711.05101 — papers/loshchilov-2019-adamw.pdf
- [x] Finding Structure with Randomness (randomized SVD) — https://arxiv.org/abs/0909.4061 — papers/halko-2011-randomized-svd.pdf
- [x] XGBoost: A Scalable Tree Boosting System — https://arxiv.org/abs/1603.02754 — papers/chen-2016-xgboost.pdf
- [x] LightGBM (NeurIPS 2017) — https://papers.nips.cc/paper/2017/hash/6449f44a102fde848669bdd9eb6b76fa-Abstract.html — papers/ke-2017-lightgbm.pdf
- [x] CatBoost — https://arxiv.org/abs/1706.09516 — papers/prokhorenkova-2018-catboost.pdf
- [x] Least Angle Regression — https://arxiv.org/abs/math/0406456 — papers/efron-2004-least-angle-regression.pdf
- [x] Regularization Paths via Coordinate Descent (glmnet) — https://www.jstatsoft.org/article/view/v033i01 — papers/friedman-2010-glmnet-coordinate-descent.pdf
- [x] k-means++ — https://theory.stanford.edu/~sergei/papers/kMeansPP-soda.pdf — papers/kmeans-pp-arthur-2007.pdf
- [x] Array programming with NumPy — https://arxiv.org/abs/2006.10256 — papers/harris-2020-numpy.pdf
- [ ] Arrow Columnar Format spec — https://arrow.apache.org/docs/format/Columnar.html — (html-only, not downloaded)
- [x] Computing Extremely Accurate Quantiles Using t-Digests — https://arxiv.org/abs/1902.04023 — papers/dunning-2019-tdigest.pdf
- [x] HyperLogLog — https://dmtcs.episciences.org/3545 — papers/flajolet-2007-hyperloglog.pdf
- [x] Count-Min Sketch — https://dimacs.rutgers.edu/~graham/pubs/papers/cm-full.pdf — papers/cormode-2005-count-min-sketch.pdf
- [x] Random Sampling with a Reservoir (Vitter) — https://www.cs.umd.edu/~samir/498/vitter.pdf — papers/vitter-1985-reservoir-sampling.pdf
- [x] Anatomy of High-Performance Matrix Multiplication (Goto/BLIS) — https://www.cs.utexas.edu/~flame/pubs/GotoTOMS.pdf — papers/goto-2008-anatomy-matmul.pdf
- [ ] Templates for Iterative Methods (CG/GMRES) — https://www.netlib.org/templates/templates.html — (html-only, not downloaded)
- [x] Visualizing Data using t-SNE — https://www.jmlr.org/papers/v9/vandermaaten08a.html — papers/vandermaaten-2008-tsne.pdf
- [x] UMAP — https://arxiv.org/abs/1802.03426 — papers/mcinnes-2018-umap.pdf
- [x] RMSNorm — https://openreview.net/pdf?id=SygkZ3MTJE — papers/zhang-2019-rmsnorm.pdf
- [x] Layer Normalization — https://arxiv.org/abs/1607.06450 — papers/ba-2016-layer-normalization.pdf
- [x] Batch Normalization — https://arxiv.org/abs/1502.03167 — papers/ioffe-2015-batch-normalization.pdf
- [x] MLIR — https://arxiv.org/abs/2002.11054 — papers/lattner-2020-mlir.pdf
- [x] TVM (OSDI'18) — https://www.usenix.org/system/files/osdi18-chen.pdf — papers/chen-2018-tvm.pdf
- [x] Integer Quantization for DL Inference — https://arxiv.org/abs/2004.09602 — papers/wu-2020-integer-quantization.pdf
- [x] LLM.int8() — https://arxiv.org/abs/2208.07339 — papers/dettmers-2022-llm-int8.pdf
- [x] GPTQ — https://arxiv.org/abs/2210.17323 — papers/frantar-2023-gptq.pdf
- [x] An Updated Set of BLAS (TOMS 2002) — https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=50982 — papers/blackford-2002-updated-blas.pdf
- [x] Mono-Forward — https://arxiv.org/abs/2501.09238 — papers/gong-2025-mono-forward.pdf

## Open questions

- **Tensor lowering path:** lower `Tensor` ops directly to LLVM IR, or introduce an MLIR-style dialect for progressive lowering (fusion → tiling → GPU dialect → LLVM)? The latter buys cross-vendor codegen + autotuning but is a large undertaking. How does it reconcile with the LLVM-22 target already chosen (see `../llvm-ir-optimization/`)?
- **Autodiff under borrow checking:** what owns the activation tape, and how do those buffers' lifetimes interact with the borrow checker? Arena/region allocator scoped to the backward pass, or an explicit `Graph` owner? Does forward-mode (dual numbers) cover enough cases to avoid taping for small models?
- **SPELA as the default on-device trainer:** since SPELA needs no backward tape, can the on-device training path skip the autodiff machinery entirely and keep ownership strictly per-layer? What shared abstraction lets a model switch between SPELA and backprop without rewriting layers? (Mirror `ml/spela-training/src/`.)
- **BLAS strategy:** ship own tiled GEMM/LAPACK-style kernels (full control, single codegen story) vs. bind vendor BLAS / cuBLAS / rocBLAS (fast to ship, less control)? Likely a swappable trait backend — but what is the default, and does LLVM 22 emit a competitive micro-kernel without intrinsics?
- **GPU offload boundary (shared with sorting plan):** one `DeviceBuffer`/`DeviceMatrix`/`DeviceTensor` whose lifetime gates kernel launch; who owns host↔device transfers, and how is the NVIDIA/AMD split abstracted (emit our own kernels via LLVM GPU backends vs. sit on cuBLAS/rocBLAS/cuDNN-style libs)?
- **Arrow interop:** adopt the Arrow physical layout verbatim for zero-copy FFI (Parquet/DuckDB/PyArrow), and if so how do shared-immutable Arrow buffers map onto Cajeta's ownership model (refcounted shared buffer + borrowed column views)?
- **Shape typing:** how much of tensor shape/broadcasting/einsum can be checked at compile time via shape generics vs. deferred to runtime? Trade-off between ergonomics and static guarantees.
- **Sketch determinism:** for parallel reductions of streaming sketches (t-digest/HLL/CMS), how is associativity/merge order handled so results are reproducible across thread counts and hardware?
- **Quantization storage:** what sub-byte packed integer types and saturating-arithmetic intrinsics does the std lib expose, and do they round-trip cleanly through LLVM IR on both CPU and GPU?
- **Citation cleanup:** done (2026-05-28). All previously "(unverified)" sources were confirmed against their landing pages / authoritative search results and corrected in place (Welford 1962 Technometrics 4(3):419–420; BLAS TOMS 28(2):135–151; LAPACK Users' Guide 3rd ed. SIAM 1999; Templates for Iterative Methods SIAM 1994; Goto/van de Geijn TOMS 34(3) 2008; Strassen Numer. Math. 13:354–356 1969; t-SNE JMLR 9:2579–2605; LightGBM/CatBoost NeurIPS author lists; glmnet JSS 33(1); Adam arXiv 2014/ICLR 2015; FlashAttention-3, Mono-Forward, TVM, LLM.int8(), GPTQ, Integer Quantization author lists; HyperLogLog, Count-Min Sketch, Vitter reservoir, t-Digest, UMAP, RMSNorm, BatchNorm, LayerNorm, Forward-Forward, Polars). Downloadable PDFs are mirrored under `papers/`; remaining items (Arrow Columnar spec, Templates for Iterative Methods) are HTML-only.
