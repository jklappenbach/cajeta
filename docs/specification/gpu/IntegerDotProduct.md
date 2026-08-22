# Quantized inner products: int8 dot product (DP4a)

Quantized inference multiplies 8-bit weights by 8-bit activations and sums into a
wide accumulator. The hardware has a dedicated unit for exactly this — four int8
lanes multiplied and reduced in one instruction (NVIDIA `dp4a`, AMD `v_dot4`,
Vulkan `OpSDot`/`OpUDot`). Cajeta exposes it as `dot` on an 8-bit vector:

```
Vector<int8,4> a = ...;   Vector<int8,4> b = ...;
int32 d = a.dot(b);          // sum(a[i] * b[i]), accumulated in int32
int32 e = a.dot(b, acc);     // acc + sum(a[i] * b[i])  — the fused dot-add
```

The result is **`int32`, not `int8`** — int8·int8 sums overflow a byte almost
immediately, so the accumulation is always wide. That widening is the whole point
of the operation, and it is what the hardware unit does.

## Signed vs unsigned comes from the element type

```
Vector<int8,4>  a = ...;  a.dot(b);   // OpSDot — lanes sign-extended
Vector<uint8,4> u = ...;  u.dot(w);   // OpUDot — lanes zero-extended
```

There is no signed/unsigned *flag*: the element type already carries it
(`int8` is signed, `uint8` is not). A byte of `200` reads as `+200` from a
`uint8` vector and `-56` from an `int8` vector — pick the vector type that
matches your quantization scheme.

## The fused dot-add is the GEMM inner loop

A quantized matmul tile accumulates a row·column across K in int32:

```
int32 acc = 0;
for (uint32 k = 0; k < K; k = k + 4) {
    Vector<int8,4> a = /* load 4 weights  */;
    Vector<int8,4> b = /* load 4 inputs   */;
    acc = a.dot(b, acc);          // one DP4a per 4 elements, no overflow
}
```

`a.dot(b, acc)` is a single hardware op (multiply-4, reduce, add to the
accumulator) — the integer analogue of `Math.fma` for the matmul inner loop.

## How it lowers — same source, every backend

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | `OpSDot` / `OpUDot ... PackedVectorFormat4x8Bit` + `OpIAdd` — the `SPV_KHR_integer_dot_product` hardware unit. |
| **CPU / AMD / NVIDIA** | a portable widening reduce (sign/zero-extend each lane to int32, multiply, sum) — same numeric result. |

The two paths are **bit-exact**; only the instruction selection differs. On a
device without the dot-product extension, the SPIR-V backend emits a validated
bit-field expansion instead — still correct, just not the dedicated unit.

### Why not the alternatives?

| Alternative | What it forces |
|---|---|
| A scalar `for` loop summing `a[i]*b[i]` | Serial; on a GPU it scalarizes the vector. And if you keep the accumulator in `int8` it overflows — you must remember to widen by hand. |
| `Vector<int8,4>` element-wise `*` then a manual reduce | The `<4 x i8>` product overflows per lane *before* you reduce; you get the wrong answer unless you widen to `<4 x i32>` first — i.e. reimplement `idotWiden`. |
| Float dot on widened data | Converts int8→float32, loses the quantized fast path, and burns FP throughput where the integer unit is free. |

`a.dot(b, acc)` is one expression that hits the silicon on Vulkan and stays
correct everywhere else.

## Staying in vector space: `dotAccum`

`dot` collapses four lanes to one scalar. For a wide kernel that is a reduce per
four elements, and reduce frequency is what costs — the same Q4_K mat-vec
reducing per sub-block measured 70.7 ms against 24.2 ms reducing once per block.

`dotAccum` is the same operation with the result kept in lanes:

```cajeta
Vector<uint8,64> w = weights.vload<64>(i);
Vector<int8,64>  a = acts.vload<64>(i);
acc = w.dotAccum(a, acc);     // acc is Vector<int32,16>; 4 lanes -> 1 lane
```

Operands are `4N` lanes of 8-bit against a `Vector<int32,N>` accumulator: lanes
`4i..4i+3` multiply, sum, and add into accumulator lane `i`. Reduce once at the
end of the block instead of once per group.

| target | what it becomes |
|---|---|
| x86 AVX512-VNNI / AVX-VNNI | `vpdpbusd` — one instruction |
| anything else LLVM supports | `llvm.vector.partial.reduce.add`, which lowers to the target's fused form |
| `CAJETA_SIMD_SCALAR_FALLBACK=1` | a scalar lane loop |
| device (`@Kernel`) | the same DP4a unit `dot` uses, per accumulator lane |

Every tier is **bit-identical**. The operation is integer, so there is no
reassociation hazard the way float accumulation has one: a faster tier changes
speed and never the answer, which is what lets the scalar tier stand as the
correctness floor. Callers never branch on target — `w.dotAccum(a, acc)` is the
only spelling, everywhere.

---

**Rules.** Integer `dot` is defined for **4-lane 8-bit** vectors — `Vector<int8,4>`
and `Vector<uint8,4>` — and yields `int32` (other integer shapes get a clean
diagnostic for now). `a.dot(b)` is `sum(a[i]*b[i])`; `a.dot(b, acc)` adds the
int32 accumulator. `dotAccum` takes `4N` 8-bit lanes against a `Vector<int32,N>`
accumulator and yields `Vector<int32,N>`; operand lane counts must match.
Signedness is the element type (`OpSDot` vs `OpUDot`). Runnable
end to end in `samples/Tour/xpu` (the `DP4a` section). See `MaskSelect.md` for the
other branchless value-type idiom and `CajetaXPU.md` for the kernel surface.
