# Sub-byte / 8-bit Float Support — Decisions & Impact

Working notes for the fp4 / fp6 / fp8 work added to Cajeta. Captures the choices
made, why, and what each one costs or constrains.

---

## Goal

Add language-level support for sub-byte and 8-bit floating-point types used in
modern ML workloads (inference and gradient training), backed by LLVM where
possible.

---

## Decisions

### 1. Which formats to expose

**Chosen:** Full OCP Microscaling (MX) set, plus AMD FNUZ variants:

| Keyword             | Bits | Use case                        | LLVM `APFloat` semantics      |
| ------------------- | ---- | ------------------------------- | ----------------------------- |
| `float4e2m1`        | 4    | MXFP4 inference                 | `APFloat::Float4E2M1FN()`     |
| `float6e2m3`        | 6    | MXFP6 (high precision)          | `APFloat::Float6E2M3FN()`     |
| `float6e3m2`        | 6    | MXFP6 (wider range)             | `APFloat::Float6E3M2FN()`     |
| `float8e4m3`        | 8    | FP8 inference (NVIDIA Hopper)   | `APFloat::Float8E4M3FN()`     |
| `float8e5m2`        | 8    | FP8 training gradients          | `APFloat::Float8E5M2()`       |
| `float8e4m3fnuz`    | 8    | AMD MI300/MI350 native E4M3     | `APFloat::Float8E4M3FNUZ()`   |
| `float8e5m2fnuz`    | 8    | AMD MI300/MI350 native E5M2     | `APFloat::Float8E5M2FNUZ()`   |

**Why this set:** Covers the dominant ML inference + training formats across the
three major hardware vendors (NVIDIA, AMD, Intel) and matches the OCP MX spec.
Naming matches PyTorch convention (`torch.float8_e4m3fn` → `float8e4m3`).

**Impact:**
- Seven new lexer tokens, seven new parser rules.
- Seven new `TYPE_ID`s and matching `_FLAG` bits (`BIT_4_FLAG`, `BIT_6_FLAG`
  added; `BIT_SIZE_MASK` widened).
- All FP `_ID`s renumbered to slot the sub-byte/fp8 entries below `FLOAT16_ID`
  in numeric rank — this is load-bearing for `CajetaType::normalize()`, which
  uses the raw flag-word comparison to decide promotion direction.

### 2. Cross-format same-bit-width casts (e.g. `float8e4m3 ↔ float8e5m2`)

**Chosen:** Auto-bridge via `float16`. Compiler inserts an intermediate fp16
step transparently.

**Why:** Most ergonomic. Matches how PyTorch / cuBLAS / cuDNN handle fp8
inter-format conversion under the hood. The user shouldn't have to write
`(float8e5m2)(float16)x` manually.

**Impact:**
- *Currently inert.* The bridge would have been one branch inside the existing
  `normalize()` switch (detect equal `getScalarSizeInBits()`, route through
  half) — but it only fires if the LLVM type is actually `isFloatingPointTy()`.
  Because we ended up storing fp8 as `i8` (see Decision #5), `isFloatingPointTy()`
  is false and the bridge never triggers. Replacing it: the explicit-throw
  branch in `normalize()` for sub-fp16 targets, which makes the unimplemented
  state loud instead of silent.
- When runtime conversion helpers are implemented, the bridge logic moves
  there: `__cajeta_fp8_e4m3_to_fp16` → `__cajeta_fp16_to_fp8_e5m2`.

### 3. Arithmetic on fp8/fp6/fp4 operands

**Chosen:** Auto-widen to `float16` for the op, truncate result back. Same model
PyTorch uses.

**Why:** No mainstream hardware exposes scalar fp8 arithmetic instructions;
even on H100/MI300, fp8 GEMMs accumulate in fp16/fp32. Scalar fp8 add/mul is
software. Auto-widen keeps user code readable.

**Impact:**
- `emitFpBinOp` helper added in `BinaryOpExpression.cpp`. Widens any
  `isFloatingPointTy()` operand with `getScalarSizeInBits() < 16` to half,
  emits the op there, truncates back.
- *Currently inert for fp8 storage.* Same reason as #2: fp8 is `i8`, so
  `isFloatingPointTy()` is false. Fp8 arithmetic currently falls through to
  `CreateAdd` on the bit pattern (semantically wrong — adds the raw bits, not
  the fp values). This is a known sharp edge; the helper is already in place
  for when fp8 either (a) moves to true LLVM FP types if LLVM ever adds them,
  or (b) gets fronted by runtime conversion calls.
- Helper is *not* inert for fp16/bf16 — those already use real `HalfTy` /
  `BFloatTy` IR types. It's a real fix for the (separately broken)
  `isFloatTy()` → `isFloatingPointTy()` typo path.

### 4. LLVM version

**Chosen — then reversed:** Originally bumped `setup.sh` to LLVM 20. Reverted
to LLVM 18 (Ubuntu 24.04 default) after the API discovery in Decision #5.

**Why bumped:** I had assumed LLVM 19/20 added IR-level `Type*` factories like
`Type::getFloat8E4M3FNTy()` and `Float8E4M3FNTyID` enum entries. They do not.

**Why reverted:** The fp8/fp6/fp4 types do not exist as `llvm::Type*` in any
released LLVM version. Verified directly against `release/20.x` and `main`
branches of `llvm-project`:

- `enum TypeID` in `llvm/include/llvm/IR/Type.h` has the same 7 FP entries it
  had in LLVM 14 (Half, BFloat, Float, Double, X86_FP80, FP128, PPC_FP128).
  No `Float8E*TyID` / `Float4E2M1FNTyID` exist.
- No `getFloat8E*Ty(ctx)` factory methods exist.
- `Type::getFloatingPointTy(LLVMContext&, const fltSemantics&)` exists but its
  implementation (`llvm/lib/IR/Type.cpp:130`) is a hardcoded if/else covering
  only the classic 7 formats and asserts on anything else.

What does exist in LLVM 18+ is `APFloat::Float8E5M2()` / `Float4E2M1FN()` etc.
returning `const fltSemantics&` — but these are *software* arithmetic helpers
for the APFloat constant-folding library, not anchors for an `llvm::Type*`.

**Impact:**
- No LLVM upgrade is required for this change.
- Whenever LLVM does eventually add IR-level Type entries for these formats,
  the registration site in `CajetaType::init()` will need to swap
  `IntegerType::get(ctx, N)` for the new factories, and `create()` should be
  called with `shareLlvmType=true`. Everything else (flags, rank, lexer,
  parser, normalize routing, BinaryOpExpression helper) stays as-is.

### 5. Storage representation — iN integer aliases

**Chosen:** Represent each new fp type by an integer LLVM type of matching bit
width: `i4` for fp4, `i6` for fp6, `i8` for fp8 variants. Cajeta's type
metadata still tags them with `FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG`; only
the LLVM-IR backing differs.

**Why:** Forced choice given Decision #4. This matches the production approach
in Triton, the PyTorch inductor, XLA, IREE, and other ML compilers that target
LLVM today.

**Impact — collision risk handled:**
- `CajetaType::create()` previously *always* registered the new type into:
  - `typeMap[TypeKey(llvmType)]` (reverse lookup by LLVM type)
  - `llvmTypeIdMap[llvmType->getTypeID()]` (reverse lookup by TypeID)

  Both are keyed by the LLVM type. Naively reusing `i8` for `float8e4m3`,
  `float8e5m2`, etc. would have the last registration **clobber the `uchar`
  and `char` entries**, breaking `CajetaType::of(llvm::Type*)` for every
  i8-typed value in the rest of the compiler.

  Fix: added a `shareLlvmType = true` default parameter to `create()`. The fp8
  registrations pass `false`, skipping both reverse-lookup maps. They live
  only in `canonicalMap` (forward lookup by name), which is enough for the
  language frontend; LLVM-value reverse lookup of an `i8` continues to return
  `uchar` (or whatever was registered first under "share" mode).

  Consequence: nothing inside Cajeta can recover "this `llvm::Value` is a
  `float8e4m3`" purely from its LLVM type — it needs to track the Cajeta
  type through the AST. This is consistent with how Triton et al. handle the
  same problem (they carry a separate dtype tag alongside the LLVM value).

**Impact — arithmetic and casts:**
- `CreateFPCast`, `CreateFAdd` etc. expect `isFloatingPointTy()` operands.
  Calling them on an i8 backing an fp8 value would emit nonsense. So
  `normalize()` for any `FLOATxE*_TYPE_ID` destination now *throws* with a
  "runtime helpers not yet implemented" error. Loud failure is better than
  silent miscompile.
- Likewise `BinaryOpExpression`'s `emitFpBinOp` helper is correctly bypassed
  by the iN representation — but the surrounding `if (isFloatingPointTy())`
  in `BinaryOpExpression.cpp` means an `fp8 + fp8` will fall to `CreateAdd`
  on the raw bits. Wrong, but not catchable without threading Cajeta type
  metadata through expression nodes (an architectural gap, not a fp-specific
  one).

### 6. Bit flag layout

**Chosen:** Insert `BIT_4_FLAG` and `BIT_6_FLAG` into the `BIT_*_FLAG` group;
widen `BIT_SIZE_MASK` to cover them.

**Why:** Existing pattern. The mask is used by code that filters out the size
sub-field when comparing flag identities.

**Impact:**
- `CajetaTypeFlags` is a `unsigned long` — plenty of room. The added bits push
  the highest currently-used flag bit from position 13 to position 16. Still
  well under 32, let alone 64.
- The renumbered `*_ID` block (Decision #1) reordered the high-half of the
  flag word. **Any persisted artifacts that store these IDs are now stale.**
  At present nothing serializes type IDs, but if a `.cajeta` archive format
  is added later, this renumbering is a breaking change to bookkeep.

### 7. The `isFloatTy()` → `isFloatingPointTy()` typo fix

**Chosen:** Replace all 6 live occurrences (plus 2 commented) in
`BinaryOpExpression.cpp`.

**Why:** `isFloatTy()` returns true *only* for 32-bit IEEE float. Every other
floating type — fp16, bf16, fp64, fp80, fp128, ppc_fp128 — silently fell to
`CreateAdd` / `CreateSub` / `CreateMul` (integer ops). Adding fp4/6/8 to a
codebase with this bug would have made it harder to debug later.

**Impact:**
- fp16, bf16, fp64, fp128 arithmetic now correctly emits `FAdd`/`FSub`/`FMul`.
- This is the only change in this commit set that materially affects code
  paths *outside* the fp4/6/8 feature.

---

## What works now

- Declaring variables of the new types: `float8e4m3 x;` parses and registers.
- Storage and load codegen — they're just `i8` (or `i4`/`i6`) loads/stores.
- Type metadata: `CajetaType::of("float8e4m3")` returns the type; canonical
  name, flags, rank, and bit-size flag are all queryable.
- `toGeneric()` returns `"number"` for these types (via the `IntegerTyID`
  case — they're stored as integers).
- The lexer/parser accept them anywhere `primitiveType` is accepted: variable
  declarations, fields, method parameters, return types.

## What does NOT work (deferred, loud)

- **Arithmetic.** `a + b` where both are fp8 currently falls to integer add of
  the bit pattern. Semantically wrong.
- **Casts to/from other FP types.** `normalize()` throws a "runtime helpers
  not yet implemented" exception with code `101`.
- **Literal parsing with type suffixes.** No grammar for `1.5f8e4m3`. Literal
  expressions still produce a generic `float` value.

## What's needed to make compute work

A minimal C/LLVM-IR runtime library. Roughly:

```c
uint16_t __cajeta_fp8_e4m3_to_fp16(uint8_t  v);
uint8_t  __cajeta_fp16_to_fp8_e4m3(uint16_t v);
uint16_t __cajeta_fp8_e5m2_to_fp16(uint8_t  v);
uint8_t  __cajeta_fp16_to_fp8_e5m2(uint16_t v);
// ... and the FNUZ, fp6, fp4 pairs
```

Implementation strategy options for those helpers:
- **Software**: bit-twiddling functions using LLVM's `APFloat` semantics offline
  to build a 256-entry LUT for fp8 → fp16, and per-mode rounding logic for
  fp16 → fp8.
- **Hardware intrinsics** (per-target): `cvt.rn.satfinite.e4m3x2.f16x2` on
  NVIDIA Hopper+; AMD WMMA on MI300; Intel AMX-FP8 on Granite Rapids.

Once those exist, `normalize()` swaps its throw for a `CreateCall` to the
matching helper, and `BinaryOpExpression`'s fp arithmetic path gains a branch
that detects FLOAT_FLAG with sub-fp16 bit width (via Cajeta type, not LLVM
type) and routes through `to_fp16 → op → from_fp16`.

---

## Architectural side notes uncovered during this work

These are *not* fp-specific but were exposed while wiring fp support:

- **`isFloatTy()` typo** (fixed). Was making fp16/fp64/fp128 silently miscompile.
- **`BinaryOpExpression` `*_EQUALS` bug** (not fixed). `CreateStore(lhs, result)`
  has args reversed — stores `lhs` *into* the freshly-computed sum Value. Out
  of scope here.
- **`ArrayIndexExpression` indexing math** (not fixed). Multiplies indices
  instead of linearizing row-major. Out of scope.
- **`CajetaType::typeMap` and `llvmTypeIdMap` assume bijection LLVM-type ↔
  Cajeta-type.** Broken by sub-byte fp aliasing onto integer storage. Worked
  around with `shareLlvmType=false`; a proper fix would track Cajeta type
  through the AST so the reverse lookup is never needed for codegen.
- **No place exists to attach "this `llvm::Value*` represents an fp8 even
  though its LLVM type is i8."** Expression nodes don't carry a resolved
  `CajetaTypePtr`. Threading that through is the prerequisite for correct
  fp8 arithmetic, and is also the prerequisite for several other items in the
  larger AST/codegen architectural review.

---

## Files touched

- `antlr4/CajetaLexer.g4` — 7 new tokens.
- `antlr4/CajetaParser.g4` — 7 new `primitiveType` alternatives.
- `src/cajeta/type/CajetaType.h` — `BIT_4_FLAG`, `BIT_6_FLAG`, `BIT_SIZE_MASK`,
  7 new `*_ID`s and `*_TYPE_ID`s, renumbered FP IDs, `create()` overload with
  `shareLlvmType` flag.
- `src/cajeta/type/CajetaType.cpp` — 7 new registrations with `IntegerType::get`
  + `shareLlvmType=false`; `toGeneric` gained `HalfTyID` case; `normalize`
  throws on sub-fp16 destinations.
- `src/cajeta/asn/expression/BinaryOpExpression.cpp` — `isFloatTy` →
  `isFloatingPointTy` (×8), `emitFpBinOp` helper for sub-fp16 widening, six
  arithmetic sites switched to use it.
- `setup.sh` — left at LLVM 18 default (bumped to 20 and reverted).
