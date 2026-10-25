// Java competitor: unit-5 lang/exceptions benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> ExceptionsBench.java; run: java -cp <out> ExceptionsBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;

public class ExceptionsBench {
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

    // library "throw (stack trace)": a normal checked Exception subclass
    static final class TraceEx extends Exception {
        TraceEx(String m) { super(m); }
    }
    // library "throw (no stack trace)": writableStackTrace=false, so no stack walk on construction
    static final class NoTraceEx extends Exception {
        NoTraceEx(String m) { super(m, null, false, false); }
    }
    // throw-recoverable: checked vs unchecked
    static final class RecoverableEx extends Exception {
        RecoverableEx(String m) { super(m); }
    }
    static final class UnrecoverableEx extends RuntimeException {
        UnrecoverableEx(String m) { super(m); }
    }

    static long f(long i, long mask) throws TraceEx {
        if (i < 0) throw new TraceEx("bench error");
        return i & mask;
    }

    // depth frames of recursion; the innermost throws when i >= 0 (always).
    static long throwerT(int depth, long i) throws TraceEx {
        if (depth <= 1) {
            if (i >= 0) throw new TraceEx("bench error");
            return i;
        }
        return throwerT(depth - 1, i) + 1;
    }
    static long throwerN(int depth, long i) throws NoTraceEx {
        if (depth <= 1) {
            if (i >= 0) throw new NoTraceEx("bench error");
            return i;
        }
        return throwerN(depth - 1, i) + 1;
    }
    static long throwerR(int depth, long i) throws RecoverableEx {
        if (depth <= 1) {
            if (i >= 0) throw new RecoverableEx("bench error");
            return i;
        }
        return throwerR(depth - 1, i) + 1;
    }
    static long throwerU(int depth, long i) {
        if (depth <= 1) {
            if (i >= 0) throw new UnrecoverableEx("bench error");
            return i;
        }
        return throwerU(depth - 1, i) + 1;
    }

    static long catchT(int depth, long iters) {
        long count = 0;
        for (long i = 0; i < iters; ++i) {
            try { long r = throwerT(depth, i); count -= r; }  // never reached
            catch (TraceEx e) { ++count; }
        }
        return count;
    }
    static long catchN(int depth, long iters) {
        long count = 0;
        for (long i = 0; i < iters; ++i) {
            try { long r = throwerN(depth, i); count -= r; }
            catch (NoTraceEx e) { ++count; }
        }
        return count;
    }
    static long catchR(int depth, long iters) {
        long count = 0;
        for (long i = 0; i < iters; ++i) {
            try { long r = throwerR(depth, i); count -= r; }
            catch (RecoverableEx e) { ++count; }
        }
        return count;
    }
    static long catchU(int depth, long iters) {
        long count = 0;
        for (long i = 0; i < iters; ++i) {
            try { long r = throwerU(depth, i); count -= r; }
            catch (UnrecoverableEx e) { ++count; }
        }
        return count;
    }

    // one virtual thread performs the work and hands its count back to the spawner
    static long inFiber(boolean trace, long iters) {
        final long[] out = new long[1];
        Thread t = Thread.ofVirtual().start(() -> { out[0] = trace ? catchT(1, iters) : catchN(1, iters); });
        try { t.join(); } catch (InterruptedException e) { throw new RuntimeException(e); }
        return out[0];
    }

    static final String LT = "throw (stack trace)";
    static final String LN = "throw (no stack trace)";
    static final String NT = "Exception subclass";
    static final String NN = "Exception subclass with writableStackTrace false; C2 may compile an inlined throw to a jump";

    public static void main(String[] args) {
        final long N = 100000000L;

        run("try-no-throw", N, "try", "stdlib", "f is a plain static method (C2 may inline it: Java has no noinline)", "", () -> {
            final long mask = MASK; long s = 0;
            for (long i = 0; i < N; ++i) {
                try { s += f(i, mask); }
                catch (TraceEx e) { s -= 1; }
            }
            return s;
        }, s -> s == 4999999950000000L);

        run("throw-catch-1", 1000000L, LT, "stdlib", NT, "", () -> catchT(1, 1000000L), c -> c == 1000000L);
        run("throw-catch-1", 1000000L, LN, "stdlib", NN, "", () -> catchN(1, 1000000L), c -> c == 1000000L);
        run("throw-catch-10", 1000000L, LT, "stdlib", NT, "", () -> catchT(10, 1000000L), c -> c == 1000000L);
        run("throw-catch-10", 1000000L, LN, "stdlib", NN, "", () -> catchN(10, 1000000L), c -> c == 1000000L);
        run("throw-catch-100", 100000L, LT, "stdlib", NT, "", () -> catchT(100, 100000L), c -> c == 100000L);
        run("throw-catch-100", 100000L, LN, "stdlib", NN, "", () -> catchN(100, 100000L), c -> c == 100000L);

        final String RN = "Cajeta RecoverableException maps to a checked exception and UnrecoverableException to an unchecked one";
        run("throw-recoverable", 1000000L, LT, "stdlib", RN + "; checked Exception subclass", "recoverable",
            () -> catchR(1, 1000000L), c -> c == 1000000L);
        run("throw-recoverable", 1000000L, LT, "stdlib", RN + "; RuntimeException subclass", "unrecoverable",
            () -> catchU(1, 1000000L), c -> c == 1000000L);

        run("throw-in-fiber", 100000L, LT, "stdlib", NT + "; one virtual thread (Thread.ofVirtual) joined", "",
            () -> inFiber(true, 100000L), c -> c == 100000L);
        run("throw-in-fiber", 100000L, LN, "stdlib", NN + "; one virtual thread (Thread.ofVirtual) joined", "",
            () -> inFiber(false, 100000L), c -> c == 100000L);
    }
}
