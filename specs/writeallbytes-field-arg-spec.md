# writeallbytes-field-arg — defect (found during cajeta-llama Unit 4)

## 1. Definition

**1.1 Symptom.** `File.writeAllBytes(path, h.data, n)` — where the data
argument is a struct **field** rather than a local variable — writes `n` bytes
of adjacent struct memory to the file instead of the array's contents. The
same corruption applies to any array argument reaching the `File` intrinsic
lowerings through an l-value shape that is not a plain local: a field
(DotExpression GEP) or an array element (ArrayIndex GEP).

**1.2 Found.** Identified statically 2026-08-08 while scoping cajeta-llama
(spec §3.7), during the survey of the file path's argument lowering; the
failing shape is pinned by `test/expression/FileIo64Tests.cpp`
(`writeAllBytesFromStructField`), written 2026-08-12 as cajeta-llama 4.1.2.

**1.3 Root cause.** The File-receiver dispatch's array-argument helper
unwrapped only `AllocaInst`:

```cpp
auto loadArrayDataPtr = [&](size_t idx) -> llvm::Value* {
    llvm::Value* arr = parameters[idx].expression->generateCode(module);
    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
        arr = builder->CreateLoad(a->getAllocatedType(), a);
    }
    return builder->CreateInBoundsGEP(i8Ty, arr, ...8...);
};
```

A local's codegen yields an alloca, which the branch loads through. A field
or element access yields a **GEP to the slot** holding the array pointer; the
branch does not fire, and the slot address itself is GEP'd +8 and handed to
the runtime as if it were the array header — so the "data" written is
whatever memory follows the slot.

**1.4 Precedent.** The identical defect existed for String arguments and was
fixed at `MethodCallExpression.cpp:386` by routing through `loadIfLValue`,
which loads reference elements through any l-value shape using the AST's
resolved type and leaves r-values untouched. The array path simply never got
the same fix.

## 2. Fix

**2.1** A shared `loadArrayArg` helper (beside `loadStringArg`,
`MethodCallExpression.cpp`) applies `loadIfLValue` to array arguments, with
the same alloca fallback for non-Expression nodes.

**2.2** Every array-argument site in the File lowerings routes through it:
`writeAllBytes`'s `loadArrayDataPtr`, `FileReader.read`, `FileWriter.write`,
and the random-access `File.read`/`File.write` — one un-audited site is
enough to keep a defect of this class alive (the COFF-JITLink lesson).

**2.3** Pinned by `FileIo64Tests.writeAllBytesFromStructField`: a 3-byte
array held in a struct field round-trips through `writeAllBytes` and the
file receives the array's bytes, verified outside the JIT.

## 3. Status

Fixed on `cajeta-llama/unit-3-4-storage-io` together with cajeta-llama
Unit 4 (the 64-bit widening touches the same lowerings; splitting the
commits would have meant editing the same lines twice).
