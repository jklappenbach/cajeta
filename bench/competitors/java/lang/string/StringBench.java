// Java string competitors — the same four workloads the Cajeta side runs over a
// ~360 KB ASCII text (base "the quick brown fox jumps over the lazy dog " × 8192):
// string-search (absent needle), string-replace (fox→feline), string-uppercase,
// string-build-concat (build 4000 chars). One schema-conformant CSV row per
// (benchmark, library), competitors/columns.txt order. Stdlib String idioms
// (indexOf / replace (literal) / toUpperCase / StringBuilder).
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners). alloc_bytes is per-thread bytes allocated for one execution,
// via the HotSpot com.sun.management.ThreadMXBean — the Java-native analog of
// Go's TotalAlloc delta / Rust's global allocator / C++ operator new override.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.function.IntPredicate;
import java.util.function.IntSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;

public class StringBench {
    static final String BASE = "the quick brown fox jumps over the lazy dog ";
    static final int REPEAT = 8192;       // BASE(44) × 8192 = 360448 = TEXTLEN
    static final String PIECE = "abcdefgh";
    static final int BUILD_ITERS = 500;   // PIECE(8) × 500 = 4000 = BUILDLEN
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    // sink to defeat dead-code elimination of the measured work.
    static volatile long sinkL;
    static volatile String sinkS;

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

    static void emit(String runId, String ts, String bench, String lib, int bytes,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mbps = med > 0 ? (double) bytes / (double) med * 1e9 / 1048576.0 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,string,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, bytes, bytes, env("PROFILE_LANG_VERSION", ""), lib,
            warmup, trials, mn, med, mean, p95, mbps, peakRssKb(), alloc, status, ok);
    }

    static long[] benchInt(int warmup, int trials, IntSupplier f, IntPredicate check, boolean[] okOut) {
        for (int i = 0; i < warmup; i++) sinkL = f.getAsInt();
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            int r = f.getAsInt();
            samples[i] = System.nanoTime() - t0;
            sinkL = r;
            ok = check.test(r);
        }
        okOut[0] = ok;
        return samples;
    }

    static long[] benchStr(int warmup, int trials, Supplier<String> f, Predicate<String> check, boolean[] okOut) {
        for (int i = 0; i < warmup; i++) sinkS = f.get();
        long[] samples = new long[trials];
        boolean ok = true;
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            String r = f.get();
            samples[i] = System.nanoTime() - t0;
            sinkS = r;
            ok = check.test(r);
        }
        okOut[0] = ok;
        return samples;
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        String text = BASE.repeat(REPEAT);
        int nbytes = text.length();
        String needle = "zzzzqx-not-present";
        boolean[] ok = new boolean[1];

        // search — absent ⇒ -1
        IntSupplier search = () -> text.indexOf(needle);
        long[] s1 = benchInt(warmup, trials, search, r -> r == -1, ok);
        boolean ok1 = ok[0];
        long a1 = allocOf(() -> sinkL = search.getAsInt());
        emit(runId, ts, "string-search", "String.indexOf", nbytes, warmup, trials, s1, ok1, a1);

        // replace — literal fox→feline, output grows
        Supplier<String> replace = () -> text.replace("fox", "feline");
        long[] s2 = benchStr(warmup, trials, replace, r -> r.length() > nbytes, ok);
        boolean ok2 = ok[0];
        long a2 = allocOf(() -> sinkS = replace.get());
        emit(runId, ts, "string-replace", "String.replace", nbytes, warmup, trials, s2, ok2, a2);

        // uppercase — same length (ASCII)
        Supplier<String> upper = () -> text.toUpperCase();
        long[] s3 = benchStr(warmup, trials, upper, r -> r.length() == nbytes, ok);
        boolean ok3 = ok[0];
        long a3 = allocOf(() -> sinkS = upper.get());
        emit(runId, ts, "string-uppercase", "String.toUpperCase", nbytes, warmup, trials, s3, ok3, a3);

        // build-concat — StringBuilder, 4000 chars
        Supplier<String> build = () -> {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < BUILD_ITERS; i++) b.append(PIECE);
            return b.toString();
        };
        long[] s4 = benchStr(warmup, trials, build, r -> r.length() == BUILD_ITERS * 8, ok);
        boolean ok4 = ok[0];
        long a4 = allocOf(() -> sinkS = build.get());
        emit(runId, ts, "string-build-concat", "StringBuilder", BUILD_ITERS * 8, warmup, trials, s4, ok4, a4);
    }
}
