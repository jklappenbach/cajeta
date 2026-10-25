// Java collection/ competitors for the Cache ids: Caffeine 3.3.1, same sizes,
// inputs and checks as the Cajeta benchmarks (capacity 1e5; 1e7 operations;
// TTL 1 ms over 1e6 entries with a 2 ms sleep before the read-back).
// Caffeine.newBuilder().maximumSize(C) (W-TinyLFU) with its default
// maintenance executor (ForkJoinPool.commonPool); each size check follows
// cleanUp(), inside the time. A miss is getIfPresent == null then put, the
// Cajeta shape.
import com.github.benmanes.caffeine.cache.Cache;
import com.github.benmanes.caffeine.cache.Caffeine;
import java.time.Duration;

public final class CacheBench {
    static final int C = 100_000, Q = 10_000_000;
    static final String LIB = "Caffeine", VER = "3.3.1";

    public static void main(String[] args) throws Exception {
        long[] keys = new long[Q];
        for (int i = 0; i < Q; i++) keys[i] = CollGen.key(i);
        if (CollGen.selected("cache-hit")) {
            Cache<Long, Long> hot = Caffeine.newBuilder().maximumSize(C).build();
            for (int i = 0; i < C; i++) hot.put(keys[i], (long) i);
            hot.cleanUp();
            int[] idx = new CollGen().indices(Q, C);
            long[] q = new long[Q];
            for (int j = 0; j < Q; j++) q[j] = keys[idx[j]];
            CollGen.row("cache-hit", LIB, VER, Q, CollGen.bench(() -> {
                long h = 0;
                for (long k : q) if (hot.getIfPresent(k) != null) h++;
                return h;
            }, h -> h == Q), "maximumSize; boxed Long");
        }
        if (CollGen.selected("cache-miss"))
            CollGen.row("cache-miss", LIB, VER, Q, CollGen.bench(() -> {
                Cache<Long, Long> c = Caffeine.newBuilder().maximumSize(C).build();
                long m = 0;
                for (int i = 0; i < Q; i++) if (c.getIfPresent(keys[i]) == null) { m++; c.put(keys[i], (long) i); }
                c.cleanUp();
                return m == Q && c.estimatedSize() == C;
            }, r -> r), "getIfPresent then put; cleanUp before estimatedSize");
        if (CollGen.selected("cache-evict"))
            CollGen.row("cache-evict", LIB, VER, Q, CollGen.bench(() -> {
                Cache<Long, Long> c = Caffeine.newBuilder().maximumSize(C).build();
                for (int i = 0; i < Q; i++) c.put(keys[i], (long) i);
                c.cleanUp();
                return c.estimatedSize();
            }, n -> n == C), "cleanUp before estimatedSize");
        if (CollGen.selected("cache-ttl"))
            CollGen.row("cache-ttl", LIB, VER, 1_000_000, CollGen.bench(() -> {
                Cache<Long, Long> c = Caffeine.newBuilder().maximumSize(1_000_000).expireAfterWrite(Duration.ofMillis(1)).build();
                for (int i = 0; i < 1_000_000; i++) c.put(keys[i], (long) i);
                try { Thread.sleep(2); } catch (InterruptedException e) { throw new RuntimeException(e); }
                long e = 0;
                for (int i = 0; i < 1_000_000; i++) if (c.getIfPresent(keys[i]) == null) e++;
                return e;
            }, e -> e == 1_000_000), "expireAfterWrite 1 ms; includes the 2 ms sleep");
    }
}
