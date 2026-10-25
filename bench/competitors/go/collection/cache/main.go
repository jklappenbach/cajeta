// Go collection/ competitors for the Cache ids: github.com/dgraph-io/ristretto
// v2.1.0 (the newest release that builds with go 1.22), same sizes, inputs and
// checks as the Cajeta benchmarks. Config: NumCounters 10x capacity, MaxCost =
// capacity at cost 1 per entry (IgnoreInternalCost), BufferItems 64. Ristretto buffers its writes
// and runs TinyLFU admission, so a Set may be dropped or rejected: the fill
// for cache-hit repeats until every key is resident, each size check counts
// the resident keys after Wait() (untimed) and accepts 0 < size <= capacity,
// and it exposes no exact length (Metrics would add per-op counting).
package main

import (
	"time"

	"github.com/dgraph-io/ristretto/v2"

	. "collgen"
)

const (
	c = 100_000
	q = 10_000_000
)

func newCache(capacity int64) *ristretto.Cache[int64, int64] {
	cc, err := ristretto.NewCache(&ristretto.Config[int64, int64]{NumCounters: 10 * capacity, MaxCost: capacity, BufferItems: 64, IgnoreInternalCost: true})
	if err != nil {
		panic(err)
	}
	return cc
}

type res struct {
	misses int64
	cc     *ristretto.Cache[int64, int64]
}

func main() {
	lib, ver := "ristretto", "v2.1.0"
	keys := make([]int64, q)
	for i := range keys {
		keys[i] = Key(int64(i))
	}
	resident := func(cc *ristretto.Cache[int64, int64]) int64 {
		cc.Wait()
		var n int64
		for _, k := range keys {
			if _, ok := cc.Get(k); ok {
				n++
			}
		}
		return n
	}
	if Selected("cache-hit") {
		hot := newCache(c)
		for round := 0; round < 100; round++ {
			missing := 0
			for i := int64(0); i < c; i++ {
				if _, ok := hot.Get(keys[i]); !ok {
					hot.Set(keys[i], i, 1)
					missing++
				}
			}
			hot.Wait()
			if missing == 0 {
				break
			}
		}
		hitQ := make([]int64, q)
		for j, i := range NewGen().Indices(q, c) {
			hitQ[j] = keys[i]
		}
		Row("cache-hit", lib, ver, q, Bench(func() int64 {
			var h int64
			for _, k := range hitQ {
				if _, ok := hot.Get(k); ok {
					h++
				}
			}
			return h
		}, func(h int64) bool { return h == q }), "filled until all 1e5 keys are resident")
		hot.Close()
	}
	if Selected("cache-miss") {
		Row("cache-miss", lib, ver, q, Bench(func() res {
			cc := newCache(c)
			var m int64
			for i, k := range keys {
				if _, ok := cc.Get(k); !ok {
					m++
					cc.Set(k, int64(i), 1)
				}
			}
			return res{m, cc}
		}, func(r res) bool { n := resident(r.cc); r.cc.Close(); return r.misses == q && n > 0 && n <= c }),
			"Sets are buffered and TinyLFU-admitted: size check 0 < resident <= capacity")
	}
	if Selected("cache-evict") {
		Row("cache-evict", lib, ver, q, Bench(func() res {
			cc := newCache(c)
			for i, k := range keys {
				cc.Set(k, int64(i), 1)
			}
			return res{0, cc}
		}, func(r res) bool { n := resident(r.cc); r.cc.Close(); return n > 0 && n <= c }),
			"Sets are buffered and TinyLFU-admitted: size check 0 < resident <= capacity")
	}
	if Selected("cache-ttl") {
		Row("cache-ttl", lib, ver, 1_000_000, Bench(func() int64 {
			cc := newCache(1_000_000)
			defer cc.Close()
			for i, k := range keys[:1_000_000] {
				cc.SetWithTTL(k, int64(i), 1, time.Millisecond)
			}
			cc.Wait()
			time.Sleep(2 * time.Millisecond)
			var e int64
			for _, k := range keys[:1_000_000] {
				if _, ok := cc.Get(k); !ok {
					e++
				}
			}
			return e
		}, func(e int64) bool { return e == 1_000_000 }), "SetWithTTL 1 ms; Wait then the 2 ms sleep inside the time")
	}
}
