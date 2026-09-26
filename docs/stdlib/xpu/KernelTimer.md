# KernelTimer

`cajeta.xpu.KernelTimer` — a per-kernel timer whose number can be stood
behind. It brackets a `KernelStream` with two `Event`s and reads the
DEVICE's own elapsed time between them (`cuEventElapsedTime` /
`hipEventElapsedTime`), which ships with the driver: no profiler, no CUPTI,
no toolkit. Every number comes with a tier that says what it means, and a
timer that cannot measure answers `-1` rather than guessing.

<!-- snippet: skip -->
```cajeta
KernelStream s #= KernelStream.current();
KernelTimer t #= KernelTimer.create();
t.begin(s);
kernel.launch(s, grid: [g], block: [b])(args);
t.end(s);
int64 ns = t.elapsedNanos();                     // waits for the end event; -1 = refused
System.stdout.println(ns + " ns at tier " + KernelTimer.tierName());
if (!KernelTimer.withinPeak(ns, bytesMoved, Device.peakBandwidthGBps(), flops, peakFlops)) {
    // the measurement implies more than the hardware can do: do not report it
}
t.destroy();
```

## Tiers

| Constant | `tierName()` | Meaning |
|---|---|---|
| `TIER_DEVICE` (2) | `device` | The device measured it between two of its own events (CUDA, HIP; ~0.5 µs resolution). The bracket includes whatever the stream did between the records, launch latency included, so bracket several back-to-back launches when that matters. |
| `TIER_HOST` (1) | `host` | The stream is synchronous (CPU), so a host timestamp at each record is the stream's tail exactly. Exact, but the host's clock. |
| `TIER_UNAVAILABLE` (0) | `unavailable` | No clock the timer can read (Vulkan). `elapsedNanos()` answers `-1`. |

## Methods

| Signature | |
|---|---|
| `static #KernelTimer create()` | Two fresh events; the caller owns the timer and must `destroy()` it |
| `void begin(KernelStream s)` | Record the start at the stream's tail; re-arms the timer |
| `void end(KernelStream s)` | Record the end at the stream's tail |
| `int64 elapsedNanos()` | Nanoseconds between the records on the backend's clock, waiting for the end; `-1` when begin or end was not recorded, the tier is unavailable, or the driver refused |
| `void destroy()` | Release both events |
| `static int32 tier()` | What a number means on the active backend |
| `static String tierName()` | The tier as the word to print beside the number |
| `static float64 clockScale()` | The factor the backend's event clock runs fast by against the host clock, already divided out of `elapsedNanos()`; 1.0 for a correct driver, 0.0 when no usable clock |
| `static float64 impliedGBps(int64 ns, int64 bytes)` | Bytes per nanosecond, which is GB/s |
| `static float64 impliedOpsPerSecond(int64 ns, float64 ops)` | Operations per second |
| `static boolean withinPeak(int64 ns, int64 bytes, float64 peakGBps, float64 ops, float64 peakOpsPerSecond)` | The standing assertion: `ns` positive and each declared work amount at or under its ceiling. A work amount with a zero ceiling is REFUSED, never waved through |

## The event clock's scale

The driver's conversion of device ticks to milliseconds has been measured
wrong: on a WSL2 RTX 4090 (driver 610.62) two events around a 2 s host sleep
with no kernel between them read 2.091 s, 4.56% fast. The timer's first use on
CUDA or HIP spends about 160 ms measuring that scale against the host clock
and divides it out of every figure; `clockScale()` reports it. A scale outside
[0.5, 2] is refused and the tier reads `unavailable`.

## Calibration

`XpuKernelTimerTests` spins a kernel on `KernelThread.clock()` for a stated
number of SM cycles and checks the timer against `Device.clockKHz()`, the
driver's maximum clock: the spin cannot finish faster than that clock allows,
and doubling the cycles must double the time within 10%. A second test checks
the device timer against the host clock around a synchronised 20 ms launch.
Read the `RESULT` lines of those tests on your part before quoting a figure.

## See also

- [KernelStream](KernelStream.md) — the stream the timer brackets
- [Device](Device.md) — `clockKHz()`, `memoryClockKHz()`, `memoryBusWidthBits()`, `peakBandwidthGBps()`
- Plan: `agents/xpu-kernel-adaptor-plan.md` Unit 5, the reason this exists
- Source: [`runtime/src/cajeta/xpu/KernelTimer.cajeta`](../../../runtime/src/cajeta/xpu/KernelTimer.cajeta)
