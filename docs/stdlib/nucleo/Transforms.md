# Transform intrinsics — Grad, Vmap, Jit

`cajeta.nucleo.transform` — compile-time function transforms. `Grad`, `Vmap`,
and `Jit` are **compiler intrinsics**, not methods: applied to a
statically-known function (a lambda literal, or a static method through the
annotation sugar), they produce a *new* function at compile time. There is no
runtime flag, no global gradient state, and no tape here — the compiled
transforms specialize and synthesize code. (The runtime, define-by-run
counterpart is [Tape](Tape.md).)

The importable types live beside the intrinsics: `GradResult<V, G>` is the
typed `{value, grads}` return bag, and `Transforms` documents the contract.

## Grad — differentiation

`Grad(f)` for a scalar-valued `f : (P...) -> S` yields
`(P...) -> GradResult<S, G>`: the forward value and the gradient with respect
to argument 0. `Grad<N>(f)` selects argument `N`.

```cajeta
package snip.grad;

import cajeta.nucleo.transform.GradResult;

public final class Demo {
    public static float32 run() {
        (float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x);
        GradResult<float32,float32> r = g(3.0f);
        return r.value * 100.0f + r.grads;   // 9*100 + 6 = 906
    }
}
```

Tensor arguments differentiate the same way — reduce to a scalar with
`Tensor.sum` or `Tensor.mean`:

```cajeta
(Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =
    Grad((Tensor<float32> t) -> Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t)));
GradResult<float32, Tensor<float32>> r = g(x);   // r.grads == 2*x
```

The differentiable primitive set: `+ - * /` and unary `-`, the scalar
intrinsics `Math.exp` / `Math.log` / `Math.sqrt`, and the tensor statics
`Tensor.{add,sub,mul,div,matmul,sum,mean,exp,log,sqrt,relu}`. Anything outside
the set is a **named, located compile error** — never a silently wrong
gradient. (`relu`'s gradient is the `Tensor.reluMask` step function, which is
itself non-differentiable — second-order `Grad` through `relu` fails loud.)

### GradAll — one backward, K parameters

`GradAll(f)` for a scalar-valued `f : (P...) -> S` yields
`(P...) -> GradResult<S, G[]>`: the forward value and the gradients with
respect to **every** parameter, as an array in argument order. `GradAll<K>(f)`
grades only the **leading K** arguments — the functional-step convention:
parameters first, data arguments after. One call returns all the grads; this
is the form a training step uses (grads for each weight in one invocation).

```cajeta
(Tensor<float32>, Tensor<float32>, Tensor<float32>)
        -> GradResult<float32, Tensor<float32>[]> step =
    GradAll<2>((Tensor<float32> w, Tensor<float32> b, Tensor<float32> x) ->
        Tensor.sum<float32,float32>(
            Tensor.add<float32>(Tensor.matmul<float32>(x, w), b)));
GradResult<float32, Tensor<float32>[]> r = step(w, b, x);
// r.grads[0] = dL/dw, r.grads[1] = dL/db — x is data, not graded
```

The K differentiated parameters must share one type (they fill one array);
mixing `Tensor<float32>` and `float32` in the leading K is a named compile
error, as are `K < 1` and `K > arity`. `Jit(GradAll<K>(f))` fuses; `@NoGrad`
helpers are constants, exactly as under `Grad`.

- Gradients are explicit return values. Calling `g` twice gives two
  independent results; there is no `.grad` accumulator and no `zero_grad`.
- `Grad` differentiates *through* same-class static helpers (they inline into
  the gradient); a helper marked `@NoGrad` is a constant instead — its value
  flows, its gradient is statically zero.
- Second order: `Grad(Grad(f))` for scalar `f` — the backward is ordinary
  differentiable source, so it differentiates again.

## Vmap — batching

`Vmap(f)` for `f : (T) -> R` yields `(T[]) -> #R[]`: `f` written for one
example runs over a leading batch axis, no hand-written loop. Composed with
`Grad`, it gives per-example gradients:

```cajeta
float32[] xs = {1.0f, 2.0f, 3.0f};
(float32[]) -> #GradResult<float32,float32>[] g =
    Vmap(Grad((float32 x) -> x * x));
GradResult<float32,float32>[] rs = g(xs);        // grads {2, 4, 6}
```

An op with no batching rule is a named compile error. Order matters and is
not normalized: `Grad(Vmap(f))` asks for the gradient of an array-valued
function, which the scalar-valued `Grad` rejects.

## Jit — fusion

`Jit(f)` has `f`'s exact signature and result; the body is fused (temporaries
eliminated) by the standard optimization pipeline. It composes over the other
transforms — `Jit(Vmap(Grad(f)))` fuses the batched, differentiated form into
one body:

```cajeta
(float32[]) -> #GradResult<float32,float32>[] g =
    Jit(Vmap(Grad((float32 x) -> x * x)));       // same signature, fused
```

## Annotation sugar

`@Grad` / `@Vmap` / `@Jit` on a **static single-return method** desugar to
the nested combinator form. The annotation nearest the declaration applies
first, so the stack reads like the nesting:

```cajeta
@Jit @Vmap @Grad
public static float32 sq(float32 x) { return x * x; }
// calls to sq are now Jit(Vmap(Grad(sq))):
GradResult<float32,float32>[] rs = T.sq(xs);     // per-example grads, fused
```

The sugar is only sugar — it runs the same driver as the explicit form, so
compositions and error messages are identical.

## Errors

Every misuse is named and located: a runtime-only function value is
`CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE`; an op outside the differentiable
or batchable set names the operator; a tensor rank mismatch names the ranks
and dtype (`Tensor.matmul<float32> rank mismatch: left operand is tensor,
right operand is scalar`). `Pmap` is recognized but reserved
(`CAJETA_ERROR_TRANSFORM_PMAP_UNIMPLEMENTED`).
