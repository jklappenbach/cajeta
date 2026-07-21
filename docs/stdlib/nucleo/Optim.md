# Optimizers and LR schedules

`cajeta.nucleo.optim` — the explicit-grads update protocol (lazy package).

## The protocol

`Optimizer` is one method deep: `step(grads)` consumes an explicit,
positional gradient bag (`grads[i]` belongs to `params[i]` — the same order
`parameters()` reports and `GradAll`'s leading args follow) and writes the
parameters in place. There is **no** `.grad` accumulator, no `zeroGrad`, no
global state; grads live for the call. Count/rank/shape mismatch throws
`OptimizerException` BEFORE any write — a failed step never partially
updates. `getLr`/`setLr` are the scheduler seam.

Any custom rule — including a non-backprop one — implements the same
interface; the built-ins are not special.

## Built-ins

- `SGD(params, lr, momentum)` — `v = momentum·v + g; p -= lr·v`.
- `Adam(params, lr)` — bias-corrected adaptive moments (β1 .9, β2 .999,
  ε 1e-8); the correction powers are explicit optimizer state.
- `AdamW(params, lr, weightDecay)` — DECOUPLED decay
  (`p -= lr·(m̂/(√v̂+ε) + wd·p)`); the moments never see the decay term,
  which is exactly how it differs from Adam-plus-L2.

State (velocity, moments) is optimizer-owned, positional, lazily
initialized on the first `step`. The optimizer borrows the parameters; the
module's lifetime governs the tensors.

## LR schedules

The core form is a PURE function `lr(step)` — `Schedules.stepLr`,
`.exponentialLr`, `.cosineLr`, `.warmupCosineLr` — the training loop reads
a value for an explicit step and applies it. Composition is function
composition. `LrSchedule` is the thin mutating wrapper for the familiar
`sched.step()` shape: it owns its step count (never a global) and writes
through `setLr`:

```cajeta
Optimizer opt = heap AdamW(net.parameters(), 3e-4f, 0.01f);
LrSchedule sched = LrSchedule.warmupCosine(opt, 3e-4f, 100, 10000);
// per iteration: sched.step(); ... opt.step(r.grads);
```

**Deferred** (plan ledger): gradient-transform wrappers (clipping,
micro-batch accumulation), serialization of optimizer state.
