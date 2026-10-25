// Java concurrency competitors for bench/concurrent/*.md — every id, the same
// workload, size and check as the Cajeta side. One v1 row per id (lib/rows.sh
// lifts it to v2). The contended rows use platform threads where Cajeta uses
// fibers; the sleep and spawn rows use virtual threads. Under
// CAJETA_BENCH_SMOKE=1 the multi-thread rows run 1e4 operations per thread
// (channels: 1e4 messages), the 1e8 single-thread rows 1e6 and the sleep row 100
// sleeps; those rows say "smoke size" in notes. Every lock and atomic is
// published to a static field so escape analysis cannot remove it; results are
// returned and checked. Timings include JIT warm-up (PROFILE_WARMUP).
//
// alloc_bytes is the main thread's allocated bytes for one execution
// (com.sun.management.ThreadMXBean); work on other threads is not seen.
import com.sun.management.ThreadMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;
import java.util.concurrent.locks.ReentrantReadWriteLock;

public class ConcurrentBench {
    static final ThreadMXBean TMX = (ThreadMXBean) ManagementFactory.getThreadMXBean();
    static String RUN_ID, TS, VER;
    static int WARMUP, TRIALS;
    static boolean SMOKE;
    static volatile Object SINK;

    interface Work { long run() throws Exception; }
    interface Body { long run(int k) throws Exception; }

    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }
    static String smokeNote(String what) { return SMOKE ? "smoke size: " + what : ""; }
    static String join(String a, String b) { return b.isEmpty() ? a : a + "; " + b; }

    static long peakRssKb() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status")))
                if (line.startsWith("VmHWM:")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static void emit(String bench, String lib, long input, int warm, long[] s, boolean ok, long alloc, String notes) {
        Arrays.sort(s);
        int n = s.length;
        long sum = 0; for (long x : s) sum += x;
        long mn = s[0], med = s[n / 2], mean = sum / n, p95 = s[Math.min(n - 1, n * 95 / 100)];
        double mops = med > 0 ? (double) input / (double) med * 1e9 / 1e6 : 0.0;
        System.out.printf("1,%s,%s,%s,concurrent,,%d,,%d,java,%s,%s,stdlib,-server,%d,%d,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,%d,-1,-1,%s,%b,%s,%n",
            RUN_ID, TS, bench, input, input, VER, lib, warm, n, mn, med, mean, p95, mops, peakRssKb(), alloc,
            ok ? "ok" : "invalid", ok, notes);
        System.out.flush();
    }

    static void bench(String id, String lib, long input, String notes, Work f, long want) {
        boolean ok = true;
        try {
            for (int i = 0; i < WARMUP; i++) SINK = f.run();
        } catch (Exception e) { ok = false; }
        long[] s = new long[TRIALS];
        for (int i = 0; i < TRIALS; i++) {
            long t0 = System.nanoTime();
            long r;
            try { r = f.run(); } catch (Exception e) { r = Long.MIN_VALUE; }
            s[i] = System.nanoTime() - t0;
            ok = ok && r == want;
        }
        long tid = Thread.currentThread().threadId();
        long a0 = TMX.getThreadAllocatedBytes(tid);
        try { SINK = f.run(); } catch (Exception e) { ok = false; }
        long alloc = TMX.getThreadAllocatedBytes(tid) - a0;
        emit(id, lib, input, WARMUP, s, ok, alloc, notes);
    }

    static long[] threads(int t, Body b) throws Exception {
        long[] out = new long[t];
        Exception[] err = new Exception[1];
        List<Thread> ts = new ArrayList<>();
        for (int k = 0; k < t; k++) {
            final int kk = k;
            ts.add(Thread.ofPlatform().start(() -> {
                try { out[kk] = b.run(kk); } catch (Exception e) { synchronized (err) { err[0] = e; } }
            }));
        }
        for (Thread th : ts) th.join();
        if (err[0] != null) throw err[0];
        return out;
    }

    // Escape sinks: the primitives live in static fields.
    static AtomicLong AL; static AtomicInteger AI; static ReentrantLock RL; static ReentrantReadWriteLock RW;
    static long V;
    static final ScopedValue<Long> SV = ScopedValue.newInstance();
    static final ThreadLocal<Long> TLV = new ThreadLocal<>();

    public static void main(String[] args) throws Exception {
        RUN_ID = env("PROFILE_RUN_ID", "local"); TS = env("PROFILE_RUN_TS", ""); VER = env("PROFILE_LANG_VERSION", "");
        WARMUP = Integer.parseInt(env("PROFILE_WARMUP", "3")); TRIALS = Integer.parseInt(env("PROFILE_TRIALS", "10"));
        SMOKE = env("CAJETA_BENCH_SMOKE", "").equals("1") || env("CAJETA_BENCH_SMOKE", "").equals("true");
        final long n1 = SMOKE ? 1_000_000L : 100_000_000L, per = SMOKE ? 10_000L : 1_000_000L;
        final long nch = SMOKE ? 10_000L : 5_000_000L, nsp = SMOKE ? 10_000L : 100_000L;
        final int nsleep = SMOKE ? 100 : 10_000;
        final String s1 = smokeNote("1e6 ops"), sc = smokeNote("1e4 ops per thread");

        bench("atomic-fetchadd", "AtomicLong", n1, join("incrementAndGet", s1), () -> {
            AL = new AtomicLong(); for (long i = 0; i < n1; i++) AL.incrementAndGet(); return AL.get();
        }, n1);
        bench("atomic-fetchadd-32", "AtomicInteger", n1, join("incrementAndGet", s1), () -> {
            AI = new AtomicInteger(); for (long i = 0; i < n1; i++) AI.incrementAndGet(); return AI.get();
        }, n1);
        bench("atomic-cas", "AtomicLong", n1, join("compareAndSet; uncontended", s1), () -> {
            AL = new AtomicLong();
            for (long i = 0; i < n1; i++) if (!AL.compareAndSet(i, i + 1)) throw new IllegalStateException("cas failed");
            return AL.get();
        }, n1);
        bench("atomic-contended-16", "AtomicLong+platform threads", 16 * per, join("16 platform threads", sc), () -> {
            AL = new AtomicLong(); final AtomicLong a = AL;
            threads(16, k -> { for (long i = 0; i < per; i++) a.incrementAndGet(); return 0; });
            return a.get();
        }, 16 * per);
        bench("mutex-uncontended", "ReentrantLock", n1, join("lock; v++; unlock on one thread", s1), () -> {
            RL = new ReentrantLock(); V = 0;
            for (long i = 0; i < n1; i++) { RL.lock(); try { V++; } finally { RL.unlock(); } }
            return V;
        }, n1);
        for (int t : new int[] {4, 16}) {
            bench("mutex-contended-" + t, "ReentrantLock+platform threads", t * per, join(t + " platform threads", sc), () -> {
                RL = new ReentrantLock(); V = 0; final ReentrantLock m = RL;
                threads(t, k -> { for (long i = 0; i < per; i++) { m.lock(); try { V++; } finally { m.unlock(); } } return 0; });
                return V;
            }, t * per);
        }
        bench("rwlock-read-heavy", "ReentrantReadWriteLock", 16 * per, join("16 platform threads 90/10 read/write", sc), () -> {
            RW = new ReentrantReadWriteLock(); V = 0; final ReentrantReadWriteLock m = RW;
            long[] reads = threads(16, k -> {
                long s = 0;
                for (long i = 0; i < per; i++) {
                    if (i % 10 == 0) { m.writeLock().lock(); try { V++; } finally { m.writeLock().unlock(); } }
                    else { m.readLock().lock(); try { s += V; } finally { m.readLock().unlock(); } }
                }
                return s;
            });
            SINK = reads;
            return V;
        }, 16 * (per / 10));
        bench("lock-uncontended", "ReentrantLock", n1, join("lock/unlock pair per pass on one thread", s1), () -> {
            RL = new ReentrantLock(); long c = 0;
            for (long i = 0; i < n1; i++) { RL.lock(); try { c++; } finally { RL.unlock(); } }
            return c;
        }, n1);
        bench("semaphore-acquire-release", "java.util.concurrent.Semaphore", 16 * per,
              join("16 platform threads through 4 permits", smokeNote("1e4 passes per thread")), () -> {
            Semaphore sem = new Semaphore(4); AtomicLong inside = new AtomicLong(), viol = new AtomicLong();
            long[] passes = threads(16, k -> {
                for (long i = 0; i < per; i++) {
                    sem.acquire();
                    try { if (inside.incrementAndGet() > 4) viol.incrementAndGet(); inside.decrementAndGet(); }
                    finally { sem.release(); }
                }
                return per;
            });
            if (viol.get() != 0) throw new IllegalStateException("more than 4 holders");
            long t = 0; for (long p : passes) t += p; return t;
        }, 16 * per);

        final long wantCh = nch * (nch + 1) / 2;
        int[][] shapes = {{1, 1}, {4, 1}, {4, 4}};
        String[] ids = {"channel-spsc", "channel-mpsc", "channel-mpmc"};
        for (int si = 0; si < 3; si++) {
            final int P = shapes[si][0], C = shapes[si][1];
            bench(ids[si], "ArrayBlockingQueue<Long>", nch, join("capacity 1000 put/take; boxed Long; a 0 sentinel per consumer; platform threads", smokeNote("1e4 messages")), () -> {
                ArrayBlockingQueue<Long> q = new ArrayBlockingQueue<>(1000);
                long pp = nch / P;
                AtomicLong sum = new AtomicLong(), got = new AtomicLong();
                List<Thread> cons = new ArrayList<>();
                for (int c = 0; c < C; c++) cons.add(Thread.ofPlatform().start(() -> {
                    long s = 0, k = 0;
                    try { for (;;) { long v = q.take(); if (v == 0) break; s += v; k++; } } catch (InterruptedException e) { return; }
                    sum.addAndGet(s); got.addAndGet(k);
                }));
                threads(P, p -> { for (long i = 0; i < pp; i++) q.put((long) p * pp + i + 1); return 0; });
                for (int c = 0; c < C; c++) q.put(0L);
                for (Thread t : cons) t.join();
                if (got.get() != nch) throw new IllegalStateException("received " + got.get());
                return sum.get();
            }, wantCh);
        }

        // fiber-sleep-wake: one virtual thread sleeps 1 ms per sample; the
        // overshoot is measured inside it.
        {
            final int warm = 10;
            final long[] samples = new long[nsleep];
            Thread vt = Thread.ofVirtual().start(() -> {
                try {
                    for (int k = 0; k < warm + nsleep; k++) {
                        long t0 = System.nanoTime();
                        Thread.sleep(1);
                        long over = System.nanoTime() - t0 - 1_000_000L;
                        if (k >= warm) samples[k - warm] = Math.max(0, over);
                    }
                } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            });
            vt.join();
            emit("fiber-sleep-wake", "virtual thread Thread.sleep", 1, warm, samples, true, 0,
                 join("row = overshoot past 1 ms per sleep over the trials; one virtual thread", smokeNote("100 sleeps")));
        }

        final long wantFl = n1 * (n1 - 1) / 2;
        bench("fiberlocal-get-set", "ScopedValue", n1, join("ScopedValue.where(K; i).run(sum += K.get()) per pass", s1), () -> {
            final long[] acc = new long[1];
            for (long i = 0; i < n1; i++) { ScopedValue.where(SV, i).run(() -> acc[0] += SV.get()); }
            return acc[0];
        }, wantFl);
        bench("fiberlocal-get-set", "ThreadLocal", n1, join("ThreadLocal<Long> set(i) then get() per pass (boxed)", s1), () -> {
            long sum = 0;
            for (long i = 0; i < n1; i++) { TLV.set(i); sum += TLV.get(); }
            return sum;
        }, wantFl);

        bench("task-spawn-await", "virtual-threads", nsp, join("Thread.ofVirtual().start + join per task", smokeNote("1e4 tasks")), () -> {
            long sum = 0;
            for (long i = 0; i < nsp; i++) {
                final long fi = i; final long[] box = new long[1];
                Thread t = Thread.ofVirtual().start(() -> box[0] = fi + 1);
                t.join();
                sum += box[0];
            }
            return sum;
        }, nsp * (nsp + 1) / 2);
    }
}
