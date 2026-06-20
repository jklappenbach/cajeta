// Java math competitors — the same three scalar f64 kernels the Cajeta side runs:
// saxpy (z = 2.5*x + y over 1M), dot-product (sum x*y over 1M), matmul (200×200,
// A identity · B). Same init ⇒ exact reference checksums (148250000.0 /
// 148499899 / 1980000). Throughput in GFLOP/s. Plain double[] scalar loops only —
// the HotSpot JIT auto-vectorizes them (the fair scalar baseline; no
// jdk.incubator.vector). numpy (BLAS) is the Python competitor.
//
// One schema-conformant CSV row per benchmark (competitors/columns.txt order),
// mirroring competitors/go/math/main.go column-for-column (language→java,
// flags→-server, alloc_bytes from the HotSpot com.sun.management.ThreadMXBean —
// the Java-native analog of Go's TotalAlloc delta). A volatile sink defeats
// dead-code elimination the way Go's checksum reuse does.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

public class MathBench {
    static final int N = 1000000;
    static final int ND = 200;
    static final double SAXPY_REF = 148250000.0;
    static final double DOT_REF = 148499899.0;
    static final double MATMUL_REF = 1980000.0;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();
    static volatile double SINK; // black hole — keeps the JIT from eliding the work

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static boolean approx(double a, double b) { return Math.abs(a - b) <= 1e-6 * Math.max(Math.abs(b), 1.0); }

    // per-thread bytes allocated for one execution — Go's TotalAlloc delta analog.
    static long allocOf(Runnable r) {
        long id = Thread.currentThread().threadId();
        long b = TMX.getThreadAllocatedBytes(id);
        r.run();
        return TMX.getThreadAllocatedBytes(id) - b;
    }

    static void emit(String runId, String ts, String bench, int input, double flops,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double gflops = med > 0 ? flops / (double) med : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,%s,math,,%d,,%d,java,%s,scalar,stdlib,-server,%d,%d,%d,%d,%d,%d,%.3f,GFLOP/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""),
            warmup, trials, mn, med, mean, p95, gflops, peakRssKb(), alloc, status, ok);
    }

    public static void main(String[] args) {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        final double[] x = new double[N];
        final double[] ys = new double[N];
        final double[] yd = new double[N];
        for (int i = 0; i < N; i++) { x[i] = i % 100; ys[i] = i % 50; yd[i] = i % 7; }

        // saxpy: z = 2.5*x + y
        {
            final double a = 2.5;
            final double[] z = new double[N];
            for (int w = 0; w < warmup; w++) for (int i = 0; i < N; i++) z[i] = a * x[i] + ys[i];
            long[] samples = new long[trials];
            boolean ok = true;
            for (int t = 0; t < trials; t++) {
                long t0 = System.nanoTime();
                double sum = 0.0;
                for (int i = 0; i < N; i++) { z[i] = a * x[i] + ys[i]; sum += z[i]; }
                samples[t] = System.nanoTime() - t0;
                SINK = sum;
                ok = approx(sum, SAXPY_REF);
            }
            long alloc = allocOf(() -> { double s = 0.0; for (int i = 0; i < N; i++) { z[i] = a * x[i] + ys[i]; s += z[i]; } SINK = s; });
            emit(runId, ts, "saxpy", N, 2.0 * N, warmup, trials, samples, ok, alloc);
        }
        // dot-product: sum x*y
        {
            for (int w = 0; w < warmup; w++) { double s = 0.0; for (int i = 0; i < N; i++) s += x[i] * yd[i]; SINK = s; }
            long[] samples = new long[trials];
            boolean ok = true;
            for (int t = 0; t < trials; t++) {
                long t0 = System.nanoTime();
                double acc = 0.0;
                for (int i = 0; i < N; i++) acc += x[i] * yd[i];
                samples[t] = System.nanoTime() - t0;
                SINK = acc;
                ok = approx(acc, DOT_REF);
            }
            long alloc = allocOf(() -> { double s = 0.0; for (int i = 0; i < N; i++) s += x[i] * yd[i]; SINK = s; });
            emit(runId, ts, "dot-product", N, 2.0 * N, warmup, trials, samples, ok, alloc);
        }
        // matmul: C = A·B, A identity, b[idx] = idx%100 (200×200)
        {
            final double[] A = new double[ND * ND];
            final double[] B = new double[ND * ND];
            final double[] C = new double[ND * ND];
            for (int i = 0; i < ND; i++) for (int j = 0; j < ND; j++) {
                int idx = i * ND + j;
                if (i == j) A[idx] = 1.0;
                B[idx] = idx % 100;
            }
            Runnable mm = () -> {
                for (int i = 0; i < ND; i++) for (int j = 0; j < ND; j++) {
                    double acc = 0.0;
                    for (int k = 0; k < ND; k++) acc += A[i * ND + k] * B[k * ND + j];
                    C[i * ND + j] = acc;
                }
            };
            for (int w = 0; w < warmup; w++) mm.run();
            long[] samples = new long[trials];
            boolean ok = true;
            for (int t = 0; t < trials; t++) {
                long t0 = System.nanoTime();
                mm.run();
                samples[t] = System.nanoTime() - t0;
                double sum = 0.0; for (double v : C) sum += v;
                SINK = sum;
                ok = approx(sum, MATMUL_REF);
            }
            long alloc = allocOf(mm);
            emit(runId, ts, "matmul", ND * ND, 2.0 * Math.pow(ND, 3), warmup, trials, samples, ok, alloc);
        }
    }
}
