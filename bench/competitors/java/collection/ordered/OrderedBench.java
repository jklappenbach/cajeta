// Java collection/ competitors, ordered half: redblacktree-insert-lookup (the
// Cajeta shape), redblacktree-range, bplustree-insert, bplustree-point-lookup,
// bplustree-range, same sizes, inputs and checks as the Cajeta benchmarks.
// java.util.TreeMap<Long,Long> (a red-black tree; the page's Java competitor
// for both pages).
import java.util.Arrays;
import java.util.Map;
import java.util.TreeMap;

public final class OrderedBench {
    static final int N = 1_000_000;

    public static void main(String[] args) {
        if (CollGen.selected("redblacktree-insert-lookup")) {
            final int m = 20000;
            CollGen.row("redblacktree-insert-lookup", "TreeMap", "jdk", m, CollGen.bench(() -> {
                TreeMap<Integer, Integer> t = new TreeMap<>();
                for (int i = 0; i < m; i++) t.put(i, i * 2);
                long s = 0;
                for (int j = 0; j < m; j++) s += t.get(j);
                return s;
            }, s -> s == (long) m * (m - 1)), "");
        }
        long[] keys = new long[N];
        for (int i = 0; i < N; i++) keys[i] = CollGen.key(i);
        if (CollGen.selected("bplustree-insert"))
            CollGen.row("bplustree-insert", "TreeMap", "jdk", N, CollGen.bench(() -> {
                TreeMap<Long, Long> t = new TreeMap<>();
                for (int i = 0; i < N; i++) t.put(keys[i], (long) i);
                return t.size() == N && t.get(keys[7]) == 7;
            }, r -> r), "boxed Long");
        TreeMap<Long, Long> tree = new TreeMap<>();
        for (int i = 0; i < N; i++) tree.put(keys[i], (long) i);
        if (CollGen.selected("bplustree-point-lookup")) {
            int[] idx = new CollGen().indices(10_000_000, N);
            long e = 0;
            long[] q = new long[idx.length];
            for (int j = 0; j < idx.length; j++) { e += idx[j]; q[j] = keys[idx[j]]; }
            final long exp = e;
            CollGen.row("bplustree-point-lookup", "TreeMap", "jdk", 10_000_000, CollGen.bench(() -> {
                long s = 0;
                for (long k : q) s += tree.get(k);
                return s;
            }, s -> s == exp), "boxed Long");
        }
        // Ranges: 1e4 scans of 1e3 entries from the first key >= key(draw % 1e6).
        int[] si = new CollGen().indices(10_000, N);
        long[] starts = new long[si.length];
        for (int j = 0; j < si.length; j++) starts[j] = CollGen.key(si[j]);
        long[][] srt = new long[N][];
        for (int i = 0; i < N; i++) srt[i] = new long[] {keys[i], i};
        Arrays.sort(srt, (a, b) -> Long.compare(a[0], b[0]));
        long re = 0;
        for (long s : starts) {
            int lo = 0, hi = N;
            while (lo < hi) { int mid = (lo + hi) >>> 1; if (srt[mid][0] < s) lo = mid + 1; else hi = mid; }
            for (int j = lo; j < Math.min(N, lo + 1000); j++) re += srt[j][1];
        }
        srt = null;
        final long rangeExp = re;
        for (String id : new String[] {"redblacktree-range", "bplustree-range"})
            if (CollGen.selected(id))
                CollGen.row(id, "TreeMap", "jdk", 10_000_000, CollGen.bench(() -> {
                    long s = 0;
                    for (long st : starts) {
                        int c = 0;
                        for (Map.Entry<Long, Long> en : tree.tailMap(st, true).entrySet()) {
                            s += en.getValue();
                            if (++c == 1000) break;
                        }
                    }
                    return s;
                }, s -> s == rangeExp), "tailMap(start).entrySet() for 1000 entries");
    }
}
