// Java competitor: unit-5 lang/loops benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> LoopsBench.java; run: java -cp <out> LoopsBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.stream.LongStream;

public class LoopsBench {
    // ---- common harness (identical in every unit-5 java lang program) ----
    // One v1-shape CSV row (32 columns) per (benchmark, variant, library). Each
    // workload returns its result; the check is applied to every timed trial's
    // result, and every result is stored to a static sink, so nothing can be
    // dead-code eliminated. OPAQUE values are static non-final fields (never
    // compile-time constants for javac or C2).
    static long MASK = -1L;  // OPAQUE mask
    static long ONE = 1L;    // OPAQUE one
    static Object SINK;      // results and escaping allocations land here
    static final String RUN_ID = env("PROFILE_RUN_ID", "local");
    static final String TS = env("PROFILE_RUN_TS", "");
    static final String LANG_VERSION = env("PROFILE_LANG_VERSION", "");
    static final int WARMUP = Math.max(0, Integer.parseInt(env("PROFILE_WARMUP", "3")));
    static final int TRIALS = Math.max(1, Integer.parseInt(env("PROFILE_TRIALS", "10")));
    static final List<String> ONLY = parseOnly(env("LANG_ONLY", ""));
    static final String FLAGS = computeFlags();

    static String env(String k, String d) { String v = System.getenv(k); return v == null ? d : v; }

    static List<String> parseOnly(String o) {
        List<String> l = new ArrayList<>();
        for (String t : o.split(",")) if (!t.isEmpty()) l.add(t);
        return l;
    }

    static boolean selected(String slug) { return ONLY.isEmpty() || ONLY.contains(slug); }

    static String clean(String s) { return s.replace(',', ';').replace('\n', ' ').replace('\r', ' '); }

    // "java 25 (C2)" plus the JVM flags actually in effect (the program's jvm.flags line).
    static String computeFlags() {
        StringBuilder f = new StringBuilder("java " + Runtime.version().feature() + " (C2)");
        for (String a : ManagementFactory.getRuntimeMXBean().getInputArguments()) f.append(' ').append(a);
        return clean(f.toString());
    }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static void emit(String slug, long input, String lib, String libver, long[] s, boolean ok,
                     String notes, String variant) {
        Arrays.sort(s);
        int n = s.length;
        long sum = 0; for (long x : s) sum += x;
        long mn = s[0], med = s[n / 2], mean = sum / n, p95 = s[Math.min(n - 1, n * 95 / 100)];
        double mops = med > 0 ? (double) input / (double) med * 1e9 / 1e6 : 0.0;
        System.out.println("1," + RUN_ID + "," + TS + "," + slug + ",lang,," + input + ",," + input + ",java,"
            + LANG_VERSION + "," + clean(lib) + "," + clean(libver) + "," + FLAGS + "," + WARMUP + "," + TRIALS + ","
            + mn + "," + med + "," + mean + "," + p95 + "," + String.format("%.2f", mops) + ",Mop/s,"
            + peakRssKb() + ",-1,-1,-1,-1," + (ok ? "ok" : "invalid") + "," + ok + "," + clean(notes) + ","
            + variant + ",cpu");
        System.out.flush();
    }

    static void emitSkipped(String slug, long input, String lib, String libver, String reason, String variant) {
        if (!selected(slug)) return;
        System.out.println("1," + RUN_ID + "," + TS + "," + slug + ",lang,," + input + ",," + input + ",java,"
            + LANG_VERSION + "," + clean(lib) + "," + clean(libver) + "," + FLAGS + "," + WARMUP + "," + TRIALS
            + ",-1,-1,-1,-1,,Mop/s," + peakRssKb() + ",-1,-1,-1,-1,skipped,," + clean(reason) + "," + variant + ",cpu");
        System.out.flush();
    }

    // run(): warmup untimed runs, then timed trials; check every trial's result.
    static <T> void run(String slug, long input, String lib, String libver, String notes, String variant,
                        Supplier<T> f, Predicate<T> check) {
        if (!selected(slug)) return;
        boolean warmOk = true;
        for (int i = 0; i < WARMUP; i++) { T r = f.get(); SINK = r; warmOk &= check.test(r); }
        long[] s = new long[TRIALS];
        boolean ok = true;
        for (int i = 0; i < TRIALS; i++) {
            long t0 = System.nanoTime();
            T r = f.get();
            s[i] = System.nanoTime() - t0;
            SINK = r;
            if (!check.test(r)) ok = false;
        }
        SINK = warmOk;
        emit(slug, input, lib, libver, s, ok, notes, variant);
    }
    // ---- end common harness ----

    static int[] EARLY;  // read through a static each repetition so the scan is redone

    public static void main(String[] args) {
        final long N = 100000000L;

        run("loop-counted", N, "for", "stdlib", "", "", () -> {
            final long mask = MASK; long s = 0;
            for (long i = 0; i < N; ++i) s += i & mask;
            return s;
        }, s -> s == 4999999950000000L);

        if (selected("loop-counted-nobounds")) {
            int[] a = new int[1000000];
            for (int k = 0; k < a.length; ++k) a[k] = k;
            run("loop-counted-nobounds", 100L * 1000000L, "int[]", "stdlib", "checked access; HotSpot may hoist the range check", "bounds-on", () -> {
                long s = 0; final int n = a.length;
                for (int p = 0; p < 100; ++p) for (int k = 0; k < n; ++k) s += a[k];
                return s;
            }, s -> s == 49999950000000L);
        }
        emitSkipped("loop-counted-nobounds", 100L * 1000000L, "int[]", "stdlib",
            "the JVM has no unchecked array access; HotSpot removes only the checks it proves redundant", "bounds-off");

        run("loop-nested", 100000000L, "for", "stdlib", "", "", () -> {
            long s = 0;
            for (long i = 0; i < 10000; ++i) for (long j = 0; j < 10000; ++j) s += i ^ j;
            return s;
        }, s -> s == 642122061696L);

        if (selected("loop-foreach-array")) {
            int[] a = new int[10000000];
            for (int k = 0; k < a.length; ++k) a[k] = k % 1000;
            run("loop-foreach-array", 10000000L, "int[] enhanced for", "stdlib", "", "", () -> {
                long s = 0; for (int x : a) s += x; return s;
            }, s -> s == 4995000000L);
        }
        if (selected("loop-foreach-arraylist")) {
            ArrayList<Integer> l = new ArrayList<>();
            for (int k = 0; k < 10000000; ++k) l.add(k % 1000);
            run("loop-foreach-arraylist", 10000000L, "ArrayList<Integer> enhanced for", "stdlib", "boxed Integer", "", () -> {
                long s = 0; for (int x : l) s += x; return s;
            }, s -> s == 4995000000L);
        }

        run("loop-range", N, "LongStream.range", "stdlib", "sequential LongStream", "", () -> {
            final long mask = MASK;
            return LongStream.range(0, N).map(i -> i & mask).sum();
        }, s -> s == 4999999950000000L);

        if (selected("loop-reduce")) {
            double[] x = new double[10000000];
            for (int k = 0; k < x.length; ++k) x[k] = (k % 1000) * 0.5;
            run("loop-reduce", 10000000L, "for", "stdlib", "sequential; Java never reassociates floating point", "", () -> {
                double s = 0.0; for (int k = 0; k < x.length; ++k) s += x[k]; return s;
            }, s -> Math.abs(s - 2497500000.0) <= 1e-6 * 2497500000.0);
        }

        if (selected("loop-early-exit")) {
            int[] a = new int[10000000];
            for (int k = 0; k < a.length; ++k) a[k] = k;
            a[5000000] = -1;
            EARLY = a;
            run("loop-early-exit", 1000L * 10000000L, "for+break", "stdlib", "", "", () -> {
                long s = 0;
                for (int r = 0; r < 1000; ++r) {
                    final int[] p = EARLY;  // re-read each repetition; no hoisting of the search
                    long found = -1;
                    for (int k = 0; k < p.length; ++k) { if (p[k] < 0) { found = k; break; } }
                    s += found;
                }
                return s;
            }, s -> s == 5000000000L);
        }
    }
}
