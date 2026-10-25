// Java competitor: unit-5 lang/dispatch benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> DispatchBench.java; run: java $(cat jvm.flags) -cp <out> DispatchBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.BinaryOperator;
import java.util.function.LongUnaryOperator;

public class DispatchBench {
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

    // call-direct target; jvm.flags keeps C2 from inlining it (dontinline,DispatchBench::direct)
    static long direct(long x) { return x + 1; }
    static long inc(long x) { return x + 1; }

    // call-virtual: an abstract base class with two overriding subclasses
    static abstract class VBase { abstract long f(long x); }
    static final class VA extends VBase { long f(long x) { return x + 1; } }
    static final class VB extends VBase { long f(long x) { return x + 2; } }

    // call-interface: an interface with two implementations
    interface IFace { long f(long x); }
    static final class IA implements IFace { public long f(long x) { return x + 1; } }
    static final class IB implements IFace { public long f(long x) { return x + 2; } }

    static <T> T combine(BinaryOperator<T> op, T a, T b) { return op.apply(a, b); }

    static class Base {}
    static class Mid extends Base {}
    static class Leaf extends Mid {}

    // held in static non-final fields so the receivers are not compile-time known
    static VBase[] VOBJS = { new VA(), new VB() };
    static IFace[] IOBJS = { new IA(), new IB() };
    static Base[] HOBJS = { new Base(), new Mid(), new Leaf() };

    public static void main(String[] args) {
        final long N = 100000000L;

        run("call-direct", N, "static method", "stdlib", "jvm.flags: -XX:CompileCommand=dontinline DispatchBench::direct", "", () -> {
            final long mask = MASK; long s = 0;
            for (long i = 0; i < N; ++i) s += direct(i & mask);
            return s;
        }, s -> s == 5000000050000000L);

        run("call-virtual", N, "abstract class", "stdlib", "bimorphic call site: C2 inlines both targets behind a class check", "", () -> {
            final VBase[] objs = VOBJS; long s = 0;
            for (long i = 0; i < N; ++i) s += objs[(int) (i & 1)].f(i);
            return s;
        }, s -> s == 5000000100000000L);

        run("call-interface", N, "interface", "stdlib", "bimorphic call site: C2 inlines both targets behind a class check", "", () -> {
            final IFace[] objs = IOBJS; long s = 0;
            for (long i = 0; i < N; ++i) s += objs[(int) (i & 1)].f(i);
            return s;
        }, s -> s == 5000000100000000L);

        run("call-template", N, "generic method", "stdlib", "erased generic; int boxed to Integer", "", () -> {
            final BinaryOperator<Integer> add = Integer::sum;
            long s = 0;
            for (long i = 0; i < N; ++i) s += combine(add, (int) (i & 1023), 1);
            return s;
        }, s -> s == 51249901696L);

        run("call-lambda", N, "LongUnaryOperator lambda", "stdlib", "captures OPAQUE k", "", () -> {
            final long k = ONE;
            final LongUnaryOperator f = x -> x + k;
            long s = 0;
            for (long i = 0; i < N; ++i) s += f.applyAsLong(i);
            return s;
        }, s -> s == 5000000050000000L);

        run("call-method-ref", N, "LongUnaryOperator method ref", "stdlib", "DispatchBench::inc", "", () -> {
            final LongUnaryOperator g = DispatchBench::inc;
            long s = 0;
            for (long i = 0; i < N; ++i) s += g.applyAsLong(i);
            return s;
        }, s -> s == 5000000050000000L);

        run("instanceof", N, "instanceof", "stdlib", "", "", () -> {
            final Base[] objs = HOBJS; long count = 0;
            for (long i = 0; i < N; ++i) if (objs[(int) (i % 3)] instanceof Mid) ++count;
            return count;
        }, c -> c == 66666666L);
    }
}
