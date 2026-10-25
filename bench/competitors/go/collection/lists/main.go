// Go collection/ competitors, sequence half: the ArrayList, LinkedList, Heap,
// ImmutableList, Collectors and binary-search ids, same sizes, inputs and
// checks as the Cajeta benchmarks (bench/src/main/cajeta/bench/collection/).
// Stdlib only: slices, container/list, container/heap, slices.Clone, an append
// loop, a map of slices, sort.Search.
package main

import (
	"container/heap"
	"container/list"
	"slices"
	"sort"

	. "collgen"
)

const n1m = 1_000_000

type minHeap []int32

func (h minHeap) Len() int           { return len(h) }
func (h minHeap) Less(i, j int) bool { return h[i] < h[j] }
func (h minHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *minHeap) Push(x any)        { *h = append(*h, x.(int32)) }
func (h *minHeap) Pop() any {
	o := *h
	n := len(o)
	x := o[n-1]
	*h = o[:n-1]
	return x
}

func sorted(a []int32) bool {
	for i := 1; i < len(a); i++ {
		if a[i] < a[i-1] {
			return false
		}
	}
	return true
}

type pair struct{ n, tail int }

func main() {
	lst1m := make([]int32, n1m)
	for i := range lst1m {
		lst1m[i] = int32(i)
	}
	idx := NewGen().Indices(n1m, n1m)
	var exp int64
	for _, x := range idx {
		exp += int64(x)
	}
	exp *= 100
	getLoop := func(l []int32) func() int64 {
		return func() int64 {
			var s int64
			for p := 0; p < 100; p++ {
				for _, j := range idx {
					s += int64(l[j])
				}
			}
			return s
		}
	}
	if Selected("arraylist-get") {
		Row("arraylist-get", "slice", "stdlib", 100_000_000, Bench(getLoop(lst1m), func(s int64) bool { return s == exp }), "indexed (bounds-checked)")
	}
	lst1e7 := make([]int32, 10_000_000)
	for i := range lst1e7 {
		lst1e7[i] = int32(i)
	}
	if Selected("arraylist-iterate") {
		Row("arraylist-iterate", "slice", "stdlib", 100_000_000, Bench(func() int64 {
			var s int64
			for p := 0; p < 10; p++ {
				for _, v := range lst1e7 {
					s += int64(v)
				}
			}
			return s
		}, func(s int64) bool { return s == 10*(10_000_000*9_999_999/2) }), "for range")
	}
	if Selected("arraylist-sort") {
		g := NewGen()
		src := make([]int32, n1m)
		for i := range src {
			src[i] = g.Next()
		}
		Row("arraylist-sort", "slices.Sort", "stdlib", n1m, Bench(func() []int32 {
			a := slices.Clone(src)
			slices.Sort(a)
			return a
		}, func(a []int32) bool { return len(a) == n1m && sorted(a) }), "includes the copy")
	}
	if Selected("linkedlist-insert-traverse") {
		const n = 40000
		Row("linkedlist-insert-traverse", "container/list", "stdlib", n, Bench(func() [2]int {
			l := list.New()
			for i := 0; i < n; i++ {
				l.PushBack(int32(i))
			}
			found := 0
			for e := l.Front(); e != nil; e = e.Next() {
				if e.Value.(int32) == n-1 {
					found = 1
					break
				}
			}
			return [2]int{l.Len(), found}
		}, func(r [2]int) bool { return r == [2]int{n, 1} }), "")
	}
	if Selected("heap-sort") {
		const n = 50000
		Row("heap-sort", "container/heap", "stdlib", n, Bench(func() []int32 {
			h := &minHeap{}
			x := int64(12345)
			for i := 0; i < n; i++ {
				x = (x*1103515245 + 12345) & 0x7FFFFFFF
				heap.Push(h, int32(x))
			}
			out := make([]int32, 0, n)
			for h.Len() > 0 {
				out = append(out, heap.Pop(h).(int32))
			}
			return out
		}, func(a []int32) bool { return len(a) == n && sorted(a) }), "")
	}
	if Selected("heap-push-pop") {
		g := NewGen()
		vals := make([]int32, 10_001_000)
		for i := range vals {
			vals[i] = g.Next()
		}
		Row("heap-push-pop", "container/heap", "stdlib", 10_000_000, Bench(func() int64 {
			h := minHeap(slices.Clone(vals[:1000]))
			heap.Init(&h)
			var c int64
			for _, v := range vals[1000:] {
				heap.Push(&h, v)
				c += int64(heap.Pop(&h).(int32))
			}
			return c
		}, func(c int64) bool { return c == 10738394301026345 }), "heap.Push then heap.Pop")
	}
	if Selected("immutablelist-build") {
		Row("immutablelist-build", "slices.Clone", "stdlib", 100_000_000, Bench(func() pair {
			t, tail := 0, 0
			for r := 0; r < 100; r++ {
				c := slices.Clone(lst1m)
				t += len(c)
				tail = int(c[999_999])
			}
			return pair{t, tail}
		}, func(p pair) bool { return p == pair{100_000_000, 999_999} }), "Go has no immutable slice: the copy is the snapshot")
	}
	if Selected("immutablelist-get") {
		frozen := slices.Clone(lst1m)
		Row("immutablelist-get", "slices.Clone", "stdlib", 100_000_000, Bench(getLoop(frozen), func(s int64) bool { return s == exp }), "a copied slice")
	}
	if Selected("collectors-to-list") {
		Row("collectors-to-list", "append loop", "stdlib", 10_000_000, Bench(func() pair {
			var out []int32
			for _, v := range lst1e7 {
				out = append(out, v)
			}
			return pair{len(out), int(out[len(out)-1])}
		}, func(p pair) bool { return p == pair{10_000_000, 9_999_999} }), "")
	}
	if Selected("collectors-group-by") {
		Row("collectors-group-by", "map of slices", "stdlib", 10_000_000, Bench(func() pair {
			g := map[int32][]int32{}
			for _, v := range lst1e7 {
				g[v%1000] = append(g[v%1000], v)
			}
			return pair{len(g), len(g[0])}
		}, func(p pair) bool { return p == pair{1000, 10000} }), "")
	}
	if Selected("binary-search") {
		const n = 50000
		Row("binary-search", "sort.Search", "stdlib", n, Bench(func() int64 {
			a := make([]int64, n)
			for i := range a {
				a[i] = int64(i * 2)
			}
			var hits int64
			x := int64(12345)
			for m := 0; m < n; m++ {
				x = (x*1103515245 + 12345) & 0x7FFFFFFF
				k := (x % n) * 2
				i := sort.Search(n, func(i int) bool { return a[i] >= k })
				if i < n && a[i] == k {
					hits++
				}
			}
			return hits
		}, func(h int64) bool { return h == n }), "")
	}
}
