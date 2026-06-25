// Java hash competitors — bulk throughput over a deterministic 1 MiB buffer
// (buf[i] = i & 0xFF), the SMHasher bulk metric — the same xxhash3/sha256/md5
// benchmark names the Cajeta side emits. One schema-conformant CSV row per
// algorithm (competitors/columns.txt order). Cross-check: each digest matches
// the cross-language reference (the identical Go/Rust/C++ runners check).
//
// Libraries: sha256 + md5 via the JDK stdlib java.security.MessageDigest. There
// is NO xxHash in the JDK and no jar is provisioned, so xxhash3 is emitted as a
// `skipped` row with a reason (mirroring how hash.sh skips siphash for all
// langs) — this one runner owns all three rows.
//
// timings honor PROFILE_WARMUP/TRIALS (the inner MessageDigest loops JIT well
// within a trial); alloc_bytes is per-thread bytes allocated for one execution
// via the HotSpot com.sun.management.ThreadMXBean — the Java-native analog of
// Go's TotalAlloc delta / Rust's global allocator / C++ operator new override.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Arrays;

public class HashBench {
    static final int N = 1048576;
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    // Cross-language reference digests over buf[i] = i & 0xFF, N = 1 MiB.
    static final String SHA256_REF = "fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83";
    static final String MD5_REF    = "c35cc7d8d91728a0cb052831bc4ef372";

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

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

    static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder(d.length * 2);
        for (byte x : d) sb.append(Character.forDigit((x >> 4) & 0xF, 16)).append(Character.forDigit(x & 0xF, 16));
        return sb.toString();
    }

    static void emit(String runId, String ts, String bench, String lib, String ver,
                     int warmup, int trials, long[] samples, boolean checkOk) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mbps = med > 0 ? (double) N / (double) med * 1e9 / 1048576.0 : 0.0;
        String status = checkOk ? "ok" : "invalid";
        long alloc = lastAlloc;
        System.out.printf(
            "1,%s,%s,%s,hash,,%d,,%d,java,%s,%s,%s,-server,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, bench, N, N, env("PROFILE_LANG_VERSION", ""), lib, ver,
            warmup, trials, mn, med, mean, p95, mbps, peakRssKb(), alloc, status, checkOk);
    }

    // xxhash3 skip row — same shape as hash.sh's skip rows (status=skipped,
    // metric columns -1, reason in the notes column).
    static void emitSkip(String runId, String ts, String bench, String reason) {
        System.out.printf(
            "1,%s,%s,%s,hash,,%d,,%d,java,,,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,%n",
            runId, ts, bench, N, N, reason);
    }

    static long lastAlloc = -1;

    interface Digester { byte[] digest() throws Exception; }

    static long[] bench(int warmup, int trials, Digester d) throws Exception {
        for (int i = 0; i < warmup; i++) d.digest();
        long[] s = new long[trials];
        for (int i = 0; i < trials; i++) {
            long t0 = System.nanoTime();
            byte[] sink = d.digest();
            s[i] = System.nanoTime() - t0;
            if (sink == null) throw new IllegalStateException();
        }
        lastAlloc = allocOf(() -> { try { d.digest(); } catch (Exception e) { throw new RuntimeException(e); } });
        return s;
    }

    static void runDigest(String runId, String ts, String bench, String algo, String ref,
                          byte[] buf, int warmup, int trials) throws Exception {
        MessageDigest md = MessageDigest.getInstance(algo);
        long[] s = bench(warmup, trials, () -> { md.reset(); return md.digest(buf); });
        md.reset();
        boolean checkOk = hex(md.digest(buf)).equals(ref);
        emit(runId, ts, bench, "java.security.MessageDigest", System.getProperty("java.version"),
             warmup, trials, s, checkOk);
    }

    public static void main(String[] args) throws Exception {
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        byte[] buf = new byte[N];
        for (int i = 0; i < N; i++) buf[i] = (byte) (i & 0xFF);

        // xxhash3 / xxhash3_128 (+ their 256k compute-bound variants) — no JDK
        // stdlib xxHash; jar not provisioned.
        emitSkip(runId, ts, "xxhash3", "no JDK stdlib xxHash; jar not provisioned");
        emitSkip(runId, ts, "xxhash3_128", "no JDK stdlib xxHash; jar not provisioned");
        emitSkip(runId, ts, "xxhash3-256k", "no JDK stdlib xxHash; jar not provisioned");
        emitSkip(runId, ts, "xxhash3_128-256k", "no JDK stdlib xxHash; jar not provisioned");

        runDigest(runId, ts, "sha256", "SHA-256", SHA256_REF, buf, warmup, trials);
        runDigest(runId, ts, "md5", "MD5", MD5_REF, buf, warmup, trials);

        // blake3 — no JDK stdlib BLAKE3; jar not provisioned.
        emitSkip(runId, ts, "blake3", "no JDK stdlib BLAKE3; jar not provisioned");
    }
}
