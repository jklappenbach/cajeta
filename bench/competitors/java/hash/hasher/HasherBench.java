// Java hasher competitors (benchmark-reference unit 8), same inputs and folds
// as bench/src/main/cajeta/bench/hash/:
//   defaulthasher-string: 1e7 hashes cycling 1024 keys "k-" + (1e13 + k).
//     String.hashCode caches its result in the String, so timing it over a
//     reused pool measures a field load; the row times the polynomial it
//     computes, uncached, over each key's Latin-1 bytes with Arrays.hashCode
//     (the same vectorized intrinsic, initial value 1 instead of 0);
//   defaulthasher-int: 1e8 Long.hashCode of 0 .. 1e8-1 (what HashMap<Long>
//     calls before its spread);
//   hash-struct: 1e7 Objects.hash(id, x, y) cycling 4096 keys (boxing and the
//     varargs array included, as the API does).
// Fold: acc ^= h; low += h & 0xFFFF. Check: a second pass reproduces (acc, low)
// and neighbours hash apart.
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import java.util.function.Supplier;

public class HasherBench {
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

    static void measure(String bench, long n, String lib, String notes, Supplier<long[]> f, boolean extraOk) {
        long[] first = f.get();
        for (int w = 0; w < warmup; w++) sink = f.get();
        long[] s = new long[trials]; boolean ok = extraOk && first[1] > 0;
        for (int t = 0; t < trials; t++) {
            long t0 = System.nanoTime(); long[] r = f.get(); s[t] = System.nanoTime() - t0;
            sink = r; ok &= Arrays.equals(r, first);
        }
        long a0 = allocated(); sink = f.get(); long alloc = allocated() - a0;
        Arrays.sort(s); int k = s.length; long sum = 0; for (long v : s) sum += v;
        long med = s[k / 2];
        double mops = med > 0 ? (double) n / med * 1e3 : 0.0;
        System.out.printf("1,%s,%s,%s,hash,,%d,,%d,java,%s,%s,%s,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%s,%s,%n",
            runId, ts, bench, n, n, ver, lib, Runtime.version(), warmup, trials, s[0], med, sum / k, s[Math.min(k - 1, k * 95 / 100)],
            mops, peakRssKb(), alloc, ok ? "ok" : "invalid", ok, notes);
    }

    public static void main(String[] args) {
        runId = env("PROFILE_RUN_ID", "local"); ts = env("PROFILE_RUN_TS", ""); ver = env("PROFILE_LANG_VERSION", "");
        warmup = Integer.parseInt(env("PROFILE_WARMUP", "3")); trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));

        byte[][] pool = new byte[1024][];
        for (int k = 0; k < 1024; k++) pool[k] = ("k-" + (10_000_000_000_000L + k)).getBytes(StandardCharsets.ISO_8859_1);
        boolean sOk = pool[0].length == 16 && Arrays.hashCode(pool[0]) != Arrays.hashCode(pool[1]);
        measure("defaulthasher-string", 10_000_000L, "java.base",
                "String.hashCode's polynomial uncached: Arrays.hashCode over the key bytes (String caches its hash)", () -> {
            long a = 0, l = 0;
            for (int i = 0; i < 10_000_000; i++) { long h = Arrays.hashCode(pool[i & 1023]); a ^= h; l += h & 0xFFFF; }
            return new long[] {a, l};
        }, sOk);

        measure("defaulthasher-int", 100_000_000L, "java.base", "Long.hashCode", () -> {
            long a = 0, l = 0;
            for (long i = 0; i < 100_000_000L; i++) { long h = Long.hashCode(i); a ^= h; l += h & 0xFFFF; }
            return new long[] {a, l};
        }, Long.hashCode(0) != Long.hashCode(1));

        long[] ids = new long[4096]; int[] xs = new int[4096], ys = new int[4096];
        for (int k = 0; k < 4096; k++) { ids[k] = k * 7919L; xs[k] = k; ys[k] = k ^ 0x5bd1; }
        boolean kOk = Objects.hash(ids[7], xs[7], ys[7]) == Objects.hash(7 * 7919L, 7, 7 ^ 0x5bd1);
        for (int k = 1; k < 4096; k++) kOk &= Objects.hash(ids[k], xs[k], ys[k]) != Objects.hash(ids[k - 1], xs[k - 1], ys[k - 1]);
        measure("hash-struct", 10_000_000L, "java.util.Objects", "Objects.hash(id x y): boxing and varargs included", () -> {
            long a = 0, l = 0;
            for (int i = 0; i < 10_000_000; i++) {
                int k = i & 4095;
                long h = Objects.hash(ids[k], xs[k], ys[k]);
                a ^= h; l += h & 0xFFFF;
            }
            return new long[] {a, l};
        }, kOk);
    }
}
