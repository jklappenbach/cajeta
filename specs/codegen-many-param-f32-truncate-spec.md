# codegen-many-param-f32-truncate — defect

## 1. Definition

A function with roughly two dozen parameters miscompiles in the backend:

```
LLVM ERROR: Cannot select: t: f32 = truncate t2
  t2: i64,ch = CopyFromReg ..., Register:i64 %121
```

`truncate` is an integer narrowing; a float32 result can never legitimately be
its output. The selector is being handed a malformed node, which means the
argument's type was lost somewhere in lowering — a float32 array parameter is
being read back as if the incoming register held an i64.

- 1.1 **Observed** 2026-07-31 in `cajeta-xgboost`, adding two out-parameters to
  `dev.cajeta.xgboost.tree.TreeBuilder.grow`, taking it from 21 to 23
  parameters. At 23 the compile aborts with the error above; at 22 (the same
  work, both statistics packed into one `float64[]`) it compiles and the whole
  suite passes. So the trigger is the parameter count, not the added types.
- 1.2 **Asymmetry worth noting.** The library alone built clean at 23
  parameters; only the `--profile=test` compile (library + tests in one unit)
  aborted. Whatever spills the argument depends on the surrounding compilation,
  so the exact cliff is unlikely to be a fixed number.
- 1.3 **Severity.** The failure is loud (an abort, not bad code), so it cannot
  silently corrupt a build — but it is a hard wall: there is no diagnostic
  naming the cause, and the message points at LLVM rather than at the user's
  signature. A caller has no way to know the fix is "pass fewer arguments".

## 2. Acceptance

- 2.1 A function with 32 parameters, including several `float32[]`, compiles and
  runs correctly under both the default and `test` profiles.
- 2.2 If a hard limit is intentional, the compiler reports it as a cajeta
  diagnostic naming the function and the limit — never as an LLVM abort.
