import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

// Row emission for the Java io competitor (v1 rows, 31 columns, area io;
// competitors/lib/rows.sh turns them into schema v2), plus the sizes.
final class BenchRow {
    interface Body { void run() throws Exception; }

    static String env(String k, String d) {
        String v = System.getenv(k);
        return (v == null || v.isEmpty()) ? d : v;
    }
    static int envInt(String k, int d) {
        try { return Integer.parseInt(env(k, "" + d)); } catch (NumberFormatException e) { return d; }
    }
    static boolean smoke() {
        String s = env("CAJETA_BENCH_SMOKE", "");
        return s.equals("1") || s.equals("true") || s.equals("yes");
    }
    static long pick(long full, long sm) { return smoke() ? sm : full; }
    static int warm(int d) { return smoke() ? 1 : envInt("PROFILE_WARMUP", d); }
    static int trials(int d) { return smoke() ? envInt("PROFILE_TRIALS", 3) : envInt("PROFILE_TRIALS", d); }
    static String sm(String what) { return smoke() ? " (smoke; the page's " + what + " is the full run)" : ""; }

    static final String RUN_ID = env("PROFILE_RUN_ID", "local");
    static final String TS = env("PROFILE_RUN_TS", "");
    static final String LANG_VER = clean(env("PROFILE_LANG_VERSION", "java " + System.getProperty("java.version")));
    static final String FLAGS = clean(env("PROFILE_JAVA_FLAGS", "java default flags (C2 JIT warmed)"));

    static String clean(String s) { return s.replace(",", ";").replace("\"", "").replace("\n", " "); }

    static long statusKb(String key) {
        try {
            for (String l : Files.readAllLines(Paths.get("/proc/self/status"))) {
                if (l.startsWith(key)) return Long.parseLong(l.split("\\s+")[1]);
            }
        } catch (Exception e) { }
        return -1;
    }
    static long peakRssKb() { return statusKb("VmHWM:"); }
    static long rssKb() { return statusKb("VmRSS:"); }

    /** utime + stime in milliseconds (USER_HZ = 100). */
    static long cpuMs() {
        try {
            String s = Files.readString(Paths.get("/proc/self/stat"));
            String[] f = s.substring(s.lastIndexOf(')') + 2).split(" ");
            return (Long.parseLong(f[11]) + Long.parseLong(f[12])) * 10;
        } catch (Exception e) { return -1; }
    }

    static long[] statsOf(long[] s) {
        long[] v = s.clone();
        Arrays.sort(v);
        long sum = 0;
        for (long x : v) sum += x;
        int n = v.length;
        return new long[] { v[0], v[n / 2], sum / n, v[Math.min(n - 1, n * 95 / 100)] };
    }

    /** `warmup` untimed runs, then `trials` timed; returns {min, median, mean, p95}. */
    static long[] measure(int warmup, int trials, Body b) throws Exception {
        for (int i = 0; i < warmup; i++) b.run();
        int t = Math.max(1, trials);
        long[] s = new long[t];
        for (int i = 0; i < t; i++) {
            long t0 = System.nanoTime();
            b.run();
            s[i] = System.nanoTime() - t0;
        }
        return statsOf(s);
    }

    static void row(String slug, long input, String lib, String libVer, int warmup, int trials, long[] st,
                    boolean ok, String notes, String unit) {
        String thr = "", u = "";
        if (!unit.isEmpty() && st[1] > 0 && input >= 0) {
            thr = String.format("%.3f", input * 1e9 / (double) st[1]);
            u = unit;
        }
        System.out.println(String.join(",", "1", RUN_ID, TS, slug, "io", "", "-1", "", "" + input,
            "java", LANG_VER, lib, clean(libVer), FLAGS, "" + warmup, "" + trials,
            "" + st[0], "" + st[1], "" + st[2], "" + st[3], thr, u, "" + peakRssKb(), "-1", "-1", "-1", "-1",
            ok ? "ok" : "invalid", ok ? "true" : "false", clean(notes) + (smoke() ? "; smoke" : ""), ""));
        System.out.flush();
    }

    static void err(String slug, String lib, Throwable e) {
        System.out.println(String.join(",", "1", RUN_ID, TS, slug, "io", "", "-1", "", "-1",
            "java", LANG_VER, lib, "", FLAGS, "0", "0", "-1", "-1", "-1", "-1", "", "", "-1", "-1", "-1", "-1", "-1",
            "invalid", "false", clean("error: " + e), ""));
        System.out.flush();
    }
}
