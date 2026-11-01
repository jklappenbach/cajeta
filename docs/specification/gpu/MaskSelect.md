# Per-lane conditionals: comparison masks and `select`

A `Vector<T,N>` / `Matrix<T,R,C>` holds many values in one register. You cannot
`if` a single lane, because a branch is whole-value, and on a GPU a
data-dependent branch serializes the warp. The mask→select pattern expresses
"do X to some lanes, Y to the rest" as data rather than control flow:

```
mask   = a < b              // <N x i1>: one bool per lane, no branch
result = mask.select(a, b)  // per lane: mask[i] ? a[i] : b[i]
```

Comparisons (`== != < <= > >=`) return the mask. `.all()` / `.any()` collapse a
mask to one `boolean` for ordinary control flow. This is the whole vocabulary.

## Vectors

Each thread carries a small vector, such as an RGBA pixel, a 3-D point, or a
feature tile. Per-element decisions are the norm, and they must stay
branchless.

**ReLU** keeps positives and zeroes the rest. It is the canonical activation:
```
Vector<float32,4> y = (x > 0.0f).select(x, zeros);
```
Without masks you scalarize into a per-lane branch that defeats vectorization:
```
for (uint32 i = 0; i < 4; i = i + 1) {        // serial; no SIMD / GPU vector op
    if (x[i] > 0.0f) { y[i] = x[i]; } else { y[i] = 0.0f; }
}
```

**`select` is the general form of `min`/`max`/`clamp`.** `a.min(b)` is exactly
this:
```
Vector<float32,4> m = (a < b).select(a, b);   // element-wise minimum
```

**Reduce to a branch.** `.all()` / `.any()` answer whole-vector questions:
```
if ((p >= lo).all() && (p <= hi).all()) { ... }   // point inside an AABB
if ((v != v).any()) { ... }                        // any NaN lane? (NaN != NaN)
```

## Matrices

The same pattern runs over R·C lanes. It wins wherever a per-element decision
must run over a whole matrix without a scalar loop.

**Prune small weights** (ML) by zeroing everything below a threshold:
```
Matrix<float32,4,4> pruned = (w > eps).select(w, zeros);
```
**Converged?** Whole-matrix equality is a reduction over the mask:
```
if ((m == prev).all()) { return; }   // fixed point reached
```

### Why not the alternatives?

| Alternative | What it forces |
|---|---|
| Nested `for r,c` + `if` per element | Scalar — no `<R*C x T>` vector op; on a GPU the per-element branch diverges the warp. |
| A parallel `boolean[R][C]` + a second apply pass | Two passes, extra memory, still scalar — you hand-roll what `select` already is. |
| Arithmetic mask `m * cond` (cond as `0.0`/`1.0`) | Only selects *against zero*; can't pick between two non-zero values, and you must hand-convert the bool to a float first. |

`(w > eps).select(w, zeros)` is one expression, flat `<R*C x T>` SSA,
register-resident and branchless. The same source runs on CPU, Vulkan, and
AMD.

---

**Rules.** Comparison operators on value types yield per-lane masks (typed
`Vector<boolean,N>` / `Matrix<boolean,R,C>`). Equality is not special, so
whole-object equality is `(a == b).all()`. Masks are register-only values.
`KernelBuffer<Vector<boolean,N>>` and bool-vector kernel args stay rejected,
because the `<N x i1>` memory layout is ABI-ambiguous. The pattern is runnable
end to end in `samples/Tour/xpu` (the `mask / select` section). See
`ValueTypeCatalog.md` for the `Vector` / `Matrix` surfaces.
