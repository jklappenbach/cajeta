# hashmap-string-value-drop — a String-VALUED HashMap lowers its slot access wrongly and faults inside `put`

> **Diagnosis corrected 2026-08-08.** The original entry called this a
> double-free on map DROP, inferred from the drop-chain dump printed at
> crash time. That was wrong: the dump is context the handler prints on
> any fault, not the failing operation. Under a debugger the fault is a
> **store through a null base inside `put` itself** — nothing is freed
> twice, and the map never reaches its drop. The title and §1 below are
> the corrected account; the workaround is unchanged and still correct.

## 1. Definition

Found 2026-08-07 implementing cajeta-cloud U2 (`Capabilities`, backed by
`HashMap<String, String>`). One `put` is enough:

```cajeta
HashMap<String, String> m = heap HashMap<String, String>();
String k = "" + "alpha";
m.put(#k, "" + "beta");     // SIGSEGV here, fault addr 0x28
```

The faulting instruction is `mov %rdx,0x28(%rax)` with **`rax = 0`**.

`MapEntry<K,V>` is `@ValueType`, so `slots` holds entries INLINE and
`slots[i]` IS the entry's address. The emitted IR nevertheless LOADS A
POINTER out of the slot and GEPs through it:

```llvm
%80 = getelementptr %"#array.MapEntry<String,String>[]", ptr %72, i64 0, i32 1, i64 %73
%81 = load ptr, ptr %80                         ; <-- element read as a REFERENCE
%val = getelementptr %"MapEntry<String,String>", ptr %81, i32 0, i32 2
call void @llvm.memcpy(ptr %val, ptr %82, i64 40, i1 false)
```

On a fresh (zeroed) table that load yields null, so the store writes
through null. The 40-byte memcpy is a second inconsistency: the `val`
slot is a single `ptr`, and `cajeta.lang.String` is an ordinary
reference class (not `@ValueType`), so a String value has no business
being copied by value at all.

The layouts disagree across instantiations of the same generic:

| instantiation | struct |
|---|---|
| `MapEntry<String, CacheNode<…>>` | `{ ptr, ptr, i64 }` — key, val, ownership word |
| `MapEntry<String, String>` | `{ ptr, ptr, ptr, i64 }` — an extra leading pointer |

The 4-field shape and the pointer load are what a REFERENCE class gets
(vtable at slot 0, accessed by pointer), so the `<String,String>`
monomorph appears to lose its `@ValueType`-ness in the member-access
path while the ARRAY element type keeps it — the array is laid out
inline, the access reads it as a reference, and they cannot both be
right.

Bounding probes (all clean, so the trigger is narrow):
- `HashMap<String, Int64>`, `HashMap<Int64, String>` — fine.
- `HashMap<Int64, Int64>` — fine, so `K == V` is NOT the trigger.
- `HashMap<String, CacheNode<…>>` — fine (the 3-field layout above).
- Only a **String VALUE** type has reproduced it.

## 2. Requirements

- **2.1** A `@ValueType` array element is accessed INLINE — no pointer
  load — in every path, for every instantiation of the generic.
- **2.2** A monomorph's `@ValueType`-ness is identical in the array
  element layout and in the member-access lowering; the two cannot
  disagree.
- **2.3** A reference-typed field is stored as a pointer, never copied
  by value.
- **2.4** A regression pin puts and reads back through
  `HashMap<String, String>`.

## 3. Workaround (in use)

Avoid String-valued HashMaps: cajeta-cloud `Capabilities` keeps parallel
`ArrayList<String>` lists (names + caveat notes) with a linear
`indexOf` scan — adapters declare fewer than a dozen capabilities, so
the scan is free.

## 4. Reproduction

The four-line program in §1 as a standalone `--emit=exe` run; the IR is
in `cajeta.runtime.__stdlib__.ll` under
`HashMap<String,String>::put`. Also cajeta-cloud @ 863164d^ (the
pre-workaround `Capabilities`).

## 5. Status

NOT fixed. Diagnosed to the lowering above on branch
`fix/registered-defects`, where the neighbouring defects were repaired;
this one needs the generics/value-type layout owner, since the fix is a
consistency question about monomorph identity rather than a local
codegen slip.
