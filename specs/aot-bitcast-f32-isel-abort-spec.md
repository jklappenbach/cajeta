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
