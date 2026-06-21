---
id: math-tensor-Cast-roundToInt
applies-to: [cajeta/math/Cast.roundToInt]
title: Cast.roundToInt — float→int under an explicit RoundingMode
description: Convert a float64 to a generic integer dtype with a chosen rounding policy (TRUNCATE/FLOOR/CEIL/NEAREST_EVEN/NEAREST_AWAY); STOCHASTIC falls back to NEAREST_EVEN.
---

# Cast.roundToInt

Use this **only when the rounding policy matters**. For an ordinary float→int
conversion you want the bare language cast `(I) x`, which lowers to `fptosi`
(truncate-toward-zero) and is the default brick `Tensor.astype` uses.
`roundToInt` exists for the cases where you need FLOOR / CEIL / round-to-nearest
(ties-even or ties-away) instead of the hardware default.

## Signature & semantics

```cajeta
public static I roundToInt<I>(float64 x, RoundingMode mode)
```

- Generic over the **output integer dtype `I`** (e.g. `int32`, `int64`). You
  must supply it explicitly: `Cast.roundToInt<int64>(...)`.
- Returns the value of `x` rounded to a whole number under `mode`, as an `I`.
- Returns a **plain value type** — no ownership transfer, no `#`, nothing to
  free, nothing nullable. Same for the `float64`/`RoundingMode` arguments.

## Parameters

- `x : float64` — the value to round. **Input must be float64.** For a float32,
  widen losslessly at the call site first:
  `Cast.roundToInt<I>((float64) f, mode)`.
- `mode : RoundingMode` — the policy (see `cajeta/math/RoundingMode`):
  - `TRUNCATE` — toward zero.
  - `FLOOR` — toward −∞.
  - `CEIL` — toward +∞.
  - `NEAREST_EVEN` — ties to even (IEEE-754 default).
  - `NEAREST_AWAY` — ties away from zero.
  - `STOCHASTIC` — **falls back to `NEAREST_EVEN`** today (the counter-based RNG
    in `cajeta.math.random` is not landed). Don't rely on it being probabilistic.

## How it works (so the result is no surprise)

`x` is first rounded to an integral **float64**, then narrowed to `I` with `(I) r`
— exact, because `r` is already whole. The fallback branch is `NEAREST_EVEN`, so
any `mode` other than the five real cases (i.e. `STOCHASTIC`) rounds ties-even.

## What it does NOT do

- **No float→narrower-float rounding.** This method only targets integer `I`.
  Narrowing a float (e.g. float32 → bfloat16 / fp8) under a chosen mode is
  deferred; use the bare `(U) value` cast, which gives hardware nearest-even.
- **No real stochastic rounding** yet (see `STOCHASTIC` above).
- No overflow/saturation guard: if the rounded value doesn't fit `I`, the final
  `(I) r` narrowing behaves like the bare cast — pick an `I` wide enough.

## Example (mirrors test/math/CastTests.cpp)

```cajeta
package test;

import cajeta.math.Cast;
import cajeta.math.RoundingMode;

public final class D {
    public static int32 run() {
        int64 a = Cast.roundToInt<int64>(2.5, RoundingMode.NEAREST_EVEN);  // 2 (tie→even)
        int64 b = Cast.roundToInt<int64>(2.5, RoundingMode.NEAREST_AWAY);  // 3
        int64 c = Cast.roundToInt<int64>(-2.5, RoundingMode.FLOOR);        // -3
        int64 d = Cast.roundToInt<int64>(-2.5, RoundingMode.CEIL);         // -2
        int64 e = Cast.roundToInt<int64>(2.5, RoundingMode.STOCHASTIC);    // 2 (→ NEAREST_EVEN)
        int32 f = Cast.roundToInt<int32>(-1.5, RoundingMode.NEAREST_EVEN); // -2 (tie→even)
        return 1;
    }
}
```
