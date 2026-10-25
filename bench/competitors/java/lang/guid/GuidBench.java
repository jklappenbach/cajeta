// Java competitor: unit-5 lang/guid benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> GuidBench.java; run: java -cp <out> GuidBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.HashSet;
import java.util.UUID;

public class GuidBench {
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

    static final String LIB = "java.util.UUID";
    static final String VER = Runtime.version().toString();

    static long distinct(UUID[] ids) {
        HashSet<UUID> set = new HashSet<>(ids.length * 2);
        for (UUID u : ids) if (u != null) set.add(u);
        return set.size();
    }

    public static void main(String[] args) {
        final int N = 1000000;

        if (selected("guid-generate")) {
            final UUID[] ids = new UUID[N];  // preallocated array of UUID references
            run("guid-generate", N, LIB, VER, "UUID.randomUUID (SecureRandom) into a preallocated UUID[]", "", () -> {
                for (int k = 0; k < N; ++k) ids[k] = UUID.randomUUID();
                return ids;
            }, p -> {
                boolean ok = distinct(p) == N;  // untimed distinct-count check
                Arrays.fill(ids, null);
                return ok;
            });
        }

        if (selected("guid-parse") || selected("guid-format")) {
            final UUID[] ids = new UUID[N];
            final String[] strs = new String[N];
            for (int k = 0; k < N; ++k) { ids[k] = UUID.randomUUID(); strs[k] = ids[k].toString(); }

            final UUID[] parsed = new UUID[N];
            run("guid-parse", N, LIB, VER, "UUID.fromString", "", () -> {
                for (int k = 0; k < N; ++k) parsed[k] = UUID.fromString(strs[k]);
                return parsed;
            }, p -> {
                long eq = 0;
                for (int k = 0; k < N; ++k) if (p[k] != null && p[k].equals(ids[k])) ++eq;
                Arrays.fill(parsed, null);  // the next trial must parse again
                return eq == N;
            });

            final String[] out = new String[N];
            run("guid-format", N, LIB, VER, "UUID.toString into a preallocated String[]", "", () -> {
                for (int k = 0; k < N; ++k) out[k] = ids[k].toString();
                return out;
            }, o -> {
                long total = 0;
                for (int k = 0; k < N; ++k) if (o[k] != null && o[k].equals(o[k].toLowerCase())) total += o[k].length();
                Arrays.fill(out, null);
                return total == 36000000L;
            });
        }
    }
}
