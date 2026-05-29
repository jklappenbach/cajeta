# Sorting — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Cajeta's collections need a sort implementation that is competitive with the best modern systems languages out of the box. The target is the current state of the art on three axes: single-threaded (cache- and branch-aware, SIMD), parallel (multicore in-place sample sort), and GPU offload (radix sort, merge). Because the project owner already has a GPU radix sort to port once NVIDIA/AMD integration lands, implementation notes are framed around (a) a strong CPU default for `Collection.sort()` and (b) a clean offload boundary so the existing GPU radix sort drops in behind the same standard-library API. A secondary goal is to understand how Cajeta's borrow checker and LLVM 22 codegen interact with the low-level tricks (block partitioning, compress-store, branchless permutation) these algorithms depend on.

## Research Index

### Single-threaded comparison sort (the default `sort()`)

- **What:** Hybrid quicksort variants that defeat adversarial/patterned inputs and minimize branch mispredictions, the modern replacement for textbook introsort.
- **Why for Cajeta:** This is the most likely default for `Collection.sort()` / `Array.sort()` on the CPU. pdqsort is what Rust's `sort_unstable` and C++ Boost.Sort ship; it is the realistic baseline to match or beat.
- **Key papers / sources:**
  - [Pattern-defeating Quicksort](https://arxiv.org/abs/2106.05123) — Orson R. L. Peters, arXiv 2021. Hybrid of quicksort + insertion sort + heapsort fallback; deterministic worst-case defense and linear time on common patterns (ascending/descending/all-equal).
  - [BlockQuicksort: Avoiding Branch Mispredictions in Quicksort](https://dl.acm.org/doi/10.1145/3274660) — Stefan Edelkamp, Armin Weiß, ACM Journal of Experimental Algorithmics Vol. 24, 2019. Decouples control flow from data flow via fixed-size blocks + a comparison buffer; >80% faster than GCC `std::sort` on random ints. (open-access ESA 2016 version: https://drops.dagstuhl.de/storage/00lipics/lipics-vol057-esa2016/LIPIcs.ESA.2016.38/LIPIcs.ESA.2016.38.pdf)
  - [orlp/pdqsort](https://github.com/orlp/pdqsort) — reference C++ implementation. Useful as a porting target / benchmark oracle.
- **Algorithms to capture:** pdqsort (pattern-defeating quicksort), introsort, BlockQuicksort block partitioning, insertion-sort small-case cutoff, heapsort fallback, ninther/median-of-medians pivot selection.
- **Implementation notes:** pdqsort's stability is "unstable" — Cajeta should expose `sort()` (stable) vs `sortUnstable()` separately as Rust does. The BlockQuicksort branchless partition relies on conditional-move / compress patterns; verify LLVM 22 lowers Cajeta's comparison-buffer loop to `cmov`/masked stores rather than branches (inspect IR + `-O2` asm). Block buffers are small fixed stack arrays — these must be expressible without tripping the borrow checker (single mutable slice partitioned into disjoint sub-slices; needs a split-at-mut primitive in the std lib).

### SIMD / vectorized sort

- **What:** Quicksort whose partition step and small-case sorting networks run on wide SIMD registers using compress-store.
- **Why for Cajeta:** 10–20x over `std::sort` for primitive keys on AVX2/AVX-512; the right choice for `sort()` over arrays of `i32/i64/f32/f64`. Highway's portability story (one source -> AVX2/AVX-512/NEON/SVE) maps well onto Cajeta's cross-architecture ambitions.
- **Key papers / sources:**
  - [Vectorized and performance-portable Quicksort](https://arxiv.org/abs/2205.05982) — Mark Blacher, Joachim Giesen, Peter Sanders, Jan Wassenberg, arXiv 2022. The vqsort algorithm: SIMD partition + sorting-network base case + robust pivot sampling, portable across ISAs.
  - [google/highway vqsort README](https://github.com/google/highway/blob/master/hwy/contrib/sort/README.md) — implementation + supported key types (16–128 bit) and platform notes.
  - [Vectorized and performance-portable Quicksort (Google Open Source Blog)](https://opensource.googleblog.com/2022/06/Vectorized%20and%20performance%20portable%20Quicksort.html) — accessible overview + benchmark numbers.
- **Algorithms to capture:** vqsort, SIMD compress-store partition, bitonic / sorting-network base cases, vectorized pivot sampling.
- **Implementation notes:** Two viable codegen paths in Cajeta: (1) emit LLVM vector IR (`<N x i32>`, `@llvm.masked.compressstore`, shuffle/`select`) and let LLVM 22 target-lower per ISA, or (2) bind Highway as an external dep. Path (1) is the cleaner long-term story and aligns with the GPU offload model (same "describe the vector op, let the backend lower it" philosophy). Specialize on monomorphized primitive element types only; fall back to pdqsort for arbitrary comparator/`Comparable` types. Watch alignment + tail handling at array boundaries.

### Parallel (multicore) sort

- **What:** In-place parallel distribution sort (sample/super-scalar samplesort) plus parallel-merge primitives.
- **Why for Cajeta:** Default for `sort()` on large collections when a thread pool is available. IPS4o is the current in-place multicore champion and degrades gracefully to a fast sequential sort.
- **Key papers / sources:**
  - [In-place Parallel Super Scalar Samplesort (IPS4o)](https://arxiv.org/abs/1705.02257) — Michael Axtmann, Sascha Witt, Daniel Ferizovic, Peter Sanders, arXiv 2017 (ESA 2017). In-place, parallel, cache-efficient, branch-misprediction-avoiding; up to 3x over closest in-place competitor and ~1.5x over BlockQuicksort even sequentially.
  - [ips4o/ips4o reference implementation](https://github.com/ips4o/ips4o) — C++ implementation with detailed inner-workings writeup.
  - [Merge Path — A Visually Intuitive Approach to Parallel Merging](https://arxiv.org/pdf/1406.2628) — Oded Green, Saher Odeh, Yitzhak Birk, 2014. Cross-diagonal partition that splits two sorted runs into independent equal-size merge tasks; the partition primitive for parallel merge sort (CPU and GPU).
  - [Deterministic Sample Sort For GPUs](https://arxiv.org/abs/1002.4464) — Frank Dehne, Hamidreza Zaboli, arXiv 2010. Deterministic sample sort guaranteeing bucket sizes, avoiding input-dependent runtime fluctuations of randomized sample sort.
- **Algorithms to capture:** IPS4o (sequential + parallel), classic sample sort, super-scalar samplesort bucket classification, Merge Path / cross-diagonal partition, deterministic sample sort.
- **Implementation notes:** Block-based in-place permutation is exactly the kind of pointer/slice juggling the borrow checker will scrutinize — need a sound "disjoint mutable block" abstraction (e.g. `split_at_mut` over a slice, plus an `unsafe`-blessed permutation core verified once). Parallelism needs Cajeta's threading/task model to exist first; design `sort()` to dispatch sequential vs parallel by length threshold + available parallelism, mirroring IPS4o's own fallback. Merge Path partition is also reusable on GPU (see below), so factor it as a backend-agnostic primitive.

### GPU sort (offload target for the existing radix sort)

- **What:** LSD radix sort (key path) and merge sort (comparator path) for large arrays resident in device memory.
- **Why for Cajeta:** The owner already has a GPU radix sort to port. This section's job is to define the API/offload boundary and confirm the algorithmic baseline (OneSweep) so the ported kernel slots behind `sort()` cleanly and stays current with CUB/rocPRIM.
- **Key papers / sources:**
  - [Onesweep: A Faster Least Significant Digit Radix Sort for GPUs](https://arxiv.org/abs/2206.01784) — Andy Adinets, Duane Merrill, arXiv 2022. Single-pass prefix-sum (decoupled look-back) cuts global traffic to ~2n per digit pass; ~1.5x over prior CUB, 29.4 GKey/s on A100 for 256M 32-bit keys. CUB `DeviceRadixSort` is now derived from this.
  - [rocPRIM device Sort docs](https://rocm.docs.amd.com/projects/rocPRIM/en/latest/device_ops/sort.html) — AMD equivalent: `radix_sort_keys`/`_desc`, `merge_sort`, segmented variants; radix sort internally selects single-block sort / merge sort (small) / onesweep (large).
  - [hipCUB documentation](https://rocm.docs.amd.com/projects/hipCUB/en/latest/) — thin header-only wrapper: rocPRIM backend on ROCm, CUB backend on CUDA. Confirms a single source-level API can target both vendors.
  - [GPU Merge Path: A GPU Merging Algorithm](https://davidbader.net/publication/2012-gm-ba/) — Oded Green, Robert McColl, David A. Bader, 2012. GPU realization of Merge Path; ~10x over Thrust's parallel merge. Basis for GPU merge sort when a custom comparator is required (radix won't apply).
- **Algorithms to capture:** OneSweep LSD radix sort, decoupled look-back single-pass scan, CUB/rocPRIM device radix sort, segmented radix sort, GPU Merge Path merge sort, block-level radix sort.
- **Implementation notes:** Mirror the hipCUB design — one Cajeta std-lib `sort()` surface, two backends (CUDA/CUB-style vs ROCm/rocPRIM-style) selected at the offload layer. The owner's existing radix kernel should be wrapped to match that surface; capture its radix width, pass count, and scan strategy and compare against OneSweep's decoupled look-back. Radix sort is keys-by-bits only: restrict the GPU radix path to primitive/`RadixKey`-able types (ints, IEEE floats with sign-flip transform, fixed-width keys) and route arbitrary comparators to GPU merge sort or back to CPU. Offload threshold + host<->device copy cost must be modeled so small sorts never leave the CPU. Decide who owns device buffers under the borrow checker (a `DeviceSlice`/`DeviceBuffer` type whose lifetime gates the kernel launch).

### Benchmarking & input-distribution methodology (cross-cutting)

- **What:** Standardized inputs and harnesses for comparing sort implementations fairly across patterns and key types.
- **Why for Cajeta:** Need an apples-to-apples benchmark to pick the default and to validate the GPU port against CPU paths and against CUB/rocPRIM.
- **Key papers / sources:**
  - [LearnedSort as a learning-augmented SampleSort: Analysis and Parallelization](https://arxiv.org/pdf/2307.08637) — Ivan Carvalho, Ramon Lawrence, arXiv 2023 (SSDBM 2023). Analysis tying learned sort to sample sort, with a parallel LearnedSort+IPS4o variant; useful for distribution-aware benchmark design.
  - [The Case for a Learned Sorting Algorithm](https://dspace.mit.edu/bitstream/handle/1721.1/145664/3318464.3389752.pdf) — Ani Kristo, Kapil Vaidya, Ugur Çetintemel, Sanchit Misra, Tim Kraska, SIGMOD 2020. CDF-model distribution sort; ~1.49x over radix sort on 1B items but degrades on heavy-duplicate inputs.
- **Algorithms to capture:** LearnedSort (model/CDF-based distribution sort), duplicate-robust LearnedSort redesign.
- **Implementation notes:** Treat learned sort as research/experimental, not the default — its duplicate-key weakness and model-training cost make it risky for a general std-lib `sort()`. Use the SOSD-style benchmark categories (random, sorted, reverse, near-sorted, few-unique, skewed/zipfian) to drive Cajeta's sort test suite and to gate the pdqsort-vs-vqsort-vs-IPS4o-vs-GPU default decision.

## PDF / paper backlog

- [x] Pattern-defeating Quicksort — https://arxiv.org/abs/2106.05123 — papers/peters-2021-pdqsort.pdf
- [x] BlockQuicksort: Avoiding Branch Mispredictions in Quicksort (ESA 2016 open access) — https://drops.dagstuhl.de/storage/00lipics/lipics-vol057-esa2016/LIPIcs.ESA.2016.38/LIPIcs.ESA.2016.38.pdf — papers/edelkamp-2016-blockquicksort.pdf
- [x] Vectorized and performance-portable Quicksort (vqsort) — https://arxiv.org/abs/2205.05982 — papers/blacher-2022-vqsort.pdf
- [x] In-place Parallel Super Scalar Samplesort (IPS4o) — https://arxiv.org/abs/1705.02257 — papers/axtmann-2017-ips4o.pdf
- [x] Merge Path — A Visually Intuitive Approach to Parallel Merging — https://arxiv.org/pdf/1406.2628 — papers/green-2014-mergepath.pdf
- [x] Onesweep: A Faster LSD Radix Sort for GPUs — https://arxiv.org/abs/2206.01784 — papers/adinets-2022-onesweep.pdf
- [x] GPU Merge Path: A GPU Merging Algorithm — https://davidbader.net/publication/2012-gm-ba/ — papers/green-2012-gpu-mergepath.pdf
- [x] The Case for a Learned Sorting Algorithm (SIGMOD'20) — https://dspace.mit.edu/bitstream/handle/1721.1/145664/3318464.3389752.pdf — papers/kristo-2020-learnedsort.pdf
- [ ] rocPRIM device Sort docs (AMD radix/merge) — https://rocm.docs.amd.com/projects/rocPRIM/en/latest/device_ops/sort.html — (html-only, not downloaded)
- [ ] hipCUB docs (cross-vendor wrapper) — https://rocm.docs.amd.com/projects/hipCUB/en/latest/ — (html-only, not downloaded)

## Open questions

- What is the default for `Collection.sort()`? Likely vqsort-style SIMD for primitive keys, pdqsort for comparator/`Comparable` types, IPS4o when parallelism is available — but the exact length/parallelism thresholds need benchmarking on the SOSD-style suite.
- Stable vs unstable surface: ship `sort()` (stable, merge/IPS4o-flavored) and `sortUnstable()` (pdqsort/vqsort), Rust-style? What does stability cost for the SIMD/GPU paths?
- How does the borrow checker express in-place block permutation (IPS4o, BlockQuicksort) soundly — is a verified `split_at_mut` + one audited `unsafe` permutation core the right shape, or can it be fully safe?
- Does LLVM 22 reliably lower Cajeta's branchless partition and masked compress-store to `cmov` / `vpcompress` across AVX2/AVX-512/NEON/SVE, or do we need target-specific intrinsics / a Highway dependency?
- GPU offload boundary: what type bound gates the radix path (`RadixKey`)? How are IEEE float keys order-transformed? Who owns `DeviceBuffer` lifetimes relative to kernel launch under borrow checking?
- How should the owner's existing GPU radix sort be reconciled with OneSweep's decoupled look-back scan — port as-is first, then upgrade the scan, or rewrite to OneSweep from the start?
- Cross-vendor strategy: emit our own kernels via the LLVM GPU backends, or sit on top of CUB/rocPRIM (hipCUB-style) for the device sort and only own the API surface?
- Where (if anywhere) does learned sort earn a place — a specialized `sortByDistribution()` for huge numeric, low-duplicate datasets, or strictly research?
- 128-bit and composite/string keys: radix needs MSD/segmented handling; which CPU path (string sample sort?) covers strings best?
