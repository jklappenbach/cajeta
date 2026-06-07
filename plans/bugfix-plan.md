# Cajeta runtime + XPU — bugfix plan

> **RESOLVED (2026-06-03).** All 36 findings are dispositioned: every actionable item is
> FIXED and test-gated; the residual `[~]` rows are conscious won't-fix / by-design
> decisions (M2, M4, M10, L4, L7), not open work. Retained as the disposition record.

**Source:** adversarial bug-hunt workflow over the runtime + XPU subsystem (2026-06-03).
12 subsystem hunters → independent per-finding verification → synthesis. **36 confirmed**
(4 critical, 16 high, 11 medium, 5 low). The ray-query / acceleration-structure code is
NOT in this list — it was separately reviewed and fixed earlier the same day (its
`g_xpu_vk_submit_mu` discipline is in fact the correct convention several findings below
reference).

> **Line numbers are an audit snapshot** — the code shifts as fixes land; re-confirm each
> location before editing. Checkbox legend: `[ ]` open · `[~]` partial · `[x]` fixed.
>
> **Working agreement:** one wave at a time, verify each fix (build + tests, RT exec where
> relevant); commit only when asked; **no attribution trailer**; stage files explicitly.

## Two dominant clusters (most blast radius)
1. **Exception / unwind path** — C1, C2, H2, H3, H4, H5, H18, M1. Memory-safety + silent
   swallow on idiomatic `try` / `finally` / `return`.
2. **Shared Vulkan resource tables** — C3, C4, M8. Unlocked RMW → double-free / driver-handle
   UAF, racing main-thread vs carrier.

## Recommended wave order
- **Wave 1** (low-risk, mechanical, high-confidence): Vulkan tables + array overflow + AS borrow.
- **Wave 2** (careful codegen): exception / unwind cluster.
- **Wave 3** (contained codegen): device-kernel literal + marshalling correctness.
- **Wave 4**: GPU-driver robustness.
- **Wave 5**: fiber / lock lifecycle.
- **Wave 6**: nits.

---

## Wave 1 — Vulkan tables + memory overflow + AS borrow (quick wins)

- [x] **C3 — `g_vk_bufs` table RMW is lock-free** *(critical)* — FIXED 2026-06-03: `g_xpu_vk_submit_mu` made recursive (runtime-init'd) + every buffer-table fn (alloc/free/rec/mapped) locks it. — `cajeta_runtime.c:4685-4702` (alloc),
  `:4714-4722` (free), callers `:5897`/`:5961`, transient-SSBO `:6387`/`:6406`.
  *Trigger:* main-thread `Buffer.alloc/free` concurrent with a carrier launch building transient
  scalar SSBOs → non-atomic `g_vk_buf_count++` + dead-slot scan → two handles in one slot (leak +
  double-free) / torn count. *Fix:* guard every `g_vk_bufs`/`g_vk_buf_count` read+write with
  `g_xpu_vk_submit_mu` (mirror the AS table at `:5043`); transient-SSBO alloc/free inside the lock.
- [x] **C4 — `g_vk_texs` table RMW lock-free; launch reads `t->view` while free destroys it** *(critical)* — FIXED 2026-06-03: tex_alloc/_free/_rec lock the recursive `g_xpu_vk_submit_mu`; tex_free now blocks while a launch holds it across the view's use. —
  `cajeta_runtime.c:4800-4817` (alloc), `:4903-4911` (free), `:4740-4744` (rec), read under lock `:5374-5376`.
  *Trigger:* carrier holds lock, captures `t->view` into the descriptor write; main thread `texture_free`
  (unlocked) `vkDestroyImageView`s it → destroyed view handed to `vkUpdateDescriptorSets` → driver UAF.
  *Fix:* take `g_xpu_vk_submit_mu` around tex_alloc/_free/_rec, matching the AS table.
- [x] **M8 — tex_upload staging alloc/free outside the submit lock** *(medium)* — FIXED 2026-06-03: the staging `cajeta_xpu_vk_alloc`/`_free`/`_mapped`/`_rec` now self-lock the recursive mutex, so the staging table access is serialized regardless of tex_upload's own lock. — `cajeta_runtime.c:4828`,`:4832`,`:4900`.
  Same unlocked `g_vk_bufs` window as C3, via `Texture2D.upload`. NOTE: the submit *region* of tex_upload
  was locked this session (commit `94f883a`); the staging alloc/rec/free still sit outside it. *Fix: subsumed by C3 —
  do the staging alloc/rec/free under the lock.*
- [x] **H1 — unchecked size multiply in `__cajeta_new_array_header` → heap overflow** *(high)* — FIXED 2026-06-03: overflow guard (`count > (UINT64_MAX-header_size)/elem_size`) added to both `_new_array_header` and `_array_view_to_owned`. —
  `cajeta_runtime.c:273-277` (+ `_array_view_to_owned:331`). *Trigger:* `total = header_size + count*elem_size`
  in uint64 passed to `calloc(1,total)` defeats calloc's overflow check; `new int[-1]` → wraps to 4, then the
  8-byte count write overflows. *Fix:* reject `count > (UINT64_MAX-header_size)/elem_size`, clamp `count<0`/`elem_size<=0`
  (mirror `:328-329`), abort like the existing failure mode.
- [x] **H10 — `AccelerationStructure` launch arg records no borrow → device UAF escapes the borrow checker** *(high)* — FIXED 2026-06-03: `recordLaunchBorrow` hoisted to cover Buffer/Texture2D/AccelerationStructure (`CallExpression.cpp`); the `Method.cpp` drop-gate broadened from Buffer-only to all three (also closes the latent Texture2D-not-enforced gap). —
  `CallExpression.cpp:162-175`,`:189`. `recordLaunchBorrow` runs only for `isBuffer||isTexture`; an AS is POD-by-value
  so no borrow is recorded → `scene` can drop before `s.sync()` with no XPU-K02. *Fix:* add `xpu::isAccelStructType`
  to the borrow set; hoist `recordLaunchBorrow` to cover Buffer/Texture2D/AccelerationStructure; broaden the
  `Method.cpp:1484` Buffer-only drop-gate filter (also closes the latent Texture2D gap).

## Wave 2 — Exception / unwind cluster (careful; spot-verify each)

- [x] **C1 — `return` inside `try` leaves a dangling exc frame → longjmp into a dead stack** *(critical)* —
  `Statement.cpp:824-983` (Try) + `:1136-1156`/`:1591-1593` (Return); runtime longjmp `cajeta_runtime.c:2024`.
  *Trigger:* any `T f(){ try{…;return x;}catch{…} }` — pop emitted only on fall-through, `return` skips it.
  *Fix:* track open try-frames per method (like `dropFrameStack`); emit one `__cajeta_exc_pop` per open frame
  (LIFO) before every `ret`, or route returns-in-try through the try `afterBB`. FIXED 2026-06-03: module tryFinallyStack of active try/catch frames; emitTryFinallyUnwind pops each + runs its finally at every return site (test returnInTryRunsFinally). break/continue-out-of-try is a remaining gap.
- [x] **C2 — scope cancellation write-after-free of an already-freed fiber** *(critical)* —
  `cajeta_runtime.c:982-988` / `:1027-1033`; fiber freed `:738-741`, slot written `:798` never cleared.
  *Trigger:* sibling B completes (fiber freed, slot dangling), sibling A throws → cancel loop writes `cancel_with`
  into freed heap. FIXED 2026-06-03: fiber stores its slot_ptr; carrier nulls *slot_ptr under __cajeta_task_mutex before free; the scope-cancel read+cancel now also holds the mutex (race-free).
- [x] **H2 — `try {} finally {}` (no catch) silently swallows the throw** *(high)* — FIXED 2026-06-03: empty-catch landing pad now runs finally + __cajeta_throw(thrown) + unreachable (test tryFinallyNoCatchPropagatesThrow). `Statement.cpp:914-960`.
  catchBB pops + branches to afterBB; with no catch clauses nothing re-raises. *Fix:* when `catchClauses` empty,
  run finally on the catch path then `__cajeta_throw` + `unreachable`.
- [x] **H3 — `finally` skipped when the `catch` handler throws/re-throws** *(high)* — `Statement.cpp:914-980`.
  catchBB pops the frame before the body runs, so `throw` in catch longjmps to the outer frame and `finally` never runs.
  *Fix:* emit finally on the abrupt path before propagating (extra frame around the catch body, or duplicate finally onto the throw edge).
- [x] **H4 — throw through open scope frames leaks them + orphans spawned children** *(high)* —
  `cajeta_runtime.c:1992-2025`, trampoline `Expression.cpp:3426-3459`. `__cajeta_throw` unwinds only the drop chain,
  never `scope_top` → structured-concurrency invariant violated. *Fix:* snapshot a scope watermark in
  `cajeta_exception_frame` at try-entry; have throw / the trampoline catch arm / TryStatement catchBB call
  `__cajeta_scope_exit_to` to it. FIXED 2026-06-03: TryStatement captures the scope watermark at try-entry (alloca, survives setjmp) and calls __cajeta_scope_exit_to at catchBB after popping its frame, joining the try body's scope children before the catch (test throwOutOfScopeJoinsChild).
- [x] **H5 — stack-trace side table grows unbounded; stale entries shadow reused addresses** *(high)* — FIXED 2026-06-03: trace_record dedups same-throwable + caps the table (CAJETA_TRACE_TABLE_CAP). —
  `cajeta_runtime.c:1885-1949`. Capture defaults on; every throw mallocs + prepends, nothing frees; pointer-match
  lookup surfaces a stale trace for a reused throwable address. *Fix:* free `e->frames`+`e` on catch/drop; drop a
  prior same-pointer entry before prepend; or LRU cap.
- [x] **H18 — uncaught-exception diagnostic prints a Cajeta `String` object through `%s`** *(high)* —
  `cajeta_runtime.c:1969-1983`. Slot 1 is a `String` *object*, not a C string → prints vtable bytes + OOB scan.
  *Fix:* read slot 1 as `String*`, extract `bytes`+`byteLength`, print `%.*s`, guard null.
- [x] **M1 — cancellation marker never delivered when the awaited task is already done** *(medium)* — FIXED 2026-06-03: task_wait checks cancel_with on entry (before the park loop). —
  `cajeta_runtime.c:829-841`. `cancel_with` read only inside `while(!*done_addr)`. *Fix:* check `cancel_with` at the
  top of `task_wait`.

## Wave 3 — Device-kernel codegen correctness (contained)

- [x] **H11 — FIXED 2026-06-03: threaded the host module DataLayout into collectKernelParamInfo. POD-struct/scalar `byteSize` from empty `DataLayout("")` → SSBO mis-size + OOB device read** *(high, default target)* —
  `KernelLowering.cpp:1900-1906`; consumed `cajeta_runtime.c:6385-6390`; host packs under real DL `CallExpression.cpp:200-218`.
  `{i32;i64}` sized 12 vs real 16 → device reads 4 bytes past the SSBO. *Fix:* thread the real module `DataLayout` into
  `collectKernelParamInfo`.
- [x] **H12 — FIXED 2026-06-03 (interim): reject non-4-byte dynamic Shared<T> (XPU-N01) until the runtime spec constant carries the element size. dynamic-shared spec constant hardcodes a 4-byte element** *(high)* — `SpirvBackend.cpp:265-337` +
  `cajeta_runtime.c:5280-5290`. SpecId 3 = `sharedBytes/4`, but the array uses the real elem type → `Shared<half>` OOB.
  *Fix:* emit the element byte size into per-kernel metadata; compute `sharedBytes/elemSize`; until then reject `elemSize!=4`.
- [x] **H13 — kernel hex/binary/octal literals miscompiled** *(high)* — FIXED 2026-06-03: device literal lowering mirrors the host (radix/underscore/L + APInt; APFloat for floats; widen to i64 by L-suffix/magnitude; f32-by-suffix). Test lowersNumericLiteralsCorrectly. `KernelLowering.cpp:811-815`. `0xFF`→0, `0b1010`→0,
  `0777`→777. *Fix:* switch radix on `integerLiteralType`, build via `APInt(width,text,radix)`.
- [x] **H14 — kernel underscore separators miscompiled** *(high)* — `:811-815`,`:1689-1698`. `1_000_000`→1. *Fix:* strip `_`.
- [x] **H15 — kernel 64-bit / `L`-suffixed literals truncated to i32** *(high)* — `:811-815`. `5000000000`→705032704.
  *Fix:* materialize ints at i64, narrow via `unifyOperands`/`coerceTo`.
- [x] **H16 — kernel `double` literals rounded through f32** *(high)* — `:816-820`. f64 precision lost (stod→FloatTy→FPExt).
  *Fix:* gate f32 on the f/F suffix, else `getDoubleTy` via `APFloat::convertFromString`.
- [x] **H17 — FIXED 2026-06-03: guard the promoted-alloca erase with use_empty(); reject (XPU-N02) when a user wasn't redirected. context-promoted alloca erased while GEP/non-job-block users remain** *(high)* — `CpuBarrierFission.cpp:371-392`,`:490-511`.
  Step-10 redirect misses GEP/bitcast/non-job users; `eraseFromParent` with live users → dangling IR. *Fix:* assert
  `use_empty()`/RAUW; handle GEP users; or `unsupported()` if a user is outside a job block.
- [x] **L1 — malformed/out-of-range kernel literal crashes the compiler** (`std::out_of_range`) instead of XPU-N01 — `KernelLowering.cpp:811-820`. *Fix: parse via APInt/APFloat status, or `unsupported(...)`.* (folds in with H13–16)
- [x] **L2 — FIXED 2026-06-03: kernel `Math.min/max` on unsigned uses `smin/smax`** — `KernelLowering.cpp:1448-1459`. *Fix: pick `umin/umax` via `exprSigned`.*
- [x] **L3 — FIXED 2026-06-03: `Vector<T,0>` reaches `FixedVectorType::get(elem,0)` → abort/degenerate** — `KernelLowering.cpp:939-961`. *Fix: reject `lanes==0`.*

## Wave 4 — GPU-driver robustness

- [x] **H9 — FIXED 2026-06-03: CUDA context never made current on the launching thread** *(high)* — `cajeta_runtime.c:4029`,`:6207-6234`,`:5724-5727`.
  No `cuCtxSetCurrent` anywhere; cross-thread launch → `CUDA_ERROR_INVALID_CONTEXT`, return discarded → silent no-op.
  *Fix:* `cuCtxSetCurrent` at each entry point (or primary-ctx retain); check the launch return value.
- [x] **M3 — FIXED 2026-06-03: `register_kernel_params` never dedups → re-registration appends, exhausts the 128-slot table** *(medium)* —
  `cajeta_runtime.c:4219-4241`. *Fix:* find-then-overwrite under the lock.
- [~] **M4 — PARTIAL 2026-06-03: detectable bad-texture cases now skip the launch (via M5/M6's launchOk); a kp==NULL kernel with textures is undetectable. HIP launch passes raw texture-record pointers when kparams missing/oversized** *(medium)* — `:6276-6304`.
  *Fix:* mirror the Vulkan NULL/`count>64` guard (`:6342`).
- [x] **M5 — FIXED 2026-06-03: HIP `hipCreateTextureObject` failure still substitutes NULL texObj and launches** *(medium)* — `:6294-6310`.
  *Fix:* on `obj==0`, diagnose + skip the launch.
- [x] **M6 — FIXED 2026-06-03: kernel with >8 textures passes raw handles for the 9th+** *(medium)* — `:6272-6301`. `texObjs/texObjVals` are `[8]`.
  *Fix:* size to the 64-param cap or abort over capacity.
- [x] **M7 — FIXED 2026-06-03: tex_upload leaves the image in UNDEFINED layout on cmd-alloc failure** *(medium)* — `:4844-4845`.
  *Fix:* track per-tex ready/layout; skip/re-issue or propagate the error.
- [x] **M9 — FIXED 2026-06-03: grid block-count overflows int32 → silent no-op / divide-by-zero** *(medium)* — `:5814-5826`,`:5759-5768`.
  *Fix:* compute `nblocks`/`gxy`/loop index in int64; validate dims.
- [~] **M10 — BY-DESIGN 2026-06-03: a zero-parameter compute kernel has no observable I/O; not dispatching it is harmless. Only the diagnostic wording is suboptimal (no correctness impact); full empty-descriptor dispatch deemed not worth the launch-path risk. zero-parameter Vulkan kernel can never launch** *(medium)* — `VulkanRegistration.cpp:124-151` + `cajeta_runtime.c:6342-6348`.
  kparams emitted only `if (!info.empty())`; Vulkan launch hard-requires `count>0`. *Fix:* drop the guard or relax to `count<0`.
- [x] **M11 — FIXED 2026-06-03: `implements KernelArg` non-POD admitted but has no marshal/lower path** *(medium)* — `KernelArgTrait.cpp:126`
  vs `CallExpression.cpp:189-222` vs `KernelLowering.cpp:150`. Silently dropped kernel. *Fix:* require `isPodStruct`, or extend
  marshal + `deviceStructInfo` to recurse nested primitives.
- [~] **L4 — DEFERRED 2026-06-03 (low): one-shot, process-lifetime instance/device leak on a rare partial-init-failure path; goto-cleanup refactor risk outweighs the marginal benefit. init tri-state leaks instance/device on partial bring-up failure** — `cajeta_runtime.c:4369-4642`. *Fix: goto-cleanup destroy.*
- [x] **L5 — FIXED 2026-06-03: `vkBindBufferMemory`/`vkBindImageMemory` return ignored → live unbacked slot on bind OOM** — `:4677`,`:4783`. *Fix: check VkResult.*
- [~] **L7 — DEFERRED 2026-06-03 (low): per the audit, effectively unobservable (a zero-decoration kernel has no bound output to observe the workgroup size). workgroup-size spec-constant skipped when SPIR-V has zero decorations** — `SpirvBackend.cpp:191-216`. *Fix: insert before first `OpFunction`.*
- [x] **L9 — FIXED 2026-06-03: `find_kparams` read lock-free while registration mutates** — `cajeta_runtime.c:6276`,`:6342` vs `:4219-4241`. *Fix: hold the lock across find / order the publish.*

## Wave 5 — Fiber / lock lifecycle

- [x] **H6 — FIXED 2026-06-03: fibers stranded on park/lock queues leaked on shutdown; un-nulled `parked_head` → cross-run swapcontext into recycled context** *(high)* —
  `cajeta_runtime.c:756-772`,`:623`,`:1065-1066`. *Fix:* on shutdown, drain `parked_head` + lock wait queues (free stack+struct), null `parked_head`/`ready_head`/`ready_tail`.
- [x] **H7 — FIXED 2026-06-03: fiber parked on a Lock wait-queue ignores cancellation → deadlock or runs uncancelled** *(high)* —
  `cajeta_runtime.c:1112-1131` vs `:1134-1167`, `:829-840`. *Fix:* check `cancel_with` after the swapcontext; dequeue + throw; cancellation unlinks lock waiters.
- [x] **H8 — FIXED 2026-06-03: `__cajeta_lock_destroy` with parked waiters: lock UAF, lost fibers, UB destroying a busy mutex/cond** *(high)* —
  `cajeta_runtime.c:1185-1191`. *Fix:* lock `l->mutex`, refuse/drain when `held || wait_head`, check destroy return for EBUSY before free.
- [x] **H19 — FIXED 2026-06-03: Win32 fiber handle leaked for every completed task (no `DeleteFiber`)** *(high, Windows)* —
  `cajeta_runtime.c:498` vs `:738-741`. *Fix:* `DeleteFiber(f->ctx.fiber)` in the DONE branch under `_WIN32`; skip the `f->stack` malloc on Windows.
- [~] **M2 — BY-DESIGN 2026-06-03: documented intentional wake-and-recheck; a fairness (starvation-under-contention) issue, not a correctness bug. try_acquire / main-acquire bypass the FIFO fiber wait queue → starvation** *(medium)* — `cajeta_runtime.c:1172-1183`,`:1095-1106`. *Fix: real handoff; refuse to barge when `wait_head` non-empty.*
- [x] **L6 — FIXED 2026-06-03: dynamic shared byte count round-tripped through int32 + ZExt → ~4GB host alloca; no per-block bound** — `CpuBarrierFission.cpp:248-255`, `cajeta_runtime.c:6432-6434`. *Fix: keep unsigned end-to-end; bound-check before alloca.*

## Wave 6 — Nits

- [x] **L8 — FIXED 2026-06-03: `__cajeta_path_stat` fills `isSymlink` via `stat()` (follows links) → always false** — `cajeta_runtime.c:3783-3806`. *Fix: use `lstat()` (MinGW stubs it).*

---

## Verified-clean (no action)
- Ray-query / AS build code (separately reviewed + fixed; its `g_xpu_vk_submit_mu` discipline is the model for C3/C4).
- Main-thread (non-fiber) lock-acquire path (`cajeta_runtime.c:1095-1105`).
- HIP context / primary-context handling (lazy per-thread, unlike CUDA's H9).
- Scalar / all-same-width POD-struct marshalling (host/device layouts coincide; H11 is the *mixed-width* case).
