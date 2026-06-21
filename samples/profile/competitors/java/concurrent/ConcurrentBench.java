// Java concurrency competitors — the same two workloads the Cajeta side runs:
// atomic-fetchadd (1M single-threaded AtomicLong.incrementAndGet() → counter==N)
// and task-spawn-await (20K: spawn a virtual thread computing i+1, join it,
// accumulate → sum == N*(N+1)/2). One schema-conformant CSV row per benchmark,
// in competitors/columns.txt order. Cross-check = the exact closed forms
// (1000000 / 200010000). The task primitive is a Java 21+ virtual thread (Loom)
// — the headline lightweight-task analog to Go's goroutine, contrast with OS
// threads.
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners). alloc_bytes is per-(submitting-)thread bytes allocated for one
// execution, via the HotSpot com.sun.management.ThreadMXBean — the Java-native
// analog of Go's TotalAlloc delta; for the spawn benchmark it only sees the
// submitting thread's allocation (an accepted limitation, mirrored across langs).
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;
import java.util.function.LongPredicate;

public class ConcurrentBench {
    static final int N_ATOMIC = 1000000;
    static final int N_SPAWN = 20000;
    static final long SPAWN_REF = 200010000L;
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

    static void emit(String runId, String ts, String bench, String lib,
                     int input, int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mops = med > 0 ? (double) input / (double) med * 1e9 / 1e6 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,concurrent,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mops, peakRssKb(), alloc, status, ok);
    }

    static long[] benchI64(int warmup, int trials, LongSupplier f, LongPredicate check, boolean[] okOut) {
        for (int i = 0; i < warmup; i++) f.getAsLong();
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            long r = f.getAsLong();
            samples[i] = System.nanoTime() - t0;
            ok = check.test(r);
        }
        okOut[0] = ok;
        return samples;
    }

    static long atomicFetchAdd() {
        AtomicLong a = new AtomicLong();
        for (int i = 0; i < N_ATOMIC; i++) a.incrementAndGet();
        return a.get();
    }

    // Spawn a virtual thread computing i+1, join (await) it, accumulate — one
    // lightweight task spawned + awaited per iteration, mirroring Go's goroutine
    // + channel loop.
    static long taskSpawnAwait() {
        long sum = 0;
        for (long i = 0; i < N_SPAWN; i++) {
            final long fi = i;
            final long[] box = new long[1];
            Thread t = Thread.ofVirtual().start(() -> box[0] = fi + 1);
            try { t.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            sum += box[0];
        }
        return sum;
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        // atomic-fetchadd
        boolean[] ok = new boolean[1];
        long[] s = benchI64(warmup, trials, ConcurrentBench::atomicFetchAdd, r -> r == N_ATOMIC, ok);
        long alloc = allocOf(ConcurrentBench::atomicFetchAdd);
        emit(runId, ts, "atomic-fetchadd", "AtomicLong", N_ATOMIC, warmup, trials, s, ok[0], alloc);

        // task-spawn-await (virtual thread + join, spawn then await each)
        s = benchI64(warmup, trials, ConcurrentBench::taskSpawnAwait, r -> r == SPAWN_REF, ok);
        alloc = allocOf(ConcurrentBench::taskSpawnAwait);
        emit(runId, ts, "task-spawn-await", "virtual-threads", N_SPAWN, warmup, trials, s, ok[0], alloc);
    }
}
