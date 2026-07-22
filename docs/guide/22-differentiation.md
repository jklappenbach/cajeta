# 22 — Differentiation

Cajeta differentiates functions two ways, both applying the same rules:

- **Compiled** — `Grad(f)` transforms a statically-known function at compile
  time. Fast, fusable, batchable; the gradient code is ordinary IR.
- **Eager** — a runtime `Tape` records ops as they execute and replays them
  in reverse. Slower per op, but it differentiates *dynamic* control flow the
  compiled path cannot specialize.

Gradients are always **explicit return values**. There is no `requires_grad`
flag, no global `.grad` accumulator, and no `zero_grad` ceremony.

## Compiled: Grad, and the value_and_grad shape

`Grad(f)` for a scalar-valued `f` returns a function yielding
`GradResult<V, G>` — the forward value and the gradient together:

```cajeta
import cajeta.nucleo.transform.GradResult;

(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x);
GradResult<float32,float32> r = g(3.0f);
// r.value == 9.0, r.grads == 6.0
```

`Grad<N>(f)` differentiates with respect to argument `N`. Tensor inputs work
the same — reduce to a scalar first (`Tensor.sum` / `Tensor.mean`), and the
gradient comes back tensor-shaped.

The differentiable set: `+ - * /`, unary `-`, `Math.exp/log/sqrt`, and the
`Tensor` elementwise/reduction/matmul statics. Anything else is a named,
located compile error — a gradient is never silently wrong or silently zero.

`Grad` differentiates *through* same-class static helpers. Mark a helper
`@NoGrad` to make it a constant: its value flows, its gradient is statically
zero — the disciplined stop-gradient.

## Batching and fusion: Vmap and Jit

Transforms compose, innermost first:

```cajeta
float32[] xs = {1.0f, 2.0f, 3.0f};
(float32[]) -> #GradResult<float32,float32>[] g =
    Jit(Vmap(Grad((float32 x) -> x * x)));
GradResult<float32,float32>[] rs = g(xs);   // per-example grads {2,4,6}, fused
```

`Vmap` lifts a per-example function over a leading batch axis; `Jit` fuses a
body without changing its signature.

The annotation stack is sugar for exactly this nesting — nearest the
declaration applies first:

```cajeta
@Jit @Vmap @Grad
public static float32 sq(float32 x) { return x * x; }
// T.sq now has the transformed signature: batch in, per-example grads out.
```

## Fusing tensor expressions: Fuse

NumPy evaluates `(t*t + t) - t` one operator at a time, allocating a fresh
tensor per op. `Fuse` collapses an elementwise tensor expression into **one
loop** that allocates only the result — and `@Fuse` on a method is the
everyday shape:

```cajeta
@Fuse
public static Tensor<float32> activate(Tensor<float32> t) {
    return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);
}
// calls run one fused pass; only the result tensor is allocated
```

The explicit form yields a reusable compiled function; building it runs
nothing — the call is the force point:

```cajeta
(Tensor<float32>) -> #Tensor<float32> g =
    Fuse((Tensor<float32> t) -> Tensor.add<float32>(Tensor.mul<float32>(t, t), t));
Tensor<float32> r = g(x);
```

Reductions (`Tensor.sum` / `mean` / `std`) stage as their own pass and the
elementwise tail fuses against their result, so the standardize shape
`(t - mean(t)) / std(t)` runs as its reduction stages plus one fused loop.

Fusion meets differentiation through one seam: `Grad(Fuse(f))` — or the
`@Grad @Fuse` stack — differentiates the fused expression. Both transforms
walk the same expression DAG, so the gradient is identical to the unfused
form's, the backward is ordinary IR (`Jit` fuses it like any other function),
and an expression that is never differentiated carries no autograd machinery
at all. A body that cannot fuse is a named, located compile error
(`CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE`), never a silent eager fallback.

## Eager: the Tape

When control flow depends on runtime data, record it:

```cajeta
import cajeta.nucleo.autograd.Tape;
import cajeta.nucleo.autograd.Var;

Tape t = heap Tape();
Var x = t.var(2.0f);
Var y = x;
int32 i = 1;
while (i < n) {            // n arrives at runtime
    y = t.mul(y, x);
    i = i + 1;
}
t.backward(y);
float32 d = t.grad(x);     // n * x^(n-1)
```

One `backward` per tape (a second call throws); record a fresh tape per
step. `t.stopGrad(v)` is the tape's `@NoGrad`. The two drivers agree wherever
both apply — moving a function from tape to `Grad` changes performance, not
gradients.

## Choosing a driver

| | Compiled `Grad` | Eager `Tape` |
|---|---|---|
| Control flow | static (specialized) | dynamic (records the path taken) |
| Cost | fused, no per-op dispatch | per-op record + replay |
| Batching | `Vmap` | — |
| Use for | hot training steps, kernels | prototypes, dynamic models, debugging |

Reference: [Transforms](../stdlib/nucleo/Transforms.md),
[Fuse](../stdlib/nucleo/Fuse.md), [Tape](../stdlib/nucleo/Tape.md).
