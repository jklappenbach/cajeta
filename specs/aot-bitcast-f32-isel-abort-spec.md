# AOT emission cannot select `bitcast i32 -> f32` (Cajeta.bitsToF32)

**Filed 2026-08-20** (found by cajeta-llama Unit 16's GGUF reader; blocks
any `--emit=exe` build whose reachable code calls `Cajeta.bitsToF32` /
`bitsToF64` on a computed operand).

## Repro (25 lines, aborts in seconds)

Compile with `cajeta --emit=exe [-–release] -o q probe.Q.run <root> <root>`:

```cajeta
package probe;
import cajeta.lang.Cajeta;
public final class Q {
    public static int32 run() {
        int8[] raw #= heap int8[8];
        raw[1] = (int8) 0x3F;
        int64 j = 0;
        float32 acc = 0.0f;
        while (j < 2) {
            int32 h = ((int32) raw[j * 2] & 255)
                | (((int32) raw[j * 2 + 1] & 255) << 8);
            acc = acc + Cajeta.bitsToF32(h << 16);
            j = j + 1;
        }
        if (acc > 0.0f) { return 1; }
        return 2;
    }
}
```

```
LLVM ERROR: Cannot select: f32 = bitcast <i32 or/shl/zext-load chain>
In function: probe.Q::run()
```

**Not the CPU.** Retested 2026-08-22 after `--cpu` changed default from
`generic` to `native`: the repro still aborts identically. The spec's
"target/feature configuration" hypothesis is therefore refuted for the CPU
dimension — a host cpu advertising the full AVX-512 feature set selects this no
better than the SSE2 baseline did. Whatever differs between the AOT and JIT
pipelines is elsewhere.

**Why this is worth scheduling.** Measured 2026-08-22 by ablation, the
workaround this defect forces — `GgufFile.halfBitsToF32`, an arithmetic decode
whose `pow2()` is a repeated-squaring loop in float64 — is **38% of
cajeta-llama's Q4_K mat-vec** (7.99 ms of 21.30 at 4096x4096). Two f16 reads
per 256-element block means 131,072 loop-driven float64 conversions per
projection, for what is a bit-shuffle and a bitcast. The same workaround
appears in `ParityRun` and in `GgufFile.singleBits`. This is not a cosmetic
defect; it is one of the two largest costs in the engine's hot kernel.

Aborts under BOTH default and `--release` exe emission; the identical
intrinsic is GREEN under the JIT (`NumpyOpsTests.floatBitsIntrinsicRoundTrip`
pins the round trip), so the divergence is in the AOT pipeline's
target/feature configuration or isel setup, not the lowering itself
(`MethodCallExpression.cpp` emits a plain `CreateBitCast(i32, float)`).

`bitcast i32 -> f32` is selectable on every x86-64 subtarget (MOVD), so
the likely suspects are the AOT TargetMachine's feature string or an
isel-pass configuration difference between the exe path and the JIT.

## Consumers worked around (revert when fixed)

`dev.cajeta.llama.io.GgufFile` and `bench.ParityRun` decode IEEE bits
arithmetically (`Float.decode`-style sign/exponent/mantissa math) instead
of bitcasting — correct but slower; both note this spec.
