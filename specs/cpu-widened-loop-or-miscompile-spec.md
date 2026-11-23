# cpu backend: a widened inner loop ORs two arms that should be selected

Status: draft (defect), filed 2026-11-22 from kernel-drivers 2.1.1.

## 1. Definition

**1.1** A `@Device` helper that returns early for part of an index range, called in a short
constant-trip loop of a kernel, gives wrong values on the cpu backend when the arm not taken holds
a shift by an out-of-range amount:

```cajeta
@Device
public static int32 scale(int32 h1, int32 h3, uint32 j) {
    if (j < 4) { return (h1 >> (int32) (8 * j)) & 63; }
    int32 s = (int32) (8 * (j - 4));            // out of range while j < 4, but not executed then
    return ((h3 >> s) & 15) | ((((h1 >> s) & 255) >> 2) & 48);
}
// in the kernel:  while (j < 8) { out[o + 2 + j] = P.scale(h1, h3, j); out[o + 10 + j] = P.min(h2, h3, j); j = j + 1; }
```

**1.2 Measured (proton, Zen 5, cajeta 5a0086ae2 and the released v0.54.3).** 262 of 768 scale
values wrong over 96 blocks; each wrong value is a superset of the right one's bits (7 read 15, 58
read 63). The host calling the same methods is right, amdgpu and vulkan are right, and
`CAJETA_XPU_CPU_NO_VECTORIZE=1` does not change it. With only one of the two stores in the loop the
values are right. With the shift written in range (`8 * (j & 3)`) they are right.

**1.3 What is known.** The kernel's IR before optimization is right. After the vectorize passes the
eight-trip loop is one `<8 x i32>` step: each arm computed for all eight lanes, with the lanes of
the arm not taken poison (shifts by -32..-8 and 32..56), `and` masks whose unused lanes are poison,
and a `shufflevector` picking lanes 0 to 3 of one arm and 4 to 7 of the other. That IR is still
right. The wrong values appear after it, so the fault is in a later IR pass of ours or in code
generation, where the select of the two arms has become an OR while the unused lanes are not zero.

**1.4 Reproducer.** `specs/schemas/cpu-widened-loop-or-miscompile/probe/Q4kBlockTest.cajeta`:
`cajeta --emit=exe --xpu-backend=cpu -o t probe.Q4kBlockTest.main <dir> <work>` prints the
mismatches.

## 2. Use cases

- **2.1** When the reproducer is built for the cpu, it prints no mismatch.
- **2.2** When the fault is located, the report names the pass or the lowering that turns the
  select into an OR, and whether it is ours or LLVM's.
- **2.3** When a device helper shifts by an amount the source never executes out of range, the cpu
  result equals the GPU backends' and the host's.
- **2.4** When the fix is in, a kernel test holds this shape on the cpu.

## 3. Open questions

- **3.1** Whether a device shift by an out-of-range amount should be defined (masked, as x86 and
  the GPUs do) so no poison is created at all. The host traps on it.
