import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

// reflect/ competitor in Java (bench/reflect/Class.md): java.lang.reflect.
//   class-lookup     Class.forName(name) 1e7 times (smoke 1e6), cycling over 4 class names; the JVM caches
//                    loaded classes, so this is the cached lookup; check: found count == n
//   field-enumerate  obj.getClass().getDeclaredFields() + Field.getName() on a 20-field class (a0..a19:
//                    10 long, 5 int, 5 double) 1e6 times (smoke 1e5); getDeclaredFields returns a fresh
//                    copy each call, as the Cajeta side allocates each name; check: fields == 20n and
//                    name bytes == 50n.
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
final class RT0 { int v; }
final class RT1 { int v; }
final class RT2 { int v; }
final class RT3 { int v; }
final class Target20 {
    public long a0;
    public long a1;
    public long a2;
    public long a3;
    public long a4;
    public long a5;
    public long a6;
    public long a7;
    public long a8;
    public long a9;
    public int a10;
    public int a11;
    public int a12;
    public int a13;
    public int a14;
    public double a15;
    public double a16;
    public double a17;
    public double a18;
    public double a19;
}

public final class ReflectBench {
    interface Body { void run() throws Exception; }
    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }
    static int envInt(String k, int d) { try { return Integer.parseInt(env(k, "" + d)); } catch (NumberFormatException e) { return d; } }
    static String clean(String s) { return s.replace(",", ";").replace("\"", ""); }
    static volatile long SINK;
    static long peakRssKb() {
        try { for (String l : Files.readAllLines(Paths.get("/proc/self/status"))) if (l.startsWith("VmHWM:")) return Long.parseLong(l.split("\\s+")[1]); }
        catch (Exception e) { }
        return -1;
    }
    static long[] measure(Body b, int warmup, int trials) throws Exception {
        for (int i = 0; i < warmup; i++) b.run();
        long[] s = new long[trials];
        for (int i = 0; i < trials; i++) { long t0 = System.nanoTime(); b.run(); s[i] = System.nanoTime() - t0; }
        Arrays.sort(s);
        long sum = 0; for (long x : s) sum += x;
        int n = s.length;
        return new long[] { s[0], s[n / 2], sum / n, s[Math.min(n - 1, n * 95 / 100)] };
    }
    static void row(String slug, long input, int w, int t, long[] st, boolean ok, String notes) {
        System.out.println(String.join(",", "1", env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, "reflect", "", "0", "",
            "" + input, "java", clean(env("PROFILE_LANG_VERSION", "java " + System.getProperty("java.version"))),
            "java.lang.reflect", System.getProperty("java.version"), "java default flags (C2 JIT warmed)", "" + w, "" + t,
            "" + st[0], "" + st[1], "" + st[2], "" + st[3], "", "", "" + peakRssKb(), "-1", "-1", "-1", "-1",
            ok ? "ok" : "invalid", ok ? "true" : "false", clean(notes), ""));
    }

    public static void main(String[] a) throws Exception {
        int warmup = envInt("PROFILE_WARMUP", 1), trials = Math.max(1, envInt("PROFILE_TRIALS", 5));
        String sv = env("CAJETA_BENCH_SMOKE", "");
        boolean sm = sv.equals("1") || sv.equals("true") || sv.equals("yes");

        final int n = sm ? 1000000 : 10000000;
        final String[] names = { "RT0", "RT1", "RT2", "RT3" };
        final long[] found = {0};
        long[] st = measure(() -> {
            long f = 0;
            for (int i = 0; i < n; i++) { Class<?> c = Class.forName(names[i & 3]); if (c != null) f++; }
            found[0] = f; SINK += f;
        }, warmup, trials);
        String n1 = "Class.forName(name) (JVM-cached) x" + n + " over 4 names; check: found == " + n;
        if (sm) n1 += "; smoke size 1e6 in place of 1e7";
        row("class-lookup", n, warmup, trials, st, found[0] == n, n1);

        final int m = sm ? 100000 : 1000000;
        final Object obj = new Target20();
        final long[] got = {0, 0};
        long[] sf = measure(() -> {
            long fields = 0, bytes = 0;
            for (int i = 0; i < m; i++) {
                Field[] fs = obj.getClass().getDeclaredFields();
                for (Field f : fs) { fields++; bytes += f.getName().length(); }
            }
            got[0] = fields; got[1] = bytes; SINK += bytes;
        }, warmup, trials);
        String n2 = "getClass().getDeclaredFields() + Field.getName() on a 20-field class x" + m + "; check: fields == " + (20L * m) + " and name bytes == " + (50L * m);
        if (sm) n2 += "; smoke size 1e5 in place of 1e6";
        row("field-enumerate", m, warmup, trials, sf, got[0] == 20L * m && got[1] == 50L * m, n2);
    }
}
