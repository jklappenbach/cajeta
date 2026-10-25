import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

// Row emission shared by the codec Java competitors (one copy per program
// directory, since each program compiles on its own). Prints v1 rows, 31
// columns, in competitors/columns.txt order; competitors/lib/rows.sh turns
// them into schema v2.
final class BenchRow {
    interface Body { Object run() throws Exception; }

    static String env(String k, String d) {
        String v = System.getenv(k);
        return (v == null || v.isEmpty()) ? d : v;
    }
    static int envInt(String k, int d) {
        try { return Integer.parseInt(env(k, "" + d)); } catch (NumberFormatException e) { return d; }
    }
    static final String RUN_ID = env("PROFILE_RUN_ID", "local");
    static final String TS = env("PROFILE_RUN_TS", "");
    static final String LANG_VER = clean(env("PROFILE_LANG_VERSION", "java " + System.getProperty("java.version")));
    static final String FLAGS = clean(env("PROFILE_JAVA_FLAGS", "java default flags (C2 JIT warmed)"));

    static String clean(String s) { return s.replace(",", " ").replace("\"", ""); }

    static volatile Object SINK;

    static long peakRssKb() {
        try {
            for (String l : Files.readAllLines(Paths.get("/proc/self/status"))) {
                if (l.startsWith("VmHWM:")) return Long.parseLong(l.split("\\s+")[1]);
            }
        } catch (Exception e) { }
        return -1;
    }

    static long allocOf(Body b) throws Exception {
        com.sun.management.ThreadMXBean t = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        long tid = Thread.currentThread().threadId();
        long a0 = t.getThreadAllocatedBytes(tid);
        SINK = b.run();
        long a1 = t.getThreadAllocatedBytes(tid);
        return a1 >= a0 ? a1 - a0 : -1;
    }

    /** Warm up, time `trials` runs; returns {min, median, mean, p95, alloc}. Result of the last trial in SINK. */
    static long[] measure(Body b, int warmup, int trials) throws Exception {
        for (int i = 0; i < warmup; i++) SINK = b.run();
        long[] s = new long[trials];
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            Object r = b.run();
            s[i] = System.nanoTime() - t0;
            SINK = r;
        }
        Object last = SINK;
        long alloc = allocOf(b);
        SINK = last;
        Arrays.sort(s);
        long sum = 0;
        for (long x : s) sum += x;
        int n = s.length;
        return new long[] { s[0], s[n / 2], sum / n, s[Math.min(n - 1, n * 95 / 100)], alloc };
    }

    static void row(String slug, String dataset, long bytes, long input, String lib, String libVer,
                    int warmup, int trials, long[] st, boolean ok, String notes, String variant, boolean mbps) {
        String thr = "", unit = "";
        if (mbps && st[1] > 0) {
            thr = String.format("%.1f", input / (double) st[1] * 1e9 / 1048576.0);
            unit = "MB/s";
        }
        System.out.println(String.join(",", "1", RUN_ID, TS, slug, "codec", dataset, "" + bytes, "", "" + input,
            "java", LANG_VER, lib, libVer, FLAGS, "" + warmup, "" + trials,
            "" + st[0], "" + st[1], "" + st[2], "" + st[3], thr, unit, "" + peakRssKb(), "-1", "" + st[4], "-1", "-1",
            ok ? "ok" : "invalid", ok ? "true" : "false", clean(notes), variant));
    }

    static void skip(String slug, String dataset, long bytes, long input, String lib, String libVer, String reason, String variant) {
        System.out.println(String.join(",", "1", RUN_ID, TS, slug, "codec", dataset, "" + bytes, "", "" + input,
            "java", LANG_VER, lib, libVer, FLAGS, "0", "0", "-1", "-1", "-1", "-1", "", "", "-1", "-1", "-1", "-1", "-1",
            "skipped", "", clean(reason), variant));
    }
}
