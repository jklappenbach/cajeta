// Java collection/ sort competitors: sort-int64 (random, ascending,
// descending, dups), sort-f64, sort-stable-int64 at the page shape (1e6; n/100
// under CAJETA_BENCH_SMOKE=1), same inputs and checks as the Cajeta benchmarks
// (CollGen): random = key(i), f64 = draw / 2^31, stable = (value, index) pairs
// sorted on the value. Arrays.sort on long[] / double[] (dual-pivot
// quicksort); the stable row is Arrays.sort on objects, as the page names:
// a Pair[] (value, index) with a comparator on the value (TimSort). The input
// copy is outside the time.
import java.util.Arrays;
import java.util.Comparator;

public final class SortBench {
    static final int FULL = 1_000_000;

    record Pair(int value, int index) { }

    static boolean sorted(long[] a) { for (int i = 1; i < a.length; i++) if (a[i] < a[i - 1]) return false; return true; }
    static boolean sorted(double[] a) { for (int i = 1; i < a.length; i++) if (a[i] < a[i - 1]) return false; return true; }

    public static void main(String[] args) {
        int n = CollGen.sized(FULL);
        String note = CollGen.sizeNote(FULL, "");
        if (CollGen.selected("sort-int64"))
            for (String v : new String[] {"random", "ascending", "descending", "dups"}) {
                long[] src = new long[n];
                for (int i = 0; i < n; i++)
                    src[i] = switch (v) { case "random" -> CollGen.key(i); case "ascending" -> i; case "descending" -> n - 1 - i; default -> i % 16; };
                CollGen.rowV("sort-int64", "Arrays.sort(long[])", "jdk", n, CollGen.benchWith(src::clone, w -> { Arrays.sort(w); return w; },
                    w -> w.length == n && sorted(w)), note, v);
            }
        if (CollGen.selected("sort-f64")) {
            CollGen g = new CollGen();
            double[] src = new double[n];
            for (int i = 0; i < n; i++) src[i] = g.next() / 2147483648.0;
            CollGen.row("sort-f64", "Arrays.sort(double[])", "jdk", n, CollGen.benchWith(src::clone, w -> { Arrays.sort(w); return w; },
                w -> w.length == n && sorted(w)), note);
        }
        if (CollGen.selected("sort-stable-int64")) {
            CollGen g = new CollGen();
            Pair[] src = new Pair[n];
            for (int i = 0; i < n; i++) src[i] = new Pair(g.next() % 100000, i);
            Comparator<Pair> byValue = Comparator.comparingInt(Pair::value);
            CollGen.row("sort-stable-int64", "Arrays.sort(Object[])", "jdk", n, CollGen.benchWith(src::clone, w -> { Arrays.sort(w, byValue); return w; },
                w -> {
                    if (w.length != n) return false;
                    for (int i = 1; i < n; i++) {
                        Pair a = w[i - 1], b = w[i];
                        if (b.value() < a.value() || (b.value() == a.value() && b.index() < a.index())) return false;
                    }
                    return true;
                }), CollGen.sizeNote(FULL, "Pair records (value index) with a comparator on the value"));
        }
    }
}
