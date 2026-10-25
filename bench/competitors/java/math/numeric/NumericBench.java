// Java math/ competitors (benchmark-reference unit 11): every math/ workload the
// pages name for Java, with the library the page names, at the sizes and with
// the checks of scratchpad WORKLOADS (the Cajeta side's definitions).
//
//   tensor-*    EJML 0.46.1 FMatrixRMaj / CommonOps_FDRM (float32)
//   linalg-*    EJML 0.46.1 DMatrixRMaj (LU solve, invert, Householder QR, SVD)
//   fft-*       JTransforms 3.1 FloatFFT_1D / FloatFFT_2D (complex64, one thread)
//   random-*    java.util.random L64X128MixRandom (JDK)
//   npy-*       org.jetbrains.bio npy 0.3.5 (in memory, through its byte-buffer seam)
//   poly-eval   plain Java Horner
//   stats-*     Commons Math 3.6.1 moments; histogram as a plain loop
//   ray/rotation/transform  JOML 1.10.8 Vector3f / Quaternionf / Matrix4f
//
// Prints v1 rows (31 columns). Env: PROFILE_RUN_ID, PROFILE_RUN_TS,
// PROFILE_WARMUP, PROFILE_TRIALS, PROFILE_LANG_VERSION, MATH_IDS,
// CAJETA_BENCH_SMOKE. A throw is a `failed` row with the message.
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.function.BooleanSupplier;
import java.util.random.RandomGenerator;
import java.util.random.RandomGeneratorFactory;

import org.apache.commons.math3.stat.descriptive.moment.Kurtosis;
import org.apache.commons.math3.stat.descriptive.moment.Mean;
import org.apache.commons.math3.stat.descriptive.moment.Skewness;
import org.apache.commons.math3.stat.descriptive.moment.Variance;
import org.ejml.data.DMatrixRMaj;
import org.ejml.data.FMatrixRMaj;
import org.ejml.dense.row.CommonOps_DDRM;
import org.ejml.dense.row.CommonOps_FDRM;
import org.ejml.dense.row.factory.DecompositionFactory_DDRM;
import org.ejml.dense.row.factory.LinearSolverFactory_DDRM;
import org.ejml.interfaces.decomposition.QRDecomposition;
import org.ejml.interfaces.decomposition.SingularValueDecomposition_F64;
import org.ejml.interfaces.linsol.LinearSolverDense;
import org.ejml.ops.ConvertMatrixData;
import org.jetbrains.bio.npy.NpyFile;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.jtransforms.fft.FloatFFT_1D;
import org.jtransforms.fft.FloatFFT_2D;

public class NumericBench {

    interface Work { void run() throws Exception; }

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static final boolean SMOKE = "1".equals(System.getenv("CAJETA_BENCH_SMOKE")) || "true".equals(System.getenv("CAJETA_BENCH_SMOKE"));
    static final String RUN_ID = env("PROFILE_RUN_ID", "local");
    static final String TS = env("PROFILE_RUN_TS", "");
    static final int WARMUP = Integer.parseInt(env("PROFILE_WARMUP", SMOKE ? "1" : "3"));
    static final int TRIALS = Math.max(1, Integer.parseInt(env("PROFILE_TRIALS", SMOKE ? "3" : "10")));
    static final String IDS = " " + env("MATH_IDS", "") + " ";
    static final String FLAGS = "-Xmx8g";

    static final String EJML = "0.46.1", JT = "3.1", CM = "3.6.1", JOMLV = "1.10.8", NPYV = "0.3.5";
    static final String JDK = Runtime.version().toString();

    static boolean want(String id) { return IDS.isBlank() || IDS.contains(" " + id + " "); }

    static long peakRssKb() {
        try {
            for (String l : Files.readAllLines(Path.of("/proc/self/status"))) {
                if (l.startsWith("VmHWM:")) return Long.parseLong(l.replaceAll("[^0-9]", ""));
            }
        } catch (Exception e) { }
        return -1;
    }

    static String clean(String s) { return s == null ? "" : s.replace(',', ';').replace('\n', ' ').replace('"', '\''); }

    static void row(String slug, String variant, long input, String lib, String ver, int warm, int trials,
                    long[] samples, String status, String checkOk, String notes) {
        long mn = -1, med = -1, mean = -1, p95 = -1;
        if (samples != null) {
            Arrays.sort(samples);
            int n = samples.length;
            long sum = 0; for (long x : samples) sum += x;
            mn = samples[0]; med = samples[n / 2]; mean = sum / n; p95 = samples[Math.min(n - 1, n * 95 / 100)];
        }
        String nt = notes;
        if (SMOKE) nt = nt.isEmpty() ? "smoke" : nt + "; smoke";
        System.out.printf("1,%s,%s,%s,math,,%d,,%d,java,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,,,%d,-1,-1,-1,-1,%s,%s,%s,%s%n",
            RUN_ID, TS, slug, input, input, clean(env("PROFILE_LANG_VERSION", "")), lib, ver, FLAGS,
            warm, trials, mn, med, mean, p95, peakRssKb(), status, checkOk, clean(nt), variant);
        System.out.flush();
    }

    /** Warmup, timed trials, then the check on the last trial's output. */
    static void measure(String slug, String variant, long input, String lib, String ver, String notes,
                        Work w, BooleanSupplier check) {
        try {
            for (int i = 0; i < WARMUP; i++) w.run();
            long[] s = new long[TRIALS];
            for (int i = 0; i < TRIALS; i++) {
                long t0 = System.nanoTime();
                w.run();
                s[i] = System.nanoTime() - t0;
            }
            boolean ok = check.getAsBoolean();
            row(slug, variant, input, lib, ver, WARMUP, TRIALS, s, ok ? "ok" : "invalid", ok ? "true" : "false", notes);
        } catch (Throwable e) {
            row(slug, variant, input, lib, ver, WARMUP, TRIALS, null, "failed", "", notes + "; threw: " + e);
        }
    }

    // ---- shared data ----
    static final int P = 10007;
    static double u(long i) { return (double) ((i * 7919L) % P) / 10007.0; }
    static double v(long i) { long m = i % P; return (double) ((m * m * 7 + i * 7919L) % P) / 10007.0; }

    static boolean relOk(double got, double want, double tol) {
        return Math.abs(got - want) <= tol * Math.max(Math.abs(want), 1e-300);
    }

    // =========================================================== tensor (EJML float32)
    static FMatrixRMaj tR; // keeps the result alive across the check

    static void tensor() {
        if (want("tensor-elementwise")) {
            int n = SMOKE ? 1_000_000 : 10_000_000;
            FMatrixRMaj a = new FMatrixRMaj(1, n), b = new FMatrixRMaj(1, n), c = new FMatrixRMaj(1, n);
            double ref = 0;
            for (int i = 0; i < n; i++) {
                a.data[i] = (float) u(i); b.data[i] = (float) u(i + 1); c.data[i] = (float) u(i + 2);
                ref += (double) a.data[i] * (double) b.data[i] + (double) c.data[i];
            }
            final double r0 = ref;
            measure("tensor-elementwise", "", n, "ejml", EJML,
                "CommonOps_FDRM.elementMult + add into fresh FMatrixRMaj per trial; check: rel err <= 1e-5" + (SMOKE ? "; smoke size n=1e6 in place of 1e7" : ""),
                () -> { FMatrixRMaj ab = CommonOps_FDRM.elementMult(a, b, (FMatrixRMaj) null); tR = CommonOps_FDRM.add(ab, c, (FMatrixRMaj) null); },
                () -> { double s = 0; for (float x : tR.data) s += x; return relOk(s, r0, 1e-5); });
            tR = null;
        }
        int rows = SMOKE ? 1000 : 10000, cols = 1000;
        if (want("tensor-broadcast") || want("tensor-reduce")) {
            FMatrixRMaj m = new FMatrixRMaj(rows, cols);
            for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) m.data[r * cols + c] = (float) u((long) r * cols + c);
            String sz = SMOKE ? "; smoke size rows=1000 in place of 10000" : "";
            if (want("tensor-broadcast")) {
                float[] row = new float[cols];
                double ref = 0;
                for (int c = 0; c < cols; c++) row[c] = (float) u(c + 5);
                for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) ref += (double) m.data[r * cols + c] + (double) row[c];
                final double r0 = ref;
                measure("tensor-broadcast", "", (long) rows * cols, "ejml", EJML,
                    "EJML has no broadcasting add: fresh FMatrixRMaj filled by a plain row loop over the EJML data arrays; check: rel err <= 1e-5" + sz,
                    () -> {
                        FMatrixRMaj out = new FMatrixRMaj(rows, cols);
                        float[] md = m.data, od = out.data;
                        for (int r = 0; r < rows; r++) {
                            int base = r * cols;
                            for (int c = 0; c < cols; c++) od[base + c] = md[base + c] + row[c];
                        }
                        tR = out;
                    },
                    () -> { double s = 0; for (float x : tR.data) s += x; return relOk(s, r0, 1e-5); });
                tR = null;
            }
            if (want("tensor-reduce")) {
                // float64 reference of the six reductions' checksum
                double[] s0 = new double[cols], mx0 = new double[cols], s1 = new double[rows], mx1 = new double[rows];
                Arrays.fill(mx0, Double.NEGATIVE_INFINITY); Arrays.fill(mx1, Double.NEGATIVE_INFINITY);
                for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
                    double x = m.data[r * cols + c];
                    s0[c] += x; s1[r] += x; mx0[c] = Math.max(mx0[c], x); mx1[r] = Math.max(mx1[r], x);
                }
                double ref = 0;
                for (int c = 0; c < cols; c++) ref += s0[c] + mx0[c] + s0[c] / rows;
                for (int r = 0; r < rows; r++) ref += s1[r] + mx1[r] + s1[r] / cols;
                final double r0 = ref;
                final FMatrixRMaj[] res = new FMatrixRMaj[6];
                measure("tensor-reduce", "", (long) rows * cols, "ejml", EJML,
                    "CommonOps_FDRM sumCols/maxCols (axis 0) and sumRows/maxRows (axis 1); mean = sum / count via CommonOps_FDRM.divide; check: rel err <= 1e-3" + sz,
                    () -> {
                        res[0] = CommonOps_FDRM.sumCols(m, null);
                        res[1] = CommonOps_FDRM.maxCols(m, null);
                        res[2] = CommonOps_FDRM.divide(CommonOps_FDRM.sumCols(m, null), (float) rows, (FMatrixRMaj) null);
                        res[3] = CommonOps_FDRM.sumRows(m, null);
                        res[4] = CommonOps_FDRM.maxRows(m, null);
                        res[5] = CommonOps_FDRM.divide(CommonOps_FDRM.sumRows(m, null), (float) cols, (FMatrixRMaj) null);
                    },
                    () -> { double s = 0; for (FMatrixRMaj x : res) for (float f : x.data) s += f; return relOk(s, r0, 1e-3); });
            }
        }
        if (want("tensor-transpose")) {
            int n = SMOKE ? 1024 : 4096;
            FMatrixRMaj m = new FMatrixRMaj(n, n);
            for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) m.data[i * n + j] = (float) u((long) i * n + j);
            measure("tensor-transpose", "", (long) n * n, "ejml", EJML,
                "CommonOps_FDRM.transpose into a fresh FMatrixRMaj; check: exact equality" + (SMOKE ? "; smoke size N=1024 in place of 4096" : ""),
                () -> { tR = CommonOps_FDRM.transpose(m, null); },
                () -> {
                    float[] t = tR.data, a = m.data;
                    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (t[j * n + i] != a[i * n + j]) return false;
                    return true;
                });
            tR = null;
        }
        if (want("tensor-cast")) {
            int n = SMOKE ? 1_000_000 : 10_000_000;
            FMatrixRMaj x = new FMatrixRMaj(1, n);
            long ref = 0;
            for (int i = 0; i < n; i++) { x.data[i] = (float) (u(i) * 1000 - 500); ref += (long) (int) (double) x.data[i]; }
            final long r0 = ref;
            final int[][] z = new int[1][];
            measure("tensor-cast", "", n, "ejml", EJML,
                "float32->float64 by ConvertMatrixData.convert(FMatrixRMaj; DMatrixRMaj); float64->int32 by a plain loop (EJML has no integer matrix); check: exact int64 checksum" + (SMOKE ? "; smoke size n=1e6 in place of 1e7" : ""),
                () -> {
                    DMatrixRMaj y = new DMatrixRMaj(1, n);
                    ConvertMatrixData.convert(x, y);
                    int[] zz = new int[n];
                    double[] yd = y.data;
                    for (int i = 0; i < n; i++) zz[i] = (int) yd[i];
                    z[0] = zz;
                },
                () -> { long s = 0; for (int q : z[0]) s += q; return s == r0; });
        }
    }

    // =========================================================== linalg (EJML float64)
    static DMatrixRMaj linA(int n) {
        DMatrixRMaj a = new DMatrixRMaj(n, n);
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) a.data[i * n + j] = u((long) i * n + j) - 0.5 + (i == j ? n : 0);
        return a;
    }

    static double maxAbs(DMatrixRMaj m) { double s = 0; for (double x : m.data) s = Math.max(s, Math.abs(x)); return s; }

    static void linalg() {
        String[] variants = {"512", "2048"};
        for (String var : variants) {
            int full = Integer.parseInt(var);
            int nSmall = full == 512 ? 128 : 256;
            int nSvd = full == 512 ? 64 : 128;
            if (want("linalg-solve")) {
                int n = SMOKE ? nSmall : full;
                DMatrixRMaj a = linA(n);
                DMatrixRMaj b = new DMatrixRMaj(n, 1);
                for (int i = 0; i < n; i++) b.data[i] = u(i + 3);
                DMatrixRMaj x = new DMatrixRMaj(n, 1);
                LinearSolverDense<DMatrixRMaj> sol = LinearSolverFactory_DDRM.lu(n);
                measure("linalg-solve", var, n, "ejml", EJML,
                    "LinearSolverFactory_DDRM.lu setA+solve (A copied per trial since the LU overwrites it; copy is O(n^2) inside the timed region); check: ||Ax-b||inf/(||A||inf||x||inf+||b||inf) <= 1e-12" + (SMOKE ? "; smoke size n=" + n + " in place of " + full : ""),
                    () -> {
                        DMatrixRMaj ac = sol.modifiesA() ? a.copy() : a;
                        if (!sol.setA(ac)) throw new IllegalStateException("singular");
                        sol.solve(b, x);
                    },
                    () -> {
                        DMatrixRMaj r = new DMatrixRMaj(n, 1);
                        CommonOps_DDRM.mult(a, x, r);
                        double rn = 0, an = 0, xn = 0, bn = 0;
                        for (int i = 0; i < n; i++) {
                            rn = Math.max(rn, Math.abs(r.data[i] - b.data[i]));
                            double rs = 0; for (int j = 0; j < n; j++) rs += Math.abs(a.data[i * n + j]);
                            an = Math.max(an, rs); xn = Math.max(xn, Math.abs(x.data[i])); bn = Math.max(bn, Math.abs(b.data[i]));
                        }
                        return rn / (an * xn + bn) <= 1e-12;
                    });
            }
            if (want("linalg-inverse")) {
                int n = SMOKE ? nSmall : full;
                DMatrixRMaj a = linA(n);
                DMatrixRMaj ai = new DMatrixRMaj(n, n);
                measure("linalg-inverse", var, n, "ejml", EJML,
                    "CommonOps_DDRM.invert(A; Ai); check: max|A*Ai-I| <= 1e-10" + (SMOKE ? "; smoke size n=" + n + " in place of " + full : ""),
                    () -> { if (!CommonOps_DDRM.invert(a, ai)) throw new IllegalStateException("singular"); },
                    () -> {
                        DMatrixRMaj p = new DMatrixRMaj(n, n);
                        CommonOps_DDRM.mult(a, ai, p);
                        for (int i = 0; i < n; i++) p.data[i * n + i] -= 1.0;
                        return maxAbs(p) <= 1e-10;
                    });
            }
            if (want("linalg-qr")) {
                int n = SMOKE ? nSmall : full;
                DMatrixRMaj a = linA(n);
                QRDecomposition<DMatrixRMaj> qr = DecompositionFactory_DDRM.qr(n, n);
                DMatrixRMaj[] qrm = new DMatrixRMaj[2];
                measure("linalg-qr", var, n, "ejml", EJML,
                    "DecompositionFactory_DDRM.qr (Householder) decompose + getQ/getR materialized (A copied per trial if the decomposition overwrites it); check: max|QR-A|/max|A| <= 1e-12" + (SMOKE ? "; smoke size n=" + n + " in place of " + full : ""),
                    () -> {
                        DMatrixRMaj ac = qr.inputModified() ? a.copy() : a;
                        if (!qr.decompose(ac)) throw new IllegalStateException("qr failed");
                        qrm[0] = qr.getQ(null, false);
                        qrm[1] = qr.getR(null, false);
                    },
                    () -> {
                        DMatrixRMaj p = new DMatrixRMaj(n, n);
                        CommonOps_DDRM.mult(qrm[0], qrm[1], p);
                        CommonOps_DDRM.subtractEquals(p, a);
                        return maxAbs(p) / maxAbs(a) <= 1e-12;
                    });
            }
            if (want("linalg-svd")) {
                int n = SMOKE ? nSvd : full;
                DMatrixRMaj a = linA(n);
                SingularValueDecomposition_F64<DMatrixRMaj> svd = DecompositionFactory_DDRM.svd(n, n, true, true, false);
                DMatrixRMaj[] usv = new DMatrixRMaj[3];
                measure("linalg-svd", var, n, "ejml", EJML,
                    "DecompositionFactory_DDRM.svd full (compact=false) with U and Vt materialized (A copied per trial if overwritten); check: max|U*diag(S)*Vt-A|/max|A| <= 1e-10" + (SMOKE ? "; smoke size n=" + n + " in place of " + full : ""),
                    () -> {
                        DMatrixRMaj ac = svd.inputModified() ? a.copy() : a;
                        if (!svd.decompose(ac)) throw new IllegalStateException("svd failed");
                        usv[0] = svd.getU(null, false);
                        usv[1] = svd.getW(null);
                        usv[2] = svd.getV(null, true);
                    },
                    () -> {
                        DMatrixRMaj t = new DMatrixRMaj(n, n), p = new DMatrixRMaj(n, n);
                        CommonOps_DDRM.mult(usv[0], usv[1], t);
                        CommonOps_DDRM.mult(t, usv[2], p);
                        CommonOps_DDRM.subtractEquals(p, a);
                        return maxAbs(p) / maxAbs(a) <= 1e-10;
                    });
            }
        }
    }

    // =========================================================== fft (JTransforms float32)
    static float[] signal(int n) {
        float[] x = new float[2 * n];
        for (int k = 0; k < n; k++) { x[2 * k] = (float) (u(2L * k) - 0.5); x[2 * k + 1] = (float) (u(2L * k + 1) - 0.5); }
        return x;
    }

    static boolean dftOk() {
        int n = 64;
        float[] x = signal(n);
        float[] y = x.clone();
        new FloatFFT_1D(n).complexForward(y);
        double err = 0;
        for (int k = 0; k < n; k++) {
            double re = 0, im = 0;
            for (int t = 0; t < n; t++) {
                double ang = -2 * Math.PI * ((long) k * t % n) / n;
                re += x[2 * t] * Math.cos(ang) - x[2 * t + 1] * Math.sin(ang);
                im += x[2 * t] * Math.sin(ang) + x[2 * t + 1] * Math.cos(ang);
            }
            err = Math.max(err, Math.max(Math.abs(re - y[2 * k]), Math.abs(im - y[2 * k + 1])));
        }
        return err <= 1e-4;
    }

    static double maxDiff(float[] a, float[] b) { double e = 0; for (int i = 0; i < a.length; i++) e = Math.max(e, Math.abs(a[i] - b[i])); return e; }

    static void fft() {
        pl.edu.icm.jlargearrays.ConcurrencyUtils.setNumberOfThreads(1);
        String common = "JTransforms complex64 in place on an interleaved float[] (copy of the input inside the timed region); plan built in setup; one thread (ConcurrencyUtils.setNumberOfThreads(1))";
        if (want("fft-1d")) {
            String[][] vs = {{"2^16", "65536", "65536"}, {"2^20", "1048576", "262144"}};
            for (String[] vv : vs) {
                int n = Integer.parseInt(SMOKE ? vv[2] : vv[1]);
                float[] x = signal(n);
                float[] y = new float[2 * n];
                FloatFFT_1D plan = new FloatFFT_1D(n);
                measure("fft-1d", vv[0], n, "jtransforms", JT,
                    common + "; check: direct DFT at N=64 max abs err <= 1e-4 and round trip at full N max abs err <= 1e-4" + (SMOKE && !vv[1].equals(vv[2]) ? "; smoke size N=2^18 in place of 2^20" : ""),
                    () -> { System.arraycopy(x, 0, y, 0, 2 * n); plan.complexForward(y); },
                    () -> {
                        float[] z = y.clone();
                        plan.complexInverse(z, true);
                        return dftOk() && maxDiff(z, x) <= 1e-4;
                    });
            }
        }
        if (want("fft-2d")) {
            int n = SMOKE ? 512 : 2048;
            float[] x = signal(n * n);
            float[] y = new float[2 * n * n];
            FloatFFT_2D plan = new FloatFFT_2D(n, n);
            measure("fft-2d", "", (long) n * n, "jtransforms", JT,
                common + "; FloatFFT_2D; check: round trip max abs err <= 1e-4" + (SMOKE ? "; smoke size 512x512 in place of 2048x2048" : ""),
                () -> { System.arraycopy(x, 0, y, 0, y.length); plan.complexForward(y); },
                () -> { float[] z = y.clone(); plan.complexInverse(z, true); return maxDiff(z, x) <= 1e-4; });
        }
    }

    // =========================================================== random (JDK)
    static void random() {
        int n = SMOKE ? 10_000_000 : 100_000_000;
        String sz = SMOKE ? "; smoke size n=1e7 in place of 1e8" : "";
        double[] buf = new double[n];
        if (want("random-uniform")) {
            RandomGenerator g = RandomGeneratorFactory.of("L64X128MixRandom").create(42L);
            measure("random-uniform", "", n, "jdk-random", JDK,
                "RandomGeneratorFactory L64X128MixRandom seed 42 nextDouble into a preallocated double[]; check: |mean-0.5| <= 1e-3 and |var-1/12| <= 1e-3" + sz,
                () -> { for (int i = 0; i < n; i++) buf[i] = g.nextDouble(); },
                () -> { double m = new Mean().evaluate(buf), va = new Variance(false).evaluate(buf); return Math.abs(m - 0.5) <= 1e-3 && Math.abs(va - 1.0 / 12) <= 1e-3; });
        }
        if (want("random-normal")) {
            RandomGenerator g = RandomGeneratorFactory.of("L64X128MixRandom").create(42L);
            measure("random-normal", "", n, "jdk-random", JDK,
                "RandomGeneratorFactory L64X128MixRandom seed 42 nextGaussian into a preallocated double[]; check: |mean| <= 1e-3 and |var-1| <= 2e-3" + sz,
                () -> { for (int i = 0; i < n; i++) buf[i] = g.nextGaussian(); },
                () -> { double m = new Mean().evaluate(buf), va = new Variance(false).evaluate(buf); return Math.abs(m) <= 1e-3 && Math.abs(va - 1.0) <= 2e-3; });
        }
    }

    // =========================================================== npy (org.jetbrains.bio npy)
    static final int NPY_N = 1 << 23;

    static byte[] referenceImage(int[] t) {
        String dict = "{'descr': '<i4', 'fortran_order': False, 'shape': (" + NPY_N + ",), }";
        StringBuilder h = new StringBuilder(dict);
        while (10 + h.length() + 1 < 128) h.append(' ');
        h.append('\n');
        byte[] hb = h.toString().getBytes(StandardCharsets.US_ASCII);
        ByteBuffer bb = ByteBuffer.allocate(10 + hb.length + 4 * t.length).order(ByteOrder.LITTLE_ENDIAN);
        bb.put((byte) 0x93).put("NUMPY".getBytes(StandardCharsets.US_ASCII)).put((byte) 1).put((byte) 0);
        bb.putShort((short) hb.length).put(hb);
        bb.asIntBuffer().put(t);
        return bb.array();
    }

    /** Payload byte-equal to the NumPy image; header a valid v1.0 '<i4' C-order (NPY_N,) header. */
    static boolean npySaveOk(byte[] got, byte[] numpyImg) {
        if (got.length < 10 || (got[0] & 0xff) != 0x93 || got[6] != 1 || got[7] != 0) return false;
        int hl = (got[8] & 0xff) | ((got[9] & 0xff) << 8);
        int dataOff = 10 + hl;
        if (dataOff % 16 != 0 || got.length != dataOff + 4 * NPY_N) return false;
        String h = new String(got, 10, hl, StandardCharsets.US_ASCII).replace(" ", "");
        if (!h.contains("'descr':'<i4'") || !h.contains("'fortran_order':False") || !h.contains("'shape':(" + NPY_N + ",)") || !h.endsWith("\n")) return false;
        return Arrays.equals(got, dataOff, got.length, numpyImg, 128, numpyImg.length);
    }

    static void npy() {
        int[] t = new int[NPY_N];
        t[0] = 7; t[NPY_N - 1] = 8247;
        byte[] img = referenceImage(t);
        String seam = "in memory through the library's byte-buffer seam NpyFile.read$npy / allocate$npy (Kotlin-internal; the public read/write take a Path)";
        if (want("npy-load-i32")) {
            int[][] out = new int[1][];
            measure("npy-load-i32", "", img.length, "npy", NPYV,
                "decode " + seam + "; check: len and sum and first/last",
                () -> {
                    kotlin.sequences.Sequence<ByteBuffer> seq = kotlin.sequences.SequencesKt.sequenceOf(ByteBuffer.wrap(img));
                    out[0] = NpyFile.INSTANCE.read$npy(seq).asIntArray();
                },
                () -> {
                    int[] a = out[0];
                    long s = 0; for (int q : a) s += q;
                    return a.length == NPY_N && s == 8254 && a[0] == 7 && a[NPY_N - 1] == 8247;
                });
        }
        if (want("npy-save-i32")) {
            byte[][] out = new byte[1][];
            measure("npy-save-i32", "", img.length, "npy", NPYV,
                "encode " + seam + " and gather the chunks into one byte[]; check: the payload is byte-equal to the numpy image and the header is a valid v1.0 header for '<i4' C-order shape (8388608;) -- not whole-image byte equality: this library pads its header to 16 bytes (80-byte header; dict written as (8388608; )}) where NumPy pads to 64 (128 bytes) so its image can never be byte-equal to NumPy's",
                () -> {
                    kotlin.sequences.Sequence<ByteBuffer> seq = NpyFile.INSTANCE.allocate$npy(t, new int[]{NPY_N}, ByteOrder.LITTLE_ENDIAN);
                    java.util.Iterator<ByteBuffer> it = seq.iterator();
                    java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(img.length);
                    while (it.hasNext()) {
                        ByteBuffer b = it.next();
                        if (b.hasArray()) bo.write(b.array(), b.arrayOffset() + b.position(), b.remaining());
                        else { byte[] tmp = new byte[b.remaining()]; b.duplicate().get(tmp); bo.write(tmp, 0, tmp.length); }
                    }
                    out[0] = bo.toByteArray();
                },
                () -> npySaveOk(out[0], img));
        }
    }

    // =========================================================== poly (plain Horner)
    static final double[] C = {0.5, -1.25, 2.0, -0.75, 1.5, -2.25, 0.25, 1.75, -1.0};

    static double horner(double x) { double y = C[0]; for (int k = 1; k < 9; k++) y = y * x + C[k]; return y; }

    static void poly() {
        if (!want("poly-eval")) return;
        int n = SMOKE ? 10_000_000 : 100_000_000;
        double[] x = new double[n], y = new double[n];
        for (int i = 0; i < n; i++) x[i] = (double) i / n;
        double integ = 0; for (int k = 0; k < 9; k++) integ += C[k] / (9 - k);
        double ref = n * integ - (horner(1.0) - horner(0.0)) / 2;
        measure("poly-eval", "", n, "plain", JDK,
            "plain Java Horner (no Java polynomial library named); check: rel err of sum <= 1e-7 vs Euler-Maclaurin" + (SMOKE ? "; smoke size n=1e7 in place of 1e8" : ""),
            () -> {
                double c0 = C[0], c1 = C[1], c2 = C[2], c3 = C[3], c4 = C[4], c5 = C[5], c6 = C[6], c7 = C[7], c8 = C[8];
                for (int i = 0; i < n; i++) {
                    double t = x[i];
                    y[i] = ((((((((c0 * t + c1) * t + c2) * t + c3) * t + c4) * t + c5) * t + c6) * t + c7) * t + c8);
                }
            },
            () -> { double s = 0; for (double q : y) s += q; return relOk(s, ref, 1e-7); });
    }

    // =========================================================== stats (Commons Math)
    static void stats() {
        if (!want("stats-moments") && !want("stats-histogram")) return;
        int n = SMOKE ? 10_000_000 : 100_000_000;
        String sz = SMOKE ? "; smoke size n=1e7 in place of 1e8" : "";
        double[] x = new double[n];
        for (int i = 0; i < n; i++) x[i] = u(i);
        if (want("stats-moments")) {
            double[] r = new double[4];
            measure("stats-moments", "", n, "commons-math3", CM,
                "Commons Math Mean + Variance(false) (population) + Skewness + Kurtosis (bias-corrected sample estimators; differ from the biased ones by O(1/n)); four passes; check: |mean-0.49995| |var-1/12| |skew| |kurt+1.2| each <= 1e-3" + sz,
                () -> {
                    r[0] = new Mean().evaluate(x);
                    r[1] = new Variance(false).evaluate(x);
                    r[2] = new Skewness().evaluate(x);
                    r[3] = new Kurtosis().evaluate(x);
                },
                () -> Math.abs(r[0] - 0.49995) <= 1e-3 && Math.abs(r[1] - 1.0 / 12) <= 1e-3 && Math.abs(r[2]) <= 1e-3 && Math.abs(r[3] + 1.2) <= 1e-3);
        }
        if (want("stats-histogram")) {
            long[] ref = new long[1000];
            for (long i = 0; i < n; i++) ref[(int) (((i * 7919L) % P) * 1000 / 10007)]++;
            long[] cnt = new long[1000];
            measure("stats-histogram", "", n, "plain", JDK,
                "Commons Math has no fixed-range histogram (EmpiricalDistribution bins over the data min/max): plain loop of 1000 equal bins over [0;1]; check: every count exact" + sz,
                () -> {
                    Arrays.fill(cnt, 0);
                    for (int i = 0; i < n; i++) {
                        int b = (int) (x[i] * 1000.0);
                        if (b > 999) b = 999;
                        if (b >= 0) cnt[b]++;
                    }
                },
                () -> Arrays.equals(cnt, ref));
        }
    }

    // =========================================================== geometry (JOML)
    static final int S = 10000;

    static void geometry() {
        if (want("ray-sphere") || want("ray-aabb")) {
            int R = SMOKE ? 1000 : 10000;
            long tests = (long) R * S;
            Vector3f[] o = new Vector3f[R], d = new Vector3f[R];
            for (int i = 0; i < R; i++) {
                o[i] = new Vector3f((float) (v(3L * i) * 2 - 1), (float) (v(3L * i + 1) * 2 - 1), -5f);
                double dx = v(3L * i + 2) * 0.2 - 0.1, dy = v(3L * i + 7) * 0.2 - 0.1, dz = 1.0;
                double len = Math.sqrt(dx * dx + dy * dy + dz * dz);
                d[i] = new Vector3f((float) (dx / len), (float) (dy / len), (float) (dz / len));
            }
            Vector3f[] c = new Vector3f[S], mx = new Vector3f[S];
            float[] rad = new float[S];
            for (int j = 0; j < S; j++) {
                double x = v(5L * j) * 4 - 2, y = v(5L * j + 1) * 4 - 2, z = v(5L * j + 2) * 4;
                double s = 0.1 + v(5L * j + 3) * 0.3;
                c[j] = new Vector3f((float) x, (float) y, (float) z);
                rad[j] = (float) (0.05 + v(5L * j + 3) * 0.2);
                mx[j] = new Vector3f((float) (x + s), (float) (y + s), (float) (z + s));
            }
            String sz = SMOKE ? "; smoke size R=1000 (1e7 tests) in place of 10000 (1e8)" : "";
            long[] hits = new long[1];
            if (want("ray-sphere")) {
                long ref = SMOKE ? 51613 : 502607;
                measure("ray-sphere", "", tests, "joml", JOMLV,
                    "JOML Vector3f sub/dot with the shared predicate (not Intersectionf.testRaySphere; whose line-vs-ray semantics differ); check: |hits-ref| <= 1e-5*tests" + sz,
                    () -> {
                        long h = 0;
                        Vector3f oc = new Vector3f();
                        for (int i = 0; i < R; i++) {
                            Vector3f oi = o[i], di = d[i];
                            for (int j = 0; j < S; j++) {
                                oi.sub(c[j], oc);
                                float b = oc.dot(di);
                                float cc = oc.dot(oc) - rad[j] * rad[j];
                                float disc = b * b - cc;
                                if (disc >= 0f && (b <= 0f || cc <= 0f)) h++;
                            }
                        }
                        hits[0] = h;
                    },
                    () -> Math.abs(hits[0] - ref) <= 1e-5 * tests);
            }
            if (want("ray-aabb")) {
                long ref = SMOKE ? 49068 : 476570;
                measure("ray-aabb", "", tests, "joml", JOMLV,
                    "JOML Vector3f components with the shared slab test (not Intersectionf.testRayAab); check: |hits-ref| <= 1e-5*tests" + sz,
                    () -> {
                        long h = 0;
                        for (int i = 0; i < R; i++) {
                            Vector3f oi = o[i], di = d[i];
                            float ix = 1f / di.x, iy = 1f / di.y, iz = 1f / di.z;
                            for (int j = 0; j < S; j++) {
                                Vector3f mn = c[j], mxj = mx[j];
                                float t0 = 0f, t1 = 1e8f;
                                float a = (mn.x - oi.x) * ix, bb = (mxj.x - oi.x) * ix;
                                if (a > bb) { float t = a; a = bb; bb = t; }
                                if (a > t0) t0 = a; if (bb < t1) t1 = bb;
                                if (t0 > t1) continue;
                                a = (mn.y - oi.y) * iy; bb = (mxj.y - oi.y) * iy;
                                if (a > bb) { float t = a; a = bb; bb = t; }
                                if (a > t0) t0 = a; if (bb < t1) t1 = bb;
                                if (t0 > t1) continue;
                                a = (mn.z - oi.z) * iz; bb = (mxj.z - oi.z) * iz;
                                if (a > bb) { float t = a; a = bb; bb = t; }
                                if (a > t0) t0 = a; if (bb < t1) t1 = bb;
                                if (t0 > t1) continue;
                                h++;
                            }
                        }
                        hits[0] = h;
                    },
                    () -> Math.abs(hits[0] - ref) <= 1e-5 * tests);
            }
        }
        if (want("rotation-compose")) {
            long n = SMOKE ? 10_000_000L : 100_000_000L;
            Quaternionf[] Q = new Quaternionf[1024];
            Vector3f[] V = new Vector3f[1024];
            for (int j = 0; j < 1024; j++) {
                double ax = v(4L * j) - 0.5, ay = v(4L * j + 1) - 0.5, az = v(4L * j + 2) - 0.5;
                double l = Math.sqrt(ax * ax + ay * ay + az * az);
                double a = v(4L * j + 3) * 2 * Math.PI, s = Math.sin(a / 2);
                Q[j] = new Quaternionf((float) (ax / l * s), (float) (ay / l * s), (float) (az / l * s), (float) Math.cos(a / 2));
                V[j] = new Vector3f((float) (v(3L * j + 11) - 0.5), (float) (v(3L * j + 12) - 0.5), (float) (v(3L * j + 13) - 0.5));
            }
            double ref = SMOKE ? 38600.96393095818 : 386090.57108821085;
            double[] sum = new double[1];
            measure("rotation-compose", "", n, "joml", JOMLV,
                "JOML Quaternionf.mul (Hamilton product) then Quaternionf.transform(Vector3f); float64 accumulator; check: |sum-ref| <= 1e-6*n" + (SMOKE ? "; smoke size n=1e7 in place of 1e8" : ""),
                () -> {
                    Quaternionf q = new Quaternionf();
                    Vector3f w = new Vector3f();
                    double s = 0;
                    for (long i = 0; i < n; i++) {
                        Q[(int) (i & 1023)].mul(Q[(int) ((i >> 10) & 1023)], q);
                        q.transform(V[(int) (i & 1023)], w);
                        s += (double) w.x + (double) w.y + (double) w.z;
                    }
                    sum[0] = s;
                },
                () -> Math.abs(sum[0] - ref) <= 1e-6 * n);
        }
        if (want("transform-apply")) {
            int m = 1_000_000;
            int passes = SMOKE ? 10 : 100;
            float[] p = new float[3 * m], out = new float[3 * m];
            for (int k = 0; k < m; k++) {
                p[3 * k] = (float) (v(3L * k) * 10 - 5); p[3 * k + 1] = (float) (v(3L * k + 1) * 10 - 5); p[3 * k + 2] = (float) (v(3L * k + 2) * 10 - 5);
            }
            double l = Math.sqrt(14.0), s = Math.sin(0.35);
            Quaternionf q = new Quaternionf((float) (1 / l * s), (float) (2 / l * s), (float) (3 / l * s), (float) Math.cos(0.35));
            Matrix4f M = new Matrix4f().translationRotateScale(new Vector3f(1f, -2f, 3f), q, 2f);
            measure("transform-apply", "", (long) m * passes, "joml", JOMLV,
                "JOML Matrix4f.translationRotateScale then transformPosition per point; 1e8 point applications as 100 passes over 1e6 points; check: |cs-1910391.7256280927| <= 2e-5*m" + (SMOKE ? "; smoke 10 passes (1e7) in place of 100" : ""),
                () -> {
                    Vector3f t = new Vector3f();
                    for (int ps = 0; ps < passes; ps++) {
                        for (int k = 0; k < m; k++) {
                            M.transformPosition(p[3 * k], p[3 * k + 1], p[3 * k + 2], t);
                            out[3 * k] = t.x; out[3 * k + 1] = t.y; out[3 * k + 2] = t.z;
                        }
                    }
                },
                () -> {
                    double cs = 0;
                    for (int k = 0; k < m; k++) cs += (double) out[3 * k] + (double) out[3 * k + 1] + (double) out[3 * k + 2];
                    return Math.abs(cs - 1910391.7256280927) <= 2e-5 * m;
                });
        }
    }

    public static void main(String[] args) {
        tensor();
        linalg();
        fft();
        random();
        npy();
        poly();
        stats();
        geometry();
    }
}
