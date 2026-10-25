// Java competitor: unit-5 lang/alloc benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> AllocBench.java; run: java -cp <out> AllocBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;

public class AllocBench {
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

    // the 32-byte object: four int64 fields all set to v
    static final class Obj {
        long a, b, c, d;
        Obj(long v) { a = v; b = v; c = v; d = v; }
    }
    record P(long x, long y) {}

    static Obj OSINK;      // every allocation is stored here so escape analysis cannot remove it
    static Obj[] RING;     // the churn ring escapes too
    static int[] ASINK;

    static long pass(Obj o, int depth) {
        if (depth == 0) return o.a;
        return pass(o, depth - 1);
    }

    static long churn(int r, long n) {
        Obj[] ring = new Obj[r];
        RING = ring;
        long s = 0;
        for (long i = 0; i < n; ++i) {
            int k = (int) (i % r);
            ring[k] = new Obj(i);  // previous occupant (oldest) dropped to the GC
            s += ring[k].a;
        }
        long ringsum = 0;
        for (Obj p : ring) if (p != null) ringsum += p.a;
        SINK = ringsum;
        return s;
    }

    public static void main(String[] args) {
        final long N = 10000000L;
        final String ESC = "stored to a static so escape analysis cannot remove it; TLAB+GC";

        run("alloc-small-object", N, "new", "stdlib", ESC, "", () -> {
            long s = 0;
            for (long i = 0; i < N; ++i) { Obj o = new Obj(i); OSINK = o; s += o.a; }
            return s;
        }, s -> s == 49999995000000L);

        run("alloc-churn-1", N, "reference ring", "stdlib", "plain reference replaced; ring array stored to a static; GC", "",
            () -> churn(1, N), s -> s == 49999995000000L);
        run("alloc-churn-10", N, "reference ring", "stdlib", "plain reference replaced; ring array stored to a static; GC", "",
            () -> churn(10, N), s -> s == 49999995000000L);

        run("alloc-transfer-chain", 1000000L, "reference pass", "stdlib",
            "no ownership transfer: a reference is passed; " + ESC, "", () -> {
            long s = 0;
            for (long i = 0; i < 1000000L; ++i) { Obj o = new Obj(i); OSINK = o; s += pass(o, 10); }
            return s;
        }, s -> s == 499999500000L);

        emitSkipped("alloc-live-set-on", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "");
        emitSkipped("alloc-live-set-off", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "");

        run("alloc-array", 100000L, "new int[1024]", "stdlib", "zeroed by the JVM; " + ESC, "", () -> {
            long s = 0;
            for (long i = 0; i < 100000L; ++i) {
                int[] a = new int[1024];
                ASINK = a;
                a[0] = (int) i; a[1023] = 1;
                s += a[0] + a[1023];
            }
            return s;
        }, s -> s == 5000050000L);

        run("alloc-struct-value", 100000000L, "record", "stdlib", "record; escape analysis scalarizes it", "", () -> {
            final long mask = MASK; long s = 0;
            for (long i = 0; i < 100000000L; ++i) { P p = new P(i, i & mask); s += p.x() + p.y(); }
            return s;
        }, s -> s == 9999999900000000L);
    }
}
