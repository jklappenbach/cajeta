// Java CLBG competitors — the same three Computer Language Benchmarks Game
// classics the Cajeta side runs, at the same sizes, verified against the
// canonical checksums: clbg-mandelbrot (800², 254099), clbg-fannkuch-redux
// (N=10, 38 / 73196), clbg-spectral-norm (N=100, ≈1.274224). Pure scalar JDK
// stdlib (java.lang.Math). One schema-conformant CSV row per benchmark, in
// competitors/columns.txt order — a faithful port of go/clbg/main.go so the
// rows line up in the report's clbg table.
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners; the inner loops OSR-compile well within a trial). alloc_bytes is
// per-thread bytes allocated for one execution, via the HotSpot
// com.sun.management.ThreadMXBean — the Java-native analog of Go's TotalAlloc
// delta / Rust's global allocator / C++ operator new override.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

public class ClbgBench {
    static final int MANDEL_N = 800;
    static final long MANDEL_REF = 254099L;
    static final int FANN_N = 10;
    static final int SPECTRAL_N = 100;
    static final double SPECTRAL_REF = 1.274224;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static int atoiDefault(String s, int d) { try { return Integer.parseInt(s.trim()); } catch (Exception e) { return d; } }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static long allocOf(Runnable r) {
        long id = Thread.currentThread().threadId();
        long b = TMX.getThreadAllocatedBytes(id);
        r.run();
        return TMX.getThreadAllocatedBytes(id) - b;
    }

    static void emit(String runId, String ts, String bench, int input,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,clbg,,%d,,%d,java,%s,stdlib,stdlib,-server,%d,%d,%d,%d,%d,%d,,,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""),
            warmup, trials, mn, med, mean, p95, peakRssKb(), alloc, status, ok);
    }

    static long mandelbrot(int n) {
        long count = 0;
        for (int py = 0; py < n; py++) {
            double ci = 2.0 * (double) py / (double) n - 1.0;
            for (int px = 0; px < n; px++) {
                double cr = 2.0 * (double) px / (double) n - 1.5;
                double zr = 0.0, zi = 0.0;
                boolean member = true;
                for (int it = 0; it < 50; it++) {
                    double nzr = zr * zr - zi * zi + cr;
                    double nzi = 2.0 * zr * zi + ci;
                    zr = nzr; zi = nzi;
                    if (zr * zr + zi * zi > 4.0) { member = false; break; }
                }
                if (member) count++;
            }
        }
        return count;
    }

    // returns {maxFlips, checksum}
    static int[] fannkuch(int n) {
        int[] perm = new int[n];
        int[] perm1 = new int[n];
        int[] count = new int[n];
        for (int i = 0; i < n; i++) perm1[i] = i;
        int maxFlips = 0, checksum = 0, permCount = 0, r = n;
        for (;;) {
            while (r != 1) { count[r - 1] = r; r--; }
            System.arraycopy(perm1, 0, perm, 0, n);
            int flips = 0, k = perm[0];
            while (k != 0) {
                for (int i = 0, j = k; i < j; i++, j--) { int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp; }
                flips++;
                k = perm[0];
            }
            if (flips > maxFlips) maxFlips = flips;
            if (permCount % 2 == 0) checksum += flips; else checksum -= flips;
            permCount++;
            for (;;) {
                if (r == n) return new int[]{maxFlips, checksum};
                int p0 = perm1[0];
                for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
                perm1[r] = p0;
                count[r]--;
                if (count[r] > 0) break;
                r++;
            }
        }
    }

    static double aElem(int i, int j) { return 1.0 / (double) ((i + j) * (i + j + 1) / 2 + i + 1); }

    static void mulAv(int n, double[] v, double[] out) {
        for (int i = 0; i < n; i++) { double s = 0.0; for (int j = 0; j < n; j++) s += aElem(i, j) * v[j]; out[i] = s; }
    }

    static void mulAtv(int n, double[] v, double[] out) {
        for (int i = 0; i < n; i++) { double s = 0.0; for (int j = 0; j < n; j++) s += aElem(j, i) * v[j]; out[i] = s; }
    }

    static double spectralNorm(int n) {
        double[] u = new double[n];
        double[] v = new double[n];
        double[] t = new double[n];
        for (int i = 0; i < n; i++) u[i] = 1.0;
        for (int it = 0; it < 10; it++) {
            mulAv(n, u, t); mulAtv(n, t, v);
            mulAv(n, v, t); mulAtv(n, t, u);
        }
        double vbv = 0.0, vv = 0.0;
        for (int i = 0; i < n; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
        return Math.sqrt(vbv / vv);
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = atoiDefault(env("PROFILE_WARMUP", "3"), 3);
        int trials = atoiDefault(env("PROFILE_TRIALS", "10"), 10);

        // mandelbrot
        {
            boolean ok = true;
            for (int w = 0; w < warmup; w++) mandelbrot(MANDEL_N);
            long[] samples = new long[trials];
            for (int tr = 0; tr < trials; tr++) {
                long t0 = System.nanoTime();
                long r = mandelbrot(MANDEL_N);
                samples[tr] = System.nanoTime() - t0;
                ok = r == MANDEL_REF;
            }
            long alloc = allocOf(() -> mandelbrot(MANDEL_N));
            emit(runId, ts, "clbg-mandelbrot", MANDEL_N * MANDEL_N, warmup, trials, samples, ok, alloc);
        }
        // fannkuch-redux
        {
            boolean ok = true;
            for (int w = 0; w < warmup; w++) fannkuch(FANN_N);
            long[] samples = new long[trials];
            for (int tr = 0; tr < trials; tr++) {
                long t0 = System.nanoTime();
                int[] res = fannkuch(FANN_N);
                samples[tr] = System.nanoTime() - t0;
                ok = res[0] == 38 && res[1] == 73196;
            }
            long alloc = allocOf(() -> fannkuch(FANN_N));
            emit(runId, ts, "clbg-fannkuch-redux", FANN_N, warmup, trials, samples, ok, alloc);
        }
        // spectral-norm
        {
            boolean ok = true;
            for (int w = 0; w < warmup; w++) spectralNorm(SPECTRAL_N);
            long[] samples = new long[trials];
            for (int tr = 0; tr < trials; tr++) {
                long t0 = System.nanoTime();
                double r = spectralNorm(SPECTRAL_N);
                samples[tr] = System.nanoTime() - t0;
                ok = Math.abs(r - SPECTRAL_REF) < 1e-3;
            }
            long alloc = allocOf(() -> spectralNorm(SPECTRAL_N));
            emit(runId, ts, "clbg-spectral-norm", SPECTRAL_N, warmup, trials, samples, ok, alloc);
        }
    }
}
