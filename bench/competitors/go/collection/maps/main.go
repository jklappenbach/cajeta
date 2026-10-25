// Go collection/ competitors, hash half: hashmap-lookup-hit/-miss/-iterate,
// hashset-contains and the ImmutableMap/ImmutableSet ids, same sizes, inputs
// and checks as the Cajeta benchmarks. The builtin map (map[int64]struct{} for
// the set); Go has no frozen map, so the freeze is maps.Clone.
package main

import (
	"maps"

	. "collgen"
)

const (
	n = 1_000_000
	q = 10_000_000
)

type pair struct {
	t int
	p int64
}

func main() {
	keys := make([]int64, n)
	m := make(map[int64]int64)
	st := make(map[int64]struct{})
	for i := range keys {
		keys[i] = Key(int64(i))
		m[keys[i]] = int64(i)
		st[keys[i]] = struct{}{}
	}
	hitIdx := NewGen().Indices(q, n)
	var hitExp int64
	hitQ := make([]int64, q)
	missQ := make([]int64, q)
	for j, i := range hitIdx {
		hitExp += int64(i)
		hitQ[j] = keys[i]
		missQ[j] = Key(int64(n) + int64(i))
	}
	halfIdx := NewGen().Indices(q, 2*n)
	var halfExp int64
	halfQ := make([]int64, q)
	for j, i := range halfIdx {
		if i < n {
			halfExp++
		}
		halfQ[j] = Key(int64(i))
	}
	hitIdx, halfIdx = nil, nil
	k7 := Key(7)
	lookup := func(mm map[int64]int64) func() int64 {
		return func() int64 {
			var s int64
			for _, k := range hitQ {
				s += mm[k]
			}
			return s
		}
	}
	contains := func(ss map[int64]struct{}) func() int64 {
		return func() int64 {
			var c int64
			for _, k := range halfQ {
				if _, ok := ss[k]; ok {
					c++
				}
			}
			return c
		}
	}
	if Selected("hashmap-lookup-hit") {
		Row("hashmap-lookup-hit", "map", "stdlib", q, Bench(lookup(m), func(s int64) bool { return s == hitExp }), "")
	}
	if Selected("hashmap-lookup-miss") {
		Row("hashmap-lookup-miss", "map", "stdlib", q, Bench(func() int64 {
			var c int64
			for _, k := range missQ {
				if _, ok := m[k]; !ok {
					c++
				}
			}
			return c
		}, func(c int64) bool { return c == q }), "comma-ok lookup")
	}
	if Selected("hashmap-iterate") {
		Row("hashmap-iterate", "map", "stdlib", q, Bench(func() int64 {
			var s int64
			for p := 0; p < 10; p++ {
				for _, v := range m {
					s += v
				}
			}
			return s
		}, func(s int64) bool { return s == 10*(int64(n)*(n-1)/2) }), "for range")
	}
	if Selected("hashset-contains") {
		Row("hashset-contains", "map[int64]struct{}", "stdlib", q, Bench(contains(st), func(c int64) bool { return c == halfExp }), "")
	}
	if Selected("immutablemap-build") {
		Row("immutablemap-build", "maps.Clone", "stdlib", 10*n, Bench(func() pair {
			r := pair{}
			for i := 0; i < 10; i++ {
				f := maps.Clone(m)
				r.t += len(f)
				r.p = f[k7]
			}
			return r
		}, func(r pair) bool { return r == pair{10 * n, 7} }), "Go has no frozen map: the copy is the freeze")
	}
	if Selected("immutablemap-lookup") {
		f := maps.Clone(m)
		Row("immutablemap-lookup", "maps.Clone", "stdlib", q, Bench(lookup(f), func(s int64) bool { return s == hitExp }), "a cloned map")
	}
	if Selected("immutableset-build") {
		Row("immutableset-build", "maps.Clone", "stdlib", 10*n, Bench(func() pair {
			r := pair{}
			for i := 0; i < 10; i++ {
				f := maps.Clone(st)
				r.t += len(f)
				if _, ok := f[k7]; ok {
					r.p = 1
				}
			}
			return r
		}, func(r pair) bool { return r == pair{10 * n, 1} }), "map[int64]struct{} cloned")
	}
	if Selected("immutableset-contains") {
		f := maps.Clone(st)
		Row("immutableset-contains", "maps.Clone", "stdlib", q, Bench(contains(f), func(c int64) bool { return c == halfExp }), "a cloned set map")
	}
}
