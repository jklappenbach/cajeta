// Java codec competitor — parse the nativejson corpus to a DOM (Jackson
// JsonNode tree) and emit one schema-conformant CSV row per (file, library),
// matching competitors/columns.txt. Library: jackson-databind, the de-facto
// standard JVM JSON library (the JDK has no stdlib JSON). Cross-check: a known
// element count per file (twitter.statuses=100 / citm.events=184 /
// canada.features=1). Jackson jars are vendored under competitors/vendor/jackson
// and placed on the classpath by codec.sh.
//
// Notes: timings include JVM JIT warmup (honoring PROFILE_WARMUP/TRIALS like the
// other runners; readTree over these corpora is large enough to OSR-compile in a
// trial). alloc_bytes is per-thread bytes allocated for one parse, via the
// HotSpot com.sun.management.ThreadMXBean — the Java-native analog of Go's
// TotalAlloc delta / Rust's global allocator / C++ operator new override.
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

public class JsonDom {
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();

    // (dataset_name, filename, expected count)
    static final String[][] CORPUS = {
        {"twitter", "twitter.json", "100"},
        {"citm_catalog", "citm_catalog.json", "184"},
        {"canada", "canada.json", "1"},
    };

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

    static int count(JsonNode node, String dataset) {
        if (node == null) return -1;
        JsonNode child = switch (dataset) {
            case "twitter" -> node.get("statuses");
            case "citm_catalog" -> node.get("events");
            case "canada" -> node.get("features");
            default -> null;
        };
        return child == null ? -1 : child.size();
    }

    static void emit(String runId, String ts, String dataset, long bytes, String lib, String ver,
                     int warmup, int trials, long[] samples, boolean ok, long alloc) {
        Arrays.sort(samples);
        int n = samples.length;
        long sum = 0; for (long x : samples) sum += x;
        int idx = Math.min(n - 1, n * 95 / 100);
        long mn = samples[0], med = samples[n / 2], mean = sum / n, p95 = samples[idx];
        double mbps = med > 0 ? (double) bytes / (double) med * 1e9 / 1048576.0 : 0.0;
        String status = ok ? "ok" : "invalid";
        System.out.printf(
            "1,%s,%s,json-dom,codec,%s,%d,,%d,java,%s,%s,%s,-server,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%b,,%n",
            runId, ts, dataset, bytes, bytes, env("PROFILE_LANG_VERSION", ""), lib, ver,
            warmup, trials, mn, med, mean, p95, mbps, peakRssKb(), alloc, status, ok);
    }

    public static void main(String[] args) throws Exception {
        String dataDir = System.getenv("PROFILE_DATA_DIR");
        if (dataDir == null || dataDir.isEmpty()) {
            System.err.println("java/codec: PROFILE_DATA_DIR unset — skipping");
            return;
        }
        String runId = env("PROFILE_RUN_ID", "local");
        String ts = env("PROFILE_RUN_TS", "");
        int warmup = Integer.parseInt(env("PROFILE_WARMUP", "3"));
        int trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));
        String ver = env("PROFILE_JACKSON_VERSION", "2.22.0");

        ObjectMapper mapper = new ObjectMapper(); // reused across trials (its intended use)

        for (String[] c : CORPUS) {
            String dataset = c[0], fname = c[1];
            int expect = Integer.parseInt(c[2]);
            Path path = Path.of(dataDir, fname);
            if (!Files.exists(path)) continue; // dataset absent → skip
            byte[] raw = Files.readAllBytes(path);
            long nbytes = raw.length;

            boolean check;
            try {
                check = count(mapper.readTree(raw), dataset) == expect;
            } catch (Exception e) {
                check = false;
            }
            long alloc = allocOf(() -> { try { mapper.readTree(raw); } catch (Exception e) { } });
            for (int i = 0; i < warmup; i++) mapper.readTree(raw);
            long[] samples = new long[trials];
            for (int i = 0; i < trials; i++) {
                long t0 = System.nanoTime();
                JsonNode node = mapper.readTree(raw);
                samples[i] = System.nanoTime() - t0;
                if (node == null) check = false;
            }
            emit(runId, ts, dataset, nbytes, "jackson-databind", ver, warmup, trials,
                 samples, check, alloc);
        }
    }
}
