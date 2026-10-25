// Java time competitors — the same two workloads the Cajeta side runs:
// time-instant-arith (1M) and time-localdate-arith (100K), using the java.time
// package (the immutable value-object date/time API). One schema-conformant CSV
// row per benchmark, competitors/columns.txt order. Cross-check = the exact
// closed-form epoch sum (500006500000 / 5000050000), identical to the Go/Rust/
// C++/Python runners. All math in UTC (no DST), so days are exactly 86400 s and
// epoch days advance by one.
//
// java.time.Instant / java.time.LocalDate are immutable, so each step allocates a
// fresh value; we accumulate the epoch second / epoch day into a long sum and
// return it (consumed by the cross-check) so the JIT can't elide the loop.
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners; the large-N inner loops are OSR-compiled well within a trial).
// alloc_bytes is per-thread bytes allocated for one execution, via the HotSpot
// com.sun.management.ThreadMXBean — the Java-native analog of Go's TotalAlloc
// delta / Rust's global allocator / C++ operator new override.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.time.LocalDate;
import java.util.Arrays;
import java.util.function.LongSupplier;
import java.util.function.LongPredicate;

public class TimeBench {
    static final int N_INSTANT = 1000000;
    static final int N_DATE = 100000;
    static final long INSTANT_REF = 500006500000L;
    static final long DATE_REF = 5000050000L;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static long allocOf(Runnable r) {
        long id = Thread.currentThread().threadId();
        long b = TMX.getThreadAllocatedBytes(id);
        r.run();
        return TMX.getThreadAllocatedBytes(id) - b;
    }

    static void emit(String runId, String ts, String bench, int input, String lib,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mops = med > 0 ? (double) input / (double) med * 1e9 / 1e6 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,time,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mops, peakRssKb(), alloc, status, ok);
    }

    static void benchI64(String runId, String ts, String bench, int input, String lib,
                         int warmup, int trials, LongSupplier f, LongPredicate check) {
        for (int i = 0; i < warmup; i++) { long unused = f.getAsLong(); if (unused == Long.MIN_VALUE) System.out.print(""); }
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            long r = f.getAsLong();
            samples[i] = System.nanoTime() - t0;
            ok = check.test(r);
        }
        long alloc = allocOf(() -> { long r = f.getAsLong(); if (r == Long.MIN_VALUE) System.out.print(""); });
        emit(runId, ts, bench, input, lib, warmup, trials, samples, ok, alloc);
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        // instant-arith: Instant.ofEpochSecond(i).plusSeconds(7).getEpochSecond()
        benchI64(runId, ts, "time-instant-arith", N_INSTANT, "java.time", warmup, trials, () -> {
            long sum = 0;
            for (long i = 0; i < N_INSTANT; i++) {
                Instant t = Instant.ofEpochSecond(i);
                Instant t2 = t.plusSeconds(7);
                sum += t2.getEpochSecond();
            }
            return sum;
        }, r -> r == INSTANT_REF);

        // localdate-arith: epoch day i → +1 day → epoch day (UTC, exact 86400s/day)
        benchI64(runId, ts, "time-localdate-arith", N_DATE, "java.time", warmup, trials, () -> {
            long sum = 0;
            for (long i = 0; i < N_DATE; i++) {
                LocalDate t = LocalDate.ofEpochDay(i);
                LocalDate t2 = t.plusDays(1);
                sum += t2.toEpochDay();
            }
            return sum;
        }, r -> r == DATE_REF);
    }
}
