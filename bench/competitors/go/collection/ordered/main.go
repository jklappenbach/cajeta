// Go collection/ competitors, ordered half: redblacktree-insert-lookup (the
// Cajeta shape), redblacktree-range, bplustree-insert, bplustree-point-lookup,
// bplustree-range, same sizes, inputs and checks as the Cajeta benchmarks. Go
// has no ordered map in the standard library; github.com/google/btree v1.1.3
// (BTreeG, degree 32) stands in, as RedBlackTree.md names. BPlusTree.md names
// no Go competitor; the same B-tree is measured there for completeness.
package main

import (
	"sort"

	"github.com/google/btree"

	. "collgen"
)

const n = 1_000_000

type kv struct{ k, v int64 }

func less(a, b kv) bool { return a.k < b.k }

func main() {
	if Selected("redblacktree-insert-lookup") {
		const m = 20000
		Row("redblacktree-insert-lookup", "google/btree", "v1.1.3", m, Bench(func() int64 {
			t := btree.NewG(32, less)
			for i := int64(0); i < m; i++ {
				t.ReplaceOrInsert(kv{i, i * 2})
			}
			var s int64
			for j := int64(0); j < m; j++ {
				e, _ := t.Get(kv{k: j})
				s += e.v
			}
			return s
		}, func(s int64) bool { return s == m*(m-1) }), "BTreeG degree 32")
	}
	keys := make([]int64, n)
	for i := range keys {
		keys[i] = Key(int64(i))
	}
	if Selected("bplustree-insert") {
		Row("bplustree-insert", "google/btree", "v1.1.3", n, Bench(func() [2]int64 {
			t := btree.NewG(32, less)
			for i, k := range keys {
				t.ReplaceOrInsert(kv{k, int64(i)})
			}
			e, _ := t.Get(kv{k: keys[7]})
			return [2]int64{int64(t.Len()), e.v}
		}, func(r [2]int64) bool { return r == [2]int64{n, 7} }), "BTreeG degree 32")
	}
	tree := btree.NewG(32, less)
	for i, k := range keys {
		tree.ReplaceOrInsert(kv{k, int64(i)})
	}
	if Selected("bplustree-point-lookup") {
		idx := NewGen().Indices(10_000_000, n)
		var exp int64
		q := make([]int64, len(idx))
		for j, i := range idx {
			exp += int64(i)
			q[j] = keys[i]
		}
		Row("bplustree-point-lookup", "google/btree", "v1.1.3", 10_000_000, Bench(func() int64 {
			var s int64
			for _, k := range q {
				e, _ := tree.Get(kv{k: k})
				s += e.v
			}
			return s
		}, func(s int64) bool { return s == exp }), "BTreeG degree 32")
	}
	// Ranges: 1e4 scans of 1e3 entries from the first key >= key(draw % 1e6).
	starts := []int64{}
	for _, i := range NewGen().Indices(10_000, n) {
		starts = append(starts, Key(int64(i)))
	}
	srt := make([]kv, n)
	for i, k := range keys {
		srt[i] = kv{k, int64(i)}
	}
	sort.Slice(srt, func(i, j int) bool { return srt[i].k < srt[j].k })
	var rangeExp int64
	for _, s := range starts {
		p := sort.Search(n, func(i int) bool { return srt[i].k >= s })
		for j := p; j < p+1000 && j < n; j++ {
			rangeExp += srt[j].v
		}
	}
	srt = nil
	for _, id := range []string{"redblacktree-range", "bplustree-range"} {
		if Selected(id) {
			Row(id, "google/btree", "v1.1.3", 10_000_000, Bench(func() int64 {
				var s int64
				for _, st := range starts {
					c := 0
					tree.AscendGreaterOrEqual(kv{k: st}, func(e kv) bool {
						s += e.v
						c++
						return c < 1000
					})
				}
				return s
			}, func(s int64) bool { return s == rangeExp }), "AscendGreaterOrEqual for 1000 entries")
		}
	}
}
