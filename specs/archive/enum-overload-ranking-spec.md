# enum-overload-ranking — an exact enum argument outranks one that decays to an integer

Plan: [enum-overload-ranking-plan](../../agents/archive/enum-overload-ranking-plan.md).
Decided with Julian 2026-11-20: fix overload resolution, then add
`KernelBuffer(uint64 elementCount, MemoryKind kind)`. Approved 2026-11-20.

## 1. Definition

An enum constant converts implicitly to `int32`, its ordinal, so `allocate(int32 kind)`
accepts `MemoryKind.Pinned`. Overload resolution uses that conversion too. When a call has
no exact match, `CajetaClass::resolveMethodImpl` decays every enum argument to `int32` and
looks again, then runs a closest-match scan that ties at zero for positional calls, so an
arbitrary overload wins.

So `heap KernelBuffer<int32>(n, MemoryKind.Pinned)` with a `uint32 n` binds the
package-private `(int64 deviceHandle, uint64 elementCount)` constructor even when a
`(uint64 elementCount, MemoryKind kind)` constructor exists. The buffer wraps address `n`
with an element count of 1, and a kernel that touches it faults (CUDA 700). MemoryKind's
own documentation shows that call. Measured 2026-11-20 with a probe constructor:
`length()` read 1, Pinned's ordinal, and reordering the declarations changed nothing.

## 2. Features

### 2.1 An exact enum match wins

When overloads remain after the exact-key lookup, a candidate whose formal has the
argument's own enum type ranks above one that needs the enum to decay to an integer.
Integer and floating-point arguments keep their present ranking, and so do calls with no
enum argument.

Use cases:
1. `f(n, MemoryKind.Pinned)` with `f(uint64, MemoryKind)` and `f(int64, uint64)` binds the first.
2. `f(0, n)` with the same two binds `f(int64, uint64)`, since no enum is involved.
3. `allocate(MemoryKind.Pinned)` with only `allocate(int32)` still binds through the decay.

### 2.2 `KernelBuffer(n, MemoryKind)` allocates that kind

A public `KernelBuffer(uint64 elementCount, MemoryKind kind)` allocates `elementCount`
elements of `kind` and releases them in `~KernelBuffer()`, as the one-argument form does.
MemoryKind's documentation then describes a call that works, and the
`KernelBuffer(0, n)` + `allocate(kind)` idiom keeps working.

Use cases:
1. `heap KernelBuffer<int32>(1, MemoryKind.Pinned)` gives a one-element pinned buffer that
   a kernel writes and the host reads with `hostLoad`.
2. Existing `KernelBuffer(0, n)` calls keep their meaning.
