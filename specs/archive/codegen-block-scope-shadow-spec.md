# codegen-block-scope-shadow — defect (FIXED 2026-07-31)

Supersedes the mis-titled `codegen-many-param-f32-truncate-spec.md`, whose
diagnosis (a parameter-count cliff) was wrong in every particular. Parameter
count is irrelevant; see 1.5.

## 1. Definition

A local variable declared inside a nested `{ ... }` block permanently rebinds
its name for the remainder of the enclosing method. When the name it shadows is
a parameter or outer local of a **different type**, code after the closing brace
resolves to the wrong slot and the compiler emits malformed LLVM IR rather than
a diagnostic.

- 1.1 **Root cause.** There is exactly one `Scope` per method
  (`Method.cpp` holds the only `ScopeStack::add`/`pop` pair).
  `Block::generateCode` maintained drop frames, arena marks, and the move log
  per block, but never any *name* scope, so every nested-block declaration was
  `putField`'d into the method-wide map — `fields[name] = field`, an
  unconditional overwrite with no shadow stack and no restore at the `}`.

- 1.2 **Failure shape.** Given a `float32[] v` parameter shadowed by a
  `float32 v` inside an `if`, a later `v[i] = …` indexes the scalar. The emitted
  IR GEPs through a float:

  ```llvm
  %424 = load float, ptr %36, align 4                              ; the scalar local
  %426 = getelementptr inbounds nuw %"#array.float32[]", float %424, i32 0, i32 0
  %427 = load i64, float %426, align 8                             ; base is not a pointer
  ```

- 1.3 **Why it surfaced so far from the cause.** `cajeta build` writes bitcode
  into a `.cja` without running the verifier or instruction selection, so a
  corrupt module ships silently. The abort only comes later, at the first
  `--emit=exe` that selects the function:

  ```
  LLVM ERROR: Cannot select: f32 = truncate i64
    i64,ch = CopyFromReg …, Register:i64 %121
  In function: dev.cajeta.xgboost.tree.TreeBuilder::grow(…)
  ```

  The message names LLVM and a function signature, never the shadowing
  declaration — in the original report the declaration was 18 lines above the
  use, and the reported "trigger" (adding a parameter) merely introduced the
  name collision.

- 1.4 **Severity.** Worse than the original spec assessed. The abort is the
  *lucky* outcome: the minimal repro in 2.1 links successfully and instead
  crashes at run time, so this could silently miscompile.

- 1.5 **Corrections to the superseded spec.** Its 1.1 blamed parameter count
  (a 23-parameter function with three `float32[]` out-params compiles fine); its
  1.2 blamed the `--profile=test` compile (the library compile produced the same
  corrupt bitcode, it just never ran instruction selection); its 1.3 called the
  failure loud and incapable of corrupting a build (1.4 above).

- 1.6 **Observed** 2026-07-31 in `cajeta-xgboost`
  `dev.cajeta.xgboost.tree.TreeBuilder.grow`: a `float32 lossChg` computed
  inside `if (s.valid) { … }` shadowed the `float32[] lossChg` out-parameter
  written by `lossChg[nid] = splitLoss;` after the block.

## 2. Acceptance — all met

- 2.1 A scalar local declared inside a block does not capture a same-named array
  parameter after the closing brace:

  ```cajeta
  static float64 f(float32[] v, int64 n) {
      if (n > (int64) 0) { float32 v = (float32) 1.5; }
      v[0] = (float32) 2.5;        // the PARAMETER
      return (float64) v[0];       // 2.5
  }
  ```

- 2.2 The shadow is still in effect inside its own block.
- 2.3 Sibling blocks reusing one name do not leak into each other.
- 2.4 Nested shadows unwind innermost-first to the outermost binding.
- 2.5 A loop body behaves as a block for 2.1–2.4.
- 2.6 No `.cja` member contains a module the LLVM bitcode reader rejects.

## 3. Fix

`Block::generateCode` snapshots the prior binding of every name the block
*directly* declares (the same `LocalVariableDeclaration` walk the arena scan
already performs) and restores it after the drop/arena teardown, in reverse.
`Scope` gained `localBinding` (this scope only, non-inserting — `getField`'s
`fields[name]` default-constructs a null entry on a miss, which would make
"unbound" indistinguishable from "bound to null") and `restoreBinding`.
`fieldList`/`allocaToField` are untouched: each field owns a distinct alloca, so
the shadowing entry never displaced anything keyed by alloca.

- 3.1 **Only genuinely shadowing names are tracked.** The first cut also
  *unbound* names that introduced no shadow, which regressed
  `XpuLaunchBorrowTests.dropBeforeSyncRejected`: analyses that run after the
  method body resolve names through this map, and `Method::destroyScope`'s
  launch-borrow gate uses `containsField`/`getField` to find a pending borrow's
  drop entry — so unbinding a block-local `KernelBuffer` made the XPU-K02
  diagnostic silently disappear. A name with no prior binding has nothing to
  corrupt, so it is now left bound exactly as before and the change touches only
  the shadowing case. Caught by the full regression sweep, not by 2.1–2.5.

Pinned by `test/parser/BlockScopedShadowTests.cpp` (5 tests, 2.1–2.5) and by
`cajeta-xgboost`, which now builds and passes 65/65 with the clearer
two-out-parameter form the packed-`float64[]` workaround had replaced.

## 4. Adjacent, not fixed

- 4.1 The IR cache (`.cajeta/cache/ir/`) keys on source and flags but not on the
  compiler binary, so a locally rebuilt compiler serves stale objects from a
  previous build. Purging `.cajeta/cache` was required to observe this fix.
  Harmless for released toolchains (the version participates in the key), a
  footgun during compiler development.
- 4.2 Archive writes run neither the LLVM verifier nor instruction selection, so
  a malformed module reaches consumers intact. Verifying on `--emit=cja` would
  have named this defect at its source.
