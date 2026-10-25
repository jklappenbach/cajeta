// The collection/ competitors' shared inputs and row format (Java side).
// Mirrors bench/src/main/cajeta/bench/collection/CollGen.cajeta exactly: the
// same draws in the same order give the same keys, queries and checks in all
// six languages.
//   next(): s ^= s << 13; s ^= s >>> 7; s ^= s << 17; return s >>> 33
//           (xorshift64, seed 88172645463325252; a 31-bit draw)
//   key(i): three xorshift64 rounds of i ^ 0x5851F42D4C957F2D
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.function.Predicate;
import java.util.function.Supplier;

final class CollGen {
    private long s = 88172645463325252L;

    static long xs(long x) {
        x ^= x << 13;
        x ^= x >>> 7;
        x ^= x << 17;
        return x;
    }

    static long key(long i) { return xs(xs(xs(i ^ 0x5851F42D4C957F2DL))); }

    int next() { s = xs(s); return (int) (s >>> 33); }

    int[] indices(int count, int range) {
        int[] o = new int[count];
        for (int i = 0; i < count; i++) o[i] = next() % range;
        return o;
    }

    static String env(String k, String d) {
        String v = System.getenv(k);
        return v == null || v.isEmpty() ? d : v;
    }

    static long num(String k, long d) {
        try { return Long.parseLong(env(k, "")); } catch (NumberFormatException e) { return d; }
    }

    static int warmup() { return (int) num("PROFILE_WARMUP", 3); }
    static int trials() { return (int) num("PROFILE_TRIALS", 10); }

    static boolean selected(String id) {
        String s = env("PROFILE_BENCH", "");
        return s.isEmpty() || Arrays.asList(s.trim().split("\\s+")).contains(id);
    }

    static long peakRssKb() {
        try {
            for (String l : Files.readAllLines(Path.of("/proc/self/status")))
                if (l.startsWith("VmHWM:")) return Long.parseLong(l.split("\\s+")[1]);
        } catch (Exception e) { }
        return -1;
    }

    static final com.sun.management.ThreadMXBean MX = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
    static volatile Object sink;

    record Measured(long[] samples, boolean ok, long alloc) { }

    /** Warm up (JIT), time `trials` calls, check the last result untimed, then one call's allocated bytes. */
    static <T> Measured bench(Supplier<T> f, Predicate<T> check) {
        for (int i = 0; i < warmup(); i++) sink = f.get();
        int t = trials();
        long[] s = new long[t];
        boolean ok = true;
        for (int i = 0; i < t; i++) {
            long t0 = System.nanoTime();
            T r = f.get();
            s[i] = System.nanoTime() - t0;
            ok = check.test(r);
            sink = r;
        }
        long a0 = MX.getCurrentThreadAllocatedBytes();
        sink = f.get();
        long alloc = MX.getCurrentThreadAllocatedBytes() - a0;
        return new Measured(s, ok, alloc);
    }

    static String clean(String s) { return s.replace(',', ' '); }

    static void row(String id, String lib, String ver, long input, Measured m, String notes) { rowV(id, lib, ver, input, m, notes, ""); }

    static void rowV(String id, String lib, String ver, long input, Measured m, String notes, String variant) {
        long[] s = m.samples().clone();
        Arrays.sort(s);
        int n = s.length;
        long sum = 0;
        for (long v : s) sum += v;
        long med = s[n / 2];
        double mops = med > 0 ? input / (double) med * 1e3 : 0;
        System.out.printf("1,%s,%s,%s,collection,,%d,,%d,java,%s,%s,%s,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%b,%s,%s%n",
            env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), id, input, input, env("PROFILE_LANG_VERSION", ""),
            clean(lib), ver, warmup(), n, s[0], med, sum / n, s[Math.min(n - 1, n * 95 / 100)], mops, peakRssKb(), m.alloc(),
            m.ok() ? "ok" : "invalid", m.ok(), clean(notes), variant);
        System.out.flush();
    }

    static void skip(String id, String lib, long input, String reason) {
        System.out.printf("1,%s,%s,%s,collection,,%d,,%d,java,%s,%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,%n",
            env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), id, input, input, env("PROFILE_LANG_VERSION", ""), clean(lib), clean(reason));
        System.out.flush();
    }

    static boolean flag(String k) { String v = env(k, ""); return v.equals("1") || v.equals("true") || v.equals("yes"); }

    /** CAJETA_BENCH_SMOKE=1 runs the first-suite rows at n/100 (CollGen.sized on the Cajeta side); CAJETA_BENCH_FULL_SIZE=1 keeps the page shape. */
    static boolean smallSmoke() { return flag("CAJETA_BENCH_SMOKE") && !flag("CAJETA_BENCH_FULL_SIZE"); }
    static int sized(int full) { return smallSmoke() ? full / 100 : full; }
    static String sizeNote(int full, String extra) {
        String s = smallSmoke() ? "smoke size n/100 (page shape " + full + ")" : "";
        if (extra.isEmpty()) return s;
        return s.isEmpty() ? extra : extra + "; " + s;
    }
    static String hex16(long v) { return String.format("%016x", v); }

    /** bench with an untimed input step: only f.apply(mk.get()) is timed. */
    static <I, T> Measured benchWith(Supplier<I> mk, java.util.function.Function<I, T> f, Predicate<T> check) {
        for (int i = 0; i < warmup(); i++) sink = f.apply(mk.get());
        int t = trials();
        long[] s = new long[t];
        boolean ok = true;
        for (int i = 0; i < t; i++) {
            I in = mk.get();
            long t0 = System.nanoTime();
            T r = f.apply(in);
            s[i] = System.nanoTime() - t0;
            ok = check.test(r);
            sink = r;
        }
        I in = mk.get();
        long a0 = MX.getCurrentThreadAllocatedBytes();
        sink = f.apply(in);
        return new Measured(s, ok, MX.getCurrentThreadAllocatedBytes() - a0);
    }
}
