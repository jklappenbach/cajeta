---
id: math-tensor-Tensor-fromProtocol
applies-to: [cajeta/math/Tensor.fromProtocol]
title: Tensor.fromProtocol — rebuild a Tensor<?> from the interop protocol
description: Switch on a TensorProtocol's runtime DType to rebuild the matching Tensor<X>, returned erased as Tensor<?> for reified-capture; ownership + the multi-fromProtocol heap-corruption trap.
---

# `Tensor.fromProtocol` — import the interop protocol back into a `Tensor`

```cajeta
public static #Tensor<?> fromProtocol(TensorProtocol p)
```

**What it does:** reads the protocol's **runtime** {@link DType} (`p.dtype()`), switches
on `kind`/`bits`/`variant`, and rebuilds the concrete `Tensor<X>` matching that dtype as
a **zero-copy shared view** over the protocol's borrowed `Storage`. It returns that
tensor **widened to `Tensor<?>`** (dtype-erased) — the dtype is recovered at runtime, so
the static type can't be `Tensor<float32>`. The caller **reified-captures** it back to the
concrete instantiation. This is the inverse of {@link Tensor#protocol}.

## You must reified-capture the result — it is not usable as `Tensor<?>`

`Tensor<?>` is an airlock type: you cannot call element ops on it. Guard with a reified
`instanceof Tensor<X>` then cast to capture:

```cajeta
import cajeta.math.Tensor;
import cajeta.math.TensorProtocol;

Tensor<float32> t = Tensor.of<float32>(data, shp);   // [[0,1,2],[3,4,5]]
TensorProtocol p = t.protocol();                     // export: borrow + metadata

Tensor<?> w = Tensor.fromProtocol(p);                // import → erased Tensor<?>
if (!(w instanceof Tensor<float32>)) { /* wrong/unsupported dtype */ }
Tensor<float32> back = (Tensor<float32>) w;          // reified-capture

back.set2(0, 0, 9.0f);                                // zero-copy: write shows through
// t.get2(0, 0) == 9.0f                               // same Storage as the exporter
```

The `instanceof` is dtype-exact: an `int32` protocol matches `Tensor<int32>`, never
`Tensor<float32>`. A bounded form (`w instanceof Tensor<? extends Floating>`) admits by
dtype **kind** (rides on numeric-bounds).

## Return ownership / null

- Returns an **owned** `#Tensor<?>`, but the `Tensor` is a **non-owning shared view**:
  its `Storage` is a borrow of the protocol's backing, kept alive by the live-set and
  freed exactly once when the last handle drops. Writes are visible through every handle
  over that `Storage` (zero-copy).
- Returns **`null`** for any dtype outside the rebuilt set. Rebuilt: `bool`; signed/
  unsigned int 8/16/32/64; `float32`/`float64`. **NOT rebuilt** (returns null): the
  128-bit ints, `float16`, `bfloat16`, and every `float8`/`float6`/`float4` variant —
  these are deferred. A caller treats `null` as "cannot import this dtype."

## What it does NOT do

- It does **not copy or fix layout.** The rebuilt view keeps the protocol's offset/shape/
  strides verbatim — a non-contiguous protocol yields a non-contiguous view. For the
  external-producer path that copies non-contiguous layouts into a fresh contiguous
  tensor, use {@link Tensor#fromProtocolContiguous} instead.
- It does **not** validate the device or read-only flag; it only switches on dtype.
- It does **not** take ownership of `p`'s `Storage` — `p` still describes a borrow of the
  exporter's buffer; the exporter keeps ownership.

## The multi-`fromProtocol` heap-corruption trap (why the backing is erased `Object`)

The bug this design exists to avoid: {@link TensorProtocol} holds its backing as `Object`
(`baseStore`), **not** `Storage<?>`. `fromProtocol`'s helper `rebuildShared<E>` downcasts
`(Storage<E>) p.base()` and passes it **inline** into the `Tensor<E>` constructor — never
binding a named `Storage<E>` local — then calls `markSharedView()` so no owning drop is
registered for that borrowed `Storage`.

Two failure modes if you deviate:

1. **Named class-typed local for the borrow** (`Storage<E> s = (Storage<E>) p.base();`)
   registers an owning drop, which virtual-drops the **shared** `Storage` at scope exit
   and frees the buffer out from under the returned view and the exporter — a
   use-after-free. Always pass the downcast borrow directly.
2. **`Storage<?>` wildcard field** in `TensorProtocol` (instead of `Object`) routes
   teardown through a materialized wildcard handle whose drop **corrupts the heap** when
   two distinct `Storage` instantiations are alive — i.e. calling `fromProtocol` on a
   `float32` and an `int32` protocol in the same scope. The `Object` erasure makes the
   upcast an identity (same pointer), so teardown hits the real `Storage` and is
   idempotent via the live-set. The regression test that guards this builds two
   `fromProtocol` tensors of different dtypes in one scope (`TensorTests`,
   `wildcardBoundedFloatingFromProtocol`).

So: do not introduce a named owning local for `p.base()`, and do not change
`TensorProtocol.baseStore` from `Object` to `Storage<?>`.
