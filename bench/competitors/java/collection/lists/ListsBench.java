// Java collection/ competitors, sequence half: the ArrayList, LinkedList, Heap,
// ImmutableList, Collectors and binary-search ids, same sizes, inputs and
// checks as the Cajeta benchmarks (bench/src/main/cajeta/bench/collection/).
// java.util only: ArrayList<Integer> (boxed, as the page notes), LinkedList,
// PriorityQueue, List.copyOf, Collectors.toList / groupingBy, Arrays.binarySearch.
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.stream.Collectors;

public final class ListsBench {
    static final int N1M = 1_000_000;

    static long getLoop(List<Integer> l, int[] idx) {
        long s = 0;
        for (int p = 0; p < 100; p++) for (int j : idx) s += l.get(j);
        return s;
    }

    static boolean sorted(List<Integer> a) {
        for (int i = 1; i < a.size(); i++) if (a.get(i) < a.get(i - 1)) return false;
        return true;
    }

    public static void main(String[] args) {
        ArrayList<Integer> lst1m = new ArrayList<>();
        for (int i = 0; i < N1M; i++) lst1m.add(i);
        int[] idx = new CollGen().indices(N1M, N1M);
        long exp = 0;
        for (int x : idx) exp += x;
        final long expGet = exp * 100;
        if (CollGen.selected("arraylist-get"))
            CollGen.row("arraylist-get", "ArrayList<Integer>", "jdk", 100_000_000,
                CollGen.bench(() -> getLoop(lst1m, idx), s -> s == expGet), "boxed Integer");
        ArrayList<Integer> lst1e7 = new ArrayList<>();
        for (int i = 0; i < 10_000_000; i++) lst1e7.add(i);
        if (CollGen.selected("arraylist-iterate"))
            CollGen.row("arraylist-iterate", "ArrayList<Integer>", "jdk", 100_000_000, CollGen.bench(() -> {
                long s = 0;
                for (int p = 0; p < 10; p++) for (int v : lst1e7) s += v;
                return s;
            }, s -> s == 10L * (10_000_000L * 9_999_999L / 2)), "boxed Integer; for-each");
        if (CollGen.selected("arraylist-sort")) {
            CollGen g = new CollGen();
            ArrayList<Integer> src = new ArrayList<>();
            for (int i = 0; i < N1M; i++) src.add(g.next());
            CollGen.row("arraylist-sort", "ArrayList.sort", "jdk", N1M, CollGen.bench(() -> {
                ArrayList<Integer> a = new ArrayList<>(src);
                a.sort(null);
                return a;
            }, a -> a.size() == N1M && sorted(a)), "boxed Integer; TimSort; includes the copy");
        }
        if (CollGen.selected("arraylist-append")) {
            final int n = CollGen.sized(10_000_000);
            CollGen.row("arraylist-append", "ArrayList<Integer>", "jdk", n, CollGen.bench(() -> {
                ArrayList<Integer> a = new ArrayList<>();
                for (int i = 0; i < n; i++) a.add(i);
                return a;
            }, a -> a.size() == n && a.get(n - 1) == n - 1), CollGen.sizeNote(10_000_000, "boxed Integer; no initial capacity"));
        }
        if (CollGen.selected("linkedlist-insert-traverse")) {
            final int n = CollGen.sized(1_000_000);
            CollGen.row("linkedlist-insert-traverse", "LinkedList", "jdk", n, CollGen.bench(() -> {
                LinkedList<Integer> l = new LinkedList<>();
                for (int i = 0; i < n; i++) l.addFirst(i);
                boolean f = l.contains(-1) || l.contains(-1);
                return l.size() == n && !f && l.getFirst() == n - 1 && l.getLast() == 0;
            }, r -> r), CollGen.sizeNote(1_000_000, "traversal = contains of an absent value"));
        }
        if (CollGen.selected("heap-sort")) {
            final int n = CollGen.sized(1_000_000);
            CollGen g = new CollGen();
            int[] vals = new int[n];
            for (int i = 0; i < n; i++) vals[i] = g.next();
            CollGen.row("heap-sort", "PriorityQueue", "jdk", n, CollGen.bench(() -> {
                PriorityQueue<Integer> h = new PriorityQueue<>();
                for (int v : vals) h.add(v);
                ArrayList<Integer> out = new ArrayList<>(n);
                while (!h.isEmpty()) out.add(h.poll());
                return out;
            }, a -> a.size() == n && sorted(a)), CollGen.sizeNote(1_000_000, ""));
        }
        if (CollGen.selected("heap-push-pop")) {
            CollGen g = new CollGen();
            int[] vals = new int[10_001_000];
            for (int i = 0; i < vals.length; i++) vals[i] = g.next();
            CollGen.row("heap-push-pop", "PriorityQueue", "jdk", 10_000_000, CollGen.bench(() -> {
                PriorityQueue<Integer> h = new PriorityQueue<>();
                for (int i = 0; i < 1000; i++) h.add(vals[i]);
                long c = 0;
                for (int i = 1000; i < vals.length; i++) { h.add(vals[i]); c += h.poll(); }
                return c;
            }, c -> c == 10738394301026345L), "add then poll");
        }
        if (CollGen.selected("immutablelist-build"))
            CollGen.row("immutablelist-build", "List.copyOf", "jdk", 100_000_000, CollGen.bench(() -> {
                long t = 0;
                int tail = 0;
                for (int r = 0; r < 100; r++) { List<Integer> c = List.copyOf(lst1m); t += c.size(); tail = c.get(999_999); }
                return t == 100_000_000 && tail == 999_999;
            }, r -> r), "");
        if (CollGen.selected("immutablelist-get")) {
            List<Integer> frozen = List.copyOf(lst1m);
            CollGen.row("immutablelist-get", "List.copyOf", "jdk", 100_000_000,
                CollGen.bench(() -> getLoop(frozen, idx), s -> s == expGet), "boxed Integer");
        }
        if (CollGen.selected("collectors-to-list"))
            CollGen.row("collectors-to-list", "Collectors.toList", "jdk", 10_000_000, CollGen.bench(
                () -> lst1e7.stream().collect(Collectors.toList()),
                o -> o.size() == 10_000_000 && o.get(9_999_999) == 9_999_999), "");
        if (CollGen.selected("collectors-group-by"))
            CollGen.row("collectors-group-by", "Collectors.groupingBy", "jdk", 10_000_000, CollGen.bench(
                () -> lst1e7.stream().collect(Collectors.groupingBy(x -> x % 1000)),
                (Map<Integer, List<Integer>> g) -> g.size() == 1000 && g.get(0).size() == 10000), "");
        if (CollGen.selected("binary-search")) {
            final int n = CollGen.sized(10_000_000);
            final int m = n / 10;
            long[] a = new long[m];
            for (int i = 0; i < m; i++) a[i] = i * 2L;
            int[] qi = new CollGen().indices(n, m);
            long[] q = new long[n];
            for (int j = 0; j < n; j++) q[j] = 2L * qi[j];
            CollGen.row("binary-search", "Arrays.binarySearch", "jdk", n, CollGen.bench(() -> {
                long hits = 0;
                for (long k : q) if (Arrays.binarySearch(a, k) >= 0) hits++;
                return hits;
            }, h -> h == n), CollGen.sizeNote(10_000_000, ""));
        }
    }
}
