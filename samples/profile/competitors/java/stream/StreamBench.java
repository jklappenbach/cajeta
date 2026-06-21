// Java stream competitors — the same two workloads the Cajeta side runs over
// xs[i] = i % 1000 (N=1M): stream-filter-map-reduce (filter evens → map +1 → sum,
// a sequential java.util.stream pipeline over a primitive LongStream) and
// stream-parallel-reduce (sum fanned out across the ForkJoinPool common pool via
// .parallel()). One schema-conformant CSV row per benchmark (competitors/columns.txt
// order). Cross-check = the exact reference sums (250000000 / 499500000).
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners; the 1M-element pipelines OSR-compile well within a trial).
// alloc_bytes is per-thread bytes allocated for one execution via the HotSpot
// com.sun.management.ThreadMXBean — the Java-native analog of Go's TotalAlloc delta.
// For the parallel benchmark that captures only the submitting thread (the worker
// threads' allocations aren't summed); an accepted limitation, still emitted.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.function.LongSupplier;

public class StreamBench {
    static final int N = 1000000;
    static final long FMR_REF = 250000000L;
    static final long PR_REF = 499500000L;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();
    static volatile long SINK; // defeats dead-code elimination of the pipeline result

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

    static void emit(String runId, String ts, String bench, String lib,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mops = med > 0 ? (double) N / (double) med * 1e9 / 1e6 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,stream,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, N, N, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mops, peakRssKb(), alloc, status, ok);
    }

    static void benchLong(String runId, String ts, String bench, String lib,
                          int warmup, int trials, LongSupplier f, long ref) {
        for (int i = 0; i < warmup; i++) SINK = f.getAsLong();
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            long r = f.getAsLong();
            samples[i] = System.nanoTime() - t0;
            SINK = r;
            ok = (r == ref);
        }
        long alloc = allocOf(() -> SINK = f.getAsLong());
        emit(runId, ts, bench, lib, warmup, trials, samples, ok, alloc);
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        long[] xs = new long[N];
        for (int i = 0; i < N; i++) xs[i] = i % 1000;

        // filter-map-reduce (sequential primitive LongStream): filter evens → map +1 → sum
        benchLong(runId, ts, "stream-filter-map-reduce", "java.util.stream", warmup, trials,
            () -> Arrays.stream(xs).filter(x -> (x & 1L) == 0).map(x -> x + 1).sum(), FMR_REF);

        // parallel-reduce (ForkJoinPool common pool via .parallel()): sum of all elements
        benchLong(runId, ts, "stream-parallel-reduce", "parallel-stream", warmup, trials,
            () -> Arrays.stream(xs).parallel().reduce(0L, Long::sum), PR_REF);
    }
}
