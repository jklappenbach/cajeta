// Go competitor: unit-5 lang/alloc benchmarks (contract: u5-competitor-spec.md).
// Every heap object is stored to a package-level sink so escape analysis
// cannot stack-allocate it; the GC reclaims it (Go has no explicit free).
package main

type Obj struct{ a, b, c, d int64 }

var (
	sinkObj *Obj
	sinkArr []int32
	ring1   [1]*Obj
	ring10  [10]*Obj
)

// pass: ten non-inlined calls deep; the last reads .a. Plain reference passing.
//
//go:noinline
func pass(o *Obj, depth int) int64 {
	if depth == 0 {
		return o.a
	}
	return pass(o, depth-1)
}

func churn(ring []*Obj, n int64) int64 {
	r := int64(len(ring))
	var s int64
	for i := int64(0); i < n; i++ {
		ring[i%r] = &Obj{i, i, i, i} // previous occupant (oldest) dropped for the GC
		s += ring[i%r].a
	}
	var ringsum int64
	for _, p := range ring {
		if p != nil {
			ringsum += p.a
		}
	}
	warmSink += int(ringsum & 1)
	for k := range ring {
		ring[k] = nil
	}
	return s
}

func main() {
	initCtx()
	const N int64 = 10000000

	run("alloc-small-object", N, "&Obj{}", "stdlib", "escapes via a package sink; GC", "", func() int64 {
		var s int64
		for i := int64(0); i < N; i++ {
			o := &Obj{i, i, i, i}
			sinkObj = o
			s += o.a
		}
		sinkObj = nil
		return s
	}, func(s int64) bool { return s == 49999995000000 })

	run("alloc-churn-1", N, "pointer ring", "stdlib", "package-level ring; plain reference replaced; GC", "",
		func() int64 { return churn(ring1[:], N) }, func(s int64) bool { return s == 49999995000000 })
	run("alloc-churn-10", N, "pointer ring", "stdlib", "package-level ring; plain reference replaced; GC", "",
		func() int64 { return churn(ring10[:], N) }, func(s int64) bool { return s == 49999995000000 })

	run("alloc-transfer-chain", 1000000, "pointer pass", "stdlib",
		"no ownership transfer: a reference is passed; escapes via a package sink; GC", "", func() int64 {
			var s int64
			for i := int64(0); i < 1000000; i++ {
				o := &Obj{i, i, i, i}
				sinkObj = o
				s += pass(o, 10)
			}
			sinkObj = nil
			return s
		}, func(s int64) bool { return s == 499999500000 })

	emitSkipped("alloc-live-set-on", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "")
	emitSkipped("alloc-live-set-off", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "")

	run("alloc-array", 100000, "make([]int32 1024)", "stdlib", "zeroed; escapes via a package sink; GC", "", func() int64 {
		var s int64
		for i := int64(0); i < 100000; i++ {
			a := make([]int32, 1024)
			sinkArr = a
			a[0] = int32(i)
			a[1023] = 1
			s += int64(a[0]) + int64(a[1023])
		}
		sinkArr = nil
		return s
	}, func(s int64) bool { return s == 5000050000 })

	run("alloc-struct-value", 100000000, "struct", "stdlib", "stack value", "", func() int64 {
		type P struct{ x, y int64 }
		m := mask
		var s int64
		for i := int64(0); i < 100000000; i++ {
			p := P{i, i & m}
			s += p.x + p.y
		}
		return s
	}, func(s int64) bool { return s == 9999999900000000 })
}
