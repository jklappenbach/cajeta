// Go collection/ sort competitors: sort-int64 (random, ascending, descending,
// dups), sort-f64, sort-stable-int64 at the page shape (1e6; n/100 under
// CAJETA_BENCH_SMOKE=1), same inputs and checks as the Cajeta benchmarks
// (CollGen): random = key(i), f64 = draw / 2^31, stable = (value, index)
// pairs packed value << 20 | index, value = draw % 1e5, sorted on the value.
// slices.Sort (pdqsort), slices.SortStableFunc. The input copy is outside the time.
package main

import (
	"slices"

	. "collgen"
)

const full = 1_000_000

func sorted[T int64 | float64](a []T) bool {
	for i := 1; i < len(a); i++ {
		if a[i] < a[i-1] {
			return false
		}
	}
	return true
}

func main() {
	n := Sized(full)
	note := SizeNote(full, "")
	if Selected("sort-int64") {
		for _, v := range []string{"random", "ascending", "descending", "dups"} {
			src := make([]int64, n)
			for i := range src {
				switch v {
				case "random":
					src[i] = Key(int64(i))
				case "ascending":
					src[i] = int64(i)
				case "descending":
					src[i] = int64(n - 1 - i)
				default:
					src[i] = int64(i % 16)
				}
			}
			RowV("sort-int64", "slices.Sort", "stdlib", int64(n), BenchWith(func() []int64 { return slices.Clone(src) },
				func(w []int64) []int64 { slices.Sort(w); return w },
				func(w []int64) bool { return len(w) == n && sorted(w) }), note, v)
		}
	}
	if Selected("sort-f64") {
		g := NewGen()
		src := make([]float64, n)
		for i := range src {
			src[i] = float64(g.Next()) / 2147483648.0
		}
		Row("sort-f64", "slices.Sort", "stdlib", int64(n), BenchWith(func() []float64 { return slices.Clone(src) },
			func(w []float64) []float64 { slices.Sort(w); return w },
			func(w []float64) bool { return len(w) == n && sorted(w) }), note)
	}
	if Selected("sort-stable-int64") {
		g := NewGen()
		src := make([]int64, n)
		for i := range src {
			src[i] = int64(g.Next()%100000)<<20 | int64(i)
		}
		Row("sort-stable-int64", "slices.SortStableFunc", "stdlib", int64(n), BenchWith(func() []int64 { return slices.Clone(src) },
			func(w []int64) []int64 {
				slices.SortStableFunc(w, func(a, b int64) int { return int(a>>20) - int(b>>20) })
				return w
			},
			func(w []int64) bool { return len(w) == n && sorted(w) }), SizeNote(full, "(value index) pairs packed in an int64; comparator on the value"))
	}
}
