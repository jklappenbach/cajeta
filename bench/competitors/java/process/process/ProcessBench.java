import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

// process/ competitor in Java (bench/process/Command.md, Process.md): java.lang.ProcessBuilder.
//   command-spawn-wait   new ProcessBuilder("/bin/true").start().waitFor() R = 1e3 times (smoke 1e2); check: every exit 0
//   process-pipe-100mb   100 MiB (smoke 10 MiB) of data[i] = (i*31+7) & 0xFF through `cat`: a writer thread
//                        writes the child's stdin, the main thread readAllBytes() its stdout; check: length and byte sum.
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
public final class ProcessBench {
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
    static void row(String slug, long input, int w, int t, long[] st, boolean ok, String notes, boolean mbps) {
        String thr = "", unit = "";
        if (mbps && st[1] > 0) { thr = String.format("%.1f", input / (double) st[1] * 1e9 / 1048576.0); unit = "MB/s"; }
        System.out.println(String.join(",", "1", env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, "process", "", "0", "",
            "" + input, "java", clean(env("PROFILE_LANG_VERSION", "java " + System.getProperty("java.version"))),
            "java.lang.ProcessBuilder", System.getProperty("java.version"), "java default flags (C2 JIT warmed)", "" + w, "" + t,
            "" + st[0], "" + st[1], "" + st[2], "" + st[3], thr, unit, "" + peakRssKb(), "-1", "-1", "-1", "-1",
            ok ? "ok" : "invalid", ok ? "true" : "false", clean(notes), ""));
    }

    public static void main(String[] a) throws Exception {
        int warmup = envInt("PROFILE_WARMUP", 1), trials = Math.max(1, envInt("PROFILE_TRIALS", 5));
        String sv = env("CAJETA_BENCH_SMOKE", "");
        boolean sm = sv.equals("1") || sv.equals("true") || sv.equals("yes");

        final int r = sm ? 100 : 1000;
        final int[] zero = {0};
        long[] st = measure(() -> {
            int z = 0;
            for (int i = 0; i < r; i++) if (new ProcessBuilder("/bin/true").start().waitFor() == 0) z++;
            zero[0] = z; SINK += z;
        }, warmup, trials);
        String n1 = "new ProcessBuilder(/bin/true).start().waitFor() x" + r + " per trial; check: zero exits == " + r;
        if (sm) n1 += "; smoke size 1e2 spawns in place of 1e3";
        row("command-spawn-wait", r, warmup, trials, st, zero[0] == r, n1, false);

        final int n = sm ? (10 << 20) : (100 << 20);
        final byte[] data = new byte[n];
        long want = 0;
        for (int i = 0; i < n; i++) { data[i] = (byte) ((i * 31 + 7) & 0xFF); want += data[i] & 0xFF; }
        final long[] got = {-1, -1};
        long[] sp = measure(() -> {
            Process p = new ProcessBuilder("cat").start();
            final Exception[] werr = {null};
            Thread wr = new Thread(() -> {
                try (OutputStream o = p.getOutputStream()) { o.write(data); } catch (Exception e) { werr[0] = e; }
            });
            wr.start();
            byte[] out;
            try (InputStream in = p.getInputStream()) { out = in.readAllBytes(); }
            wr.join();
            if (werr[0] != null) throw werr[0];
            p.waitFor();
            long s = 0; for (byte b : out) s += b & 0xFF;
            got[0] = out.length; got[1] = s; SINK += s;
        }, warmup, trials);
        String n2 = "ProcessBuilder(cat).start(); writer thread on getOutputStream + getInputStream().readAllBytes(); check: length " + n + " and byte sum " + want;
        if (sm) n2 += "; smoke size 10 MiB in place of 100 MiB";
        row("process-pipe-100mb", n, warmup, trials, sp, got[0] == n && got[1] == want, n2, true);
    }
}
