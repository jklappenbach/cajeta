// Java collection/ competitors, hash half: hashmap-lookup-hit/-miss/-iterate,
// hashset-contains and the ImmutableMap/ImmutableSet ids, same sizes, inputs
// and checks as the Cajeta benchmarks. java.util.HashMap<Long,Long> and
// HashSet<Long> (boxed), Map.copyOf and Set.copyOf (the JDK's immutable
// collections).
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

public final class MapsBench {
    static final int N = 1_000_000, Q = 10_000_000;

    public static void main(String[] args) {
        firstSuite();
        long[] keys = new long[N];
        HashMap<Long, Long> m = new HashMap<>();
        HashSet<Long> st = new HashSet<>();
        for (int i = 0; i < N; i++) { keys[i] = CollGen.key(i); m.put(keys[i], (long) i); st.add(keys[i]); }
        int[] hitIdx = new CollGen().indices(Q, N);
        long e = 0;
        long[] hitQ = new long[Q], missQ = new long[Q];
        for (int j = 0; j < Q; j++) { e += hitIdx[j]; hitQ[j] = keys[hitIdx[j]]; missQ[j] = CollGen.key(N + (long) hitIdx[j]); }
        final long hitExp = e;
        int[] halfIdx = new CollGen().indices(Q, 2 * N);
        long h = 0;
        long[] halfQ = new long[Q];
        for (int j = 0; j < Q; j++) { if (halfIdx[j] < N) h++; halfQ[j] = CollGen.key(halfIdx[j]); }
        final long halfExp = h;
        final long k7 = CollGen.key(7);
        if (CollGen.selected("hashmap-lookup-hit"))
            CollGen.row("hashmap-lookup-hit", "HashMap", "jdk", Q, CollGen.bench(() -> lookup(m, hitQ), s -> s == hitExp), "boxed Long");
        if (CollGen.selected("hashmap-lookup-miss"))
            CollGen.row("hashmap-lookup-miss", "HashMap", "jdk", Q, CollGen.bench(() -> {
                long c = 0;
                for (long k : missQ) if (!m.containsKey(k)) c++;
                return c;
            }, c -> c == Q), "containsKey; boxed Long");
        if (CollGen.selected("hashmap-iterate"))
            CollGen.row("hashmap-iterate", "HashMap", "jdk", Q, CollGen.bench(() -> {
                long s = 0;
                for (int p = 0; p < 10; p++) for (long v : m.values()) s += v;
                return s;
            }, s -> s == 10L * ((long) N * (N - 1) / 2)), "values() for-each");
        if (CollGen.selected("hashset-contains"))
            CollGen.row("hashset-contains", "HashSet<Long>", "jdk", Q, CollGen.bench(() -> contains(st, halfQ), c -> c == halfExp), "boxed Long");
        if (CollGen.selected("immutablemap-build"))
            CollGen.row("immutablemap-build", "Map.copyOf", "jdk", 10L * N, CollGen.bench(() -> {
                long t = 0, p = 0;
                for (int r = 0; r < 10; r++) { Map<Long, Long> f = Map.copyOf(m); t += f.size(); p = f.get(k7); }
                return t == 10L * N && p == 7;
            }, r -> r), "");
        if (CollGen.selected("immutablemap-lookup")) {
            Map<Long, Long> f = Map.copyOf(m);
            CollGen.row("immutablemap-lookup", "Map.copyOf", "jdk", Q, CollGen.bench(() -> lookup(f, hitQ), s -> s == hitExp), "boxed Long");
        }
        if (CollGen.selected("immutableset-build"))
            CollGen.row("immutableset-build", "Set.copyOf", "jdk", 10L * N, CollGen.bench(() -> {
                long t = 0;
                boolean p = false;
                for (int r = 0; r < 10; r++) { Set<Long> f = Set.copyOf(st); t += f.size(); p = f.contains(k7); }
                return t == 10L * N && p;
            }, r -> r), "");
        if (CollGen.selected("immutableset-contains")) {
            Set<Long> f = Set.copyOf(st);
            CollGen.row("immutableset-contains", "Set.copyOf", "jdk", Q, CollGen.bench(() -> contains(f, halfQ), c -> c == halfExp), "boxed Long");
        }
    }

    static long lookup(Map<Long, Long> m, long[] q) {
        long s = 0;
        for (long k : q) s += m.get(k);
        return s;
    }

    static long contains(Set<Long> st, long[] q) {
        long c = 0;
        for (long k : q) if (st.contains(k)) c++;
        return c;
    }

    // hashmap-int, hashmap-string, hashset-dedup at the page shape (n/100 under
    // CAJETA_BENCH_SMOKE=1): insert into an empty map/set, then look up.
    static void firstSuite() {
        final int ni = CollGen.sized(N);
        if (CollGen.selected("hashmap-int")) {
            long[] ks = new long[ni];
            for (int i = 0; i < ni; i++) ks[i] = CollGen.key(i);
            CollGen.row("hashmap-int", "HashMap", "jdk", ni, CollGen.bench(() -> {
                HashMap<Long, Long> m = new HashMap<>();
                for (int i = 0; i < ni; i++) m.put(ks[i], (long) i);
                long s = 0;
                for (long k : ks) s += m.get(k);
                return s;
            }, s -> s == (long) ni * (ni - 1) / 2), CollGen.sizeNote(N, "boxed Long"));
        }
        if (CollGen.selected("hashmap-string")) {
            String[] ks = new String[ni], ps = new String[ni];
            for (int i = 0; i < ni; i++) { ks[i] = CollGen.hex16(CollGen.key(i)); ps[i] = new String(ks[i].toCharArray()); }
            CollGen.row("hashmap-string", "HashMap", "jdk", ni, CollGen.bench(() -> {
                HashMap<String, Long> m = new HashMap<>();
                for (int i = 0; i < ni; i++) m.put(ks[i], (long) i);
                long s = 0;
                for (String p : ps) s += m.get(p);
                return s;
            }, s -> s == (long) ni * (ni - 1) / 2), CollGen.sizeNote(N, "lookups through separately allocated equal Strings (hash recomputed once each)"));
        }
        if (CollGen.selected("hashset-dedup")) {
            final int nd = CollGen.sized(10 * N);
            int[] vals = new CollGen().indices(nd, nd / 10);
            boolean[] seen = new boolean[nd / 10];
            int e = 0;
            for (int v : vals) if (!seen[v]) { seen[v] = true; e++; }
            final int exp = e;
            CollGen.row("hashset-dedup", "HashSet", "jdk", nd, CollGen.bench(() -> {
                HashSet<Integer> st = new HashSet<>();
                for (int v : vals) st.add(v);
                return st.size();
            }, c -> c == exp), CollGen.sizeNote(10 * N, "boxed Integer"));
        }
    }
}
