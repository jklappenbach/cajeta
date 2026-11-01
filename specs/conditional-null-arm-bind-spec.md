# Conditional with a null arm bound by `=` takes title (defect)

## 1. Definition

- **1.1** A local bound with plain `=` from a conditional whose arms are `null`
  and a field read takes the title of the object the field holds. The local's
  drop frees it at scope exit, while the field still points at it. No
  diagnostic is reported.
- **1.2** Found 2026-11-01 in cajeta-llm (llm-cpu-parity 5.2.2). `Linear`
  bound `KernelBuffer<float32> yC = c == null ? null : c.yDev;` (and the same
  for `c.payloadDev`), so every fused decode launch freed the v projection's
  output buffer and weight. The next kernel read a reused pointer and the
  engine took SIGSEGV on the 8B Q8_0 and Q5_K_M.
- **1.3** Measured on the dev compiler (main at a7da6fc9f), jit-run:

  ```cajeta
  static int64 viaTernary(Holder c) {
      Cell x = c == null ? null : c.cell;   // plain =
      return x == null ? 0L : x.v;
  }
  ```

  | form | live count across the call | field read after a churn |
  |---|---|---|
  | `Cell x = c.cell;` | 5 -> 5 | 8247 |
  | `Cell x = c == null ? null : c.cell;` | 5 -> 4 | 107800 (the churn's allocation) |

- **1.4** CLAUDE.md §3 rule 7 gives `null` the title-taking spelling for a
  producer written in place. A conditional mixing a producer arm and a lending
  arm appears to take the producer's meaning for the whole expression. The
  `#=` form of a conditional was fixed on 2026-10-06 (field-store-ownership
  6.2.6, spec 4.2.11); the plain `=` local binding was not covered.
- **1.5** Non-goals: changing what `#=` from a conditional does.

## 2. Requirements

- **2.1** When a conditional's arms are `null` and a lending read (a field, a
  slot, a plain parameter, a borrowed local) and it is bound with `=`, the local
  records a borrow, and nothing is freed at its scope exit.
- **2.2** When every non-null arm is a producer (`heap T(...)`, a `#T` call), the
  binding takes the title as today.
- **2.3** When the arms mix a producer and a lending read, the compiler either
  decides per arm at run time, as `#=` from a conditional does, or rejects the
  binding with a diagnostic that names the arms. Never a silent title.
- **2.4** A test asserts each case FIRES and DOES NOT FIRE, with a liveCount and
  a read-after-churn canary, as OwnershipArrayCanaryTests does.
