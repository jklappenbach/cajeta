// concurrent/tasks-1m-footprint, Java virtual threads (Tasks.md): park 1e6
// virtual threads in take() on one LinkedBlockingQueue (Java's channel), read
// the resident set while all are parked, then put one token per thread and join
// them all. Own process, so peak_rss_kb (the row's memory metric) is this
// workload's (heap, stack chunks and JVM). One trial: the row is memory; the
// time is spawn + park + release. Skips under 16 GB MemAvailable.
// CAJETA_BENCH_SMOKE=1 parks 1e4 threads and says so in notes.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicLong;

public class FootprintBench {
    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }

    static long kb(String path, String key) {
        try {
            for (String l : Files.readAllLines(Path.of(path)))
                if (l.startsWith(key)) return Long.parseLong(l.substring(key.length()).trim().split("\\s+")[0]);
        } catch (Exception e) { }
        return -1;
    }

    public static void main(String[] args) throws Exception {
        boolean smoke = env("CAJETA_BENCH_SMOKE", "").equals("1") || env("CAJETA_BENCH_SMOKE", "").equals("true");
        int n = smoke ? 10_000 : 1_000_000;
        String head = String.format("1,%s,%s,tasks-1m-footprint,concurrent,,%d,,%d,java,%s,virtual-threads",
            env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), n, n, env("PROFILE_LANG_VERSION", ""));
        long avail = kb("/proc/meminfo", "MemAvailable:");
        if (avail >= 0 && avail < 16L * 1024 * 1024) {
            System.out.printf("%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,MemAvailable %d MiB is under the 16 GB this row needs,%n", head, avail / 1024);
            return;
        }
        long t0 = System.nanoTime();
        LinkedBlockingQueue<Integer> q = new LinkedBlockingQueue<>();
        AtomicLong started = new AtomicLong(), done = new AtomicLong();
        long before = kb("/proc/self/status", "VmRSS:");
        Thread[] ts = new Thread[n];
        for (int i = 0; i < n; i++) {
            ts[i] = Thread.ofVirtual().start(() -> {
                started.incrementAndGet();
                try { q.take(); done.incrementAndGet(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            });
        }
        while (started.get() < n) Thread.sleep(1);
        Thread.sleep(10);
        long parkedRss = kb("/proc/self/status", "VmRSS:");
        long parked = started.get();
        for (int i = 0; i < n; i++) q.put(1);
        for (Thread t : ts) t.join();
        long ns = System.nanoTime() - t0;
        boolean ok = parked == n && done.get() == n;
        long grew = parkedRss - before;
        String notes = String.format("one LinkedBlockingQueue; %d virtual threads parked in take(); RSS grew %d KiB while parked = %d B/task; one trial",
            n, grew, grew * 1024 / n) + (smoke ? "; smoke size: 1e4 tasks" : "");
        System.out.printf("%s,stdlib,-server,0,1,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,-1,-1,%d,%s,%b,%s,%n",
            head, ns, ns, ns, ns, (double) n / ns * 1e3, kb("/proc/self/status", "VmHWM:"), grew, ok ? "ok" : "invalid", ok, notes);
    }
}
