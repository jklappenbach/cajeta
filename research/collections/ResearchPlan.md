# Collections — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Collections are the load-bearing core of any systems standard library, and Cajeta's value proposition — C++/Java ergonomics with Rust-style borrow-checked memory and first-class GPU integration — places unusual demands on them. We need (a) state-of-the-art mutable maps/sets (SIMD-probed Swiss/F14-class open-addressing tables) that lower cleanly to LLVM 22 IR and respect borrow rules; (b) immutable/persistent collections (HAMT, RRB-Trees) whose structural sharing turns "copy" into a borrow-friendly cheap operation; and (c) GPU-resident spatial collections (BSP, LBVH/Morton, spatial hashing, sweep-and-prune) so games/graphics workloads get massively parallel collision detection out of the box. This index captures the primary papers and the canonical implementations to crib API and codegen decisions from.

## Research Index

### Hash maps — SIMD-probed open addressing (Swiss tables / F14 / hashbrown / Boost)

- **What:** Open-addressing hash tables that store a dense byte-per-slot metadata array (a 7-bit tag of the hash, "H2") and use a SIMD compare (SSE2/NEON) to test 8–16 slots of a chunk/group in one instruction, raising max load factor (~14/16) while keeping probing cache-local.
- **Why for Cajeta:** This is the obvious default `HashMap`/`HashSet`. The metadata/control-byte layout maps directly to LLVM vector intrinsics and is portable across x86 (SSE2/AVX) and ARM (NEON), aligning with the GPU/multi-arch ambitions. Flat storage (value inline in the array) minimizes indirections and plays well with move semantics under a borrow checker.
- **Key papers / sources:**
  - [Abseil Swiss Tables design notes](https://abseil.io/about/design/swisstables) — Benzaquen, Evlogimenos, Kulukundis, Perepelitsa (Google), 2017/2018. Canonical description of H1/H2 split, control bytes, group-wise SIMD probing.
  - [Open-sourcing F14 for memory-efficient hash tables](https://engineering.fb.com/2019/04/25/developer-tools/f14/) — Bronson & Shi (Meta), Engineering at Meta 2019. 14-way chunked probing, interleaved tag+payload SIMD layout, reference-counted-tombstone deletion (Amble–Knuth 1974).
  - [rust-lang/hashbrown](https://github.com/rust-lang/hashbrown) — Amanieu d'Antras et al. Rust port of SwissTable; std `HashMap` since Rust 1.36. Shows borrow-friendly API surface (entry API, `raw_entry`) relevant to Cajeta.
  - [Inside boost::unordered_flat_map (HN discussion)](https://news.ycombinator.com/item?id=33654407) — community discussion of Boost's open-addressing flat map design. (secondary)
- **Algorithms to capture:** SwissTable group probing (H1 index / H2 tag); F14 chunked 14-way probing; Robin Hood insertion (below); tombstone vs. backward-shift deletion; load-factor-driven resize/rehash.
- **Implementation notes:** Expose control-byte scan as an LLVM `<16 x i8>` icmp + `llvm.experimental.vector.reduce` / movemask pattern; provide scalar fallback for targets without vector ISA. Borrow checker: iterator invalidation on rehash must be a compile-time-tracked exclusive borrow of the map; `entry`-style APIs let mutation happen under one borrow. Decide flat (`value_type` inline, moved on resize) vs. node (stable addresses) variants — node variant needed when long-lived references/borrows must survive resizes.

### Hash maps — collision-resolution strategies (Robin Hood, cuckoo)

- **What:** Robin Hood hashing minimizes probe-distance variance by displacing the "richer" element on insert; cuckoo hashing guarantees O(1) worst-case lookup via two (or d) hash functions and eviction chains.
- **Why for Cajeta:** Robin Hood is a strong alternative/companion to Swiss probing for predictable latency (real-time/games). Cuckoo and d-ary cuckoo are relevant for GPU hash tables (bounded lookup, parallel-friendly) and for read-heavy concurrent maps.
- **Key papers / sources:**
  - [Robin Hood Hashing (Preliminary Report)](https://www.researchgate.net/publication/221499171_Robin_Hood_Hashing_Preliminary_Report) — Celis, Larson, Munro, IEEE FOCS 1985. Origin of the displacement strategy; constant expected probes even for near-full tables.
  - [Robin Hood hashing (NIST DADS entry)](https://xlinux.nist.gov/dads/HTML/robinHoodHashing.html) — concise reference definition.
  - [Cuckoo hashing: Further analysis](https://www.researchgate.net/publication/222707668_Cuckoo_hashing_Further_analysis) — Luc Devroye & Pat Morin, Information Processing Letters 86(4):215–219, 2003. Analysis following Pagh & Rodler (2001). Worst-case O(1) lookup, insertion failure/rehash behavior.
  - [Efficient d-ary Cuckoo Hashing at High Load Factors by Bubbling Up](https://arxiv.org/pdf/2501.02312) — William Kuszmaul (CMU) & Michael Mitzenmacher (Harvard), arXiv 2501.02312, 2025. Recent technique pushing cuckoo load factors higher — relevant for memory-tight GPU tables.
- **Algorithms to capture:** Robin Hood insert with probe-distance steal + backward-shift delete; cuckoo eviction chain + cycle detection/rehash; d-ary "bubbling up" placement.
- **Implementation notes:** Robin Hood pairs naturally with the Swiss control-byte layout (store probe distance or reuse H2). For Cajeta GPU tables, prefer cuckoo/d-ary (lock-free, bounded probes) since unbounded probe chains stall warps.

### Learned / SIMD-probed adaptive indexes

- **What:** Treat an index as a model that predicts a key's position; replace/augment B-trees and hash buckets with learned (often piecewise-linear or NN) models.
- **Why for Cajeta:** Speculative but high-upside for read-mostly, append-rarely datasets (e.g. asset tables, symbol tables in the compiler). Worth a `learned_index` experimental container behind the same map interface.
- **Key papers / sources:**
  - [The Case for Learned Index Structures](https://arxiv.org/abs/1712.01208) — Kraska, Beutel, Chi, Dean, Polyzotis, SIGMOD 2018 (arXiv 1712.01208, 2017). **VERIFIED** title/authors/year. Up to 70% faster than cache-optimized B-trees, ~order-of-magnitude smaller.
- **Algorithms to capture:** Recursive Model Index (RMI); model-as-hash-function for hash-index slot prediction; error-bounded last-mile search.
- **Implementation notes:** Keep behind a feature flag; the model-evaluation hot path is a candidate for GPU/SIMD codegen. Immutable once trained — fits the persistent-collection story (rebuild on bulk change).

### Immutable / persistent maps & sets — HAMT

- **What:** Hash Array Mapped Trie: a hashmap stored as a wide (typically 32-way) bitmap-indexed trie giving near-hashmap lookup/update with structural sharing and O(1) "copy" (share the root).
- **Why for Cajeta:** Persistent collections are a borrow-checker superpower: "modify" returns a new version sharing most nodes, so you can hand out an immutable snapshot (shared borrow) cheaply while another path builds the next version. Backbone of Clojure/Scala collections.
- **Key papers / sources:**
  - [Ideal Hash Trees](https://www.researchgate.net/publication/2378571_Ideal_Hash_Trees) — Phil Bagwell, EPFL tech report 2001. Origin of HAMT.
  - [Optimizing Hash-Array Mapped Tries for Fast and Lean Immutable JVM Collections](https://michael.steindorfer.name/publications/oopsla15.pdf) — Steindorfer & Vinju, OOPSLA 2015. CHAMP: compaction/canonical layout, big memory + iteration wins.
  - [Fast and Lean Immutable Multi-Maps on the JVM based on Heterogeneous HAMTs](https://arxiv.org/pdf/1608.01036) — Steindorfer & Vinju, arXiv 1608.01036, 2016. Heterogeneous node specialization.
- **Algorithms to capture:** HAMT bitmap-indexed node + popcount slot addressing; path copying for persistence; transient/mutable batch builder; CHAMP canonicalization.
- **Implementation notes:** popcount → LLVM `llvm.ctpop`. Structural sharing needs reference counting or a GC region; under Cajeta's borrow model a node is immutable once shared, and a "transient" builder takes a unique/owned borrow to mutate in place before freezing. Provide both `PersistentMap` (shared) and `TransientMap` (owned, mutable) APIs.

### Immutable / persistent vectors — RRB-Trees & immer

- **What:** Relaxed Radix Balanced trees: wide radix-balanced trees relaxed to allow under-full nodes, enabling O(log n) concatenation, split, and insert-at while keeping near-O(1) index/update/iterate of a Clojure/Scala persistent vector.
- **Why for Cajeta:** The persistent `Vector`/`Seq`. Cheap snapshots + efficient slice/concat are ideal for editor buffers, undo stacks, and parallel divide-and-conquer (concat results without copying). `immer` proves it works in a systems language with manual memory.
- **Key papers / sources:**
  - [RRB-Trees: Efficient Immutable Vectors](https://infoscience.epfl.ch/bitstreams/e5d662ea-1e8d-4dda-b917-8cbb8bb40bf9/download) — Phil Bagwell & Tiark Rompf, EPFL tech report (EPFL-REPORT-169879) 2011. **PDF VERIFIED** (EPFL Infoscience full text). Original RRB structure (relaxed inner nodes, O(log n) concat/split).
  - [RRB Vector: A Practical General Purpose Immutable Sequence](https://infoscience.epfl.ch/record/213452/files/rrbvector.pdf) — Nicolas Stucki, Tiark Rompf, Vlad Ureche, Phil Bagwell, ICFP 2015 (ACM DOI 10.1145/2858949.2784739). **PDF VERIFIED** (EPFL mirror). Production-quality refinements (display/focus optimization).
  - [Persistence for the Masses: RRB-Vectors in a Systems Language](https://public.sinusoid.es/misc/immer/immer-icfp17.pdf) — Juan Pedro Bolívar Puente, ICFP 2017 (Proc. ACM PL, DOI 10.1145/3110260). **PDF VERIFIED (DOI matches)**. The `immer` C++ library: RRB-vectors with manual memory mgmt + transients.
  - [Improving RRB-Tree Performance through Transience](https://hypirion.com/thesis.pdf) — Jean Niklas L'orange, MSc thesis 2014. Transient (mutable-batch) optimization details.
- **Algorithms to capture:** Radix-balanced indexing + relaxed-node size tables; path copying; tail/focus optimization; transient (owned) batch mutation; concatenation rebalancing.
- **Implementation notes:** Mirror `immer`'s approach for memory: reference-counted (or region/GC) shared nodes, with a transient mode requiring an exclusive borrow. Branching factor 32 (radix 5) → index math is shifts/masks, trivial LLVM IR. This is the persistent counterpart to the flat dynamic array.

### Spatial collections — BSP trees (CPU & GPU)

- **What:** Binary Space Partitioning recursively splits space by hyperplanes (often polygon-coincident) into a tree giving a strict back-to-front/front-to-back ordering for any viewpoint; also used for solid modeling (CSG) and PVS.
- **Why for Cajeta:** A core graphics/gaming collection. Owner flagged BSP as a "big win" for CPU and GPU. Good for static-scene visibility, CSG, and as a partitioning primitive.
- **Key papers / sources:**
  - [On Visible Surface Generation by A Priori Tree Structures](https://dl.acm.org/doi/10.1145/800250.807481) — Henry Fuchs, Zvi M. Kedem, Bruce F. Naylor, ACM SIGGRAPH Computer Graphics Vol. 14(3), pp. 124–133, July 1980 (DOI 10.1145/800250.807481). Origin of BSP trees. (primary source paywalled at ACM DL; overview via [Binary space partitioning — Wikipedia](https://en.wikipedia.org/wiki/Binary_space_partitioning))
  - [Binary space partitioning (Wikipedia)](https://en.wikipedia.org/wiki/Binary_space_partitioning) — survey of BSP construction, traversal, Doom usage, and the 1983 Ikonas real-time micro-code follow-up. (secondary, for orientation)
- **Algorithms to capture:** BSP construction (splitting-plane selection, polygon splitting); in-order traversal for painter's algorithm; CSG via BSP; leafy BSP / PVS.
- **Implementation notes:** Tree build is recursive on CPU; GPU side wants a linearized/flattened node array (like LBVH below) for stackless traversal. Splitting-plane heuristics (balance vs. split count) are the tunable. Borrow checker: build phase owns the geometry mutably, then freeze to an immutable, shareable traversable tree.

### Spatial collections — LBVH / Morton-code BVH construction on GPU

- **What:** Linear BVH: assign each primitive a Morton code from its centroid (Z-order curve), radix-sort, then build the internal-node hierarchy in parallel respecting that order. HLBVH does a two-level (coarse/fine) sort for dynamic geometry.
- **Why for Cajeta:** This is the headline GPU win — massively parallel acceleration-structure construction for ray tracing and collision. Karras's node-numbering scheme makes hierarchy emission fully parallel (no inter-node dependencies).
- **Key papers / sources:**
  - "Maximizing Parallelism in the Construction of BVHs, Octrees, and k-d Trees" — Tero Karras (NVIDIA), High-Performance Graphics 2012. **PDF VERIFIED** ([course mirror](https://cgvr.cs.uni-bremen.de/teaching/mpar_literatur/Parallel%20BVH%20Construction,%20with%20Application%20to%20Collision%20Detection.pdf)). Karras-style ordered tree build; node numbering yields each node's primitive range with no global knowledge.
  - [Thinking Parallel, Part III: Tree Construction on the GPU](https://developer.nvidia.com/blog/thinking-parallel-part-iii-tree-construction-gpu/) — Tero Karras, NVIDIA Developer Blog. **VERIFIED** (Karras HPG 2012, ~50x over top-down). Practical walkthrough; Parts I/II cover collision detection and traversal.
  - [HLBVH: Hierarchical LBVH Construction for Real-Time Ray Tracing of Dynamic Geometry](https://research.nvidia.com/sites/default/files/pubs/2010-06_HLBVH-Hierarchical-LBVH/HLBVH-final.pdf) — Pantaleoni & Luebke, HPG 2010. **PDF VERIFIED (resolves, 924 KB)**. Two-level Morton sort exploiting spatial coherence; 2–4x faster BVH update for dynamic meshes.
  - [Fast Parallel Construction of High-Quality Bounding Volume Hierarchies](https://research.nvidia.com/sites/default/files/pubs/2013-07_Fast-Parallel-Construction/karras2013hpg_paper.pdf) — Tero Karras & Timo Aila (NVIDIA), HPG 2013 (ACM DOI 10.1145/2492045.2492055). **PDF VERIFIED** (free NVIDIA mirror). Treelet restructuring for SAH-quality BVHs in parallel.
- **Algorithms to capture:** Morton code encoding (3D centroid → interleaved bits); parallel radix sort; Karras ordered/in-place hierarchy emission; HLBVH two-level sort; treelet restructuring (SAH refinement); stackless GPU traversal.
- **Implementation notes:** Morton encoding = bit-interleave (LLVM bit ops / `pdep` where available, or shift-and-mask). Needs the GPU offload path: emit a kernel (NVPTX / AMDGPU via LLVM 22) for radix sort + hierarchy build; nodes stored in a flat SoA array. This collection straddles host (build orchestration) and device (kernel) — Cajeta's GPU integration must let the same type be host-allocated and device-resident.

### Spatial collections — GPU broad-phase collision (spatial hashing, sweep-and-prune)

- **What:** Broad-phase culling on GPU: spatial subdivision (uniform grid + cell-ID hashing, sort by cell, test same-cell pairs) and massively parallel sweep-and-prune (sort AABB endpoints along axes, prune non-overlapping).
- **Why for Cajeta:** Owner's explicit "big win": million-body collision culling at interactive rates. Pairs with LBVH to give a complete GPU physics/collision collection layer.
- **Key papers / sources:**
  - [Real-Time Collision Culling of a Million Bodies on Graphics Processing Units](https://graphics.ewha.ac.kr/gSaP/gSaP.pdf) — Fuchang Liu, Takahiro Harada, Youngeun Lee, Young J. Kim, ACM Trans. Graphics 29(6) Art. 154, SIGGRAPH Asia 2010 (DOI 10.1145/1882261.1866180). **PDF VERIFIED** (Ewha mirror). GPU sweep-and-prune + spatial subdivision; million moving objects interactively.
  - [Chapter 32. Broad-Phase Collision Detection with CUDA (GPU Gems 3)](https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-32-broad-phase-collision-detection-cuda) — Le Grand, NVIDIA GPU Gems 3, 2007. Spatial-subdivision broad phase: cell-ID hashing, radix sort, same-cell pair tests.
  - [PSCC: Parallel Self-Collision Culling with Spatial Hashing on GPUs](https://min-tang.github.io/home/PSCC/files/pscc.pdf) — Tang et al. Spatial-hashing self-collision culling for deformable meshes on GPU.
  - [Thinking Parallel, Part I: Collision Detection on the GPU](https://developer.nvidia.com/blog/thinking-parallel-part-i-collision-detection-gpu/) — Tero Karras, NVIDIA Developer Blog. Broad/narrow-phase framing leading into LBVH. (companion to Part III above)
- **Algorithms to capture:** Uniform-grid spatial hashing (cell ID = hash of grid coords); sort-by-cell + swath traversal; parallel sweep-and-prune (sort endpoints, persistent overlap pairs); home-cell/phantom-cell scheme to dedupe pairs across cells.
- **Implementation notes:** Same host/device duality as LBVH. The grid hash and endpoint sort are the same radix-sort primitive — share a GPU `sort` collection algorithm (see sibling `gpu-utils` project). Output is a pair list whose size is data-dependent → needs device-side dynamic allocation or a two-pass count/scan/emit (prefix sum) pattern. Cajeta GPU API must support atomic counters / prefix-sum for stream compaction.

## PDF / paper backlog

- [x] The Case for Learned Index Structures — https://arxiv.org/pdf/1712.01208 (verified) — papers/kraska-2018-learned-index.pdf
- [x] HLBVH: Hierarchical LBVH Construction for Real-Time Ray Tracing of Dynamic Geometry — https://research.nvidia.com/sites/default/files/pubs/2010-06_HLBVH-Hierarchical-LBVH/HLBVH-final.pdf (verified) — papers/pantaleoni-2010-hlbvh.pdf
- [x] Maximizing Parallelism in the Construction of BVHs, Octrees, and k-d Trees (Karras 2012) — https://cgvr.cs.uni-bremen.de/teaching/mpar_literatur/Parallel%20BVH%20Construction,%20with%20Application%20to%20Collision%20Detection.pdf (verified) — papers/karras-2012-parallel-bvh.pdf
- [x] Fast Parallel Construction of High-Quality BVHs (Karras & Aila 2013) — https://research.nvidia.com/sites/default/files/pubs/2013-07_Fast-Parallel-Construction/karras2013hpg_paper.pdf (free NVIDIA mirror; ACM DOI 10.1145/2492045.2492055) — papers/karras-2013-fast-parallel-hq-bvh.pdf
- [x] Persistence for the Masses: RRB-Vectors in a Systems Language (immer, 2017) — https://public.sinusoid.es/misc/immer/immer-icfp17.pdf (verified) — papers/puente-2017-immer-rrb.pdf
- [x] RRB-Trees: Efficient Immutable Vectors (Bagwell & Rompf 2011) — https://infoscience.epfl.ch/bitstreams/e5d662ea-1e8d-4dda-b917-8cbb8bb40bf9/download (EPFL Infoscience full text; record 169879) — papers/bagwell-2011-rrb-trees.pdf
- [x] RRB Vector: A Practical General Purpose Immutable Sequence (2015) — https://infoscience.epfl.ch/record/213452/files/rrbvector.pdf (EPFL mirror; ACM DOI 10.1145/2858949.2784739) — papers/stucki-2015-rrb-vector.pdf
- [x] Improving RRB-Tree Performance through Transience (L'orange 2014) — https://hypirion.com/thesis.pdf — papers/lorange-2014-rrb-transience-thesis.pdf
- [x] Ideal Hash Trees (Bagwell 2001) — https://lampwww.epfl.ch/papers/idealhashtrees.pdf (EPFL full text) — papers/bagwell-2001-ideal-hash-trees.pdf
- [x] Optimizing HAMTs for Fast and Lean Immutable JVM Collections (CHAMP, OOPSLA 2015) — https://michael.steindorfer.name/publications/oopsla15.pdf — papers/steindorfer-2015-champ-hamt.pdf
- [x] Fast and Lean Immutable Multi-Maps on the JVM based on Heterogeneous HAMTs (Steindorfer & Vinju 2016) — https://arxiv.org/pdf/1608.01036 — papers/steindorfer-2016-heterogeneous-hamt.pdf
- [ ] Robin Hood Hashing (Celis, Larson, Munro, FOCS 1985) — https://www.researchgate.net/publication/221499171_Robin_Hood_Hashing_Preliminary_Report — (html-only / ResearchGate, not downloaded; FOCS original paywalled at IEEE)
- [ ] Cuckoo Hashing: Further Analysis (Devroye & Morin, IPL 86(4):215–219, 2003) — https://www.researchgate.net/publication/222707668_Cuckoo_hashing_Further_analysis — (html-only / ResearchGate, not downloaded)
- [x] Efficient d-ary Cuckoo Hashing at High Load Factors by Bubbling Up (Kuszmaul & Mitzenmacher 2025) — https://arxiv.org/pdf/2501.02312 — papers/cuckoo-2025-dary-bubbling-up.pdf
- [x] Real-Time Collision Culling of a Million Bodies on GPUs (Liu et al. 2010) — https://graphics.ewha.ac.kr/gSaP/gSaP.pdf (Ewha mirror; ACM DOI 10.1145/1882261.1866180) — papers/liu-2010-million-bodies-collision.pdf
- [ ] Broad-Phase Collision Detection with CUDA (GPU Gems 3, Ch. 32) — https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-32-broad-phase-collision-detection-cuda — (html-only, not downloaded)
- [x] PSCC: Parallel Self-Collision Culling with Spatial Hashing on GPUs — https://min-tang.github.io/home/PSCC/files/pscc.pdf — papers/tang-pscc-spatial-hashing.pdf
- [ ] On Visible Surface Generation by A Priori Tree Structures (Fuchs, Kedem, Naylor 1980) — https://dl.acm.org/doi/10.1145/800250.807481 — (paywalled, not downloaded)

## Open questions

- **Borrow model vs. structural sharing:** For HAMT/RRB persistent collections, do we lean on reference counting (immer-style) or a region/GC subsystem? How does "frozen" immutability interact with Cajeta's exclusive-vs-shared borrow distinction, and what does the `transient`/builder API look like?
- **Default `HashMap` implementation:** Swiss flat vs. F14 chunked vs. Robin Hood — pick one default and benchmark on Cajeta's own workloads (compiler symbol tables, asset registries). Provide node-variant for reference-stable entries needed by long-lived borrows.
- **SIMD portability in LLVM 22:** confirm the control-byte scan lowers well to AVX2/AVX-512 and NEON, and what the scalar fallback codegen looks like; verify `pdep`/`pext` availability handling for Morton encoding.
- **Host/device collection duality:** What is the standard-library shape for a collection that is built/orchestrated on host but resident and traversed on device (LBVH, spatial grid)? Need a unified-memory or explicit-transfer story, plus device-side dynamic sizing (atomics + prefix sum) for data-dependent outputs like collision pair lists.
- **Shared GPU primitives:** radix sort, prefix sum/scan, and stream compaction underpin LBVH, spatial hashing, and sweep-and-prune. Should these live as standard-library GPU algorithms (cf. sibling `cpp/gpu-utils`) that collections compose, rather than being reimplemented per structure?
- **Learned indexes:** worth a real experimental container, or premature? If included, training is a build step — does it fit the persistent/immutable rebuild-on-change model?
- **BSP on GPU:** BSP build is inherently sequential/recursive; is GPU BSP construction worthwhile, or do we restrict GPU to traversal of a host-built, flattened BSP? Find primary GPU-BSP literature (current index is light here).
