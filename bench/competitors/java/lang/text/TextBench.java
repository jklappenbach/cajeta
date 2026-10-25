// Java competitor: unit-5 lang/text benchmarks (contract: u5-competitor-spec.md).
// Build: javac -d <out> TextBench.java; run: java -cp <out> TextBench.
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.nio.charset.StandardCharsets;

public class TextBench {
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

    record CpResult(long count, long sum) {}

    public static void main(String[] args) {
        if (selected("string-codepoint-walk")) {
            final String base = "Hello, wörld! Καλημέρα κόσμε, "
                + "こんにちは世界 🚀✨ ";
            if (base.getBytes(StandardCharsets.UTF_8).length != 74) {
                System.err.println("base size " + base.getBytes(StandardCharsets.UTF_8).length + " != 74"); System.exit(1);
            }
            final String text = base.repeat(14170);
            final long bytes = text.getBytes(StandardCharsets.UTF_8).length;
            run("string-codepoint-walk", bytes, "String.codePoints", "stdlib",
                "codePoints().forEach over the UTF-16 String; input counted in UTF-8 bytes", "", () -> {
                final long[] acc = new long[2];
                text.codePoints().forEach(c -> { acc[0]++; acc[1] += c; });
                return new CpResult(acc[0], acc[1]);
            }, r -> r.count() == 580970L && r.sum() == 3748489290L);
        }

        if (selected("string-byteat-scan")) {
            final byte[] b = "the quick brown fox jumps over the lazy dog\n".repeat(23832).getBytes(StandardCharsets.UTF_8);
            run("string-byteat-scan", b.length, "getBytes(UTF_8) byte[] index", "stdlib", "", "", () -> {
                long c = 0; final int n = b.length;
                for (int k = 0; k < n; ++k) if (b[k] == '\n') ++c;
                return c;
            }, c -> c == 23832L);
        }

        if (selected("string-split")) {
            final String text = "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa,".repeat(18397) + "end";
            run("string-split", 100L * text.length(), "String.split", "stdlib", "single-char fastpath (no regex)", "", () -> {
                long total = 0;
                for (int r = 0; r < 100; ++r) { String[] parts = text.split(","); SINK = parts; total += parts.length; }
                return total;
            }, t -> t == 18397100L);
        }

        if (selected("string-compare")) {
            final String[] a = new String[64], b = new String[64];
            for (int k = 0; k < 64; ++k) {
                StringBuilder sb = new StringBuilder("abcdefghijklmnopqrstuvwxyz0123");
                sb.append((char) ('0' + k / 10)).append((char) ('0' + k % 10));
                a[k] = sb.toString();
                if ((k & 1) != 0) sb.setCharAt(31, 'x');
                b[k] = sb.toString();  // a fresh String object either way
            }
            run("string-compare", 10000000L, "String.equals", "stdlib", "", "", () -> {
                long eq = 0;
                for (long i = 0; i < 10000000L; ++i) if (a[(int) (i & 63)].equals(b[(int) (i & 63)])) ++eq;
                return eq;
            }, e -> e == 5000000L);
        }

        run("stringbuilder-append", 1000000L, "StringBuilder", "stdlib", "", "", () -> {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 1000000; ++i) sb.append("0123456789abcdef");
            String out = sb.toString();
            SINK = out;
            return (long) out.length();
        }, n -> n == 16000000L);

        run("stringbuilder-append-mixed", 1000000L, "StringBuilder.append(int/double)", "stdlib", "", "", () -> {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 1000000; ++i) {
                int m = i % 3;
                if (m == 0) sb.append(i);
                else if (m == 1) sb.append((double) (i % 1000) + 0.5);
                else sb.append("abc");
            }
            String out = sb.toString();
            SINK = out;
            return (long) out.length();
        }, n -> n == 4592962L);

        if (selected("stringbuilder-tostring")) {
            final StringBuilder builder = new StringBuilder();
            for (int r = 0; r < 1048576; ++r) builder.append("0123456789abcdef");
            run("stringbuilder-tostring", 100L, "StringBuilder.toString", "stdlib", "copies 16 MiB per materialization", "", () -> {
                long total = 0;
                for (int r = 0; r < 100; ++r) { String s = builder.toString(); SINK = s; total += s.length(); }
                return total;
            }, t -> t == 1677721600L);
        }
    }
}
