// Java sort competitors — sort 50K long across the same four input patterns the
// Cajeta side uses (random / ascending / descending / dups), plus a 50K double
// random sort and a stable long sort. One schema-conformant CSV row per
// (benchmark, variant, library), competitors/columns.txt order. Same splitmix64
// input sequence as every other language. Cross-check: output non-decreasing and
// (for long) wrapping sum preserved. Libraries: java.util.Arrays.sort — the
// dual-pivot quicksort for primitives, and Timsort (stable) for boxed Long[].
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
import java.util.Arrays;

public class SortBench {
    static final int N = 50000;
    static final long SEED = 0x123456789ABCDEF0L;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    // splitmix64 — identical sequence to every other language's generator.
    static final class Split {
        long s;
        Split(long seed) { s = seed; }
        long next() {
            s += 0x9E3779B97F4A7C15L;
            long z = s;
            z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
            z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
            return z ^ (z >>> 31);
        }
    }

    static long[] makeI64(String variant) {
        Split g = new Split(SEED);
        long[] v = new long[N];
        for (int i = 0; i < N; i++) {
            switch (variant) {
                case "random" -> v[i] = g.next();
                case "ascending" -> v[i] = i;
                case "descending" -> v[i] = N - 1 - i;
                default -> v[i] = i % 16; // dups
            }
        }
        return v;
    }

    static double[] makeF64() {
        Split g = new Split(SEED);
        double[] v = new double[N];
        for (int i = 0; i < N; i++) v[i] = (double) Long.remainderUnsigned(g.next(), 100000000L) / 7.0;
        return v;
    }

    static long wsum(long[] v) { long a = 0; for (long x : v) a += x; return a; } // two's-complement wrap

    static long allocOf(Runnable r) {
        long id = Thread.currentThread().threadId();
        long b = TMX.getThreadAllocatedBytes(id);
        r.run();
        return TMX.getThreadAllocatedBytes(id) - b;
    }

    interface I64Sort { void sort(long[] w); }
    interface F64Sort { void sort(double[] w); }

    static void emit(String runId, String ts, String bench, String variant, String lib,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mels = med > 0 ? (double) N / (double) med * 1e9 / 1e6 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,sort,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.2f,Melem/s,%d,-1,%d,-1,-1,%s,%b,,%s%n",
            runId, ts, bench, N, N, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mels, peakRssKb(), alloc, status, ok, variant);
    }

    static void benchI64(String runId, String ts, String bench, String variant, String lib,
                         long[] input, int warmup, int trials, I64Sort sorter) {
        long want = wsum(input);
        for (int i = 0; i < warmup; i++) sorter.sort(input.clone());
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long[] w = input.clone();
            long t0 = System.nanoTime();
            sorter.sort(w);
            samples[i] = System.nanoTime() - t0;
            ok = isSorted(w) && wsum(w) == want;
        }
        long alloc = allocOf(() -> sorter.sort(input.clone()));
        emit(runId, ts, bench, variant, lib, warmup, trials, samples, ok, alloc);
    }

    static void benchF64(String runId, String ts, String lib,
                         double[] input, int warmup, int trials, F64Sort sorter) {
        for (int i = 0; i < warmup; i++) sorter.sort(input.clone());
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            double[] w = input.clone();
            long t0 = System.nanoTime();
            sorter.sort(w);
            samples[i] = System.nanoTime() - t0;
            ok = isSorted(w);
        }
        long alloc = allocOf(() -> sorter.sort(input.clone()));
        emit(runId, ts, "sort-f64", "", lib, warmup, trials, samples, ok, alloc);
    }

    static boolean isSorted(long[] v) { for (int i = 1; i < v.length; i++) if (v[i] < v[i - 1]) return false; return true; }
    static boolean isSorted(double[] v) { for (int i = 1; i < v.length; i++) if (v[i] < v[i - 1]) return false; return true; }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        for (String variant : new String[]{"random", "ascending", "descending", "dups"})
            benchI64(runId, ts, "sort-int64", variant, "Arrays.sort", makeI64(variant), warmup, trials, Arrays::sort);

        // stable (random only) — box to Long[] so Arrays.sort uses Timsort (stable).
        {
            long[] base = makeI64("random");
            benchI64(runId, ts, "sort-stable-int64", "", "Arrays.sort(Timsort)", base, warmup, trials, w -> {
                Long[] boxed = new Long[w.length];
                for (int i = 0; i < w.length; i++) boxed[i] = w[i];
                Arrays.sort(boxed); // Timsort, stable
                for (int i = 0; i < w.length; i++) w[i] = boxed[i];
            });
        }

        benchF64(runId, ts, "Arrays.sort", makeF64(), warmup, trials, Arrays::sort);
    }
}
