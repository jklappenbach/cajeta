// Java hash competitors (benchmark-reference unit 8): the hash/ ids of
// bench/hash/XXHash3.md and SipHash.md, same inputs, folds and checks as
// bench/src/main/cajeta/bench/hash/:
//   xxhash3 / xxhash3_128 / siphash: 1e7 hashes of a 32-byte key whose first 8
//     bytes are i little-endian (byte j = (j*31+7) & 0xFF);
//   xxhash3-256k / xxhash3_128-256k: 1e4 hashes of a 256 KB buffer, same rule.
// Libraries (jars pinned by sha256 in competitors/pins.tsv, read from
// competitors/third_party/java/): OpenHFT zero-allocation-hashing 2026.0 for
// XXH3-64/128, Guava 33.7.2 Hashing.sipHash24 for SipHash-2-4 (the JDK has
// neither). JIT-warmed by the warmup calls.
import com.google.common.hash.HashFunction;
import com.google.common.hash.Hashing;
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import java.util.function.Supplier;
import net.openhft.hashing.LongHashFunction;
import net.openhft.hashing.LongTupleHashFunction;

public class HashBench {
    static final long N_KEYS = 10_000_000L, N_BIG = 10_000L;
    static final int BIG = 262_144;
    static volatile Object sink;
    static String runId, ts, ver; static int warmup, trials;

    static String env(String k, String d) { String v = System.getenv(k); return v == null || v.isEmpty() ? d : v; }
    static long peakRssKb() {
        try {
            for (String l : java.nio.file.Files.readAllLines(java.nio.file.Path.of("/proc/self/status")))
                if (l.startsWith("VmHWM:")) return Long.parseLong(l.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }
    static long allocated() {
        return ((com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean()).getCurrentThreadAllocatedBytes();
    }

    static void measure(String bench, long bytes, String lib, String libver, String notes, Supplier<long[]> f, long[] want) {
        for (int w = 0; w < warmup; w++) sink = f.get();
        long[] s = new long[trials]; boolean ok = true;
        for (int t = 0; t < trials; t++) {
            long t0 = System.nanoTime(); long[] r = f.get(); s[t] = System.nanoTime() - t0;
            sink = r; ok &= Arrays.equals(r, want);
        }
        long a0 = allocated(); sink = f.get(); long alloc = allocated() - a0;
        Arrays.sort(s); int k = s.length; long sum = 0; for (long v : s) sum += v;
        long med = s[k / 2];
        double mbps = med > 0 ? (double) bytes / med * 1e9 / 1048576.0 : 0.0;
        System.out.printf("1,%s,%s,%s,hash,,%d,,%d,java,%s,%s,%s,-server,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%s,%s,%n",
            runId, ts, bench, bytes, bytes, ver, lib, libver, warmup, trials, s[0], med, sum / k, s[Math.min(k - 1, k * 95 / 100)],
            mbps, peakRssKb(), alloc, ok ? "ok" : "invalid", ok, notes);
    }

    static byte[] buf(int n) { byte[] b = new byte[n]; for (int j = 0; j < n; j++) b[j] = (byte) ((j * 31 + 7) & 0xFF); return b; }
    static void putLe(byte[] b, long v) { for (int j = 0; j < 8; j++) b[j] = (byte) (v >>> (8 * j)); }

    public static void main(String[] args) {
        runId = env("PROFILE_RUN_ID", "local"); ts = env("PROFILE_RUN_TS", ""); ver = env("PROFILE_LANG_VERSION", "");
        warmup = Integer.parseInt(env("PROFILE_WARMUP", "3")); trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));
        byte[] key = buf(32), big = buf(BIG);
        LongHashFunction x64 = LongHashFunction.xx3();
        LongTupleHashFunction x128 = LongTupleHashFunction.xx128();
        String zv = "2026.0";

        measure("xxhash3", N_KEYS * 32, "zero-allocation-hashing", zv, "LongHashFunction.xx3", () -> {
            long a = 0;
            for (long i = 0; i < N_KEYS; i++) { putLe(key, i); a ^= x64.hashBytes(key); }
            return new long[] {a, 0};
        }, new long[] {0x15ae4a024a0e82deL, 0});
        measure("xxhash3_128", N_KEYS * 32, "zero-allocation-hashing", zv, "LongTupleHashFunction.xx128 into a reused long[2]", () -> {
            long lo = 0, hi = 0; long[] h = new long[2];
            for (long i = 0; i < N_KEYS; i++) { putLe(key, i); x128.hashBytes(key, h); lo ^= h[0]; hi ^= h[1]; }
            return new long[] {lo, hi};
        }, new long[] {0x7f4adb2d9f387c7fL, 0xb318fb3ec50537efL});
        measure("xxhash3-256k", N_BIG * BIG, "zero-allocation-hashing", zv, "LongHashFunction.xx3", () -> {
            long a = 0;
            for (long i = 0; i < N_BIG; i++) { putLe(big, i); a ^= x64.hashBytes(big); }
            return new long[] {a, 0};
        }, new long[] {0x4f52c915522f9fe0L, 0});
        measure("xxhash3_128-256k", N_BIG * BIG, "zero-allocation-hashing", zv, "LongTupleHashFunction.xx128 into a reused long[2]", () -> {
            long lo = 0, hi = 0; long[] h = new long[2];
            for (long i = 0; i < N_BIG; i++) { putLe(big, i); x128.hashBytes(big, h); lo ^= h[0]; hi ^= h[1]; }
            return new long[] {lo, hi};
        }, new long[] {0x4f52c915522f9fe0L, 0x3c843c6da63e2dc3L});

        HashFunction sip = Hashing.sipHash24(0x0706050403020100L, 0x0f0e0d0c0b0a0908L);
        byte[] vec = new byte[15]; for (int j = 0; j < 15; j++) vec[j] = (byte) j;
        boolean vecOk = sip.hashBytes(vec).asLong() == 0xa129ca6149be45e5L;
        measure("siphash", N_KEYS * 32, "guava", "33.7.2-jre", "Hashing.sipHash24(k0 k1).hashBytes (key 00..0f)", () -> {
            long a = 0;
            for (long i = 0; i < N_KEYS; i++) { putLe(key, i); a ^= sip.hashBytes(key).asLong(); }
            return new long[] {vecOk ? a : 0, 0};
        }, new long[] {0x1a6999cca3276432L, 0});
    }
}
