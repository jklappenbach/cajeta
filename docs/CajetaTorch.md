# CajetaTorch.md

A faithful port of the PyTorch surface to cajeta, packaged as
`cajeta.torch` (separate from stdlib, separate from `cajeta.math`). The
target user is someone with a working PyTorch script who wants to run
the equivalent in cajeta with minimal edits. API names, method
signatures, and ordering mirror `torch.*` wherever the differences
would be pure Python noise; cajeta type and naming conventions assert
themselves only where PyTorch's choices are pythonic accidents
(generics, ownership, encoding).

Implementation lands incrementally as `.cajeta` files under
`./libraries/cajeta.torch/src/`. Ships as its own package with its
own version cadence.

## Why a separate library from cajeta.math

`cajeta.math` (see CajetaMath.md) is the numpy + scipy + scikit-learn
equivalent — broad numerical and classical ML, with `Tensor` shaped
to mirror `np.ndarray`. The autograd / `nn` modules sketched there
are designed to be PyTorch-style but aren't trying to be a 1:1
migration target.

`cajeta.torch` is that 1:1 migration target. Its goal is "PyTorch
script ports cleanly," not "build the best deep-learning framework
from first principles." Two different audiences:

- **cajeta.math** users are writing new code, mixing tensor ops with
  classical ML, and want a coherent native cajeta API.
- **cajeta.torch** users are porting an existing PyTorch codebase or
  reading a PyTorch paper's reference implementation; they want
  `torch.nn.Linear`, `torch.optim.AdamW`, `loss.backward()` to do
  what they already mean.

Keeping them separate means cajeta.torch can chase PyTorch API
churn without dragging stdlib or cajeta.math along. cajeta.torch's
`Tensor` is its own type, distinct from `cajeta.math.tensor.Tensor`,
because PyTorch's tensor has autograd-aware semantics baked in
(every op records into the graph by default; numpy's doesn't). The
two share lower layers — both delegate matmul / fft / random to the
same underlying kernels — but the user-facing types are independent.

## Goals

- **API faithfulness.** Method names, argument order, default values,
  return shapes match `torch.*` exactly when nothing else is at
  stake. `tensor.view(...)`, `tensor.mean(dim=)`, `F.relu(...)`,
  `nn.Linear(in, out, bias=)` all read as if straight from PyTorch.
- **State-dict compatibility.** Checkpoints save and load in a format
  binary-compatible with PyTorch's `.pt` (pickled state-dict subset),
  so weights move between ecosystems. Full pickle isn't replicated;
  just the state-dict tensor + metadata layout researchers actually
  share.
- **Eager-first.** No graph-tracing requirement up front. Code
  written for PyTorch's eager mode runs the same way in cajeta.torch.
  JIT / trace / compile lives behind opt-in scopes if it lands at all.
- **Device abstraction in v1, even if CPU-only is the only backend.**
  `tensor.to("cuda")` should compile and degrade gracefully (or
  error with a clear message) so user code doesn't need conditional
  device logic the day the GPU backend lands.
- **Mixed precision built in.** PyTorch's `autocast` + `GradScaler`
  shape, layered onto cajeta.math's RoundingMode-aware casting (see
  CajetaMath.md). fp4 / fp8 training is a first-class scenario.
- **Familiar training loop.** The standard `model.train(); for batch
  in loader: optimizer.zero_grad(); loss = ...; loss.backward();
  optimizer.step()` shape works verbatim.

## Non-goals (v1)

- **CUDA / accelerator backend.** CPU only. Device API exists; only
  `Device.CPU` resolves.
- **TorchScript / torch.jit.** Eager only. `@jit.script` / `@jit.trace`
  decorators don't ship. May land later as `cajeta.torch.jit` if
  there's demand.
- **torch.compile / Inductor.** No graph compilation. Same eager
  baseline. Compilation is a follow-up library, not part of v1
  cajeta.torch.
- **Distributed training.** No `torch.distributed`, no DDP, no FSDP,
  no RPC. Single-process. Multi-process / multi-host is its own
  follow-up.
- **ONNX export.** No `torch.onnx`. State-dict load/save is the only
  interop guarantee. ONNX may land separately.
- **Pre-trained model zoo.** No `torchvision.models`. Users load
  weights via state-dict. A separate `cajeta.torch.models` package can
  curate later.
- **Quantization toolkit.** No `torch.quantization`. The fp4/fp8
  training story comes via cajeta.math's casting + autocast, not via
  PyTorch's separate quant API. PTQ / QAT may land later.

## Package layout

```
cajeta.torch                     — Tensor, dtype, device, factory functions
                            (zeros, ones, arange, linspace, randn, ...),
                            element-wise + reduction ops, indexing /
                            slicing, casting (to / type / cuda / cpu)
cajeta.torch.autograd            — Function (forward + backward), backward(),
                            grad(), no_grad / enable_grad scopes,
                            gradcheck, anomaly detection
cajeta.torch.nn                  — Module, Parameter, ParameterList,
                            ModuleList, ModuleDict, Sequential;
                            layers (Linear, Conv*, BN, LN, Dropout,
                            Embedding, MHA, Transformer*); losses
                            (MSE, CE, BCE, KLDiv, Huber, ...);
                            init (xavier, kaiming, normal, uniform)
cajeta.torch.nn.functional       — Stateless versions of every nn op:
                            relu, gelu, softmax, log_softmax,
                            cross_entropy, conv2d, batch_norm, ...
cajeta.torch.optim               — SGD, Adam, AdamW, RMSProp, Adagrad,
                            Adadelta, Adamax, NAdam, RAdam, LAMB,
                            Lion; lr_scheduler.{StepLR, MultiStepLR,
                            ExponentialLR, CosineAnnealingLR,
                            ReduceLROnPlateau, OneCycleLR, ...}
cajeta.torch.distributions       — Normal, Bernoulli, Categorical,
                            Multinomial, Beta, Gamma, Dirichlet,
                            MultivariateNormal, MixtureSameFamily,
                            ...; sample(), log_prob(), entropy(),
                            kl_divergence
cajeta.torch.utils.data          — Dataset, IterableDataset, DataLoader,
                            Sampler, RandomSampler, BatchSampler,
                            collate_fn, num_workers via cajeta.thread
                            fibers, prefetch
cajeta.torch.linalg              — matmul, inv, pinv, solve, lstsq, eig,
                            eigh, svd, qr, cholesky, det, slogdet,
                            norm, ... — namespace + signatures
                            mirror PyTorch's torch.linalg exactly.
                            Backed by cajeta.math.linalg.
cajeta.torch.fft                 — fft, ifft, fftn, ifftn, rfft, irfft,
                            fftshift, fftfreq. Backed by cajeta.math.signal.
cajeta.torch.special             — gamma, lgamma, erf, erfc, beta, digamma,
                            polygamma, ... — mirrors PyTorch's
                            torch.special namespace.
cajeta.torch.random              — manual_seed, get_rng_state,
                            set_rng_state, fork_rng. Distinct PRNG
                            stream per device. Backed by cajeta.math.random.
cajeta.torch.io                  — save / load (state-dict-compatible .pt),
                            tensor I/O helpers (from_numpy / numpy()
                            via npy bridge)
cajeta.torch.amp                 — autocast scope, GradScaler for fp16 /
                            bf16 / fp8 training
cajeta.torch.utils               — checkpoint, clip_grad_norm_,
                            clip_grad_value_, parameter counting,
                            module summary
cajeta.torch.profiler            — scope-based timing + memory tracking
                            (CPU only in v1) — mirrors PyTorch's
                            torch.profiler API.
```

Deferred to follow-up packages (not v1 cajeta.torch):
```
cajeta.torch.cuda                — accelerator backend
cajeta.torch.distributed         — DDP / FSDP / RPC
cajeta.torch.jit                 — script / trace
cajeta.torch.compile             — graph compilation
cajeta.torch.onnx                — ONNX export
cajeta.torch.quantization        — PTQ / QAT
```

---

## torch.Tensor

The user-facing type. Wraps a `cajeta.math.tensor.Tensor` for storage
and dispatches arithmetic through it; adds the autograd graph hookup
and PyTorch-shaped methods.

```cajeta
public final class Tensor {
    // Construction (mirrors torch factory functions)
    public static Tensor zeros(Shape shape, DType dtype = DType.FLOAT32, Device device = Device.CPU, boolean requiresGrad = false);
    public static Tensor ones(Shape shape, DType dtype = DType.FLOAT32, Device device = Device.CPU, boolean requiresGrad = false);
    public static Tensor full(Shape shape, float64 value, DType dtype = DType.FLOAT32, Device device = Device.CPU);
    public static Tensor empty(Shape shape, DType dtype = DType.FLOAT32, Device device = Device.CPU);
    public static Tensor arange(float64 start, float64 end, float64 step = 1.0, DType dtype = DType.FLOAT32);
    public static Tensor linspace(float64 start, float64 end, int64 steps, DType dtype = DType.FLOAT32);
    public static Tensor eye(int64 n, int64 m = -1, DType dtype = DType.FLOAT32);
    public static Tensor randn(Shape shape, DType dtype = DType.FLOAT32, Device device = Device.CPU);
    public static Tensor rand(Shape shape, DType dtype = DType.FLOAT32, Device device = Device.CPU);
    public static Tensor randint(int64 low, int64 high, Shape shape, DType dtype = DType.INT64);
    public static Tensor fromArray(Array<float32> data, Shape shape);

    // Inspection
    public Shape  shape();
    public Shape  size();              // alias for shape (PyTorch compat)
    public int64  numel();
    public int8   dim();               // rank
    public DType  dtype();
    public Device device();
    public boolean requiresGrad();

    // Autograd
    public Tensor grad;                // populated by backward()
    public void   backward(Tensor gradient = null, boolean retainGraph = false);
    public Tensor detach();
    public void   requiresGrad_(boolean req);   // in-place; trailing _

    // Element-wise arithmetic — operator overloads + named methods
    public Tensor operator+(Tensor other);
    public Tensor operator-(Tensor other);
    public Tensor operator*(Tensor other);
    public Tensor operator/(Tensor other);
    public Tensor add(Tensor other, float64 alpha = 1.0);
    public Tensor sub(Tensor other, float64 alpha = 1.0);
    public Tensor mul(Tensor other);
    public Tensor div(Tensor other);
    public Tensor matmul(Tensor other);
    public Tensor operator@(Tensor other);   // matmul shorthand

    // In-place variants (PyTorch trailing-underscore convention)
    public Tensor add_(Tensor other);
    public Tensor mul_(Tensor other);
    public Tensor zero_();
    public Tensor fill_(float64 value);

    // Reductions
    public Tensor sum(int8 dim = -1, boolean keepdim = false);
    public Tensor mean(int8 dim = -1, boolean keepdim = false);
    public Tensor max(int8 dim = -1, boolean keepdim = false);
    public Tensor min(int8 dim = -1, boolean keepdim = false);
    public Tensor std(int8 dim = -1, boolean unbiased = true, boolean keepdim = false);
    public Tensor var(int8 dim = -1, boolean unbiased = true, boolean keepdim = false);
    public Tensor argmax(int8 dim);
    public Tensor argmin(int8 dim);
    public Tensor norm(float64 p = 2.0, int8 dim = -1);

    // Shape ops
    public Tensor view(Shape newShape);
    public Tensor reshape(Shape newShape);
    public Tensor transpose(int8 dim0, int8 dim1);
    public Tensor permute(int8... dims);
    public Tensor squeeze(int8 dim = -1);
    public Tensor unsqueeze(int8 dim);
    public Tensor expand(Shape shape);
    public Tensor repeat(int64... sizes);
    public Tensor flatten(int8 startDim = 0, int8 endDim = -1);
    public Tensor cat(Tensor[] tensors, int8 dim = 0);     // static-ish
    public Tensor stack(Tensor[] tensors, int8 dim = 0);

    // Indexing / slicing
    public Tensor at(Index... indices);
    public Tensor maskedSelect(Tensor mask);
    public void   indexPut_(Index[] indices, Tensor values);
    public Tensor gather(int8 dim, Tensor index);
    public void   scatter_(int8 dim, Tensor index, Tensor src);

    // Casting / device transfer
    public Tensor to(DType dtype);
    public Tensor to(Device device);
    public Tensor to(Device device, DType dtype);
    public Tensor cpu();
    public Tensor cuda(int8 deviceIndex = 0);
    public Tensor type(DType dtype);
    public Tensor float();             // -> float32
    public Tensor double();            // -> float64
    public Tensor half();              // -> float16
    public Tensor bfloat16();
    public Tensor long();              // -> int64
    public Tensor int();               // -> int32

    // Numpy bridge
    public Array<float32> numpy();
    public static Tensor fromNumpy(Array<float32> data, Shape shape);
}

public final class Device {
    public DeviceType type;
    public int8       index;

    public static Device CPU;

    public Device(DeviceType type, int8 index = 0);
    public String toString();          // "cpu", "cuda:0", etc.
}

public enum DeviceType { CPU, CUDA, MPS, XLA }
```

**Storage strategy.** A `torch.Tensor` holds:
- a `cajeta.math.tensor.Tensor` for the actual element buffer + shape,
- a `Device` tag,
- `requiresGrad: boolean`,
- `grad: Tensor` (populated post-backward),
- `gradFn: Function` (set by autograd-recording ops, null on leaves).

Ops that don't need grad (called inside a `no_grad` scope, or where
no input has `requiresGrad`) skip the graph hookup and run as a thin
pass-through to the underlying cajeta.math tensor op.

---

## torch.autograd

PyTorch-shaped: dynamic graphs built op-by-op during forward, walked
in reverse on `.backward()`.

```cajeta
public abstract class Function<Inputs, Output> {
    // Override these two. PyTorch's Function.forward gets a `ctx`
    // for stashing tensors needed in backward; we mirror that.
    public abstract Output forward(Context ctx, Inputs inputs);
    public abstract Inputs backward(Context ctx, Output gradOutput);

    // The user-facing entry point. Records into the graph if any
    // input requires grad; otherwise dispatches forward directly.
    public static Output apply(Inputs inputs);
}

public final class Context {
    public void   saveForBackward(Tensor... tensors);
    public Tensor[] savedTensors();
    public boolean  needsInputGrad(int8 idx);
}

public class NoGradScope implements AutoCloseable {
    public NoGradScope();
    public void close();
}

public class EnableGradScope implements AutoCloseable {
    public EnableGradScope();
    public void close();
}

// Convenience predicate
public static boolean isGradEnabled();
```

Built-in `Function` registrations cover every torch.Tensor op (add,
mul, matmul, conv*, attention, every nn layer's primitive). User
code writes custom ops by subclassing `Function` and calling
`apply(...)`.

`backward()` walks gradFn references, accumulating gradients into
each leaf tensor's `.grad` field. `retain_graph=true` keeps the
intermediate gradFn refs alive for a second backward; default
behavior frees them.

`gradcheck(fn, inputs)` mirrors PyTorch's numerical-gradient check
for verifying custom Function implementations.

---

## torch.nn

```cajeta
public abstract class Module {
    // The user override.
    public abstract Tensor forward(Tensor... inputs);

    // Parameter discovery via @Parameter field annotation. The
    // compiler walks declared fields, recursing into sub-Modules
    // and ModuleList / ModuleDict containers. Cached after first
    // access.
    public Iterable<Tensor>            parameters(boolean recurse = true);
    public Iterable<Pair<String, Tensor>> namedParameters(boolean recurse = true);
    public Iterable<Module>            children();
    public Iterable<Module>            modules();

    // Train / eval mode (BN / Dropout consult this)
    public Module train(boolean mode = true);
    public Module eval();
    public boolean training;

    // State dict
    public StateDict stateDict();
    public void      loadStateDict(StateDict sd, boolean strict = true);

    // Device transfer (recurses into children)
    public Module to(Device device);
    public Module to(DType dtype);
    public Module cpu();
    public Module cuda(int8 deviceIndex = 0);
    public Module half();
    public Module float();
    public Module double();
    public Module bfloat16();

    // Initialization helper
    public Module apply(Function<Module, Void> fn);

    // Hooks for forward / backward instrumentation
    public Handle registerForwardHook(ForwardHook hook);
    public Handle registerForwardPreHook(ForwardPreHook hook);
    public Handle registerBackwardHook(BackwardHook hook);
}

public final class Parameter extends Tensor {
    // A Tensor that's automatically registered as a learnable
    // parameter when assigned to a Module field. Default
    // requiresGrad = true.
    public Parameter(Tensor data, boolean requiresGrad = true);
}
```

### Layer set (v1)

Linear: `Linear`, `Bilinear`, `Identity`.

Convolutions: `Conv1d`, `Conv2d`, `Conv3d`, `ConvTranspose1d`, `ConvTranspose2d`,
`ConvTranspose3d`, `Unfold`, `Fold`.

Pooling: `MaxPool{1,2,3}d`, `AvgPool{1,2,3}d`, `AdaptiveAvgPool{1,2,3}d`,
`AdaptiveMaxPool{1,2,3}d`.

Padding: `ZeroPad2d`, `ConstantPad{1,2,3}d`, `ReflectionPad{1,2,3}d`,
`ReplicationPad{1,2,3}d`.

Normalization: `BatchNorm{1,2,3}d`, `LayerNorm`, `GroupNorm`, `InstanceNorm{1,2,3}d`,
`RMSNorm`, `LocalResponseNorm`.

Recurrent: `RNN`, `LSTM`, `GRU` (with their cell forms `RNNCell`, `LSTMCell`, `GRUCell`).

Attention / transformer: `MultiheadAttention`, `TransformerEncoderLayer`,
`TransformerDecoderLayer`, `TransformerEncoder`, `TransformerDecoder`,
`Transformer`.

Activation modules + functional pairs: `ReLU`, `LeakyReLU`, `PReLU`, `ELU`,
`SELU`, `CELU`, `GELU`, `SiLU`, `Mish`, `Softplus`, `Softsign`, `Tanh`,
`Sigmoid`, `Hardsigmoid`, `Hardswish`, `Hardtanh`, `LogSigmoid`, `Softmax`,
`LogSoftmax`, `Softmin`.

Dropout: `Dropout`, `Dropout1d`, `Dropout2d`, `Dropout3d`, `AlphaDropout`,
`FeatureAlphaDropout`.

Sparse: `Embedding`, `EmbeddingBag`.

Loss: `MSELoss`, `L1Loss`, `SmoothL1Loss`, `HuberLoss`, `BCELoss`,
`BCEWithLogitsLoss`, `CrossEntropyLoss`, `NLLLoss`, `KLDivLoss`,
`PoissonNLLLoss`, `GaussianNLLLoss`, `MarginRankingLoss`, `HingeEmbeddingLoss`,
`MultiLabelMarginLoss`, `CosineEmbeddingLoss`, `TripletMarginLoss`,
`CTCLoss`.

### Containers

`Sequential`, `ModuleList`, `ModuleDict`, `ParameterList`, `ParameterDict`.

### Initialization (`cajeta.torch.nn.init`)

`uniform_`, `normal_`, `constant_`, `ones_`, `zeros_`, `eye_`, `dirac_`,
`xavierUniform_`, `xavierNormal_`, `kaimingUniform_`, `kaimingNormal_`,
`orthogonal_`, `sparse_`, `truncNormal_`. Trailing-underscore = in-place,
matching PyTorch.

---

## torch.optim

```cajeta
public abstract class Optimizer {
    public abstract void step();
    public void          zeroGrad(boolean setToNone = true);

    public StateDict     stateDict();
    public void          loadStateDict(StateDict sd);

    // Param groups: each group has its own lr / weight_decay /
    // momentum / etc., overriding the optimizer's defaults.
    public void          addParamGroup(ParamGroup group);
    public ParamGroup[]  paramGroups();
}

public class SGD extends Optimizer {
    public SGD(Iterable<Tensor> params,
               float64 lr,
               float64 momentum = 0.0,
               float64 dampening = 0.0,
               float64 weightDecay = 0.0,
               boolean nesterov = false);
}

public class Adam extends Optimizer {
    public Adam(Iterable<Tensor> params,
                float64 lr = 1e-3,
                Pair<float64, float64> betas = (0.9, 0.999),
                float64 eps = 1e-8,
                float64 weightDecay = 0.0,
                boolean amsgrad = false);
}

public class AdamW extends Optimizer { /* same shape, decoupled WD */ }
public class RMSProp extends Optimizer { /* alpha, momentum, centered */ }
public class Adagrad extends Optimizer { /* lr_decay, initial_accumulator_value */ }
public class Adadelta extends Optimizer { /* rho */ }
public class Adamax extends Optimizer { /* infinity-norm Adam */ }
public class NAdam extends Optimizer { /* Nesterov Adam */ }
public class RAdam extends Optimizer { /* Rectified Adam */ }
public class LAMB extends Optimizer { /* large-batch Adam */ }
public class Lion extends Optimizer { /* sign-based momentum */ }
```

LR schedulers (`cajeta.torch.optim.lr_scheduler`):
`StepLR`, `MultiStepLR`, `ExponentialLR`, `CosineAnnealingLR`,
`CosineAnnealingWarmRestarts`, `ReduceLROnPlateau`, `LambdaLR`,
`MultiplicativeLR`, `OneCycleLR`, `CyclicLR`, `LinearLR`,
`PolynomialLR`, `SequentialLR`, `ChainedScheduler`.

---

## torch.utils.data

```cajeta
public abstract class Dataset<T> {
    public abstract int64 length();
    public abstract T     getItem(int64 index);
}

public abstract class IterableDataset<T> {
    public abstract Iterator<T> iterator();
}

public class DataLoader<T> implements Iterable<Batch<T>> {
    public DataLoader(Dataset<T> dataset,
                      int64 batchSize = 1,
                      boolean shuffle = false,
                      Sampler sampler = null,
                      int8 numWorkers = 0,           // fiber-based when > 0
                      Function<T[], Batch<T>> collateFn = null,
                      boolean pinMemory = false,
                      boolean dropLast = false,
                      int64 prefetchFactor = 2,
                      boolean persistentWorkers = false);

    public Iterator<Batch<T>> iterator();
    public int64 length();
}
```

Workers run as cajeta.thread fibers (no separate process model
needed since cajeta's runtime is fiber-scheduled). `prefetchFactor`
controls how many batches each worker stages ahead.

---

## torch.amp — automatic mixed precision

```cajeta
public class autocast implements AutoCloseable {
    public autocast(DType dtype = DType.FLOAT16, boolean enabled = true);
    public void close();
}

public class GradScaler {
    public GradScaler(float64 initScale = 65536.0,
                      float64 growthFactor = 2.0,
                      float64 backoffFactor = 0.5,
                      int64   growthInterval = 2000,
                      boolean enabled = true);

    public Tensor scale(Tensor outputs);
    public void   step(Optimizer optimizer);
    public void   update(Tensor newScale = null);
    public void   unscale_(Optimizer optimizer);
}
```

Same training-loop pattern as PyTorch:
```cajeta
GradScaler scaler = heap GradScaler();
for (Batch<...> batch : loader) {
    optimizer.zeroGrad();
    try (autocast a = heap autocast(DType.FLOAT16)) {
        Tensor output = model.forward(batch.input);
        Tensor loss = lossFn.forward(output, batch.target);
    }
    scaler.scale(loss).backward();
    scaler.step(optimizer);
    scaler.update();
}
```

The autocast scope swaps the dtype-policy for ops inside; cajeta.math's
RoundingMode-aware casting (CajetaMath.md "Prerequisite: cajeta.math
expansion") is what makes the down-casts well-defined. fp8 training
is a supported autocast dtype in v1.

---

## torch.io — state-dict compatibility with PyTorch

```cajeta
public final class StateDict {
    public Map<String, Tensor> tensors;
    public Map<String, Object> metadata;

    public void saveTo(String path);
    public static StateDict loadFrom(String path);
}

public static void save(Object obj, String path);
public static <T> T   load(String path);
```

The `.pt` format uses Python's pickle, which we don't replicate
fully. cajeta.torch reads and writes the well-defined subset
checkpoints actually use:

- Tensor headers (dtype, shape, strides, storage offset)
- Tensor buffers (raw bytes in tensor's dtype)
- Module path -> tensor key mapping
- Plain-old-data metadata (numbers, strings, lists, dicts)

This covers the vast majority of published checkpoints. Pickle ops
outside this subset (custom classes, arbitrary code execution) are
flagged as load errors, not silently approximated. A separate
`cajeta.torch.io.safetensors` module ships at the same time for the safer
modern format, and is the recommended save path for new code.

---

## Implementation sequence

A reasonable order, given dependencies:

1. **torch.Tensor + cajeta.math.tensor backing.** No autograd yet.
   Eager ops, factory functions, indexing, shape ops, casting,
   device tag (CPU only resolves). Mirrors the torch namespace
   surface enough to run a numpy-style script.
2. **torch.autograd.** Function + Context, backward graph, no_grad
   / enable_grad scopes. Built-in Function registrations for every
   tensor op landed in (1).
3. **torch.nn: Module, Parameter, Sequential.** The framework
   skeleton — parameter discovery, train/eval mode, state dict.
   Empty-but-correct.
4. **torch.nn: linear + activation + loss + simple optimizer.**
   Enough to train an MLP. `Linear`, `ReLU`, `MSELoss`,
   `CrossEntropyLoss`, `SGD`, `Adam`. Validates the autograd path
   end-to-end.
5. **torch.utils.data.** Dataset, DataLoader (single-worker first),
   collate, samplers. Lets training loops run on real datasets.
6. **torch.io: state-dict save / load.** Checkpointing. Inter-op
   with PyTorch checkpoints lands here.
7. **torch.nn: convolutions + normalization + dropout.** The
   conv-net surface. Enough to train ResNet-style models.
8. **torch.nn: attention + transformer.** The language-model
   surface. MHA, encoder/decoder layers, the full Transformer
   wrapper.
9. **torch.optim: full optimizer + scheduler set.** AdamW,
   RMSProp, all the LR schedulers. Round out training-loop
   ergonomics.
10. **torch.distributions.** Normal, Categorical, Bernoulli,
    Multinomial first; the rest as needed. Sampling + log_prob +
    entropy + KL.
11. **torch.amp.** autocast + GradScaler. fp16 / bf16 / fp8
    training. Hooks into cajeta.math casting.
12. **torch.linalg + torch.fft + torch.special + torch.random.**
    Surface mirrors of torch.* sub-namespaces; mostly thin wrappers
    over cajeta.math equivalents.
13. **torch.utils.data: multi-worker DataLoader.** Fiber-backed
    workers + prefetch + persistent workers. Single-worker path is
    enough for v1; multi-worker is performance, not correctness.
14. **torch.profiler + torch.utils helpers.** Scope-based timing,
    grad clipping, parameter counting, module summary.

The gating step is (2) — once autograd is correct, every nn layer
and optimizer is a straightforward implementation against a stable
foundation.

Deferred (not v1, separate follow-up libraries):
- torch.cuda / accelerator backend
- torch.distributed
- torch.jit / torch.compile
- torch.onnx
- torch.quantization
- cajeta.torch-models (pre-trained model curation)

---

## Open questions

- **Naming convention.** PyTorch uses `snake_case` for methods
  (`zero_grad`, `state_dict`, `load_state_dict`). cajeta is
  camelCase elsewhere. Strict PyTorch faithfulness says keep
  snake_case so `model.load_state_dict(...)` ports verbatim;
  cajeta-house style says camelCase. Worth a deliberate decision
  before locking the API. Initial lean: camelCase for everything,
  with the rationale that PyTorch users adapt fast and consistency
  with cajeta.math / stdlib matters more long-term.
- **Tensor: own type or alias of cajeta.math.tensor.Tensor?** Sketch
  above says own type. The reverse — alias plus extension methods
  for autograd hookup — would mean fewer types in the ecosystem
  but couples cajeta.math's tensor evolution to cajeta.torch's. Probably
  worth keeping them distinct; revisit if the duplication becomes
  painful.
- **Pickle subset for .pt loading.** Where exactly to draw the
  "safe subset" line. Tensors + plain data is uncontroversial.
  Optional dict key types (frozenset, tuple) are fine too. Custom
  reduce_ex / __setstate__ — refuse. Document the boundary clearly
  so users know what to expect.
- **autocast composition with no_grad.** PyTorch's behavior here
  has subtle interactions; mirror exactly or pick a cleaner story?
  Lean: mirror exactly, since the goal is migration ease.
- **Eager vs trace.** Confirmed eager-only for v1. If `cajeta.torch.jit`
  ever lands, decide whether to mirror TorchScript syntax (limited
  Python subset annotated with `@jit.script`) or design a cajeta-
  native scope-based form (`withTrace { ... }`).
- **Shared tensor backing with cajeta.math.** When user code uses
  both libraries (e.g. cajeta.math.linalg result fed into a cajeta.torch
  forward pass), do tensors round-trip zero-copy through the
  shared backing buffer, or does each library own its own copy?
  Worth resolving before either library lands a public API.
