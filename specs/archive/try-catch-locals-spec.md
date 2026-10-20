# try-catch-locals — a local changed inside `try` keeps its value after a throw

## 1. Definition

- **1.1** `try` is lowered on setjmp/longjmp (`Statement.cpp:892`). At `--opt=O1` and
  above, LLVM keeps a local in a register across the `setjmp`. When a throw
  `longjmp`s back, the catch and the code after it read the register as it was at
  `try` entry, not the value the body last stored. C makes such locals
  indeterminate unless they are `volatile`. Cajeta has no such rule and must not
  acquire one: the program is valid and the compiler is wrong.
- **1.2** Measured 2026-10-20 at 89e58ed9 (`s = s + pick(i)` in a loop that throws
  at `i == 3`): O0 prints `catch s=6 i=3 after s=6 i=3`; O1, O2, O3 and
  `--release` print `s=0 i=0` in both places. v0.43.1 `--release` gives the same
  wrong answer. The JIT (`jit-run`) is correct. Every optimized release build
  since setjmp `try` has been able to return wrong results silently.
- **1.3** Three sites emit the setjmp: `try` (`Statement.cpp:942`, and the catch
  frame at `:1076`), the after-throw wrapper (`Method.cpp:1381`), and the task
  trampoline (`Expression.cpp:5488`). The fix covers every function that calls a
  `returns_twice` function, whichever site emitted it.
- **1.4** Non-goals: replacing setjmp `try` with zero-cost (table-driven)
  exceptions. That changes the cost of entering a `try` and of throwing, touches
  fibers and the Windows unwinder, and is its own spec if wanted.

## 2. Correct values after a throw

- **2.1** When a local is assigned inside a `try` body and a throw reaches the
  catch, the catch reads the last value the body stored.
- **2.2** When the catch completes normally, the code after the `try` reads the
  same value.
- **2.3** When the local is a parameter, a loop variable, or is assigned in a
  callee-inlined body, 2.1 and 2.2 hold.
- **2.4** When `try` statements nest, 2.1 and 2.2 hold at each level.
- **2.5** When a `finally` runs after a throw, it reads the last stored value.
- **2.6** When the same program is built at O0, O1, O2, O3, `--release` and through
  the JIT, every build prints the same output.

## 3. Cost

- **3.1** When a function has no `try`, its code is unchanged.
- **3.2** When a function has a `try`, only locals that are both stored after a
  `setjmp` and read on a path from its second return stay in memory; every
  other local is still promoted to a register.
- **3.3** When the tensor-element-access benchmarks (flatGet, flatSet, Npy) run,
  their `--release` numbers do not regress beyond noise.
