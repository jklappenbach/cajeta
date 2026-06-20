// Java collection competitors — the same four workloads the Cajeta side runs:
// hashmap-int / hashmap-string (Ankerl insert+find shape), hashset-dedup,
// arraylist-append. One schema-conformant CSV row per (benchmark, library),
// competitors/columns.txt order. A faithful port of competitors/go/collection/
// main.go: identical sizes (50000 / 30000 / 100000 / 100000), identical
// construction (pre-sized HashMaps where Go pre-sizes its map, unsized HashSet/
// ArrayList where Go leaves them unsized), identical closed-form cross-checks.
// Containers: JDK stdlib java.util.HashMap / HashSet / ArrayList.
//
// Notes: timings honor PROFILE_WARMUP/TRIALS like the other runners (the large-N
// inner loops are OSR-compiled well within a trial). alloc_bytes is per-thread
// bytes allocated for one execution, via the HotSpot com.sun.management
// .ThreadMXBean — the Java-native analog of Go's TotalAlloc delta.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.function.LongSupplier;

public class CollectionBench {
    static final int N_INT = 50000;
    static final int N_STR = 30000;
    static final int N_SET = 100000;
    static final int SET_HALF = 50000;
    static final int N_VEC = 100000;
    static final long INT_REF = 2499950000L;
    static final long STR_REF = 449985000L;
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

    interface Check { boolean ok(long r); }

    static void emit(String runId, String ts, String bench, String lib, int input,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mops = med > 0 ? (double) input / (double) med * 1e9 / 1e6 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,collection,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mops, peakRssKb(), alloc, status, ok);
    }

    static void bench(String runId, String ts, String bench, String lib, int input,
                      int warmup, int trials, LongSupplier f, Check check) {
        for (int i = 0; i < warmup; i++) f.getAsLong();
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            long r = f.getAsLong();
            samples[i] = System.nanoTime() - t0;
            ok = check.ok(r);
        }
        long alloc = allocOf(f::getAsLong);
        emit(runId, ts, bench, lib, input, warmup, trials, samples, ok, alloc);
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        // hashmap-int — pre-sized HashMap (Go: make(map[int64]int64, nInt)).
        bench(runId, ts, "hashmap-int", "HashMap", N_INT, warmup, trials, () -> {
            HashMap<Long, Long> m = new HashMap<>(N_INT);
            for (long i = 0; i < N_INT; i++) m.put(i, i * 2);
            long sum = 0;
            for (long j = 0; j < N_INT; j++) sum += m.get(j);
            return sum;
        }, r -> r == INT_REF);

        // hashmap-string — pre-sized HashMap (Go: make(map[string]int64, nStr)).
        bench(runId, ts, "hashmap-string", "HashMap", N_STR, warmup, trials, () -> {
            HashMap<String, Long> m = new HashMap<>(N_STR);
            for (long i = 0; i < N_STR; i++) m.put("key" + i, i);
            long sum = 0;
            for (long j = 0; j < N_STR; j++) sum += m.get("key" + j);
            return sum;
        }, r -> r == STR_REF);

        // hashset-dedup — unsized HashSet (Go: make(map[int64]struct{})).
        bench(runId, ts, "hashset-dedup", "HashSet", N_SET, warmup, trials, () -> {
            HashSet<Long> set = new HashSet<>();
            for (long i = 0; i < N_SET; i++) set.add(i % SET_HALF);
            return set.size();
        }, r -> r == SET_HALF);

        // arraylist-append — unsized ArrayList (Go: var v []int64).
        bench(runId, ts, "arraylist-append", "ArrayList", N_VEC, warmup, trials, () -> {
            ArrayList<Long> v = new ArrayList<>();
            for (long i = 0; i < N_VEC; i++) v.add(i);
            return v.size();
        }, r -> r == N_VEC);
    }
}
