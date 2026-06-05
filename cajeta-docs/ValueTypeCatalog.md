# Cajeta value-type catalog

> **Status: design-stage reference.** This catalogs the operator-overloaded value
> types cajeta intends to ship and their canonical operator-overload signatures, so
> that when it is time to write definitions the surface is already specified. It is a
> companion to [`OperatorOverloading.md`](OperatorOverloading.md) and depends on the
> value-type operator-overloading **mechanism change** (see
> `plans/value-type-overloading-plan.md`): today only non-primitive classes dispatch
> operators; value types are `@ValueType` POD classes that dispatch via a relaxed gate
> with their operators force-inlined to flat IR (preserving by-value GPU marshalling).

Priority: **now** = needed for current ML/GPU work · **soon** = next · **later** = justified-but-speculative.

> **Corrections (2026-06-04) — these override the raw per-row notes below, which were
> generated before the design decisions were locked:**
> - **No templated operators; matmul `*` is compiler-intrinsic for built-in `Matrix`/`Tensor`**
>   (owner decision). Operator overloads are concrete (specific LHS × specific RHS). Any row
>   marking K-generic matmul as *"FORBIDDEN" / "needs method-templated operators"* OR as a
>   `operator*<K>` template is superseded: for the compiler-known value types `Matrix`/`Tensor`,
>   `M1 * M2` lowers to matmul by the compiler reading `R/K/C` off the (already concrete) operand
>   types in codegen — exactly like `Vector`'s operators, no template anywhere. Element-wise
>   multiply is the method `a.hadamard(b)`.
> - **Instance mutating operators are forbidden on value types** (owner decision). Any
>   `operator[]=` / `operator++` / `operator--` row is conceptual only: value types are by-value
>   Copy, so in-place mutation would mutate a copy. Writes use whole-value reassignment
>   (`m = m.with(...)`, `m[r][c]=v` rewrites the whole value via flat-lane insert); `m += n`
>   desugars to `m = m + n`. Read `operator[]` is fine.
> - The catalog is **auto-generated** from the design workflow and still has duplicate/conflicting
>   rows for a few types (e.g. `Matrix`); a consolidation pass is pending. Treat the
>   conventions section + these corrections as authoritative when they disagree with a row.

## Operator-signature conventions

Canonical conventions the catalog/docs must follow (grounded in OperatorOverloading.md and CajetaLlvmVisitor.h:1074-1167 enforcement): (1) BINARY arithmetic/bitwise/comparison (+, -, *, /, %, &, |, ^, <<, >>, ==, <) and NON-mutating UNARY (+, -, !, ~): `public static` with explicit operands, no `this`. Binary = exactly 2 params; unary = exactly 1 param. Shape: `public static R operator+(LHS a, RHS b)` and `public static R operator-(T a)`. Return type R is the owned result. (2) For a value type V, the canonical homogeneous arithmetic signature is `public static V operator+(V a, V b)`; mixed forms like `public static V operator*(V a, float32 s)` (and the swapped `(float32 s, V a)` if commutativity is wanted) are declared explicitly — there is NO implicit scalar broadcast for value types. (3) COMPARISON: declare only `operator==` and `operator<`; the compiler DERIVES `!=` (¬==), `>` (swapped <), `>=` (¬<), `<=` (swapped ¬<) via BinaryOpExpression.cpp:715-754. Both `==` and `<` return `bool`. Do not declare the derived four unless a partial order needs them. (4) MUTATING UNARY ++/-- and INDEXED []/[]=: INSTANCE methods, NO static. `operator++()` 0 params return void; `operator[](IdxT i)` 1 param returns element (a borrow tied to receiver); `operator[]=(IdxT i, ElemT v)` 2 params return void. (5) COMPOUND-ASSIGN (+=, -=, ...): omit by default (auto-derived as `a = a + b` from the static binary form); declare an explicit instance `operator+=(RHS b)` (1 param, void) only for in-place mutation. (6) @ValueType operators that must run in kernels are additionally marked @Device and constrained to primitive scalar/vector ops (no alloca/GEP/heap) so they device-inline cleanly. (7) NO templated operators — operator overloads are concrete (a specific LHS type × a specific RHS type). Shape-generic matmul is NOT a user operator: for the compiler-known value types `Matrix`/`Tensor`, `M1 * M2` is **compiler-intrinsic** (the compiler reads `R/K/C` off the concrete operand types and emits matmul, like `Vector`'s operators — no template, no inference machinery). Element-wise multiply is the method `a.hadamard(b)`. User-defined value types use only concrete operator overloads. (8) Operator bodies should be small/pure to honor the AlwaysInline size guard and the by-value/no-side-effect marshalling contract.

## Type index

- **AABB (BoundingBox)** — _now_ — Axis-aligned bounding box for collision detection, spatial queries, and BVH nodes.
- **Color (RGBA)** — _now_ — 4-component color representation (R,G,B,A).
- **Complex<T>** — _now_ — Cartesian complex numbers: a + bi.
- **CooperativeMatrix<T,Rows,Cols,Use>** — _now_ — Device-only tile-sized matrix multiply via hardware matrix cores (WMMA), living distributed across a wavefront's per-invocation registers.
- **DualNumber<T>** — _now_ — Automatic differentiation (forward-mode) for SPELA per-layer local-loss gradients.
- **Float16** — _now_ — IEEE 754 binary16 (16-bit half precision) as a standalone scalar value type.
- **Matrix<T,R,C>** — _now_ — Fixed-size R×C matrix.
- **Ray** — _now_ — Parametric ray for intersection tests, ray casting, visibility queries.
- **Tensor<T,Dims...>** — _now_ — N-dimensional array value type for ML workloads (the workhorse of SPELA/Prism).
- **Transform (Mat4)** — _now_ — Homogeneous 4×4 transformation matrix for 3D graphics.
- **Vector<T,N>** — _now_ — Fixed-width numeric vector with N lanes of element type T, lowering to LLVM <N x T>.
- **BFloat16** — _soon_ — Google's bfloat16 (16-bit brain float) format: 1 sign + 8 exponent + 7 mantissa bits, matching the range of float32 but with coarser mantissa precision.
- **Float8E4M3** — _soon_ — 8-bit floating point, E4M3 format (4-bit exponent, 3-bit mantissa, no implicit bit, no -0).
- **Float8E5M2** — _soon_ — 8-bit floating point, E5M2 format (5-bit exponent, 2-bit mantissa, implicit bit, supports ±∞).
- **Plane** — _soon_ — Planar surface defined by normal + distance from origin.
- **Quaternion<T>** — _soon_ — Unit quaternion for 3-D rotations: w + x*i + y*j + z*k.
- **Rect (Bounds2D)** — _soon_ — 2D axis-aligned rectangle for UI layout, sprite bounds, texture regions, 2D spatial queries.
- **SaturatingInt<T>** — _soon_ — Signed/unsigned integer wrapper that saturates on overflow (a += b caps at T.
- **WrappingInt<T>** — _soon_ — Signed/unsigned integer wrapper with wrapping (two's-complement) arithmetic on overflow.
- **FixedPoint<T, uint32 Frac>** — _later_ — Deterministic fixed-point arithmetic for finance (pennies, cents), physics-based games (deterministic lockstep multiplayer), and embedded ML inference where float rounding non-determinism is unacceptable.
- **Interval<T>** — _later_ — Validated numerics for robust geometric computation, physics simulation, and scientific computing where correctness matters more than speed.
- **Range (Interval)** — _later_ — 1D interval [min, max] for 1D spatial queries, clip planes, animation timelines, parameter ranges.

---

## AABB (BoundingBox)

- **Priority:** now  ·  **Families:** geometric-graphics
- **Type params:** `<T extends Numeric = float32>`
- **Element types:** T: float32 (graphics/ray tracing); int32 (discrete spatial indexing)
- **Purpose:** Axis-aligned bounding box for collision detection, spatial queries, and BVH nodes. Foundation for ray tracing (ray query) and spatial indexing (SpatialIndex).
- **Representation:** Two Vector<T,3> endpoints (minPoint, maxPoint). Distinct type (not generic pair) to enable semantic methods and prevent confusion.
- **GPU notes:** Used as bounding-box geometry in ray-query operations (AccelerationStructure.buildAabbs). Kernel-arg marshalling: two Vector<T,3> fields, passed by value. Device: intersection/overlap queries compile to SIMD min/max. SpatialIndex wraps each datum in an AABB; ray query walks the BVH (ray-box intersection native in RT hardware).
- **Depends on:** Vector<T,3> (foundation exists), Numeric bound (from Vector)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator|` | static-binary | `public static AABB<T> operator\| (AABB<T> a, AABB<T> b)` | Binary-OR semantics: 'a or b' means 'encompass both' |
| `operator&` | static-binary | `public static AABB<T> operator& (AABB<T> a, AABB<T> b)` | Binary-AND semantics: 'a and b' means 'common region' |
| `operator==` | static-binary | `public static boolean operator== (AABB<T> a, AABB<T> b)` | Floating-point AABBs use tolerance |

**Methods:** `public boolean contains(Vector<T,3> point)`, `public boolean containsBox(AABB<T> other)`, `public boolean overlaps(AABB<T> other)`, `public Vector<T,3> center()`, `public Vector<T,3> halfExtent()`, `public Vector<T,3> extent()`, `public T surfaceArea()`, `public T volume()`, `public AABB<T> expand(Vector<T,3> point)`, `public AABB<T> expand(T epsilon)`, `public T distanceToPoint(Vector<T,3> point)`

## Color (RGBA)

- **Priority:** now  ·  **Families:** geometric-graphics
- **Type params:** `no type parameter (fixed float32)`
- **Element types:** float32 (normalized [0,1] per channel, typical); float16 (RGBA16F textures); uint8 (packed RGBA32)
- **Purpose:** 4-component color representation (R,G,B,A). Alias or distinct type for graphics and image processing. Foundation type for texture sampling, framebuffer output, and UI rendering.
- **Representation:** Four color channels (red, green, blue, alpha). Recommendation: distinct type with Vector-like ops, not an alias, to enable semantic methods (sRGB conversion, premultiply).
- **GPU notes:** Texture2D.sample(sampler, u, v) currently extracts lane-0 of <4 x float32> (cajeta-gpu-plan A3/S6 RGBA bridge, deferred). Once that lands, sample() returns Color directly. Color marshalling matches framebuffer formats (float32 RGBA, half RGBA, uint8 RGBA); no marshalling penalty.
- **Depends on:** Vector<float32,4> or distinct type system, Texture2D (for return type of sample())

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Color operator+ (Color a, Color b)` | Semantics depend on context (unclamped for HDR; clamped for UI) |
| `operator*` | static-binary | `public static Color operator* (Color c, float32 scale)` | Scale-channel multiplication, not uniform |
| `operator*` | static-binary | `public static Color operator* (Color a, Color b)` | Common in fragment shaders |
| `operator==` | static-binary | `public static boolean operator== (Color a, Color b)` | Floating-point tolerance recommended |

**Methods:** `public float32 red()`, `public float32 green()`, `public float32 blue()`, `public float32 alpha()`, `public Color withAlpha(float32 newAlpha)`, `public Color toSRgb()`, `public static Color fromSRgb(Color srgbColor)`, `public Color premultiply()`, `public Color unpremultiply()`

## Complex<T>

- **Priority:** now  ·  **Families:** Value Types — Survey Gaps (Operator-Overloaded), numeric-complex
- **Type params:** `<T extends Floating>`
- **Element types:** float16, float32, float64 (element type constrained by <T extends Floating>)
- **Purpose:** Cartesian complex numbers: a + bi. Element type T must be float16, float32, or float64 (Floating bound). Supports arithmetic (+/-/*/÷), conjugate, absolute magnitude, phase angle, conversion to/from polar form.
- **Representation:** Struct with two T fields (real, imag); lowered as aggregate (either two separate SSA values or a packed struct per ABI). By-value on stack for small T. Host and device lowering identical.
- **GPU notes:** Complex<float32> and Complex<float16> vector SIMD-friendly: two LLVM scalars = struct or pair of SSA values. Device lowering mirrors host (no special intrinsics needed; all ops decompose to scalar arithmetic). Vector<Complex<T>, N> is deferred — would be useful for FFT/signal work but requires operator-overload gateway unlocking Complex first. Device math (sin/cos/atan2 for phase) already supported via Math.{sin,cos,atan2} device lowering (xpu-plan B2-Inc1).
- **Depends on:** Floating (marker bound), hash() for structural equality

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Complex<T> operator+ (Complex<T> a, Complex<T> b)` | Standard component-wise addition |
| `operator-` | static-binary | `public static Complex<T> operator- (Complex<T> a, Complex<T> b)` | Standard component-wise subtraction |
| `operator-` | static-unary | `public static Complex<T> operator- (Complex<T> a)` | Unary negation (distinguished by arity) |
| `operator*` | static-binary | `public static Complex<T> operator* (Complex<T> a, Complex<T> b)` | Complex multiplication; matches <T extends Floating> for correctness |
| `operator/` | static-binary | `public static Complex<T> operator/ (Complex<T> a, Complex<T> b)` | Complex division via conjugate; raises exception if b is zero |
| `operator==` | static-binary | `public static boolean operator== (Complex<T> a, Complex<T> b)` | Structural equality; != derived automatically |
| `operator*` | static-binary | `public static Complex<T> operator* (Complex<T> a, T k)` | Scalar multiplication; asymmetric LHS/RHS |
| `operator*` | static-binary | `public static Complex<T> operator* (T k, Complex<T> a)` | Reversed scalar multiplication (requires explicit overload per spec §2) |
| `operator/` | static-binary | `public static Complex<T> operator/ (Complex<T> a, T k)` | Scalar division; k ≠ 0 required |
| `operator- (unary)` | static-unary | `public static Complex<T> operator- (Complex<T> z)` | Arity 1 |

**Methods:** `public Complex<T> conjugate()`, `public T abs()`, `public T absSq()`, `public T phase()`, `toPolar(…)`, `public static Complex<T> fromPolar(T magnitude, T phase)`, `public int64 hash() override`, `public T real()`, `public T imag()`, `public static Complex<T> conj(Complex<T> z)`, `public static T abs(Complex<T> z)`, `public static T absSq(Complex<T> z)`, `public static T arg(Complex<T> z)`, `public static Complex<T> exp(Complex<T> z)`, `public static Complex<T> log(Complex<T> z)`, `public static Complex<T> pow(Complex<T> z, T exponent)`, `public static Complex<T> sin(Complex<T> z)`, `public static Complex<T> cos(Complex<T> z)`

## CooperativeMatrix<T,Rows,Cols,Use>

- **Priority:** now  ·  **Families:** tensor-ml
- **Type params:** `<T extends Numeric, uint32 Rows, uint32 Cols, uint32 Use>`
- **Element types:** float16 (IEEE binary16), float32
- **Purpose:** Device-only tile-sized matrix multiply via hardware matrix cores (WMMA), living distributed across a wavefront's per-invocation registers. Maps to SPIR-V OpTypeCooperativeMatrixKHR. A single fused MMA (multiply-add) is one tile-sized operation on the matrix-core register file. T is the element type (float16, float32); Rows=Cols for v1 square tiles; Use is 0 (MatrixA), 1 (MatrixB), 2 (Accumulator).
- **Representation:** Opaque device-only type (no host layout). At device lowering, alloca a target("spirv.CooperativeMatrixKHR", T, scope, Rows, Cols, Use) in Function storage; each value lives in register file, never heap/addressable memory.
- **GPU notes:** VULKAN-ONLY in v1 (SPV_KHR_cooperative_matrix + SPV_KHR_vulkan_memory_model). Register residency on RDNA3 WMMA cores; bit-exact matmul via hardware fused MMA (CM5 verified on Radeon 8060S). Other backends: clean unsupported diagnostic. No method-templated operators needed — each overload (splat/mma/load/store) specializes on concrete (T, Rows, Cols, Use) from the class instantiation, resolved at device-lowering time by the DeviceLowerer. Device lowering intercepts method calls; no user-visible MMA implementation.
- **Depends on:** Buffer<T> (global device-memory binding), Non-type template parameter substrate (uint32 Rows/Cols/Use constants), float16 element type (IEEE binary16 — remapped from prior bfloat misnomer in CM5a)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `load` | instance-mutating-unary | `public void load(Buffer<T> src, uint32 offset, uint32 layout, uint32 stride)` | Instance method (mutates this in-place with the loaded data). Device lowering emits OpCooperativeMatrixLoadKHR via llvm.spv.cooperative.matrix.load. Storage class is always Global (Buffer<T>) in v1; Workgroup (LDS) load source deferred as CM6 optimization. |
| `splat` | instance-mutating-unary | `public void splat(T value)` | Instance method. Device lowering emits OpCompositeConstruct (single scalar per element). Required for the accumulator Use=2. |
| `mma` | instance-mutating-unary | `public void mma(CooperativeMatrix<T,Rows,Cols,0> a, CooperativeMatrix<T,Rows,Cols,1> b)` | Instance method (mutates this). Device lowering emits OpCooperativeMatrixMulAddKHR via llvm.spv.cooperative.matrix.muladd. No method-templated parameters needed — the overload is resolved by the three distinct types (result, a, b) which flow through substitution/lowering independently. |
| `store` | instance-mutating-unary | `public void store(Buffer<T> dst, uint32 offset, uint32 layout, uint32 stride)` | Instance method. Device lowering emits OpCooperativeMatrixStoreKHR via llvm.spv.cooperative.matrix.store. |

**Methods:** `public void load(Buffer<T> src, uint32 offset, uint32 layout, uint32 stride)`, `public void splat(T value)`, `mma(…)`, `public void store(Buffer<T> dst, uint32 offset, uint32 layout, uint32 stride)`

## DualNumber<T>

- **Priority:** now  ·  **Families:** Value Types — Survey Gaps (Operator-Overloaded)
- **Type params:** `<T extends Floating>`
- **Element types:** float16, float32, float64 (floating-point only; integer autodiff is out of scope)
- **Purpose:** Automatic differentiation (forward-mode) for SPELA per-layer local-loss gradients. A dual number carries both the value and its derivative with respect to one independent variable. Essential for SPELA's closed-form per-layer update — avoids the full reverse-mode autodiff stack that generic deep learning frameworks need.
- **Representation:** Two fields: real (T) and dual (T). Lowers to `struct { T val; T deriv; }` by value. Marshals to two consecutive elements in device buffers.
- **GPU notes:** By-value marshalling as two-field struct; device kernel forward pass uses dual numbers, back-substitutes gradient into per-layer parameter updates. No reverse-mode accumulation graph needed — SPELA's closed-form is the feature. On non-Vulkan (CPU/AMD/NVIDIA, non-ray-query paths): device lowering treats as a simple 2-element composite, no special ops.
- **Depends on:** Vector<T,N> (for broadcasting scalar operations), Math.{sin,cos,exp,log} (device math B2)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static DualNumber<T> operator+ (DualNumber<T> a, DualNumber<T> b)` | Asymmetric LHS/RHS: DualNumber<T> + T (dual-only LHS, scalar RHS) must declare a second overload; scalars on RHS broadcast via (T → DualNumber(T, 0)) |
| `operator-` | static-binary | `public static DualNumber<T> operator- (DualNumber<T> a, DualNumber<T> b)` | Two-operand form; unary negation separate |
| `operator- (unary)` | static-unary | `public static DualNumber<T> operator- (DualNumber<T> a)` | Arity 1 disambiguates from binary - |
| `operator*` | static-binary | `public static DualNumber<T> operator* (DualNumber<T> a, DualNumber<T> b)` | Asymmetric: DualNumber<T> * T (scalar broadcast); scalar * DualNumber<T> requires second overload |
| `operator/` | static-binary | `public static DualNumber<T> operator/ (DualNumber<T> a, DualNumber<T> b)` | b.val != 0 precondition; scalar RHS via broadcast |
| `operator==` | static-binary | `public static boolean operator== (DualNumber<T> a, DualNumber<T> b)` | Structural equality; != derives automatically |
| `operator<` | static-binary | `public static boolean operator< (DualNumber<T> a, DualNumber<T> b)` | >/<=/>=  derive from < |

**Methods:** `public T value()`, `public T deriv()`, `public static DualNumber<T> sin(DualNumber<T> x)`, `public static DualNumber<T> cos(DualNumber<T> x)`, `public static DualNumber<T> exp(DualNumber<T> x)`, `public static DualNumber<T> log(DualNumber<T> x)`, `public static DualNumber<T> pow(DualNumber<T> base, T exponent)`

## Float16

- **Priority:** now  ·  **Families:** numeric-complex
- **Type params:** `(no type parameters — scalar only)`
- **Element types:** N/A (scalar, not parameterized). Can be element type of Vector<float16, N> and Matrix<float16, R, C> once those types exist.
- **Purpose:** IEEE 754 binary16 (16-bit half precision) as a standalone scalar value type. Distinct from a Vector<float16, N>. Supports arithmetic, comparisons, and explicit casting to/from float32/float64. Primary use: ML models (FP16 training/inference), graphics (16-bit vertex attributes, texture formats), and dense tensor storage.
- **Representation:** LLVM `half` type (Type::getHalfTy). Lowered as a scalar in registers or stack. By-value marshalling to kernels. Device lowering: native float arithmetic on modern GPUs (Tesla, RDNA, Vulkan VK_FORMAT_R16_SFLOAT).
- **GPU notes:** Already supported in the type system (CajetaType::FLOAT16_TYPE_ID, llvm::Type::getHalfTy). CM5a (2026-06-04) remapped `float16` from bfloat to IEEE-half to match device cooperative-matrix component types. Device Math intrinsics (B2-Inc1) already lower float16 ops to native instructions. No additional backend work needed; already green on CPU/Vulkan/AMD.
- **Depends on:** none; primitive

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static float16 operator+ (float16 a, float16 b)` | Lower to LLVM fadd (f16 type) |
| `operator-` | static-binary | `public static float16 operator- (float16 a, float16 b)` | Lower to LLVM fsub |
| `operator*` | static-binary | `public static float16 operator* (float16 a, float16 b)` | Lower to LLVM fmul |
| `operator/` | static-binary | `public static float16 operator/ (float16 a, float16 b)` | Lower to LLVM fdiv |
| `operator==` | static-binary | `public static boolean operator== (float16 a, float16 b)` | Lower to LLVM fcmp oeq |
| `operator<` | static-binary | `public static boolean operator< (float16 a, float16 b)` | Lower to LLVM fcmp olt |

**Methods:** `public float32 toFloat32()`, `public float64 toFloat64()`, `public static float16 fromFloat32(float32 x, RoundingMode mode = NEAREST_EVEN)`, `public boolean isNaN()`, `public boolean isInfinite()`, `public boolean isFinite()`

## Matrix<T,R,C>

- **Priority:** now  ·  **Families:** geometric-graphics, linear-algebra, tensor-ml
- **Type params:** `<T extends Numeric, uint32 R, uint32 C>`
- **Element types:** non-bool numeric primitives (same as Vector<T,N>)
- **Purpose:** Fixed-size R×C matrix. Value type for small linear algebra (3×3 rotations, 4×4 transforms). Proposed flat vector representation <R*C x T> or array-of-rows Vector<T,C>[R]. By-value kernel marshalling, register residency on GPU (proposed).
- **Representation:** Flat `<R*C x T>` LLVM vector type (or array-of-vectors), by-value POD marshalling. Kernel-arg marshalling identical to Vector — register residency, guaranteed inlining, no memory traffic.
- **GPU notes:** Planned register residency if flat <R*C x T>. By-value kernel marshalling (all-primitive fields, POD). Device lowering: flat-vector ops or row-loop inlining. CooperativeMatrix is separate device-only type (matrix-core register file, not a host value type).
- **Depends on:** Numeric, Vector<T,C> (for row access), linalg library

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Matrix<T,R,C> operator+ (Matrix<T,R,C> a, Matrix<T,R,C> b)` | Same shape matrix on both sides; no broadcasting in v1 |
| `operator-` | static-binary | `public static Matrix<T,R,C> operator- (Matrix<T,R,C> a, Matrix<T,R,C> b)` | Same-shape requirement |
| `operator* (scalar)` | static-binary | `public static Matrix<T,R,C> operator* (Matrix<T,R,C> m, T k)` | Returns fresh matrix |
| `operator* (scalar-left)` | static-binary | `public static Matrix<T,R,C> operator* (T k, Matrix<T,R,C> m)` | Two-overload pattern |
| `operator*` | static-binary | `public static Matrix<T,R,N> operator* (Matrix<T,R,K> a, Matrix<T,K,C> b)` | K-GENERIC requires method-templated operators (forbidden in OperatorOverloading.md 8). **CANNOT BE EXPRESSED TODAY**. Deferred; workaround: static matmul(a,b) method instead |
| `operator/ (element-wise)` | static-binary | `public static Matrix<T,R,C> operator/ (Matrix<T,R,C> a, Matrix<T,R,C> b)` | Distinct from matrix inverse (which is a method) |
| `operator-` | static-unary | `public static Matrix<T,R,C> operator- (Matrix<T,R,C> m)` | Returns fresh matrix |
| `operator== != < <= > >=` | static-binary | `public static Matrix<boolean,R,C> operator<cmp> (Matrix<T,R,C> a, Matrix<T,R,C> b)` | **Per-lane `<R*C x i1>` mask** (value-type comparison rule), NOT a reduced boolean. Same-shape matrix or broadcast scalar RHS. Whole-matrix equality is `(a == b).all()`; blend with `.select(a,b)`. See `MaskSelect.md`. |
| `operator[]` | instance-index-read | `public Vector<T,C> operator[] (uint32 rowIdx)` | Single-index returns i-th row. Multi-index m[r,c] deferred to future language extension (OperatorOverloading.md 5 'Future extension') |
| `operator[]=` | instance-index-write | `public void operator[]= (uint32 rowIdx, Vector<T,C> row)` | Accepts whole Vector<T,C> as new row; requires mutable borrow |
| `operator*` | static-binary | `public static Matrix<T,R,C2> operator* (Matrix<T,R,C1> a, Matrix<T,C1,C2> b)` | Method-templated — **BLOCKING GAP**. Same decision needed as Tensor<T,...> matmul. On Vulkan with matching CooperativeMatrix tile sizes, device lowering targets hardware MMA. |
| `operator*` | static-binary | `public static Matrix<T,R,C> operator* (Matrix<T,R,C> m, T scalar)` | Static binary (scalar broadcast). |
| `operator[]` | instance-index-read | `public T operator[] (int32 i)` | Instance. Deferred: 2-D index syntax m[r][c] or m[r,c] returning Vector<T,C> (a row). |
| `operator[]=` | instance-index-write | `public void operator[]= (int32 i, T value)` | Instance (mutating). |
| `operator*` | static-binary | `public static Matrix<T,R,K> operator* (Matrix<T,R,C> a, Matrix<T,C,K> b)` | BLOCKED: method-templated operators forbidden per OperatorOverloading.md §8. Workaround: explicit overloads for (2×2)·(2×2), (3×3)·(3×3), (4×4)·(4×4), or surface as matmul(a,b) free function. |
| `operator*` | static-binary | `public static Matrix<T,R,C> operator* (T scalar, Matrix<T,R,C> m)` | Two-overload pattern per OperatorOverloading.md §2 |
| `operator/` | static-binary | `public static Matrix<T,R,C> operator/ (Matrix<T,R,C> m, T scalar)` | Scalar broadcast division |
| `operator[]` | instance-index-read | `public Vector<T,C> operator[] (uint32 row)` | Single-index access; multi-index [r,c] deferred |
| `operator[]=` | instance-index-write | `public void operator[]= (uint32 row, Vector<T,C> value)` | Requires mutable borrow of receiver |

**Methods (live):** `transpose()`, `identity()` (square), `row(i)`, `col(j)`, `hadamard(other)`. **Mask** (on a `Matrix<boolean,R,C>` from a comparison): `all()`/`any()` → `boolean`, `select(whenTrue, whenFalse)` → per-lane blend (`MaskSelect.md`). **Deferred:** `inverse()`, `determinant()`, `norm()`, `matmul()` method form (use the `*` intrinsic).

## Ray

- **Priority:** now  ·  **Families:** geometric-graphics
- **Type params:** `<T extends Floating = float32>`
- **Element types:** T: float32 (graphics); float16 (low-precision approximation)
- **Purpose:** Parametric ray for intersection tests, ray casting, visibility queries. Used in graphics (picking, shadows) and science (Monte-Carlo transport). Foundation type for ray-query operations.
- **Representation:** Origin (Vector<T,3>) + Direction (Vector<T,3> unit normalized). Parametric: P(t) = origin + t*direction, t ∈ [tMin, tMax].
- **GPU notes:** Primarily a host type for camera/graphics setup and spatial-query construction. Device-side: RayQuery (kernel-local opaque) represents traversal state. Ray host type marshals scalar components into RayQuery.initialize(). Origin/direction normalized on host; kernel does not re-normalize.
- **Depends on:** Vector<T,3> (foundation exists), AABB<T> (for intersection testing)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator==` | static-binary | `public static boolean operator== (Ray<T> a, Ray<T> b)` | Floating-point tolerance recommended |

**Methods:** `public Ray(Vector<T,3> origin, Vector<T,3> direction, T tMin, T tMax)`, `public Vector<T,3> pointAt(T t)`, `public Ray<T> normalize()`, `public Vector<T,3> getOrigin()`, `public Vector<T,3> getDirection()`, `public T distanceToPoint(Vector<T,3> point)`, `public boolean intersectAABB(AABB<T> box)`

## Tensor<T,Dims...>

- **Priority:** now  ·  **Families:** tensor-ml
- **Type params:** `<T extends Numeric, Dims... extends uint32>`
- **Element types:** float16 (IEEE binary16), float32, float64, int8, int16, int32, int64; bfloat16 (future distinct keyword, distinct from float16); fp8 E4M3 / E5M2 (future, stored as i8, semantics via runtime helpers)
- **Purpose:** N-dimensional array value type for ML workloads (the workhorse of SPELA/Prism). Parameterized by element type T and shape dimensions (Dims = compile-time shape constants, e.g., Tensor<float32, 64, 32, 16> for a 3-D 64×32×16 tensor). Represents dense numeric data: matrices, batched matrices, feature maps, activations. On host: stack/heap allocation, element/slice access, elementwise + reductions. On device: GPU register/shared/global residency, cooperative-matrix binding, matmul/conv kernels.
- **Representation:** By-value POD on host (small fixed-shape tensor: flat <R*C*D x T> on stack or heap; the representation is an implementation detail, not part of the surface). On device: kernel-local allocations (Private for register, Shared for LDS, Global for buffers via Buffer<T>). No vtable; no heap indirection for shape metadata (compile-time-constant dims).
- **GPU notes:** On Vulkan compute kernels, a small Tensor passed by value marshals as a POD struct (field-by-field) to a descriptor-SSBO (read-only); large tensors are Buffer<T> parameters (bound as descriptors). A Tensor operand in a @Kernel is device-only (no host stub). Device lowering: elementwise ops → vectorized LLVM ops (fmul, fsub, etc.) that map to SIMD/GPU vector instructions per element type. Matmul on compatible device shapes targets CooperativeMatrix when available (Vulkan); fallback is a generic tiling loop. Device lowering does NOT inline arbitrary user operator-method calls inside kernels — only cooperative-matrix/ray-query method bodies are intercepted/lowered.
- **Depends on:** Element type T (Numeric primitives + float16/bfloat16/fp8), Non-type template parameter substrate (compile-time uint32 dimension constants), Method-templated static operators (BLOCKED; design decision needed for matmul), Multi-index operator[i,j] syntax (blocked on lexer pass per OperatorOverloading.md)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Tensor<T,Dims...> operator+ (Tensor<T,Dims...> a, Tensor<T,Dims...> b)` | Static binary. Returns a fresh Tensor. Operands must have identical shape at compile time; dimension mismatch rejected at type-check. Device: lowers to vectorized fmul/fadd or integer add per backend (no cooperative-matrix path for non-MMA ops). |
| `operator-` | static-binary | `public static Tensor<T,Dims...> operator- (Tensor<T,Dims...> a, Tensor<T,Dims...> b)` | Static binary. |
| `operator-` | static-unary | `public static Tensor<T,Dims...> operator- (Tensor<T,Dims...> v)` | Static unary (arity=1). |
| `operator*` | static-binary | `public static Tensor<T,DimA,DimK> operator* (Tensor<T,DimA,DimK> a, Tensor<T,DimK,DimB> b)` | Method-templated operators FORBIDDEN per OperatorOverloading.md §8/§14. This is a **blocking gap**: the natural expression `a * b` where K varies per call cannot be expressed today because the overload signature `operator*(Tensor<T,R,K>, Tensor<T,K,C>) -> Tensor<T,R,C>` requires a K type parameter *in the method signature*, not the class. **DECISION REQUIRED**: (a) extend the grammar to permit method-templated static operators on non-templated class (allowing `public static` with a template parameter disjoint from the class's own); (b) rename to `matmul(a, b)` method-style call; or (c) accept the limitation and document `@TensorOps.matmul` as the idiom. Recommend (a) or (b). GPU device lowering: on Vulkan targets an appropriately-sized CooperativeMatrix MMA subgrid; on CPU a GEneric tiling loop. Dynamic rank not supported v1; static rank of 2 required. |
| `operator*` | static-binary | `public static Tensor<T,Dims...> operator* (Tensor<T,Dims...> v, T scalar)` | Static binary (asymmetric LHS/RHS). Scalar broadcast via vecops::splat/coerceArithPair at each lane. |
| `operator/` | static-binary | `public static Tensor<T,Dims...> operator/ (Tensor<T,Dims...> v, T scalar)` | Static binary. |
| `operator==` | static-binary | `public static Tensor<boolean,Dims...> operator== (Tensor<T,Dims...> a, Tensor<T,Dims...> b)` | Static binary. Returns boolean tensor. Derivation: operator!= auto-derived as negation of ==. |
| `operator[]` | instance-index-read | `public T operator[] (int32 i)` | Instance method. Single index (flat order) in v1. Multi-index m[r,c] deferred to future extension (requires lexer work to distinguish multi-index from comma-expr per OperatorOverloading.md §5). Multi-dimensional indexing syntax is a **noted gap** — documented as needing a lexer pass to emit distinct tokens. |
| `operator[]=` | instance-index-write | `public void operator[]= (int32 i, T value)` | Instance method (mutating). Flat order. Multi-index deferred. |

**Methods:** `public T sum() — or — public Tensor<T,OtherDims...> sum(uint32 axis)`, `public T mean() — or — public Tensor<T,OtherDims...> mean(uint32 axis)`, `public T max() — or — public Tensor<T,OtherDims...> max(uint32 axis)`, `public T min() — or — public Tensor<T,OtherDims...> min(uint32 axis)`, `public Tensor<T,DimC,DimA,DimB> transpose(uint32 axes...)`, `public Tensor<T,R,C> matmul(Tensor<T,K,C> other)`, `public Tensor<T,NewDims...> reshape(NewDims... shape)`

## Transform (Mat4)

- **Priority:** now  ·  **Families:** geometric-graphics
- **Type params:** `no type parameter (fixed float32)`
- **Element types:** float32 (fixed, not parameterized)
- **Purpose:** Homogeneous 4×4 transformation matrix for 3D graphics. Translation, rotation, scale, perspective. Foundation for camera matrices, object poses, viewport transforms.
- **Representation:** 4×4 row-major float32 matrix = Matrix<float32,4,4>. Reuses Matrix flat <16 x float32> representation.
- **GPU notes:** Transform is specialization of Matrix<float32,4,4>; no separate lowering. Vertex shaders use Transforms for model/view/projection matrices. By-value kernel-arg: passed as <16 x float32>. Construct functions synthesized at compile-time (host JIT); device kernels consume by value.
- **Depends on:** Matrix<float32,4,4> (Transform is alias/wrapper), Vector<float32,3>, Vector<float32,4> (used in operator* signatures), Quaternion<float32> (extractRotation output)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator*` | static-binary | `public static Transform operator* (Transform a, Transform b)` | Applies a then b: (a*b)*v = a*(b*v). Non-commutative. |
| `operator*` | static-binary | `public static Vector<float32,3> operator* (Transform t, Vector<float32,3> p)` | Point = (x,y,z,1). Handles translation + rotation + perspective. |
| `operator*` | static-binary | `public static Vector<float32,4> operator* (Transform t, Vector<float32,4> p)` | Variant for custom w; used in graphics pipelines |
| `operator==` | static-binary | `public static boolean operator== (Transform a, Transform b)` | Tolerance recommended; exact equality rarely meaningful |

**Methods:** `public Transform inverse()`, `public Transform transpose()`, `public static Transform translation(float32 x, float32 y, float32 z)`, `public static Transform rotationX(float32 angleRadians)`, `public static Transform rotationY(float32 angleRadians)`, `public static Transform rotationZ(float32 angleRadians)`, `public static Transform scale(float32 x, float32 y, float32 z)`, `perspective(…)`, `orthographic(…)`, `lookAt(…)`, `public Vector<float32,3> extractTranslation()`, `public Quaternion<float32> extractRotation()`

## Vector<T,N>

- **Priority:** now  ·  **Families:** linear-algebra
- **Type params:** `<T extends Numeric, uint32 N>`
- **Element types:** non-bool numeric primitives: int8, int16, int32, int64, float32, float64, float16, bfloat16, float8e4m3, and other fp dtypes
- **Purpose:** Fixed-width numeric vector with N lanes of element type T, lowering to LLVM <N x T>. PRIMITIVE_FLAG|VECTOR_FLAG value type: passed by value, no heap, no vtable. Element-wise arithmetic, component/index access, dot/length/normalize geometry helpers.
- **Representation:** LLVM FixedVectorType <N x T>, flat SSA value, register-resident on GPU. By-value kernel-arg marshalling (KernelArgTrait: PRIMITIVE_FLAG admits by value).
- **GPU notes:** Register residency: <N x T> flat SSA, no memory traffic. Native SIMD/GPU vector instructions. By-value kernel marshalling (PRIMITIVE_FLAG). Device lowering: KernelLowering.cpp intercepts known types, emits intrinsic IR. Guaranteed inlining. Live: CPU JIT, Vulkan, AMD, NVIDIA (emit-only).
- **Depends on:** Numeric (element type), vecops (VectorOps.h helpers)

> **Status (2026-06-05) — Vector operators are COMPILER INTRINSICS, not declared overloads.**
> `Vector<T,N>` is a bare `CajetaVector` carrying `PRIMITIVE_FLAG | VECTOR_FLAG`, so it is
> excluded from the operator-dispatch gate on both counts (`!CajetaClass` and `PRIMITIVE_FLAG`).
> Its `+ - * / == []` are intercepted in `BinaryOpExpression`/`ArrayIndexExpression` (host) and
> `KernelLowering` (device) and emitted as flat `<N x T>` SSA — **the signatures in the table
> below are documentary** (the canonical surface a future declared form would expose), not source
> the compiler reads. Consequences:
> - Vector is the **register-residency reference** the `@ValueType` mechanism preserves: emitted
>   IR is byte-identical to the pre-mechanism intrinsic path, pinned by the S0 golden oracle
>   (`test/expression/VectorHostGoldenIrTests.cpp` host + `XpuVectorDeviceTests` device).
> - Vector's `operator[]` read and `operator[]=` element-write are intrinsic mutable-vector ops
>   and are **exempt from the S3 `@ValueType` mutating-operator ban** — that ban
>   (`CAJETA_ERROR_VALUE_TYPE_MUTATING_OPERATOR`) forbids *declaring* a mutating operator on a
>   by-value `@ValueType` **CajetaClass**; Vector is not one, and `v[i] = x` mutates the value's
>   inline slot directly via `insertelement`.
> - **Synthesizing declared Vector operator signatures** (making the surface above real source
>   backed by the interception, gate still bypassing `CajetaVector`) is **deferred**
>   (documentary-first, plan Decision #5). The intrinsic path is the implementation; a declared
>   façade would be ergonomic sugar only.

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Vector<T,N> operator+ (Vector<T,N> a, Vector<T,N> b)` | Lowers to <N x T> fadd or add per element type (VectorOps.h) |
| `operator-` | static-binary | `public static Vector<T,N> operator- (Vector<T,N> a, Vector<T,N> b)` | Lowers to <N x T> fsub or sub |
| `operator* (scalar)` | static-binary | `public static Vector<T,N> operator* (Vector<T,N> v, T k)` | Lowers to vecops::splat(k,N) then fmul/mul; implemented via BinaryOpExpression.cpp coerceArithPair |
| `operator* (reversed)` | static-binary | `public static Vector<T,N> operator* (T k, Vector<T,N> v)` | Two-overload pattern avoids reverse dispatch |
| `operator/` | static-binary | `public static Vector<T,N> operator/ (Vector<T,N> a, Vector<T,N> b)` | Signedness flag inherited from element type (CajetaVector.cpp:54) |
| `operator-` | static-unary | `public static Vector<T,N> operator- (Vector<T,N> v)` | Lowers to <N x T> fneg or sub-from-zero |
| `operator== != < <= > >=` | static-binary | `public static Vector<boolean,N> operator<cmp> (Vector<T,N> a, Vector<T,N> b)` | **Per-lane `<N x i1>` mask** (value-type comparison rule), NOT a reduced boolean. Scalar RHS broadcasts. Reduce with `.all()`/`.any()`, blend with `.select(a,b)`. See `MaskSelect.md`. |
| `operator[]` | instance-index-read | `public T operator[] (uint32 i)` | Instance form per OperatorOverloading.md 5; i must be <N |
| `operator[]=` | instance-index-write | `public void operator[]= (uint32 i, T value)` | Requires mutable borrow at call site; v is mutable borrow receiver |

**Component / swizzle reads:** `.x/.y/.z/.w` (and `.r/.g/.b/.a`) read one lane; a 2-4 letter **swizzle** `.xy`/`.xyz`/`.xxyy`/`.zyx` reads a `Vector<T,M>` (repeats allowed; lowers to `shufflevector`). Out-of-range letters → `CAJETA_ERROR_VECTOR_COMPONENT`. Swizzle *writes* (`v.xy = …`) deferred.

**Methods:**
- geometry/math: `dot(other)`, `length()`, `normalize()`; `cross(other)` (3-D), `reflect(n)`, `refract(n, eta)`, `distance(other)`; `min(b)`, `max(b)`, `clamp(lo, hi)`, `lerp(b, t)` — *float element, v1; integer min/max deferred*.
- **mask** (on a `Vector<boolean,N>` from a comparison): `all()`/`any()` → `boolean`, `select(whenTrue, whenFalse)` → per-lane blend. **Masks are register-only** — `Buffer<Vector<boolean,N>>` / bool-vector kernel args stay ABI-rejected.

## BFloat16

- **Priority:** soon  ·  **Families:** numeric-complex
- **Type params:** `(no type parameters — scalar only)`
- **Element types:** N/A (scalar). Can be element type of Vector<bfloat16, N> and Matrix<bfloat16, R, C>.
- **Purpose:** Google's bfloat16 (16-bit brain float) format: 1 sign + 8 exponent + 7 mantissa bits, matching the range of float32 but with coarser mantissa precision. Used in TPU training, some NVIDIA tensor-core workflows, and quantized LLM inference. Distinct from IEEE float16 (which has 5 exp + 10 mantissa).
- **Representation:** LLVM `bfloat` type (Type::getBFloatTy). Lowered as a scalar. By-value marshalling. Device: native on recent NVIDIA (NVIDIA compute ≥8.0, sm_90), AMD, and TPUs; emulated on older/other targets via float32 → truncate.
- **GPU notes:** LLVM support: Type::getBFloatTy exists; arithmetic intrinsics map to bf16 fcmp/fadd/etc. Device math (xpu-plan C2, Tier-2) — port OpenCL-flavor bfloat16 atomics/ops to Vulkan Shader flavor (SPV_KHR_bfloat16 capability). Deferred: bfloat16 arithmetic is Tier-2 (in LLVM but not Shader-flavor-exposed); needs SPIR-V backend port. Cooperative matrix CM5c only exposes f16/f16→f32 config on current hardware; bfloat16 configs may surface on newer hardware and will need device support. For now, treat as 'language keyword planned, type system pending'.
- **Depends on:** none; primitive (deferred in type system until keyword lands)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static bfloat16 operator+ (bfloat16 a, bfloat16 b)` | Lower to LLVM fadd (bf16) |
| `operator-` | static-binary | `public static bfloat16 operator- (bfloat16 a, bfloat16 b)` | Lower to LLVM fsub |
| `operator*` | static-binary | `public static bfloat16 operator* (bfloat16 a, bfloat16 b)` | Lower to LLVM fmul |
| `operator/` | static-binary | `public static bfloat16 operator/ (bfloat16 a, bfloat16 b)` | Lower to LLVM fdiv |
| `operator==` | static-binary | `public static boolean operator== (bfloat16 a, bfloat16 b)` | Lower to LLVM fcmp oeq (bf16) |
| `operator<` | static-binary | `public static boolean operator< (bfloat16 a, bfloat16 b)` | Lower to LLVM fcmp olt |

**Methods:** `public float32 toFloat32()`, `public float64 toFloat64()`, `public static bfloat16 fromFloat32(float32 x, RoundingMode mode = NEAREST_EVEN)`, `public float16 toFloat16(RoundingMode mode = NEAREST_EVEN)`

## Float8E4M3

- **Priority:** soon  ·  **Families:** numeric-complex
- **Type params:** `(no type parameters — scalar only)`
- **Element types:** N/A (scalar). Element type of Vector<float8e4m3, N> useful for dense tensor storage and training.
- **Purpose:** 8-bit floating point, E4M3 format (4-bit exponent, 3-bit mantissa, no implicit bit, no -0). Used in very-low-precision ML training (especially with stochastic rounding and gradient clipping), quantized LLM inference, and dense tensor formats (OCP Tensor Float 32 extended). Narrower range than float8e5m2 but finer precision near zero.
- **Representation:** Opaque 8-bit type (registered as FLOAT8E4M3_TYPE_ID in CajetaType). Lowered to i8 storage, arithmetic via float32 (load as f32, compute, truncate back). Device: LLVM backend has SPIRVSubtarget float8 modes; emulate on CPU via C intrinsics.
- **GPU notes:** Device: SPIR-V backend has SPV_KHR_float_controls + float8 modes; NVIDIA libdevice has fp8 math; AMD OCml supports fp8 GEMM. CPU: intrinsic C quantization routines via llvm.convert.to.fp8. Priority: xpu-plan C2 Tier-3 — no LLVM backend support yet (E4M3 is not in the current SPIR-V backend). Deferred pending upstream LLVM fp8 work (LLVM 25+?). For now, type skeleton exists in runtime (boxed Float8E4M3 per CajetaMath.md), but primitive float8e4m3 scalar and arithmetic operators land when the type system + device lowering infrastructure exist (Part B1/B2 in xpu-plan).
- **Depends on:** RoundingMode (for casting); cajeta.math.math for quantization helpers

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static float8e4m3 operator+ (float8e4m3 a, float8e4m3 b)` | Compute in f32, then quantize; rounding mode inherited from context or default NEAREST_EVEN |
| `operator-` | static-binary | `public static float8e4m3 operator- (float8e4m3 a, float8e4m3 b)` | Saturates to float8e4m3 range on overflow |
| `operator*` | static-binary | `public static float8e4m3 operator* (float8e4m3 a, float8e4m3 b)` | Key operation for low-precision training; rounding mode critical |
| `operator/` | static-binary | `public static float8e4m3 operator/ (float8e4m3 a, float8e4m3 b)` | Raises exception if b == 0 |
| `operator==` | static-binary | `public static boolean operator== (float8e4m3 a, float8e4m3 b)` | NaN ≠ NaN by IEEE convention, but bit patterns differ so == is false |

**Methods:** `public float32 toFloat32()`, `public float64 toFloat64()`, `fromFloat32(…)`, `public float8e4m3 saturatingAdd(float8e4m3 other)`, `public float8e4m3 saturatingSub(float8e4m3 other)`, `public float8e4m3 saturatingMul(float8e4m3 other)`, `public boolean isNaN()`, `public boolean isInfinite()`, `public boolean isFinite()`

## Float8E5M2

- **Priority:** soon  ·  **Families:** numeric-complex
- **Type params:** `(no type parameters — scalar only)`
- **Element types:** N/A (scalar). Element type of Vector<float8e5m2, N>.
- **Purpose:** 8-bit floating point, E5M2 format (5-bit exponent, 2-bit mantissa, implicit bit, supports ±∞). Used in quantized neural networks where range matters more than mantissa precision, and in OCP Tensor Float 32. Complements Float8E4M3 (wider range, coarser precision).
- **Representation:** Opaque 8-bit type (FLOAT8E5M2_TYPE_ID). Stored as i8, arithmetic via float32 upconvert/downconvert, same path as E4M3.
- **GPU notes:** Deferred as per Float8E4M3 — C2 Tier-3. Device support available on NVIDIA sm_90, AMD RDNA, via downstream LLVM patches. Host: C intrinsic quantization.
- **Depends on:** RoundingMode

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static float8e5m2 operator+ (float8e5m2 a, float8e5m2 b)` | Rounding mode applies to downconversion |
| `operator-` | static-binary | `public static float8e5m2 operator- (float8e5m2 a, float8e5m2 b)` | Same rounding model as all float8 ops |
| `operator*` | static-binary | `public static float8e5m2 operator* (float8e5m2 a, float8e5m2 b)` | Overflow saturates to ±∞ |
| `operator/` | static-binary | `public static float8e5m2 operator/ (float8e5m2 a, float8e5m2 b)` | b == 0 → ±∞ (standard IEEE infinity behavior) |
| `operator==` | static-binary | `public static boolean operator== (float8e5m2 a, float8e5m2 b)` | NaN ≠ NaN |

**Methods:** `public float32 toFloat32()`, `public float64 toFloat64()`, `fromFloat32(…)`, `public float8e4m3 toFloat8E4M3(RoundingMode mode = NEAREST_EVEN)`, `public boolean isInfinite()`, `public boolean isNaN()`, `public boolean isFinite()`

## Plane

- **Priority:** soon  ·  **Families:** geometric-graphics
- **Type params:** `<T extends Floating = float32>`
- **Element types:** T: float32 (typical)
- **Purpose:** Planar surface defined by normal + distance from origin. Used in visibility testing, clipping, collision detection. Graphics-adjacent (frustum culling).
- **Representation:** Normal vector (Vector<T,3> unit normalized) + scalar distance (d). Implicit form: normal · (point - origin) = d.
- **GPU notes:** Host/graphics type for camera frustum setup and geometric predicates. Device: used in fragment shaders for lighting/visibility (compute signed distance via dot product). No special device lowering; by-value struct (normal + scalar).
- **Depends on:** Vector<T,3> (foundation exists)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator==` | static-binary | `public static boolean operator== (Plane<T> a, Plane<T> b)` | Floating-point tolerance recommended |

**Methods:** `public Plane(Vector<T,3> normal, T distance)`, `public T signedDistance(Vector<T,3> point)`, `public Vector<T,3> project(Vector<T,3> point)`, `public Vector<T,3> reflect(Vector<T,3> vector)`, `public Vector<T,3> getNormal()`, `public T getDistance()`

## Quaternion<T>

- **Priority:** soon  ·  **Families:** Value Types — Survey Gaps (Operator-Overloaded), geometric-graphics, linear-algebra, tensor-ml
- **Type params:** `<T extends Numeric>`
- **Element types:** numeric primitives T: float32, float64 (float-only for most methods)
- **Purpose:** Unit quaternion for 3-D rotations: w + x*i + y*j + z*k. By-value semantics. Composition via operator*, vector rotation, interpolation (slerp/nlerp).
- **Representation:** Four fields (normalized): w (scalar), x, y, z (vector). Lowers to `struct { T w; T x; T y; T z; }` or `[4 x T]` by value. Can use Vector<T,4> as backing for swizzle-friendly layouts.
- **GPU notes:** Register residency: {T,T,T,T} = 4 floats = 16 bytes, small SSA aggregate. By-value kernel marshalling (4 numeric fields, POD). Device lowering: methods inline to vector/scalar ops. Used in graphics, robotics, game physics. Unrelated to CooperativeMatrix (device-only matrix core).
- **Depends on:** Numeric, Vector<T,3>, Matrix<T,3,3>, Math (sin, cos, sqrt, atan2)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator* (composition)` | static-binary | `public static Quaternion<T> operator* (Quaternion<T> a, Quaternion<T> b)` | Implements quaternion product rule |
| `operator* (rotate vector)` | static-binary | `public static Vector<T,3> operator* (Quaternion<T> q, Vector<T,3> v)` | Applies the rotation represented by q: q · v · q^(-1); distinct from quaternion composition |
| `operator==` | static-binary | `public static boolean operator== (Quaternion<T> a, Quaternion<T> b)` | Returns single boolean |
| `operator-` | static-unary | `public static Quaternion<T> operator- (Quaternion<T> q)` | Distinct from conjugate in intent, same mathematical result |
| `operator*` | static-binary | `public static Quaternion<T> operator* (Quaternion<T> q1, Quaternion<T> q2)` | Static binary. Non-commutative (q1*q2 ≠ q2*q1 in general). Device: lowers to 16 fmul + 4 fadd per the quaternion product formula. |
| `operator*` | static-binary | `public static Quaternion<T> operator* (Quaternion<T> p, Quaternion<T> q)` | Quaternion×Vector rotation requires separate overload: `Quaternion<T> * Vector<T,3>` returns rotated vector (no fourth component). Quaternion·Quaternion conjugate (inverse for unit quats) is p* = (w, -x, -y, -z) |
| `operator+` | static-binary | `public static Quaternion<T> operator+ (Quaternion<T> p, Quaternion<T> q)` | Not the group operation (that's *); used for linear interpolation setup |
| `operator-` | static-binary | `public static Quaternion<T> operator- (Quaternion<T> p, Quaternion<T> q)` | Rarely used standalone; for lerp/slerp |
| `operator==` | static-binary | `public static boolean operator== (Quaternion<T> p, Quaternion<T> q)` | Structural; note p and -p represent the same rotation but compare !=. A tolerance-based approxEqual(p,q,eps) is separate. |

**Methods:** `public static Quaternion<T> identity()`, `public static Quaternion<T> fromAxisAngle(Vector<T,3> axis, T angleRadians)`, `public static Quaternion<T> fromEuler(T roll, T pitch, T yaw)`, `public static Quaternion<T> fromMatrix(Matrix<T,3,3> m)`, `public static Quaternion<T> fromTwoVectors(Vector<T,3> from, Vector<T,3> to)`, `public Quaternion<T> inverse()`, `public Quaternion<T> conjugate()`, `public Quaternion<T> normalize()`, `public T norm()`, `public T dot(Quaternion<T> other)`, `public Matrix<T,3,3> toMatrix()`, `public Vector<T,3> toEuler()`, `public static Quaternion<T> slerp(Quaternion<T> a, Quaternion<T> b, T t)`, `public static Quaternion<T> nlerp(Quaternion<T> a, Quaternion<T> b, T t)`, `public T magnitude()`, `public T angle()`, `public static Quaternion<T> conjugate(Quaternion<T> q)`, `public static Quaternion<T> inverse(Quaternion<T> q)`, `public static T norm(Quaternion<T> q)`, `public static Quaternion<T> normalize(Quaternion<T> q)`, `public static T dot(Quaternion<T> p, Quaternion<T> q)`, `public static Quaternion<T> slerp(Quaternion<T> p, Quaternion<T> q, T t)`, `public static Vector<T,3> rotateVector(Quaternion<T> q, Vector<T,3> v)`, `public static Matrix<T,3,3> toMatrix(Quaternion<T> q)`

## Rect (Bounds2D)

- **Priority:** soon  ·  **Families:** geometric-graphics
- **Type params:** `<T extends Numeric = float32>`
- **Element types:** T: float32 (continuous UI/graphics); uint32 (pixel-grid texture regions); int32 (discrete spatial indexing)
- **Purpose:** 2D axis-aligned rectangle for UI layout, sprite bounds, texture regions, 2D spatial queries. Foundation for UI rendering and 2D graphics.
- **Representation:** Two Vector<T,2> endpoints (minPoint, maxPoint). Analogous to AABB but in 2D.
- **GPU notes:** 2D graphics-only type (UI, sprite rendering, texture atlasing). Host-side construction; device use optional (2D compute shaders). Marshalled as two Vector<T,2> by value.
- **Depends on:** Vector<T,2> (foundation exists), Numeric bound (from Vector)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator|` | static-binary | `public static Rect<T> operator\| (Rect<T> a, Rect<T> b)` | Analogous to AABB union |
| `operator&` | static-binary | `public static Rect<T> operator& (Rect<T> a, Rect<T> b)` | Element-wise max(min)/min(max) |
| `operator==` | static-binary | `public static boolean operator== (Rect<T> a, Rect<T> b)` | Standard equality semantics |

**Methods:** `public boolean contains(Vector<T,2> point)`, `public boolean containsRect(Rect<T> other)`, `public boolean overlaps(Rect<T> other)`, `public Vector<T,2> center()`, `public Vector<T,2> extent()`, `public T width()`, `public T height()`, `public T area()`, `public Rect<T> expand(Vector<T,2> point)`, `public Rect<T> inset(T amount)`

## SaturatingInt<T>

- **Priority:** soon  ·  **Families:** numeric-complex
- **Type params:** `<T extends Integral>`
- **Element types:** int8, int16, int32, int64, uint8, uint16, uint32, uint64 (element type constrained by <T extends Integral>)
- **Purpose:** Signed/unsigned integer wrapper that saturates on overflow (a += b caps at T.MAX if overflow, floors at T.MIN on underflow). Useful for graphics (clamped color channels), signal processing (fixed-point saturation), and neural-network quantization where wrapping would corrupt semantics. Distinct from wrapping-arithmetic variants.
- **Representation:** Wrapper around T (int8/16/32/64 or corresponding unsigned). Stored by value. Host lowering: checked arithmetic + conditional MAX/MIN assignment. Device: use LLVM saturating arithmetic intrinsics (llvm.sadd.sat / llvm.uadd.sat on targets that support them; fallback to scalar clamp).
- **GPU notes:** Requires device-lowering knowledge of saturating intrinsics. NVPTX (`llvm.nvvm.sad.*`), AMD (`llvm.amdgcn.add.sat`), and SPIR-V (none standard; emulate via checked arithmetic) all differ. Deferred until device math (xpu-plan B2) standardizes the lowering surface.
- **Depends on:** Integral (marker bound)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static SaturatingInt<T> operator+ (SaturatingInt<T> a, SaturatingInt<T> b)` | Host: overflow check + conditional MIN/MAX; Device: llvm.sadd.sat / llvm.uadd.sat where available |
| `operator-` | static-binary | `public static SaturatingInt<T> operator- (SaturatingInt<T> a, SaturatingInt<T> b)` | Saturating subtraction |
| `operator*` | static-binary | `public static SaturatingInt<T> operator* (SaturatingInt<T> a, SaturatingInt<T> b)` | Saturating multiplication; requires wider intermediate (e.g. int16 * int16 → int32, then clamp) |
| `operator==` | static-binary | `public static boolean operator== (SaturatingInt<T> a, SaturatingInt<T> b)` | Structural equality; != derived |

**Methods:** `public SaturatingInt<T> saturatingAdd(SaturatingInt<T> other)`, `public SaturatingInt<T> saturatingSub(SaturatingInt<T> other)`, `public SaturatingInt<T> saturatingMul(SaturatingInt<T> other)`, `public SaturatingInt<T> saturatingNeg()`

## WrappingInt<T>

- **Priority:** soon  ·  **Families:** numeric-complex
- **Type params:** `<T extends Integral>`
- **Element types:** int8, int16, int32, int64, uint8, uint16, uint32, uint64 (element type constrained by <T extends Integral>)
- **Purpose:** Signed/unsigned integer wrapper with wrapping (two's-complement) arithmetic on overflow. Matches Rust's std::num::Wrapping; useful for modular arithmetic, checksums, hash functions, and explicit bit-manipulation code where wrapping is the intended behavior.
- **Representation:** Wrapper around T (int8/16/32/64 or unsigned). Stored by value. Lowering: plain unchecked arithmetic (same as built-in primitives); semantics are that overflow wraps.
- **GPU notes:** No special device lowering needed — wrapping is the default arithmetic semantics on both CPU and GPU. Plain IR emit identical to built-in int arithmetic.
- **Depends on:** Integral (marker bound)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static WrappingInt<T> operator+ (WrappingInt<T> a, WrappingInt<T> b)` | Standard wrapping arithmetic; compile to plain IR add |
| `operator-` | static-binary | `public static WrappingInt<T> operator- (WrappingInt<T> a, WrappingInt<T> b)` | Wrapping subtraction |
| `operator*` | static-binary | `public static WrappingInt<T> operator* (WrappingInt<T> a, WrappingInt<T> b)` | Wrapping multiplication (let IR truncate wide product) |
| `operator==` | static-binary | `public static boolean operator== (WrappingInt<T> a, WrappingInt<T> b)` | Structural equality; != derived |

**Methods:** `public WrappingInt<T> wrappingAdd(WrappingInt<T> other)`, `public WrappingInt<T> wrappingSub(WrappingInt<T> other)`, `public WrappingInt<T> wrappingMul(WrappingInt<T> other)`, `public WrappingInt<T> wrappingNeg()`

## FixedPoint<T, uint32 Frac>

- **Priority:** later  ·  **Families:** Value Types — Survey Gaps (Operator-Overloaded)
- **Type params:** `<T extends Integral, uint32 Frac>`
- **Element types:** int32, int64, uint32, uint64
- **Purpose:** Deterministic fixed-point arithmetic for finance (pennies, cents), physics-based games (deterministic lockstep multiplayer), and embedded ML inference where float rounding non-determinism is unacceptable. Trades range for precision: `Frac` bits hold the fractional part, the rest the integer. No rounding surprises across platforms.
- **Representation:** Single integral field `raw` (T, usually int32/int64). The value is `raw / 2^Frac` (logical), but stored as the raw bit pattern. Lowers to `struct { T raw; }` by value, with operator codegen scaling by `2^Frac`.
- **GPU notes:** By-value raw-field marshalling; integer arithmetic only, no float ops, so fully deterministic across all backends (CPU/AMD/NVIDIA/Vulkan). Common in physics-engine kernels (collision detection, constraint solvers) and financial simulations. No special GPU support needed — shifts/multiplies are standard integer ops.
- **Depends on:** Math.abs (for fixed-point absolute value), Integer bit shift operators (no overload; built-in)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static FixedPoint<T,Frac> operator+ (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | Frac must match (template constraint); overflow on T is caller's responsibility (use wider T for accumulation) |
| `operator-` | static-binary | `public static FixedPoint<T,Frac> operator- (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | Two-operand binary |
| `operator- (unary)` | static-unary | `public static FixedPoint<T,Frac> operator- (FixedPoint<T,Frac> a)` | Arity 1 |
| `operator*` | static-binary | `public static FixedPoint<T,Frac> operator* (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | Intermediate (a.raw * b.raw) must use double-width to avoid overflow; result is truncated (not rounded, unless explicitly asked). Requires K-generic `Operator*` if the scaling is part of the signature — METHOD-TEMPLATED OPERATORS FORBIDDEN per §8 of OperatorOverloading.md. **Workaround: scale-by-Frac is a codegen detail, not part of the signature; multiply is written inline or via a helper macro, and the compiler emits the shift. The Frac parameter is a type-template arg, not a method arg, so it flows through at type construction, not at call site.** |
| `operator/` | static-binary | `public static FixedPoint<T,Frac> operator/ (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | b.raw != 0; intermediate (a.raw << Frac) must not overflow |
| `operator==` | static-binary | `public static boolean operator== (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | Exact comparison (no tolerance) |
| `operator<` | static-binary | `public static boolean operator< (FixedPoint<T,Frac> a, FixedPoint<T,Frac> b)` | >/<=/>=  derive from < |

**Methods:** `public static FixedPoint<T,Frac> from(float64 realValue)`, `public T toInt()`, `public float64 toFloat()`, `public T frac()`, `public static FixedPoint<T,Frac> round(FixedPoint<T,Frac> a, uint32 newFrac)`, `public static FixedPoint<T,Frac> abs(FixedPoint<T,Frac> a)`

## Interval<T>

- **Priority:** later  ·  **Families:** Value Types — Survey Gaps (Operator-Overloaded)
- **Type params:** `<T extends Floating>`
- **Element types:** float32, float64 (float16 interval width too coarse for meaningful bounds in most cases)
- **Purpose:** Validated numerics for robust geometric computation, physics simulation, and scientific computing where correctness matters more than speed. Tracks the range [lo, hi] guaranteed to contain the true value, catching rounding errors and instability in accumulations. Used in point-in-polygon, ray-triangle intersection, constraint-solver iterations, uncertainty quantification.
- **Representation:** Two fields: lo (T, rounded down) and hi (T, rounded up). Lowers to `struct { T lo; T hi; }` by value. Requires rounding-mode control at the arithmetic level (not just the type level) — CPU support is native (FENV / FLT_ROUNDS), GPU support varies (SPIR-V has no RoundingMode cap; cuComplex on NVIDIA is native; RDNA has rounding instructions).
- **GPU notes:** Device support is incomplete: SPIR-V lacks an `RoundingMode` capability or instruction-level rounding control. On CPU (LLJIT) and AMD (AMDGPU has rounding instructions per ISA), rounding is implementable. On Vulkan, must simulate via software (expensive — two instructions per op instead of one, plus atomics for certain patterns). NVIDIA CUDA has native rounding in PTX. **Recommendation: defer full GPU support to after B2/B3/C1 lands; CPU-only interval arithmetic is useful for host-side validation/constraint solving (ODE integrators, geometric predicates). Surface the type; mark device kernel use as XPU-N02 (not supported) until GPU rounding is available.**
- **Depends on:** Math.{sqrt,sin,cos} (transcendental interval bounds), fenv (host rounding control; see GPU notes)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator+` | static-binary | `public static Interval<T> operator+ (Interval<T> a, Interval<T> b)` | Requires fenv rounding-mode control; see GPU notes |
| `operator-` | static-binary | `public static Interval<T> operator- (Interval<T> a, Interval<T> b)` | Subtraction swaps and negates the RHS interval |
| `operator* (interval-interval)` | static-binary | `public static Interval<T> operator* (Interval<T> a, Interval<T> b)` | More complex than add/subtract; handles signs across zero |
| `operator/` | static-binary | `public static Interval<T> operator/ (Interval<T> a, Interval<T> b)` | Precondition check: 0 ∉ b |
| `operator==` | static-binary | `public static boolean operator== (Interval<T> a, Interval<T> b)` | Structural; `contains` (value-in-interval test) is a separate method |
| `operator<` | static-binary | `public static boolean operator< (Interval<T> a, Interval<T> b)` | Intervals can overlap or be incomparable |

**Methods:** `public T width()`, `public T midpoint()`, `public boolean contains(T x)`, `public static Interval<T> sqrt(Interval<T> x)`, `public static Interval<T> sin(Interval<T> x)`, `public static Interval<T> cos(Interval<T> x)`, `public static Interval<T> intersection(Interval<T> a, Interval<T> b)`, `public static Interval<T> union(Interval<T> a, Interval<T> b)`

## Range (Interval)

- **Priority:** later  ·  **Families:** geometric-graphics
- **Type params:** `<T extends Numeric>`
- **Element types:** T: any Numeric (float32 for continuous; int32/uint32 for discrete counts/bounds)
- **Purpose:** 1D interval [min, max] for 1D spatial queries, clip planes, animation timelines, parameter ranges. Lightweight foundation for range-based algorithms.
- **Representation:** Two scalar endpoints (min, max). Pure scalar pair (no Vector involved).
- **GPU notes:** Utility type (no special GPU lowering). Used in host-side logic (clip planes, timelines); kernel-local ranges typically computed inline (loop bounds, parameter constraints), not marshalled as type.
- **Depends on:** Numeric bound (primitive validation)

| Operator | Form | Signature | Notes |
|---|---|---|---|
| `operator|` | static-binary | `public static Range<T> operator\| (Range<T> a, Range<T> b)` | Binary-OR: encompass both |
| `operator&` | static-binary | `public static Range<T> operator& (Range<T> a, Range<T> b)` | Binary-AND: common region |
| `operator==` | static-binary | `public static boolean operator== (Range<T> a, Range<T> b)` | Standard equality semantics |

**Methods:** `public boolean contains(T value)`, `public boolean containsRange(Range<T> other)`, `public boolean overlaps(Range<T> other)`, `public T length()`, `public T clamp(T value)`, `public Range<T> expand(T value)`

