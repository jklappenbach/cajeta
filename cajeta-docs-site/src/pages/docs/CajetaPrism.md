---
title: 'cajeta.prism'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Stdlib'
description: 'A specification for cajeta.prism, a deep-learning framework designed from first principles for cajeta — leveraging the type system, ownership model, fiber runtime, and cajeta.ml numerical foundation. ...'
---

A specification for `cajeta.prism`, a deep-learning framework
designed from first principles for cajeta — leveraging the type
system, ownership model, fiber runtime, and `cajeta.ml` numerical
foundation. Not a port of any existing framework. The goal is to
take everything researchers learned from PyTorch / JAX / TensorFlow /
Flax / Burn / MLX and ask: what would the framework look like if
designed today, in a language with cajeta's properties, without
backward compatibility constraints?

For the PyTorch migration target, see `CajetaTorch.md`. cajeta.prism
and cajeta.torch coexist; they have different audiences and different
goals. Most users will pick one or the other for a given project.

Implementation lands incrementally as `.cajeta` files under
`./libraries/cajeta.prism/src/`. Ships as its own package.

## Why a separate library from cajeta.torch

`cajeta.torch` exists to migrate PyTorch code with minimal edits.
That mandate — API faithfulness — locks in decisions we'd otherwise
revisit: `nn.Module` carries hidden mutable state because Python's
object model rewards it; tensors track gradients implicitly via a
graph built op-by-op because Python can't catch grad-vs-no-grad at
compile time; randomness lives in global RNG state because writing
out the seed everywhere is verbose in Python.

Cajeta lets us do better. Shape + dtype + device + grad-tracking
all live in the type system. Modules are immutable structs of
parameters; training updates them by producing a new struct, with
the compiler eliding the copy. Randomness is a value threaded
explicitly through the call graph, JAX-style, so the compiler (and
the human reader) can see exactly where stochasticity enters.
Transformations like `grad`, `jit`, `vmap`, `pmap` are language-
native: they take pure functions and return pure functions, and
the compiler can reason about them.

cajeta.prism is for users who:
- Are starting fresh, not porting a PyTorch repo.
- Want compile-time guarantees on shape, dtype, gradient flow.
- Want functional transformations as a primary tool, not bolt-on.
- Care about reproducibility being built in, not bolted on.
- Want a framework that takes advantage of cajeta's memory model and
  fiber runtime instead of fighting against them.

## Goals

- **Shapes in the type system.** A `Tensor[(B, T, D), float32, CPU]`
  is a different type from `Tensor[(B, T, V), float32, CPU]`. Matmul
  signatures statically reject mismatched inner dimensions.
  Reshape, transpose, broadcast are checked at compile time when the
  shapes are statically known; gradual fallback to runtime checks
  when they aren't.
- **Compile-time autograd.** `Tensor[..., grad]` and `Tensor[..., nograd]`
  are different types. `grad(fn)` produces a new pure function; no
  graph build at runtime, no Python-style "variables that secretly
  record." When the function is sufficiently typed, the compiler
  emits the backward pass at compile time.
- **Functional transformations are the primitives.**
  `jit`, `grad`, `vmap`, `pmap`, `scan`, `checkpoint` take pure
  functions and return pure functions. Composable in any order.
  No `Module.train()` flag, no `set_grad_enabled()`, no implicit
  mode — the function you call IS the function that runs.
- **Modules are immutable values.** A `Module` is a struct of
  parameters; `apply(module, input) -> output` is a pure function;
  optimizer steps return a new `(module, optimizer_state)` pair.
  Looks like Equinox / Flax. Mutation in cajeta is opt-in and
  visible.
- **Explicit randomness.** No global RNG. `RngKey` values thread
  through every call that consumes randomness. `split(key, n)` makes
  N independent keys. Two runs with the same key are bit-identical;
  reproducibility is the default.
- **First-class mixed precision.** dtype is a type parameter, not a
  runtime tag. `Tensor[..., float8e4m3]` and `Tensor[..., float32]`
  participate in the same arithmetic via cajeta.math's casting; the
  rounding mode is a parameter on the cast, not autocast magic.
- **Owned tensors, borrowed gradients.** Cajeta's memory model
  applies. A tensor has a single owner; gradients borrow the weight
  buffer rather than holding a second reference. No reference
  cycles, no leak under normal use, no .detach() to free memory.
- **Fiber-native data pipeline.** Loaders run as fiber pools with
  bounded prefetch buffers, integrated with cajeta.thread's
  scheduler. No process-fork dance, no GIL workarounds.
- **Distributed from day one.** Multi-device sharding via `pmap`
  and `pjit`; multi-host coordination via the same primitives.
  Designed in, not retrofitted.
- **Differentiable everything sensible.** Discrete ops (sampling,
  argmax, top-k) carry well-defined surrogate gradients (Gumbel-
  Softmax, straight-through, score-function) selectable per call,
  not silently zero.

## Non-goals (v1)

- **PyTorch API parity.** That's cajeta.torch's job. Migrating
  PyTorch code to cajeta.prism is a rewrite, not a port.
- **Python interop / dynamic shape inference at runtime.** Shapes
  that are unknown at compile time are supported but reported
  warning-level (compiler can't optimize as hard). No "shape comes
  from `int(input())`" magic.
- **CUDA / accelerator backend.** CPU only at v1. The
  `Device` type parameter exists; only `CPU` resolves. Backend
  abstraction designed in so accelerators slot in cleanly.
- **Pre-trained model zoo.** No bundled weights. Model definitions
  may live in a sibling package (`cajeta.prism.models`); weights are
  loaded from external sources via the checkpoint format.
- **Visual / dashboard tooling.** No built-in TensorBoard
  equivalent. Logging is structured (json-lines); a separate
  visualization library can consume it.
- **Symbolic / TF-style graph definition.** Forward pass is written
  as ordinary cajeta code; transformations like `jit` operate on it
  AOT.

## Package layout

```
cajeta.prism.tensor       — Typed Tensor, Shape, DType, Device,
                            factory functions, ops, broadcasting,
                            indexing, shape ops
cajeta.prism.autograd     — grad, value_and_grad, jacobian, hessian,
                            jvp, vjp, gradient surrogates
cajeta.prism.transform    — jit, vmap, pmap, scan, checkpoint, remat
cajeta.prism.random       — RngKey, split, normal, uniform,
                            categorical, ... (PRNG-key-threaded)
cajeta.prism.module       — Module trait, Parameter wrapper,
                            tree_map / tree_flatten over module
                            parameter trees, parameter init
cajeta.prism.layer        — Linear, Conv*, BN, LN, RMSNorm,
                            Dropout, Embedding, Attention,
                            TransformerBlock — immutable structs
cajeta.prism.act          — relu, gelu, silu, mish, softmax, ...
                            (pure functions; no Module wrapper)
cajeta.prism.loss         — mse, cross_entropy, kl_div, ... (pure
                            functions, not Module instances)
cajeta.prism.optim        — optimizer state as data; init / step
                            return new state; SGD, Adam, AdamW,
                            Lion, Adafactor, ...
cajeta.prism.schedule     — LR schedules as pure functions of step ->
                            lr; cosine, warmup-then-decay, polynomial
cajeta.prism.data         — Dataset, Loader (fiber pool, bounded
                            prefetch), Sampler, collate
cajeta.prism.checkpoint   — Pure binary save / load. No pickle.
                            Versioned format; safetensors-compatible
                            tensor encoding.
cajeta.prism.distribute   — Mesh, ShardingSpec, pjit, pmap,
                            collectives (all_reduce, all_gather,
                            reduce_scatter, broadcast)
cajeta.prism.distributions — Normal, Categorical, Bernoulli, ...,
                             with reparameterizable / score-function
                             gradient choice
cajeta.prism.metric       — Pure-function metrics (accuracy,
                            precision, recall, F1, AUC, ...) plus
                            a stateful Aggregator wrapper for
                            running totals across batches
cajeta.prism.text         — Tokenizer trait + BPE / Unigram /
                            WordPiece implementations; the bare
                            minimum to build language-model pipelines
cajeta.prism.vision       — Image loaders (jpeg / png), augmentation
                            ops (resize, crop, flip, color jitter),
                            ToTensor / Normalize
cajeta.prism.profile      — Op-level timing, memory tracing, flop
                            counting; outputs structured json-lines
```

Deferred to follow-ups:
```
cajeta.prism.models           — Curated model definitions
cajeta.prism.cuda             — CUDA backend
cajeta.prism.metal            — Metal backend
cajeta.prism.serve            — Inference serving
cajeta.prism.export           — ONNX / SavedModel export
cajeta.prism.compile          — XLA-equivalent graph compiler
```

---

## cajeta.prism.tensor

The foundational type. Shape, dtype, device, and grad-tracking all
live in the type. The elements are stored in a `cajeta.ml.tensor.
Tensor` underneath; this layer adds the type-level metadata.

```cajeta
public final class Tensor<S extends Shape, T extends DType, D extends Device, G extends GradMode> {
    // The wrapped data. Shape / dtype / device are reflected at
    // compile time via S, T, D; the runtime data is a cajeta.ml
    // tensor specialized over T's element type.
    private cajeta.ml.tensor.Tensor<T::Element> data;

    // ----- factories (return values, not heap allocations) -----
    public static <S, T, D> Tensor<S, T, D, NoGrad> zeros();
    public static <S, T, D> Tensor<S, T, D, NoGrad> ones();
    public static <S, T, D> Tensor<S, T, D, NoGrad> full(T::Element value);
    public static <S, T, D> Tensor<S, T, D, NoGrad> arange(T::Element start, T::Element stop, T::Element step);
    public static <S, T, D> Tensor<S, T, D, NoGrad> linspace(T::Element start, T::Element stop, int64 steps);
    public static <S, T, D> Tensor<S, T, D, NoGrad> normal(RngKey key);
    public static <S, T, D> Tensor<S, T, D, NoGrad> uniform(RngKey key, T::Element low, T::Element high);

    // ----- shape inspection (compile-time when S is concrete) -----
    public S      shape();
    public int64  numel();
    public int8   rank();

    // ----- arithmetic (compile-checked broadcast) -----
    public <S2> Tensor<Broadcast<S, S2>, T, D, G | gradOf(rhs)> operator+(Tensor<S2, T, D, ?> rhs);
    public <S2> Tensor<Broadcast<S, S2>, T, D, G | gradOf(rhs)> operator-(Tensor<S2, T, D, ?> rhs);
    public <S2> Tensor<Broadcast<S, S2>, T, D, G | gradOf(rhs)> operator*(Tensor<S2, T, D, ?> rhs);
    public <S2> Tensor<Broadcast<S, S2>, T, D, G | gradOf(rhs)> operator/(Tensor<S2, T, D, ?> rhs);

    // matmul: shape[m,k] @ shape[k,n] -> shape[m,n] — k must match
    public <K, N> Tensor<MatMulShape<S, (K, N)>, T, D, G | gradOf(rhs)>
        matmul(Tensor<(K, N), T, D, ?> rhs);

    // ----- shape ops (compile-time shape arithmetic) -----
    public <S2> Tensor<S2, T, D, G> reshape();              // S2 must be totalSize-equal to S
    public <perm> Tensor<Permute<S, perm>, T, D, G> transpose();
    public Tensor<Squeeze<S>, T, D, G> squeeze(int8 axis);
    public Tensor<Unsqueeze<S>, T, D, G> unsqueeze(int8 axis);

    // ----- reductions (statically remove the reduced axis) -----
    public <axis> Tensor<RemoveAxis<S, axis>, T, D, G> sum();
    public <axis> Tensor<RemoveAxis<S, axis>, T, D, G> mean();
    public <axis> Tensor<RemoveAxis<S, axis>, T, D, G> max();

    // ----- precision casting (RoundingMode-controlled, see cajeta.math) -----
    public <T2 extends DType> Tensor<S, T2, D, G> cast(RoundingMode mode = RoundingMode.NEAREST_EVEN);

    // ----- device transfer -----
    public <D2 extends Device> Tensor<S, T, D2, G> to(D2 device);

    // ----- grad mode flip -----
    public Tensor<S, T, D, RequiresGrad> requireGrad();
    public Tensor<S, T, D, NoGrad>       stopGradient();
}

public abstract class Shape { /* type-level tuple of dims */ }
public final class S0 extends Shape {}
public final class S1<A extends Dim> extends Shape {}
public final class S2<A extends Dim, B extends Dim> extends Shape {}
public final class S3<A extends Dim, B extends Dim, C extends Dim> extends Shape {}
public final class S4<A extends Dim, B extends Dim, C extends Dim, D extends Dim> extends Shape {}

public abstract class Dim { /* concrete: D1, D2, D3, ..., or DUnknown for runtime-determined */ }
public abstract class DType { /* Float32, Float16, BFloat16, Float8E4M3, Int32, Int64, Bool, ... */ }
public abstract class Device { /* CPU, CUDA<0>, CUDA<1>, ..., Mesh */ }
public abstract class GradMode { /* RequiresGrad | NoGrad */ }
```

Compile-time shape arithmetic (`Broadcast`, `MatMulShape`, `Permute`,
`RemoveAxis`, `Squeeze`, `Unsqueeze`) is built from cajeta's
type-parameter machinery. When all inputs have concrete shapes, the
output shape is concrete; when any input is `DUnknown`, the result
falls back to runtime checks with a compile-time warning.

Dynamic shapes (read from a file, computed at runtime) are
supported via `Tensor<SUnknown, ...>` — works, but loses the
optimization opportunities of statically-known shapes.

---

## cajeta.prism.autograd

Pure-function-shaped automatic differentiation. `grad(fn)` returns a
new function that computes the gradient of `fn` at any input. No
graph build, no `.backward()`, no implicit state.

```cajeta
// Take a function (X -> scalar) and return a function (X -> X)
// that computes the gradient at the input.
public static <X, R extends Tensor<S0, ?, ?, ?>>
    Function<X, X> grad(Function<X, R> fn);

// Same but returns (value, gradient) so you don't recompute the
// forward pass.
public static <X, R extends Tensor<S0, ?, ?, ?>>
    Function<X, (R, X)> valueAndGrad(Function<X, R> fn);

// Higher-order: jacobian of a vector-valued function.
public static <X, R> Function<X, Jacobian<X, R>> jacobian(Function<X, R> fn);
public static <X, R> Function<X, Hessian<X, R>>  hessian(Function<X, R> fn);

// Forward-mode (jvp) and reverse-mode (vjp) primitives.
public static <X, R> Function<(X, X), (R, R)>             jvp(Function<X, R> fn);
public static <X, R> Function<X, (R, Function<R, X>)>     vjp(Function<X, R> fn);

// Surrogate-gradient registration for non-differentiable ops.
public enum Surrogate {
    STRAIGHT_THROUGH,        // pass gradient unchanged through e.g. argmax
    GUMBEL_SOFTMAX,          // for categorical sampling
    SCORE_FUNCTION,          // REINFORCE-style
    REPARAMETERIZE,          // for distributions that support it
    ZERO,                    // explicit "no gradient through here"
}
```

Implementation: when `fn` is composed of registered primitives (the
ops defined in `cajeta.prism.tensor`), the compiler generates the
backward pass at compile time. User code never sees a graph object.
Higher-order derivatives compose naturally — `grad(grad(fn))` is a
function, not a special case.

---

## cajeta.prism.transform

Functional transformations. Each takes a pure function and returns a
pure function. Composable in any order: `jit(grad(vmap(fn)))` is a
function from input to gradient, vectorized over the leading axis,
JIT-compiled.

```cajeta
// Compile a function. First call traces + lowers; subsequent calls
// dispatch to the compiled artifact.
public static <X, Y> Function<X, Y> jit(Function<X, Y> fn);

// Vectorize over a specified input axis. fn that takes (T, U) and
// returns V, vmapped over axis 0 of T, becomes (T-with-leading-axis,
// U) -> V-with-leading-axis.
public static <X, Y> Function<Vmapped<X>, Vmapped<Y>> vmap(Function<X, Y> fn, int8 inAxis = 0, int8 outAxis = 0);

// Parallelize across devices (mesh axes). Same shape transformation
// as vmap but the leading axis is distributed across the mesh.
public static <X, Y> Function<Pmapped<X>, Pmapped<Y>>
    pmap(Function<X, Y> fn, Mesh mesh, ShardingSpec spec);

// Loop over a leading axis with a carry: fn(carry, x) -> (carry, y).
// Lowers to an efficient compiled loop, not Python-style step-by-step.
public static <C, X, Y> Function<(C, X), (C, Y)>
    scan(Function<(C, X), (C, Y)> fn);

// Recompute a function's intermediate activations during backward
// instead of saving them — trade compute for memory.
public static <X, Y> Function<X, Y> checkpoint(Function<X, Y> fn);
```

These are the load-bearing primitives. The framework's expressive
power comes from composing them: `vmap` + `grad` for per-example
gradients, `pmap` + `grad` for distributed training, `scan` for
recurrent computation without Python-level loops, `checkpoint` for
deep models that don't fit activations in memory.

---

## cajeta.prism.random

JAX-style explicit RNG keys. No global state. Two runs with the
same `RngKey` produce bit-identical results.

```cajeta
public final class RngKey {
    // Construction from a seed integer.
    public static RngKey of(int64 seed);

    // Split one key into N independent keys. The mathematical
    // construction guarantees the children are uncorrelated with
    // the parent and with each other; this is the only way to get
    // multiple independent RNG streams.
    public RngKey[] split(int8 n);
    public (RngKey, RngKey) split2();   // common case
}

// Distribution sampling: every call takes a RngKey and returns a
// tensor. Same key -> same output, every time, on every device.
public static <S, T, D>
    Tensor<S, T, D, NoGrad> normal(RngKey key, T::Element mean = 0, T::Element std = 1);

public static <S, T, D>
    Tensor<S, T, D, NoGrad> uniform(RngKey key, T::Element low = 0, T::Element high = 1);

public static <S, D>
    Tensor<S, Int64, D, NoGrad> categorical(RngKey key, Tensor<?, ?, D, ?> logits);

public static <S, D>
    Tensor<S, Bool, D, NoGrad> bernoulli(RngKey key, Tensor<?, Float32, D, ?> probs);
```

Forcing keys to be threaded through every random operation makes
reproducibility the default. The cost is a few extra arguments;
the benefit is that any two researchers running the same script
with the same seed get the same numbers, period.

---

## cajeta.prism.module

Modules are immutable structs of parameters. There is no
`.train()` flag, no `.eval()` flag, no hidden state. A `Module`'s
forward pass is `apply(module, input) -> output`, a pure function
of the module and the input. Optimizer steps return a new module.

```cajeta
public interface Module<X, Y> {
    public Y apply(X input);

    // Compiler-synthesized via @Parameter field annotations: walks
    // the module struct, returning a flat list of parameter tensors.
    // Used by optimizers and serialization.
    public Tensor<?, ?, ?, ?>[] parameters();

    // Same walk, but rebuilds the module with the supplied
    // parameters. Used by optimizer.step to produce the updated
    // module. The compiler synthesizes an efficient field-wise
    // copy.
    public Self withParameters(Tensor<?, ?, ?, ?>[] newParams);

    // Random init given an RngKey. Compiler-synthesized for
    // structurally-defined modules; user-provided for ones that
    // need custom init.
    public static <Self> Self init(RngKey key, ModuleSpec<Self> spec);
}

// Field annotation: marks a struct field as a learnable parameter.
// The synthesizer for parameters() / withParameters() walks fields
// with this annotation, recursing into sub-Modules.
public @interface Parameter {
    boolean trainable() default true;
}
```

Stateful behaviors that PyTorch buries in the module (BN running
stats, Dropout's RNG, training-mode toggle) become explicit
parameters / inputs:

- **BatchNorm running stats** — second return value of `apply`,
  threaded back as input on the next call. Functional, not
  hidden state.
- **Dropout** — takes an `RngKey` as an input, returns the masked
  tensor. No global "training mode" check; if the caller wants no
  dropout, they pass `NoMaskKey` (or call the no-dropout variant).
- **Training vs eval mode** — the function the user calls IS the
  mode. `model.applyTrain(input, key)` vs `model.applyEval(input)`.
  No flag to forget to set.

---

## cajeta.prism.layer

Layers are immutable structs of parameters with a synthesized
`apply` method. Transformer block as the canonical example:

```cajeta
public final struct Linear<In extends Dim, Out extends Dim, T extends DType>
        implements Module<Tensor<S2<?, In>, T, ?, ?>, Tensor<S2<?, Out>, T, ?, ?>> {
    @Parameter public Tensor<S2<Out, In>, T, ?, NoGrad> weight;
    @Parameter public Tensor<S1<Out>,    T, ?, NoGrad> bias;

    public Tensor<S2<?, Out>, T, ?, ?> apply(Tensor<S2<?, In>, T, ?, ?> x) {
        return x.matmul(weight.transpose()) + bias;
    }

    public static <In, Out, T> Linear<In, Out, T> init(RngKey key) {
        var (k1, k2) = key.split2();
        // Kaiming initialization, computed per dtype's epsilon
        return Linear {
            weight: Tensor.normal<S2<Out, In>, T, ?>(k1)
                     * (2.0 / In.value).sqrt(),
            bias:   Tensor.zeros<S1<Out>, T, ?>(),
        };
    }
}

public final struct LayerNorm<D extends Dim, T extends DType>
        implements Module<Tensor<?, T, ?, ?>, Tensor<?, T, ?, ?>> {
    @Parameter public Tensor<S1<D>, T, ?, NoGrad> scale;
    @Parameter public Tensor<S1<D>, T, ?, NoGrad> bias;
    public T::Element epsilon;

    public Tensor<?, T, ?, ?> apply(Tensor<?, T, ?, ?> x) {
        var mean = x.mean<lastAxis>();
        var variance = (x - mean).square().mean<lastAxis>();
        var normalized = (x - mean) / (variance + epsilon).sqrt();
        return normalized * scale + bias;
    }
}

public final struct MultiHeadAttention<D extends Dim, H extends Dim, T extends DType>
        implements Module<...> {
    @Parameter public Linear<D, D, T> qProj;
    @Parameter public Linear<D, D, T> kProj;
    @Parameter public Linear<D, D, T> vProj;
    @Parameter public Linear<D, D, T> outProj;

    public Tensor<...> apply(Tensor<S3<B, T_, D>, T, Dev, G> x) {
        var q = qProj.apply(x).reshape<S4<B, T_, H, D / H>>();
        var k = kProj.apply(x).reshape<S4<B, T_, H, D / H>>();
        var v = vProj.apply(x).reshape<S4<B, T_, H, D / H>>();
        var scores = q.matmul(k.transpose<-1, -2>()) / (D / H).sqrt();
        var attn = scores.softmax<lastAxis>();
        var ctx = attn.matmul(v).reshape<S3<B, T_, D>>();
        return outProj.apply(ctx);
    }
}
```

Layer set for v1: `Linear`, `Conv1d/2d/3d`, `BatchNorm*d`, `LayerNorm`,
`RMSNorm`, `GroupNorm`, `Dropout`, `Embedding`, `MultiHeadAttention`,
`TransformerBlock`, `RotaryEmbedding`, `MoE` (mixture of experts).

---

## cajeta.prism.optim

Optimizer state is data; `init` and `step` are pure functions; no
optimizer object holds parameters in a hidden ref-keep cycle.

```cajeta
public interface Optimizer<S, P> {
    public S          init(P params);
    public (S, P)     step(S state, P params, P grads);
}

public struct AdamState {
    public int64 step;
    public Tensor<?, Float32, ?, NoGrad>[] m;     // first moment per param
    public Tensor<?, Float32, ?, NoGrad>[] v;     // second moment per param
}

public final class Adam implements Optimizer<AdamState, Tensor<?, ?, ?, ?>[]> {
    public float64 lr;
    public float64 beta1;
    public float64 beta2;
    public float64 epsilon;
    public float64 weightDecay;

    public Adam(float64 lr = 1e-3, float64 beta1 = 0.9, float64 beta2 = 0.999,
                float64 epsilon = 1e-8, float64 weightDecay = 0.0);

    public AdamState init(Tensor<?, ?, ?, ?>[] params);

    public (AdamState, Tensor<?, ?, ?, ?>[]) step(
        AdamState state,
        Tensor<?, ?, ?, ?>[] params,
        Tensor<?, ?, ?, ?>[] grads);
}
```

Training loop shape:

```cajeta
var rng = RngKey.of(42);
var (initKey, dropKey) = rng.split2();

var model = Transformer.init(initKey, modelSpec);
var optimizer = new Adam(lr: 3e-4);
var optState = optimizer.init(model.parameters());

for (int64 stepIdx = 0; stepIdx < numSteps; stepIdx++) {
    var (batchKey, dropKey2) = dropKey.split2();
    dropKey = dropKey2;
    var batch = loader.next();

    var (loss, grads) = valueAndGrad((m) -> {
        var logits = m.applyTrain(batch.input, batchKey);
        return crossEntropy(logits, batch.target);
    })(model);

    var (newOptState, newParams) = optimizer.step(
        optState, model.parameters(), grads);
    model = model.withParameters(newParams);
    optState = newOptState;
}
```

Optimizer set for v1: `SGD` (with optional momentum / Nesterov),
`Adam`, `AdamW`, `RMSProp`, `Adafactor`, `Lion`, `Sophia`. Adding
new optimizers is implementing the two-method interface; no hidden
contract.

---

## cajeta.prism.distribute

Designed in from day one. A `Mesh` is a typed grid of devices; a
`ShardingSpec` says how a tensor's axes map to mesh axes;
`pjit` / `pmap` runs a function across the mesh.

```cajeta
public final class Mesh<Axes extends MeshAxes> {
    public static <Axes> Mesh<Axes> of(Device[] devices, Axes spec);
}

public final class ShardingSpec<S, MeshAxes> {
    // Per-tensor-axis specifies which mesh axis (or REPLICATE) it shards along.
}

// pjit: like jit, but the resulting function is sharded across the
// mesh according to the input/output sharding specs.
public static <X, Y, M extends MeshAxes>
    Function<X, Y> pjit(
        Function<X, Y> fn,
        Mesh<M> mesh,
        ShardingSpec<X, M> inSpec,
        ShardingSpec<Y, M> outSpec);

// Collectives (used inside pjit'd functions).
public static <S, T, D, G> Tensor<S, T, D, G> allReduce(Tensor<S, T, D, G> x, ReduceOp op = SUM);
public static <S, T, D, G> Tensor<...>        allGather(Tensor<S, T, D, G> x, int8 axis);
public static <S, T, D, G> Tensor<...>        reduceScatter(Tensor<S, T, D, G> x, int8 axis, ReduceOp op = SUM);
public static <S, T, D, G> Tensor<S, T, D, G> broadcast(Tensor<S, T, D, G> x, int8 srcDevice);
```

Multi-host coordination uses cajeta.thread fibers + cajeta.io.net (when
it lands) for control-plane communication; tensor data moves over
the highest-bandwidth fabric available (NVLink / Infiniband / ethernet
fall-through).

---

## cajeta.prism.checkpoint

Pure binary format. No pickle. No language-specific code paths.
Versioned header + tensor records.

```cajeta
public final class Checkpoint<M> {
    public M       model;
    public int64   step;
    public Map<String, Tensor<?, ?, ?, ?>> auxiliary;   // optimizer state, etc.
    public Map<String, String>             metadata;    // commit hash, config, ...

    public void saveTo(Path path);
    public static <M> Checkpoint<M> loadFrom(Path path, ModuleSpec<M> spec);
}
```

Format:
```
+---------------------------------+
| magic (8 bytes): "CAJEPRSM"     |
| format version (4 bytes)        |
| metadata length (4 bytes)       |
| metadata (json, UTF-8)          |
| tensor count (4 bytes)          |
| tensor index (per-entry):       |
|   name (length-prefixed)        |
|   dtype (4 bytes)               |
|   shape (length-prefixed dims)  |
|   data offset (8 bytes)         |
|   data length (8 bytes)         |
| ... (more entries)              |
| tensor data (concatenated, raw  |
|   little-endian per dtype)      |
+---------------------------------+
```

Compatible with safetensors at the tensor-encoding layer (same byte
representation per dtype) so `cajeta.prism.checkpoint` and
`safetensors` files round-trip.

---

## Implementation sequence

A reasonable order, given dependencies:

1. **cajeta.prism.tensor** with statically-known shapes only first.
   Type-level shape arithmetic, broadcast / matmul / reshape /
   reductions. Wraps cajeta.ml.tensor.Tensor for storage.
2. **cajeta.prism.random.** RngKey + split + the standard
   distributions. Self-contained, low dependency.
3. **cajeta.prism.autograd: grad / value_and_grad** for functions
   over the tensor type. Compile-time backward generation. Validates
   the type-level grad-mode design end-to-end.
4. **cajeta.prism.module + a small layer set** (`Linear`,
   `LayerNorm`, `Dropout`). Parameter-walk synthesis. Training loop
   shape works.
5. **cajeta.prism.optim: SGD + Adam.** Pure-function step. Trains
   an MLP end-to-end on toy data.
6. **cajeta.prism.transform: jit.** Function compilation. The trace
   sees a typed function and lowers it to optimized kernels. Big
   payoff: no per-op Python-level overhead, even before any GPU
   support.
7. **cajeta.prism.transform: vmap + scan.** Per-example gradients,
   compiled loops. Many existing models become much terser.
8. **cajeta.prism.layer: convolutions + attention.** Transformer
   block. Language-model training reachable.
9. **cajeta.prism.checkpoint.** Binary save/load. Resumable
   training loops.
10. **cajeta.prism.data.** Dataset + Loader (fiber pool +
    bounded prefetch). Real datasets.
11. **cajeta.prism.distribute: Mesh + pjit + collectives.**
    Multi-CPU sharding works first; multi-host comes once
    cajeta.io.net is in.
12. **cajeta.prism.transform: pmap.** Layered on distribute.
13. **cajeta.prism.autograd: jvp + vjp + jacobian + hessian.**
    Higher-order derivatives. Useful for second-order optimizers
    and Bayesian methods.
14. **cajeta.prism.distributions.** Reparameterizable + score-
    function gradient choices. Hooks autograd.
15. **cajeta.prism.text + cajeta.prism.vision + cajeta.prism.metric.**
    Domain-flavored extensions. Independent of each other.
16. **cajeta.prism.profile.** Op-level timing and memory tracing.
    Hooks the same trace path that `jit` uses.

The gating step is (1) — every other layer touches the typed tensor
shape. (3) is the second-most load-bearing because the autograd
design choice (compile-time backward generation) determines the
shape of every transformation that follows.

Deferred to follow-ups (separate libraries):
- cajeta.prism.cuda / cajeta.prism.metal — accelerator backends
- cajeta.prism.compile — XLA-equivalent graph compiler
- cajeta.prism.export — ONNX / SavedModel export
- cajeta.prism.serve — inference serving
- cajeta.prism.models — curated model implementations

---

## Open questions

- **Dynamic shapes UX.** When a shape is unknown at compile time,
  the type degrades to `SUnknown`. Does the user write
  `Tensor<SUnknown, Float32, CPU, NoGrad>` explicitly, or does the
  compiler infer it with a one-line warning? Lean: infer + warn,
  with an option to demand-static-shapes per file.
- **Compile-time backward feasibility.** Generating the backward
  pass at compile time works cleanly for primitives we control;
  control flow inside a user function (if / for over runtime
  values) is the hard case. JAX traces; we'd want to do better.
  Whether cajeta's effect system + the AST-as-data we already have
  via the visitor is enough to lower control flow into a backward-
  generatable form is the open design question.
- **Module synthesis vs annotation.** The sketch above uses
  `@Parameter` field annotations the compiler walks. Alternative:
  any field whose type is `Tensor<..., NoGrad>` (matching a
  parameter's grad-mode signature) is automatically a parameter.
  Cleaner — no annotation — but couples parameter discovery to
  grad mode, which has independent meaning. Lean: keep
  `@Parameter` so the two concerns are decoupled.
- **In-place vs out-of-place.** Pure-functional design says
  every op returns a new tensor. Cajeta's ownership model lets
  the compiler elide the copy when the input is single-owned.
  Should the surface API expose explicit in-place variants
  (`addInPlace`) for cases where the user knows ownership is
  exclusive? PyTorch's trailing-underscore convention is one
  option; another is no in-place API at all and rely on the
  compiler. Lean: no in-place API; trust the optimizer.
- **Tensor backing relationship with cajeta.ml.tensor.** prism's
  Tensor wraps cajeta.ml.tensor's. Round-trip across libraries
  should be zero-copy via the shared backing buffer; same question
  raised in CajetaML.md / CajetaTorch.md. Cohesive resolution
  needed across all three before any goes public.
- **Distributed control-plane minimum.** Multi-host needs cajeta.io.net.
  cajeta.io.net needs the reactor work that's blocked on the harness.
  Single-host pmap (across CPU "devices" — really just thread pools
  pretending) ships first; multi-host waits.
- **Naming for the typed shape primitives.** `S2<A, B>` reads OK
  in code samples but is awkward. Alternatives: `Shape<A, B>` with
  rank inferred from arity (cleaner), or `Shape2<A, B>` (verbose
  but unambiguous). Worth picking before any code lands.
