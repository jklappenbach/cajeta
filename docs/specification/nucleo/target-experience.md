# Núcleo — Target Experience (the destination, in code)

> Status: vision draft (2026-06-22). This paints **what finished Cajeta code looks like**
> when núcleo + the façades + the language sugar are all in place. It is the *destination*;
> the three sequencing paths (language-first / method-now / pragmatic-split) differ only in
> the **order** each line below becomes real, and in which method-call fallbacks stand in
> during the interim. Companion to `python-stack-analysis.md`.

### Feature-dependency legend
Every snippet is tagged with what it leans on, so the picture stays honest:
- ✅ **in hand** — works on today's language.
- ➕ **cheap** — small grammar/codegen add (`@` matmul, `**`, unary `-`, bracket slicing,
  multi-index, `...`/newaxis, tuple destructuring, type aliases).
- 🔨 **foundational** — a real language project (records → typed schemas; annotation-transform
  hook → `@Grad`/`@Jit`/`@Vmap`; compile-time string DSL → `einsum`).

What is *already* true and load-bearing for the whole picture: **named arguments + defaults**
(✅), **operator overloading** (✅), **monomorphized dtype** so `Tensor<MXFP4>` is real packed
storage (✅), **const-generic fixed-size shapes** (✅), `var` inference (✅), method chaining (✅).

---

## 1. The unification — one set of bytes, four views

The thesis made concrete: a Parquet column, a tensor, and a render input are the same buffer.

```cajeta
import dev.cajeta.nucleo.frame.Table;
import cajeta.math.Tensor;

// Open a Parquet file, pull a non-null f32 column — zero copy. ✅(today via method) ➕(sugar)
var prices = ParquetFile.open("ticks.parquet").column<float32>("price");

// That column IS a 1-D tensor buffer — same bytes, no marshalling. (invariant §2.3) ✅
Tensor<float32> t = prices.asTensor();        // zero-copy: validity bitmap absent => raw buffer

// Fused, lowered to GPU, differentiable — all over the same memory.            ➕(@) 🔨(@Grad)
var normalized = (t - t.mean()) / t.std();    // one fused kernel, no temporaries

// And it can hand back to the Python world with no serialization (C Data Interface). ✅
var handle = prices.exportArrow();            // ArrowArray/ArrowSchema -> pyarrow sees it live
```

---

## 2. Torch façade — a training step that reads like PyTorch but is typed and compiled

```cajeta
import dev.cajeta.torch as torch;
import dev.cajeta.torch.nn;
import dev.cajeta.torch.optim;

public class MLP : nn.Module {
    nn.Linear fc1 = nn.Linear(784, 256);
    nn.Linear fc2 = nn.Linear(256, 10);

    public Tensor<float32> forward(Tensor<float32> x) {
        return fc2(fc1(x).relu());            // operator() call sugar + method chain      ✅➕
    }
}

var model = heap MLP();
var opt   = optim.AdamW(model.parameters(), lr: 3e-4);     // named args                    ✅

// @Grad turns this into a mid-level-IR autodiff pass — NOT a runtime tape.           🔨(hook)
@Grad
float32 step(Tensor<float32> x, Tensor<int64> y) {
    var logits = model(x);
    return torch.crossEntropy(logits, y);
}

for (var batch : loader) {
    var loss = step.withGrads(batch.x, batch.y);   // returns value + grads, compiled+fused
    opt.step(loss.grads);                          // explicit grads — no global .grad soup
    opt.zeroGrad();                                // (kept for familiarity; functional path is first-class)
    System.stdout.println("loss=" + loss.value);
}
```

What changed from PyTorch, on purpose:
- `requires_grad` bools and the global `.grad` accumulator → **explicit grads returned by the
  `@Grad` transform** (the footgun is gone; the familiar `zeroGrad()` still exists).
- the tape → a **compiled, fused backward pass** (the cost-to-run win).
- dtype/device live in the type, so a host/device mix is a **compile error**, not a 2 a.m. stack trace.

---

## 3. Dataframe — Polars-shaped, lazy, with the schema *in the type*

```cajeta
import dev.cajeta.nucleo.frame.*;

// The distinctive move: schema is a record, known at compile time.             🔨(records)
record Tick { Instant ts; float64 price; float64 size; Symbol venue; }

// Lazy + expression-based; the optimizer plans the whole chain before executing. ✅(engine) ➕(sugar)
Table<Tick> ticks = Table.scanParquet<Tick>("ticks.parquet");

var vwap = ticks
    .filter(col.price > 0.0)                       // comparison → predicate, pushed down
    .groupBy(col.venue)
    .agg( (col.price * col.size).sum() / col.size.sum() as "vwap" )
    .sort(col.vwap, descending: true)
    .collect();                                    // nothing ran until here

// df.price is a typed field access — a typo is a COMPILE error, not a runtime KeyError. 🔨
var spread = ticks.price.max() - ticks.price.min();

// Time-series resample (the one genuinely-worth-porting pandas feature).               ✅
var minuteBars = ticks.resample(col.ts, every: 1.minutes).agg(col.price.last());
```

Gone, deliberately: the `Index`/`MultiIndex`, `inplace=`, `object` dtype, NaN-as-missing
(real nullable `float64?` columns instead), and the copy/view ambiguity.

---

## 4. The flagship — differentiable splat rendering, where everything meets

```cajeta
import dev.cajeta.nucleo.geometry.SplatScene;
import dev.cajeta.torch.optim;

// A splat scene is a table of millions of rows — and its columns are tensor buffers. 🔨(records)
record Splat { Vec3 pos; Vec3 scale; Quat rot; float32 opacity; SH3 color; }

Table<Splat> scene = SplatScene.load("garden.splat");

var opt = optim.Adam(scene.columns(), lr: 1e-2);

// The render is differentiable: gradients flow back into the table's columns.    🔨(@Grad)
@Grad
float32 photometricLoss(Camera cam, Image<float32> target) {
    var rendered = scene.render(cam);              // GPU rasterize, fused, differentiable
    return (rendered - target).square().mean();
}

for (var view : trainingViews) {
    var loss = photometricLoss.withGrads(view.cam, view.image);
    opt.step(loss.grads);                          // gradient descent over splat COLUMNS
}
// dataframe (the table) + tensor (the columns) + autograd (@Grad) + rendering (.render) —
// all on ONE set of bytes. No subsystem boundary crossed.
```

---

## 5. SciPy façade — pure functions, typed returns (no positional tuple bags)

```cajeta
import dev.cajeta.scipy.optimize;
import dev.cajeta.scipy.signal;
import dev.cajeta.nucleo.linalg;

// Typed return record instead of scipy's OptimizeResult grab-bag.                    🔨/➕
var fit = optimize.minimize(rosenbrock, x0: start, method: BFGS);
System.stdout.println("converged=" + fit.success + " at " + fit.x);

// Destructuring a typed multiple-return.                                              ➕
var (q, r) = linalg.qr(a);

// Sparse ARRAYS only (we skip scipy's deprecated sparse-matrix mistake).             ✅
var laplacian = SparseTensor.fromCoo(rows, cols, vals, shape: [n, n]);
var x = linalg.sparse.cg(laplacian, b);            // conjugate gradient

// Signal processing reads like scipy.signal.                                          ✅
var filtered = signal.sosfiltfilt(butter(order: 4, cutoff: 0.2), raw);
```

---

## 6. The moat — things Python structurally cannot do

```cajeta
// (a) einsum validated at COMPILE TIME — a wrong contraction won't compile.       🔨(CT-DSL)
var attn = einsum<"bhqd,bhkd->bhqk">(query, keys);     // string parsed once, at compile time

// (b) Units of measure — zero runtime cost dimensional analysis. Achievable EARLY.   ✅(today)
Quantity<float64, Meters>  d = 12.0.meters;
Quantity<float64, Seconds> t = 3.0.seconds;
var v = d / t;                                          // type: Quantity<f64, Meters/Seconds>
// var bad = d + t;                                     // COMPILE ERROR: m + s

// (c) Compile-time shape checking where it pays (fixed dims).                          ✅
Matrix<float32, 4, 4> view = camera.viewMatrix();
Matrix<float32, 4, 4> proj = camera.projMatrix();
var mvp = proj @ view;                                  // ✓ shapes line up at compile time   ➕(@)
// var bad = proj @ vec3;                               // COMPILE ERROR: 4x4 @ 3-vector

// (d) Masks from comparisons, type-checked indexing.                                ✅➕
var positives = data[data > 0.0];                       // boolean mask, not stringly-typed

// (e) Annotations ARE compile-time transforms (not runtime wrappers).               🔨(hook)
@Jit @Vmap
Tensor<float32> batchedKernel(Tensor<float32> x) { ... } // batching + fusion as IR passes
```

---

## 7. Interop — never an island

```cajeta
// Zero-copy out to the Python ecosystem via the C Data Interface (no libarrow link). ✅
var arrowHandle = frame.exportArrow();         // pyarrow / Polars / DuckDB read it live

// Zero-copy in from a numpy array handed across the same ABI.                         ✅
var t = Tensor.importArrow<float32>(externalHandle);

// MX formats survive interop as Arrow extension types (bytes move; semantics stay ours). ✅
var packed = column.as<MXFP4>();               // tools that don't know MXFP4 still move the bytes
```

---

## What the picture tells us about the paths

Read the tags down the page:
- The **substrate, math, units, interop, fixed-size shapes, named-arg APIs** (✅) are reachable
  on today's language — a large, useful surface exists *now*.
- The **numpy *feel*** — `@`, `a[1:5, ::2]`, `a[..., None]`, `(q, r) = ...` (➕) — is a small,
  bounded grammar effort that makes everything above read like Python instead of Java.
- The **moat** — typed schemas (`Table<Tick>`), `@Grad`/`@Jit`/`@Vmap`, compile-time `einsum`
  (🔨) — is gated on exactly three language projects: **records**, the **annotation-transform
  hook**, and a **compile-time string DSL**. Nothing in the flagship (§4) is reachable without
  the first two.

So the destination is firm and singular; the paths differ only in whether we make the ➕ and 🔨
features real *before* the façades (sugary + typed from day one, slower first demo), *after*
(method-call fallbacks now, re-touch later), or *split* (➕ now, 🔨 scheduled as the moat
projects, flagship riding the transform-hook track). That is the decision `python-stack-analysis.md`
§5 leaves open — and this document is what it's choosing *between paths to*, not between
destinations.
```
