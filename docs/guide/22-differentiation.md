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
[Tape](../stdlib/nucleo/Tape.md).
