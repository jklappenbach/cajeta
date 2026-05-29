# Concurrency — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Cajeta targets native systems programming with Rust-style borrow checking, an LLVM 22 backend, and first-class GPU offload. Concurrency is therefore a first-class design axis, not an afterthought: the language must offer cheap user-space concurrency (fibers / M:N green threads), an efficient work-stealing scheduler, and synchronization primitives whose *safety* is enforced by the borrow checker and the type system rather than left to programmer discipline. This index surveys the canonical scheduling algorithms (Cilk, Go, Tokio, Loom, boost.fiber), structured-concurrency control flow, lock-free/wait-free data structures, and—critically for a "next-generation" language—static and runtime deadlock-freedom techniques (session/typestate types, lock-ordering analysis, runtime cycle detection). The aim is to choose a coherent concurrency model that composes with ownership and with GPU codegen.

## Research Index

### Work-stealing schedulers (the algorithmic core)

- **What:** Each worker owns a double-ended queue (deque) of ready tasks; it pushes/pops its own end LIFO and steals from the *other* end of a randomly chosen victim when idle. Provably efficient for fully-strict (fork-join) computations.
- **Why for Cajeta:** This is the proven default for an M:N runtime (used by Cilk, Go, Tokio, .NET TPL, Java F/J, boost.fiber). It is the strongest baseline for Cajeta's scheduler and maps cleanly onto fork/join parallelism the compiler can emit.
- **Key papers / sources:**
  - [Scheduling Multithreaded Computations by Work Stealing](https://dl.acm.org/doi/10.1145/324133.324234) — Blumofe & Leiserson, JACM 1999. Proves expected runtime ~ T₁/P + O(T∞) and O(S₁·P) space bounds for fully-strict computations. (DOI verified; landing page is paywalled/403 but DOI resolves. PDF mirror: https://www.csd.uwo.ca/~mmorenom/CS433-CS9624/Resources/Scheduling_multithreaded_computations_by_work_stealing.pdf)
  - [Cilk: An Efficient Multithreaded Runtime System](https://publications.csail.mit.edu/lcs/pubs/pdf/MIT-LCS-TM-548.pdf) — Blumofe, Joerg, Kuszmaul, Leiserson, Randall, Zhou, PPoPP 1995. The "work-first" principle and the THE protocol for low-overhead deque access. (PDF URL verified, downloads as valid PDF.)
  - [Scheduling Parallel Programs by Work Stealing with Private Deques](https://www.chargueraud.org/research/2013/ppopp/full.pdf) — Acar, Charguéraud, Rainey, PPoPP 2013. Work-stealing without concurrent (shared) deques, using message passing between workers — relevant to lock-free runtime design. (URL verified via search index.)
  - [Work stealing — Wikipedia](https://en.wikipedia.org/wiki/Work_stealing) — overview / entry point, not primary.
  - [Dynamic Circular Work-Stealing Deque](https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf) — David Chase & Yossi Lev (Sun Microsystems Laboratories), SPAA 2005. The growable lock-free deque used by modern work-stealing runtimes. (Title/authors verified from PDF; ACM DOI 10.1145/1073970.1073974.)
- **Algorithms to capture:** Random victim selection work-stealing; the THE protocol (Cilk deque without locks on the fast path); work-first vs help-first policies; the Chase–Lev lock-free growable deque.
- **Implementation notes:** Cajeta's scheduler should own a per-worker Chase–Lev-style deque; the borrow checker must understand "task spawn" as a move of captured state into the task. Fork/join (`spawn`/`sync`) lowers naturally to LLVM IR with the spawned closure as a separate function; the continuation after a steal needs a stack-switching ABI decision (see fibers below).

### Go runtime scheduler (G-M-P, production M:N)

- **What:** The G-M-P model: G = goroutine, M = OS thread, P = logical processor (scheduling context, count = GOMAXPROCS). Each P has a local run queue + a global queue; idle Ps steal half of a random P's queue; a global queue is polled ~1/61 of ticks for fairness; integrated netpoller hands blocked-on-IO Gs back to the runtime.
- **Why for Cajeta:** The reference for a *language-integrated* (not library) M:N scheduler with preemption, syscall handoff, and an I/O poller — exactly the surface Cajeta's stdlib runtime must provide.
- **Key papers / sources:**
  - [Go's work-stealing scheduler — rakyll.org](https://rakyll.org/scheduler/) — concise authoritative summary of G-M-P + stealing half the victim queue. (URL from search index; treat as secondary.)
  - [Scheduling In Go, Part II — Ardan Labs](https://www.ardanlabs.com/blog/2018/08/scheduling-in-go-part2.html) — detailed run-queue / handoff walkthrough. (secondary)
  - [Scalable Go Scheduler Design Doc](https://docs.google.com/document/d/1TTj4T2JO42uD5ID9e89oa0sLKhJYD0Y_kqxDv3I3XMw/edit) — Dmitry Vyukov, May 2012. The G-M-P design doc behind the Go 1.1 scheduler. (Title/author verified; Google Doc — HTML only, not a PDF.)
- **Algorithms to capture:** G-M-P dispatch; steal-half; global-queue fairness polling (1/61); hand-off (`handoffp`) on blocking syscalls; asynchronous preemption via signals.
- **Implementation notes:** Cajeta can adopt G-M-P but must reconcile preemption with deterministic destructor/borrow semantics — async preemption at arbitrary safepoints complicates RAII drop ordering. Decide early whether goroutine-equivalents are stackful (easy blocking syscalls) or stackless (cheaper, but blocking I/O needs the poller).

### Rust async + Tokio (stackless futures, zero-cost)

- **What:** `async fn` compiles to a state machine implementing `Future`/`poll`; the executor (Tokio) drives readiness via a work-stealing multithreaded scheduler with per-worker queues, a LIFO "next task" slot for locality, and a global injector queue.
- **Why for Cajeta:** The model of *stackless* coroutines as compiler-generated state machines is directly applicable to an LLVM backend and avoids per-task stacks. Tokio's evolution shows the practical tuning (LIFO slot, batching, throttled stealing).
- **Key papers / sources:**
  - [Making the Tokio scheduler 10x faster](https://tokio.rs/blog/2019-10-scheduler) — Carl Lerche, Tokio blog 2019. Per-processor run queues, stealing, LIFO slot, and the false-sharing / atomics lessons. (URL verified.)
  - [Async-await on stable Rust!](https://blog.rust-lang.org/2019/11/07/Async-await-stable/) — Rust blog 2019. Pin/`Future` semantics that make the state-machine transform sound. (URL from search index.)
  - [Zero-cost futures in Rust](http://aturon.github.io/blog/2016/08/11/futures/) — Aaron Turon, 2016. The originating design of demand-driven (poll-based) futures. (Title/author/date verified, posted 2016-08-11.)
- **Algorithms to capture:** Poll-based readiness (vs callback); waker propagation; per-worker deque + LIFO slot + global injector; stealing with a steal-count cap to limit contention.
- **Implementation notes:** Cajeta should reuse LLVM's coroutine intrinsics (see below) to lower `async` to a state machine. Key borrow-checker interaction: self-referential futures require a `Pin`-equivalent (a no-move guarantee) that Cajeta's ownership model must express, possibly as a typestate ("pinned").

### Fibers: stackful vs stackless (boost.fiber, fiber_context)

- **What:** Stackful fibers each own a separate call stack and can suspend at *any* nested call depth (cooperative context switch); stackless coroutines suspend only at the top frame and are compiled to state machines. boost.fiber provides round_robin, work_stealing, numa::work_stealing, shared_work schedulers over stackful fibers.
- **Why for Cajeta:** Cajeta must pick (or offer both): stackful gives drop-in blocking-style code and arbitrary suspension but costs a stack per fiber; stackless is allocation-light and GPU-friendly but constrains suspension points.
- **Key papers / sources:**
  - [Boost.Fiber — Scheduling](https://www.boost.org/doc/libs/1_68_0/libs/fiber/doc/html/fiber/scheduling.html) and [Worker threads](https://www.boost.org/doc/libs/1_68_0/libs/fiber/doc/html/fiber/worker.html) — Oliver Kowalke. Documents the work_stealing fiber scheduler and its static-instance constraints. (URLs from Boost docs index.)
  - [P0876: fiber_context — fibers without scheduler](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0876r11.pdf) — Kowalke, Crowl, et al., WG21 2022. Proposes a primitive stack-switch context for C++; informs Cajeta's low-level fiber ABI. (URL verified via search; WG21 paper.)
  - [Project Loom: Fibers and Continuations for the JVM](https://cr.openjdk.org/~rpressler/loom/Loom-Proposal.html) — Ron Pressler, OpenJDK. Separates *continuation* (stack capture) from *scheduler*; argues stackful continuations + pluggable scheduler. (URL verified.)
- **Algorithms to capture:** Cooperative stack switching (save/restore SP, callee-saved regs); segmented vs growable stacks; continuation capture/resume/yield split.
- **Implementation notes:** A stackful path needs a context-switch primitive in the Cajeta runtime (assembly per target ABI, like boost.context). The borrow checker must treat a fiber's captured borrows as live across suspension — a suspended fiber holding a `&mut` keeps that borrow alive, which interacts with lifetime inference.

### Stackless coroutine lowering via LLVM (codegen path)

- **What:** LLVM represents a coroutine as an ordinary function with `llvm.coro.*` intrinsics; the CoroSplit pass splits it into a ramp function plus resume/destroy functions and builds a heap (or elided) coroutine frame holding live state across suspends.
- **Why for Cajeta:** This is the concrete LLVM 22 mechanism Cajeta's frontend can target to implement `async`/generators/stackless fibers without writing its own state-machine transform.
- **Key papers / sources:**
  - [Coroutines in LLVM (docs)](https://llvm.org/docs/Coroutines.html) — LLVM project. Intrinsics (`coro.id`, `coro.begin`, `coro.suspend`, `coro.save`, `coro.end`, `coro.free`) and passes (CoroEarly, CoroSplit, CoroAnnotationElide, CoroElide, CoroCleanup). (URL verified.)
  - [CoroSplit.cpp source](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/Coroutines/CoroSplit.cpp) — implementation reference. (URL from search index.)
- **Algorithms to capture:** Suspend-point switch lowering (suspend/resume/destroy cases); coroutine-frame layout & spill of cross-suspend values; heap-allocation elision (CoroElide).
- **Implementation notes:** Cajeta emits `coro.*` intrinsics; the borrow checker must compute which values are live across a suspend (they land in the frame) and forbid borrows that would dangle when the frame is moved. Frame allocation should hook Cajeta's allocator/ownership so frames are freed deterministically.

### Structured concurrency (control-flow discipline)

- **What:** Concurrent tasks are scoped to a lexical block ("nursery"/scope); the block does not exit until all children complete; errors and cancellation propagate up the scope tree. Eliminates orphaned/leaked tasks the way structured programming eliminated `goto`.
- **Why for Cajeta:** Strongly synergistic with ownership: a scope can *borrow* parent data and guarantee children finish before the borrow ends — making concurrent borrows sound at compile time. This is a differentiating, "next-generation" default.
- **Key papers / sources:**
  - [Notes on structured concurrency, or: Go statement considered harmful](https://vorpus.org/blog/notes-on-structured-concurrency-or-go-statement-considered-harmful/) — Nathaniel J. Smith, 2018-04-25. The nursery pattern; the go/goto analogy. (Title/author/date verified.)
  - [Structured Concurrency: A Review](https://dl.acm.org/doi/10.1145/3547276.3548519) — ICPP Workshops 2022. Survey across languages/runtimes. (DOI from search; ACM may 403.)
  - [Structured concurrency — Wikipedia](https://en.wikipedia.org/wiki/Structured_concurrency) — history (Sústrik/libdill 2016, Smith/Trio 2017, JEP 453 Java). (entry point)
  - [libdill](https://sustrik.github.io/libdill/) — Martin Sústrik, 2016. Original C library for structured concurrency (coroutines + channels via `go`/`bundle`). (URL/author verified.)
- **Algorithms to capture:** Nursery/task-scope join-on-exit; cancellation token / cooperative cancellation propagation; error aggregation across siblings.
- **Implementation notes:** Make a `scope { ... }` block the primary concurrency entry; the scope's lifetime bounds child borrows so `&` and `&mut` into the parent stack are provably valid for the children's duration. Cancellation must run destructors (drop) on unwind — design cancellation as a checked suspend point.

### Synchronization primitives & lock-free / wait-free data structures

- **What:** Mutexes, RW locks, condition variables, semaphores, channels; and non-blocking structures: Treiber stack (CAS on top), Michael–Scott two-lock and lock-free queue, Chase–Lev deque, hazard pointers / epoch-based reclamation for safe memory reclamation.
- **Why for Cajeta:** The stdlib needs both blocking primitives (whose safety the type system can guard) and lock-free building blocks for the runtime itself (run queues, channels). Memory reclamation is the hard part under manual/borrowed memory.
- **Key papers / sources:**
  - [Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms](https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf) — Maged M. Michael & Michael L. Scott, PODC 1996. The MS-queue. (Canonical author PDF verified; ACM DOI 10.1145/248052.248106.)
  - [Non-blocking algorithm — Wikipedia](https://en.wikipedia.org/wiki/Non-blocking_algorithm) — defines lock-free vs wait-free vs obstruction-free. (entry point)
  - [A Wait-Free Stack](https://arxiv.org/abs/1510.00116) — Seep Goel, Pooja Aggarwal, Smruti R. Sarangi (IIT Delhi), arXiv 2015. First general-purpose wait-free stack construction. (Title/authors verified from PDF.)
  - [Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects](https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf) — Maged M. Michael, IEEE TPDS 15(6), 2004. The canonical hazard-pointer reclamation scheme (also solves ABA). (Title/author/venue verified; ACM/IEEE DOI 10.1109/TPDS.2004.8.)
  - [Systems Programming: Coping with Parallelism](https://dominoweb.draco.res.ibm.com/reports/rj5118.pdf) — R. Kent Treiber, IBM Research Report RJ 5118, 1986. The original lock-free (Treiber) stack via compare-and-swap. (Title/author/year verified via IBM Research catalog & Semantic Scholar; only IBM-hosted PDFs exist and were unreachable from this environment.)
  - [Dynamic Circular Work-Stealing Deque](https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf) — David Chase & Yossi Lev, SPAA 2005. For the runtime deque. (Title/authors verified from PDF; ACM DOI 10.1145/1073970.1073974.)
- **Algorithms to capture:** Treiber stack; Michael–Scott lock-free queue (with the ABA-avoiding tagged pointers); Chase–Lev growable deque; hazard pointers (Michael 2004); epoch-based reclamation (Fraser); RCU.
- **Implementation notes:** Cajeta needs `atomic<T>` with explicit memory orderings lowered to LLVM atomic IR / fences. The borrow model conflicts with shared mutable lock-free state — these structures need an `unsafe`-equivalent escape hatch with audited invariants. Reclamation (hazard pointers/epochs) substitutes for GC; integrate with the ownership system rather than bolt-on.

### Static deadlock-freedom: session types & typestate

- **What:** Type systems that statically guarantee well-typed concurrent programs never deadlock: binary/multiparty session types (channel protocols as types), linear-logic-based systems (πDILL, CP, HCP), and priority/ordering-annotated types that forbid cyclic wait. Typestate tracks an object's protocol state in its type.
- **Why for Cajeta:** This is the headline "next-generation" opportunity: encode lock-ordering and channel protocols in the type system so the *compiler* rejects deadlock-prone code — a natural extension of borrow checking from aliasing to ordering.
- **Key papers / sources:**
  - [Contrasting Deadlock-Free Session Processes (Extended Version)](https://arxiv.org/abs/2504.15845) — Jaramillo & Pérez, ECOOP 2025 (extended ver.). Compares HCP (hypersequent linear logic) vs Padovani's priority-based system for deadlock freedom. (URL + authors verified.)
  - [Manifest Deadlock-Freedom for Shared Session Types](https://www.cs.cmu.edu/~fp/papers/esop19.pdf) — Balzer, Toninho, Pfenning, ESOP 2019. Deadlock freedom for *shared* (not just linear) session types via resource ordering. (PDF URL verified as valid PDF; authors per CMU page — confirm author list.)
  - [A Gentle Overview of Asynchronous Session-based Concurrency: Deadlock Freedom by Typing](https://arxiv.org/pdf/2412.08232) — arXiv 2024. Tutorial-level intro to the area. (URL from search index.)
  - [Comparing Type Systems for Deadlock Freedom](https://arxiv.org/pdf/1810.00635) — Dardha & Pérez. Relates Kobayashi-style and CP-style approaches. (URL from search index.)
  - [Deadlock-free Context-free Session Types](https://arxiv.org/pdf/2506.20356) — Mordido & Pérez. Cyclic recursive topologies. (URL from search index.)
- **Algorithms to capture:** Session-type duality checking; linear-logic cut-elimination = deadlock-free communication; Kobayashi's usage/ordering (priority) inference; typestate transition checking.
- **Implementation notes:** Channels are the most natural carrier — give Cajeta channel endpoints session-typed protocols checked at compile time. Linearity (each endpoint used exactly once, in order) overlaps heavily with the borrow checker's move semantics, so the two analyses can share machinery. Start with binary session types on channels; multiparty and shared-session ordering are research-grade.

### Runtime deadlock detection & lock-ordering analysis

- **What:** Build a lock-order (resource-allocation / wait-for) graph and detect cycles; static lock-ordering lint; dynamic detectors (GoodLock and successors) that flag potential cyclic lock acquisitions even on non-deadlocking runs; incremental online cycle detection.
- **Why for Cajeta:** A pragmatic complement to (incomplete) static guarantees — a debug-mode runtime detector plus a compile-time lock-order lint catches the deadlocks the type system can't.
- **Key papers / sources:**
  - [A Randomized Dynamic Program Analysis Technique for Detecting Real Deadlocks](https://people.eecs.berkeley.edu/~ksen/papers/deadlock.pdf) — Pallavi Joshi, Chang-Seo Park, Koushik Sen (UC Berkeley), PLDI 2009 ("DeadlockFuzzer"/active testing). (Title/authors verified from PDF; note: Naik is not an author of this paper.)
  - [Dynamic Cycle Detection for Lock Ordering](https://whileydave.com/2020/12/19/dynamic-cycle-detection-for-lock-ordering/) — David J. Pearce. Incremental topological-order cycle detection (as in Abseil's mutex). (URL from search index.)
  - [Using Runtime Analysis to Guide Model Checking of Java Programs](https://link.springer.com/chapter/10.1007/10722468_15) — Klaus Havelund, SPIN 2000 (Springer LNCS 1885). Introduces the original GoodLock deadlock-detection algorithm. (Title/author/venue verified; Springer landing page is paywalled. iGoodlock / generalized GoodLock: see Bensalem & Havelund and Joshi et al.)
- **Algorithms to capture:** Wait-for-graph cycle detection (DFS); GoodLock lock-tree analysis; iGoodlock (context-augmented, no explicit graph); Pearce–Kelly incremental cycle detection; DeadlockFuzzer randomized scheduling.
- **Implementation notes:** Provide a build-flag-gated runtime that records lock-acquire order per thread and checks the global lock-order DAG with incremental cycle detection (Pearce). Since Cajeta owns the mutex type, instrumentation is free and zero-cost when disabled. Pair with a static lint over the borrow/effect analysis.

## PDF / paper backlog

- [x] Scheduling Multithreaded Computations by Work Stealing — https://www.csd.uwo.ca/~mmorenom/CS433-CS9624/Resources/Scheduling_multithreaded_computations_by_work_stealing.pdf — papers/blumofe-leiserson-1999-work-stealing.pdf
- [x] Cilk: An Efficient Multithreaded Runtime System — https://publications.csail.mit.edu/lcs/pubs/pdf/MIT-LCS-TM-548.pdf — papers/blumofe-1995-cilk-runtime.pdf
- [x] Work Stealing with Private Deques — https://www.chargueraud.org/research/2013/ppopp/full.pdf — papers/acar-2013-private-deques.pdf
- [x] P0876r11: fiber_context — https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0876r11.pdf — papers/kowalke-2022-p0876r11-fiber-context.pdf
- [ ] Project Loom Proposal — https://cr.openjdk.org/~rpressler/loom/Loom-Proposal.html — (html-only, not downloaded)
- [ ] LLVM Coroutines docs — https://llvm.org/docs/Coroutines.html — (html-only, not downloaded)
- [ ] Making the Tokio scheduler 10x faster — https://tokio.rs/blog/2019-10-scheduler — (html-only, not downloaded)
- [x] Contrasting Deadlock-Free Session Processes — https://arxiv.org/abs/2504.15845 — papers/jaramillo-perez-2025-contrasting-deadlock-free.pdf
- [x] Manifest Deadlock-Freedom for Shared Session Types — https://www.cs.cmu.edu/~fp/papers/esop19.pdf — papers/balzer-2019-manifest-deadlock-freedom.pdf
- [x] Comparing Type Systems for Deadlock Freedom — https://arxiv.org/pdf/1810.00635 — papers/dardha-perez-2018-comparing-type-systems-deadlock.pdf
- [x] A Gentle Overview of Async Session-based Concurrency — https://arxiv.org/pdf/2412.08232 — papers/2024-gentle-overview-async-session-concurrency.pdf
- [x] A Randomized Dynamic Analysis for Detecting Real Deadlocks — https://people.eecs.berkeley.edu/~ksen/papers/deadlock.pdf — papers/joshi-2009-randomized-deadlock-detection.pdf
- [x] Deadlock-free Context-free Session Types (Mordido & Pérez) — https://arxiv.org/pdf/2506.20356 — papers/mordido-perez-2025-deadlock-free-context-free-session.pdf
- [x] A Wait-Free Stack (Goel, Aggarwal, Sarangi) — https://arxiv.org/abs/1510.00116 — papers/2015-wait-free-stack.pdf
- [x] Simple, Fast, and Practical Concurrent Queues (Michael–Scott, PODC'96) — https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf — papers/michael-scott-1996-concurrent-queues.pdf
- [x] Chase & Lev, Dynamic Circular Work-Stealing Deque (SPAA 2005) — https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf — papers/chase-lev-2005-dynamic-circular-work-stealing-deque.pdf
- [ ] Treiber stack (IBM RJ 5118, 1986) — https://dominoweb.draco.res.ibm.com/reports/rj5118.pdf — (IBM-hosted PDF unreachable from this environment; citation verified, not downloaded)
- [x] Hazard Pointers (Maged Michael, IEEE TPDS 2004) — https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf — papers/michael-2004-hazard-pointers.pdf
- [ ] Scalable Go Scheduler Design Doc (Vyukov) — https://docs.google.com/document/d/1TTj4T2JO42uD5ID9e89oa0sLKhJYD0Y_kqxDv3I3XMw/edit — (Google Doc / html-only, not downloaded)
- [ ] Zero-cost futures in Rust (Turon, 2016) — http://aturon.github.io/blog/2016/08/11/futures/ — (html-only, not downloaded; URL verified)
- [ ] Notes on structured concurrency (N. Smith, 2018) — https://vorpus.org/blog/notes-on-structured-concurrency-or-go-statement-considered-harmful/ — (html-only, not downloaded; URL verified)

## Open questions

- Stackful vs stackless as Cajeta's *default* task representation — or both, with stackless `async` on the LLVM coro path and stackful fibers for blocking-style code? What is the ABI for stack switching across GPU/host boundaries?
- How far can session/typestate types go before they become ergonomically prohibitive? Can binary session types on channels be the safe default with an `unsafe` escape, mirroring the borrow checker?
- How does borrow checking interact with values *live across a suspend point* (coroutine frame) and with cancellation/unwind (must drop run deterministically)? Does Cajeta need a `Pin`-equivalent typestate?
- Preemption model: cooperative-only (deterministic drops, but needs yield points) vs async signal-based preemption (Go-style fairness, harder RAII)? 
- GPU offload: can the work-stealing scheduler and structured-concurrency scopes span host + device, or is device concurrency a distinct model (warps/blocks) that the language surfaces separately? How do channels/session types map onto GPU kernels?
- Memory reclamation for lock-free structures without GC: hazard pointers vs epoch-based reclamation vs leveraging ownership — which integrates best with the borrow checker?
- Should lock-ordering be a compile-time *effect* tracked alongside borrows, with a debug runtime cycle-detector as backstop? What is the zero-cost-when-disabled instrumentation design?
- ~~Verify and pin down the still-`unverified` citations (Chase–Lev, Treiber, hazard pointers, Vyukov design doc, Turon futures, Smith structured-concurrency post) before they enter any authoritative reference list.~~ Done (2026-05-28): all six confirmed. Chase–Lev, hazard pointers, Treiber, Turon, Smith, and Vyukov's design doc now have verified titles/authors/years. Treiber's RJ 5118 and Vyukov's design doc exist only as IBM-hosted PDF / Google Doc respectively and could not be archived as local PDFs.
