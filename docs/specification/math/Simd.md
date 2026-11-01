# SIMD on `Vector<T, N>` — Specification v1

_This spec extends the existing built-in `Vector<T, N>`
(`src/cajeta/type/CajetaVector`, `VectorOps.h`) with the data-parallel
classification ops a SIMD JSON scanner needs. `Vector<T,N>` is already a
compiler-intercepted value type that lowers to LLVM `<N x T>`. It runs on both
CPU (host JIT/AOT) and GPU (SPIR-V/AMD), and today it does construction,
swizzles, lane-wise arithmetic, and geometry helpers (dot/length/normalize/…).
This spec adds compare→mask, `movemask`, `tableLookup` (pshufb), `clmul`,
bitwise lane ops, and `Buffer` block load/store, so CPU SIMD and GPU kernels
share one lane vocabulary. The companion plan is `plans/Simd-plan.md`, and the
tour is `docs/specification/math/Simd-tour.md`._

## 1. Why this exists

SIMD is a differentiator. It is a capability that sets Cajeta apart from Java
and other competitors, and it is more than a numerics nicety. Cajeta compiles
natively and can expose first-class portable SIMD, and a JVM library is
structurally unable to match hand-tuned native vector code. The most visible
proof is the most-benchmarked workload, JSON. Today Cajeta's tokenizer runs at
~0.45–0.64× of Jackson, a hand-tuned scalar Java tokenizer, after the
allocation fix + SWAR. Closing that gap and beating Jackson is a concrete,
demonstrable edge over the Java ecosystem.

The catch is documented in `[[per-call-scope-enter-malloc]]` and the JSON
bench. The parser that beats Jackson is simdjson, and simdjson is fast because
of SIMD instructions. It uses `pcmpeqb`/`pshufb` to classify 16–32 bytes per
instruction, `pmovmskb` to extract a bitmask, and `pclmulqdq` for the
in-string mask. Pure SWAR (our `loadU64`/`ctz64`) cannot replicate those. It
does ~6 byte-compares per word where SIMD does one. So the JSON win requires
giving Cajeta real SIMD. And SIMD pays off far beyond JSON, in image/audio, ML
kernels, hashing, UTF-8 validation, compression and search, so it is a
load-bearing language capability, built once.

The goal is a portable SIMD vector type and operation surface sufficient to
implement a simdjson-class JSON stage-1 scanner in pure Cajeta, that lowers
to SSE2/AVX2 on x86 and NEON on ARM via LLVM, with a 128-bit portable baseline.

## 2. Prior art (and what we take)

| Mechanism | Shape | What we take / reject |
|---|---|---|
| C/C++ intrinsics (`__m128i`, `_mm_*`) | target-specific, per-ISA | **Reject** the per-ISA surface; keep the *operations*. |
| C++ `std::simd` (P1928) / Rust `core::simd` | portable fixed-width `simd<T, N>`; lane ops + masks | **Adopt as the model**: portable type, lane-wise ops, mask type, target-agnostic. |
| .NET `Vector<T>` / `Vector128<T>` | portable + fixed-width; HW-accelerated | Adopt the fixed-width `Vector128`-style explicit width (predictable codegen). |
| Google Highway | portable, runtime-dispatched widths | **Adopt later**: target-feature widths + runtime dispatch is the v2 story. |
| LLVM vector IR (`<N x T>`) | the lowering layer | **Adopt as the implementation**: our ops *are* LLVM vector IR; LLVM lowers per target. |
| simdjson | the proof | the v1 acceptance target — stage 1 in pure Cajeta. |

The consensus we adopt is a portable, explicit-width `Vector<T, N>` with
lane-wise operators and an integer mask extraction, lowered through LLVM's
vector IR. There are no per-ISA intrinsics in user code.

## 3. Design

### 3.1 The type

```cajeta
// N lanes of element type T. A value type (no heap, no ownership) — lives in a
// SIMD register / stack slot. Maps 1:1 to the LLVM vector type <N x T>.
public struct Vector<T, N> { ... }   // N is a non-type (compile-time int) param
```

- `T` ∈ { `int8`,`int16`,`int32`,`int64`, `uint8`…`uint64`, `float32`,`float64` }.
- `N` is a compile-time constant, and `N * sizeof(T)` is the bit width.
- The 128-bit baseline (`Vector<int8,16>`, `<int32,4>`, `<float32,4>`, …) is
  guaranteed on every target (SSE2 / NEON). 256-bit (`<int8,32>`) is allowed
  and lowers to AVX2 where available, otherwise to two 128-bit ops, because
  LLVM splits it.
- **Value semantics.** Vectors are copied and never borrowed. There is no `#`
  and no drop, and they pass in registers. Loads and stores move bytes; see
  § 3.4.

Named aliases exist for ergonomics, as thin `using`-style names: `i8x16`,
`i32x4`, `u8x16`, `f32x4`, `f64x2`, `i8x32`, … The full list is in § 5.

### 3.2 Masks

Lane comparisons produce a mask, which is one bit per lane packed into an
integer, the `pmovmskb` result. It is an integer rather than a vector of
bools because the JSON algorithms do their cross-lane logic (prefix-XOR string
masking, `ctz` to find the next structural index) on integers:

```cajeta
i8x16 v = ...;
int32 m = v.eqMask(needle);   // bit i set iff lane i == needle  (16-bit mask)
int32 first = Cajeta.ctz64((int64) m);   // first matching lane
```

The mask width is `N` bits, which is 16 for a 128-bit i8 vector and 32 for
256-bit. For non-byte element widths the mask is one bit per lane, so 4 for
`i32x4`.

### 3.3 Operation surface (v1)

The operations are grouped, and they are lane-wise unless noted.

- **Construct / move:** `splat(T)`, `load(Buffer, off)` (unaligned), `loadAligned`,
  `store(Buffer, off, v)`, `zero()`.
- **Arithmetic:** `+ - *` (wrapping for ints, IEEE for floats), `min`, `max`,
  `abs`; saturating `addSat`/`subSat` for the small int types.
- **Bitwise:** `& | ^ ~`, `shl`/`shr` (by a scalar or per-lane).
- **Compare → mask:** `eqMask`, `ltMask`, `gtMask`, `leMask`, `geMask`.
- **Compare → vector mask** (all-ones/all-zeros lanes, for `select`): `eq`, `lt`, …
- **Select / blend:** `select(maskVec, a, b)`.
- **Movemask:** `mask() -> int32` (sign/MSB bit of each lane).
- **Shuffle:** `shuffle(indices)` is a constant-index permute. `tableLookup(idx)`
  is a dynamic per-lane byte gather from a 16-entry table, the `pshufb`
  primitive and the heart of simdjson's classifier.
- **Reductions:** `reduceOr`, `reduceAnd`, `reduceAdd`, `reduceMax`, `reduceMin`.
- **Lanes:** `get(i)`, `with(i, T)` (extract/insert; `i` constant).
- **Convert / reinterpret:** `as<U,M>()` (bitcast, same bits), `widen`/`narrow`,
  `toFloat`/`toInt`.
- **Carryless multiply:** `clmul(a, b)` is the `pclmulqdq` primitive for the
  branch-free in-string prefix mask. It is specialised; see § 7.

### 3.4 Loads/stores over `Buffer`

SIMD reads bytes from memory, and the natural source is `cajeta.io.Buffer`
(`[[cajeta-logging-state]]` sibling work). `i8x16.load(buf, off)` lowers to a
`load <16 x i8>, align 1` of `buf.data + off`, the same header+offset ABI
`Cajeta.loadU64` uses. The caller guarantees `off + N <= byteCount`. The hot
primitive has no per-call bounds check, and a checked `loadChecked` is
available.

## 4. The JSON payoff (the v1 acceptance driver)

simdjson stage 1 in pure Cajeta is the thing this feature exists to enable:

1. For each 16/32-byte block: `eqMask('"')`, `eqMask('\\')`, and a `tableLookup`
   classifier producing structural (`{}[]:,`) and whitespace masks. That is a
   handful of SIMD ops per block, where the scalar reader works per byte.
2. Backslash/escape handling and the in-string mask via `clmul`-based
   prefix-XOR over the quote mask (carry parity across blocks).
3. `structural & ~inString` gives the structural bitmask. `ctz`-iterate it to
   emit the structural index stream.

Stage 2 walks the index stream with no per-byte work. Acceptance is the same
token counts as the scalar reader (twitter 29573 / citm 85035 / canada 223236)
and ≥ Jackson throughput on this machine.

## 5. Surface API (v1 as specified)

```cajeta
package cajeta.simd;

public struct Vector<T, N> {
    public static Vector<T, N> splat(T value);
    public static Vector<T, N> load(cajeta.io.Buffer<int8> buf, int64 off);   // unaligned
    public static Vector<T, N> zero();

    public Vector<T, N> add(Vector<T, N> o);    // + - * min max abs ...  (also operators)
    public Vector<T, N> and(Vector<T, N> o);    // & | ^ ~ shl shr
    public int32 eqMask(T needle);              // ltMask/gtMask/leMask/geMask
    public int32 mask();                        // movemask
    public Vector<T, N> tableLookup(Vector<int8, N> indices);  // pshufb
    public Vector<T, N> shuffle(...);           // constant-index permute
    public T reduceOr();                        // reduceAnd/Add/Max/Min
    public T get(int32 lane);                   // constant lane
    public void store(cajeta.io.Buffer<int8> buf, int64 off);
    // <U, M> Vector<U, M> as();                // bitcast reinterpret
}

// Aliases:  i8x16 i16x8 i32x4 i64x2  u8x16…  f32x4 f64x2  i8x32 i32x8  (256-bit)
// Carryless multiply (specialised, the in-string-mask primitive):
//   public static int64 Simd.clmul(int64 a, int64 b);
```

## 6. Portability and target features

- The SSE2 / NEON baseline (128-bit) is always available, with no feature
  gate.
- AVX2 (256-bit) is used when the target enables it. Otherwise a 256-bit
  vector lowers to two 128-bit halves, which LLVM handles correctly but more
  slowly.
- `pclmulqdq` (clmul) and `pshufb` are baseline on x86-64-v2+ and on all NEON.
  v1 targets x86-64-v2 and aarch64. A runtime CPU-dispatch layer
  (Highway-style) is v2 (§ Non-goals).
- Endianness: lane 0 is the lowest-addressed element (little-endian lane order),
  matching `loadU64`. This is documented, and v1 supports little-endian targets.

## 7. Codegen / runtime

This is pure codegen with no runtime C function, like `loadU64`/`ctz64`:

- `Vector<T,N>` ⇒ LLVM `<N x T>`; values live in vector registers / stack.
- Lane ops ⇒ LLVM vector `add`/`and`/`icmp`/`fcmp`/`shufflevector`/…
- `eqMask` ⇒ `icmp eq <N x T>` → `<N x i1>` → `bitcast <N x i1> to iN`. This
  is the movemask, and LLVM lowers it to `pmovmskb` on x86.
- `tableLookup` ⇒ `@llvm.x86.ssse3.pshuf.b.128` on x86 / `@llvm.aarch64.neon.tbl1`
  on ARM, behind the portable method. This is the one place we select per target.
- `clmul` ⇒ `@llvm.x86.pclmulqdq` (x86) / NEON `pmull` sequence.
- `load`/`store` ⇒ unaligned vector `load`/`store, align 1`.

A new `VECTOR` type kind in the type system (§ Plan Phase 0) carries `(T, N)` and
maps to the LLVM vector type. Operators dispatch to the vector IR.

## 8. Safety, capabilities, ownership

1. **No capability.** SIMD is pure register computation. It needs no
   `network`, `filesystem` or `clock`, and it is hermetic.
2. **Value type.** There is no heap, no `#` and no drop, and copies are
   register moves.
3. **Bounds.** `load`/`store` read or write `N` bytes. The unchecked hot form
   trusts the caller's `off + width <= byteCount`. `loadChecked`/`storeChecked`
   bounds-check, gated by `--bounds`. Out-of-range constant lane indices are a
   compile error.
4. **No UB from lane ops.** Integer lane arithmetic wraps, with no per-lane
   overflow trap, because SIMD is wrapping by nature. Float lanes are IEEE.

## 9. Interaction with the rest of the language

| Feature | Interaction |
|---|---|
| Templates | `Vector<T, N>` monomorphizes on `(T, N)`; `N` is a non-type (int) template arg (`[[templates-not-generics]]`; non-type defaults unsupported in v1). |
| `cajeta.io.Buffer` | the canonical SIMD load/store source; `loadU64`/`ctz64` are the scalar siblings of the vector ops. |
| Ownership | value type — no `#`/drop; orthogonal to the borrow checker. |
| Overflow checks | lane arithmetic is always wrapping, independent of `--overflow-checks`. |
| XPU / GPU | **shared** — the *same* built-in `Vector<T,N>` is the GPU kernel vector (Stage-5 SPIR-V/AMD) and the host CPU SIMD register. Core ops (load/store, lane arith, compare) are identical IR on both; the CPU-only extraction ops (`mask`/`tableLookup`/`clmul`) have GPU analogs in the subgroup/wave/`Quad` ops. |
| Capabilities | none required. |

## 10. Non-goals (v1)

- **Runtime CPU dispatch** (pick AVX-512 vs AVX2 vs SSE at runtime). v1 compiles
  for a target feature level (x86-64-v2 / aarch64), and Highway-style dispatch
  is v2.
- **AVX-512 / SVE scalable vectors.** v1 is fixed 128/256-bit only.
- **Masked/predicated loads, gather/scatter.** These are v2.
- **Auto-vectorization of scalar Cajeta loops.** That is orthogonal and is
  LLVM's job. This spec is explicit SIMD.
- **A full numeric `Vec3`/`Vec4` math type.** That is a separate math library,
  and `Vector<T,N>` is the SIMD primitive it would build on.

## 11. Open questions for review

1. **Operators vs methods.** Expose `a + b` / `a & b` operator overloads on
   `Vector`, or methods only (`a.add(b)`)? We lean toward operators for
   arith/bitwise and methods for compare/shuffle/reduce, which matches
   `std::simd`.
2. **Mask type.** Plain `int32` movemask (chosen, ergonomic for `ctz`) vs a
   distinct `Mask<N>` type (more type-safe, more surface). We lean toward
   `int32`.
3. **Width policy.** Force the 128-bit baseline only in v1 (simplest, portable)
   and add 256-bit in the same release, or 128 first? We lean toward the 128
   baseline first, with 256 as a fast-follow in the same plan.
4. **`pshufb`/`clmul` exposure.** Portable methods (`tableLookup`, `Simd.clmul`)
   that pick the per-target intrinsic, vs a raw `@intrinsic` escape hatch. We
   lean toward portable methods only, with no raw intrinsics in user code.
5. **Package home.** `cajeta.simd` (chosen) vs `cajeta.lang`. We lean toward
   `cajeta.simd`, which is explicit, is not auto-imported, and does not clash
   with any math `Vector`.
