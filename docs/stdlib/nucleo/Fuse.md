# Fused tensor expressions — Fuse

`cajeta.nucleo.expr` — compile-time fusion of elementwise tensor expressions.
`Fuse` is a **compiler intrinsic**, not a method: applied to a
statically-known tensor expression (a lambda literal, or a static method
through `@Fuse`), it collapses N tensor operators into **one loop** at
compile time. NumPy allocates a fresh tensor per operator; a fused
expression reads each input element once, writes each output element once,
and allocates only the result.

## Fuse — one kernel, no temporaries

```cajeta
(Tensor<float32>) -> #Tensor<float32> g =
    Fuse((Tensor<float32> t) ->
        Tensor.sub<float32>(Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t));
Tensor<float32> r = g(x);    // ONE pass; allocates only r
```

Building the fused function runs nothing and allocates nothing; the **call
is the force point** — there is no `.eval`. The returned function is
reusable: compile once, call per batch.

The fusible elementwise set: `Tensor.{add,sub,mul,div}`, the scalar-broadcast
family `Tensor.{add,sub,mul,div}Scalar`, `negate`, and `exp` / `log` /
`sqrt`. A shared sub-expression is computed once.

## Reductions stage

A reduction (`Tensor.sum` / `mean` / `std`) bounds the fusion region: it
runs as its own pass, and the elementwise tail fuses against its scalar
result. The standardize shape is the headline:

```cajeta
(Tensor<float32>) -> #Tensor<float32> g =
    Fuse((Tensor<float32> t) ->
        Tensor.divScalar<float32>(
            Tensor.subScalar<float32>(t, Tensor.mean<float32,float32>(t)),
            Tensor.std<float32,float32>(t, 0)));
// mean and std stage once each; (t - mean) / std is one fused loop
```

A reduction as the **whole** expression fuses the elementwise body into the
accumulation itself — `Fuse(t -> Tensor.sum(Tensor.mul(t, t)))` is
scalar-valued and allocates no tensor at all.

## @Fuse — the everyday shape

```cajeta
@Fuse
public static Tensor<float32> activate(Tensor<float32> t) {
    return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);
}
```

Calls to the annotated method produce the fused result — same driver as the
explicit form, identical values, identical allocation counts.

## The autograd seam

`Fuse` and [`Grad`](Transforms.md) consume the **same** forward DAG, so they
compose without a translation layer:

```cajeta
(Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =
    Grad(Fuse((Tensor<float32> t) ->
        Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t))));
// identical gradient to the unfused form; @Grad @Fuse is the sugar spelling
```

The backward of a fused expression is ordinary IR — `Jit` fuses it like any
other function — and an expression that is never differentiated carries no
autograd machinery in its emitted code.

## Errors

A body that cannot fuse is the named, located compile error
`CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE` — never a silent eager fallback:
an op outside the elementwise set (`matmul`), or `std` as the whole fused
expression (usable inside an elementwise body). A runtime-only function
value is `CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE`, as for every transform
intrinsic.
