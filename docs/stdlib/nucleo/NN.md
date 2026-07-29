# Neural-net core — Module, Parameter, Linear, losses, mode

`cajeta.nucleo.nn` — the backend-neutral neural-net spine (lazy package;
pulls `cajeta.math`). v1 is **float32** and non-generic (the generic
`Module<T>` is blocked on recorded compiler gaps — see the plan ledger).

## Module and Parameter

A module is an ordinary class extending `Module`, owning `Parameter` fields
(optimizable tensors), sub-module fields (composition), and plain
`Tensor<float32>` fields (buffers — never optimizer-updated):

```cajeta
public class MLP extends Module {
    Linear fc1;
    Linear fc2;
    public MLP() {
        this.fc1 = heap Linear(784, 256, 42);
        this.fc2 = heap Linear(256, 10, 43);
    }
    public #Tensor<float32> forward(Tensor<float32> x) {
        return this.fc2.forward(Tensor.relu<float32>(this.fc1.forward(x)));
    }
}
```

- `parameters()` — every parameter, module tree walked by reflection in
  DECLARED order, parent-first, depth-first into sub-modules. No
  registration list; adding a field is enough. Elements are borrows.
- `parameterNames()` — the stable dotted paths (`fc1.weight`), same order,
  as an owning `ArrayList<String>` — the future serializer's key basis.
- `buffers()` — the plain tensor fields.
- There is **no** `requires_grad` flag: WHICH tensors train is decided by
  which collection the optimizer is constructed over (freeze = construct
  over `model.head.parameters()`).
- Invocation is `model.forward(x)` (call-operator sugar is syntax-sugar-spec
  work).

## The functional training step

`GradAll<K>` (see `Transforms.md`) grades the LEADING K args in one
backward. The bridge to modules is the ORDER CONTRACT: spell the parameters
as the leading args in `parameters()` order; `r.grads` then feeds
`opt.step` directly:

```cajeta
Parameter[] ps = net.parameters();          // [fc1.w, fc1.b, fc2.w, fc2.b]
(...) -> GradResult<float32, Tensor<float32>[]> step =
    GradAll<4>((Tensor<float32> w1, Tensor<float32> b1,
                Tensor<float32> w2, Tensor<float32> b2,
                Tensor<float32> x, Tensor<float32> t) ->
        Losses.mse(Tensor.add<float32>(Tensor.matmul<float32>(
            Tensor.relu<float32>(Tensor.add<float32>(
                Tensor.matmul<float32>(x, w1), b1)), w2), b2), t));
GradResult<float32, Tensor<float32>[]> r =
    step(ps[0].get(), ps[1].get(), ps[2].get(), ps[3].get(), x, t);
opt.step(r.grads);
```

## Losses

Free differentiable functions (no base class, no registration): `Losses.mse`
(mean), `Losses.mseSum`, `Losses.mseNone` (elementwise) — reduction selects
the FUNCTION so the return type reflects it. `Grad` inlines qualified static
calls, so `Losses.mse(...)` differentiates through; a custom loss is any
static single-return function over differentiable primitives.
**Deferred:** `crossEntropy` (needs axis-wise reductions).

## Train / eval mode

`Modes` scopes the mode per FIBER — no global flag, reentrant by
construction; the default (no scope) is EVAL, so inference is safe without
ceremony:

```cajeta
Modes.train(() -> {
    GradResult<float32, Tensor<float32>[]> r = step(...);
    opt.step(r.grads);
});
float32 val = Losses.mse(net.forward(vx), vy);   // eval by default
```

`Dropout(p, seed)` is the mode-sensitive proof layer: inverted dropout in
train (zeroed with probability p, survivors scaled 1/(1-p), masks drawn
from its own seeded Philox `Generator`), identity in eval.
**Deferred:** BatchNorm (running-stat buffers + axis reductions).

## Rank accessors (a recurring trap)

`Tensor.get1/set1` index by `strideDims[0]` — **rank-1 only**. A `[B,O]`
tensor uses `get2/set2` (or `getAt/setAt` with a multi-index). Misuse on a
rank-2 tensor bounds-traps (or silently reads the wrong element when the
scaled index stays in range).
