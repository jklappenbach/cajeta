// Go competitor: unit-5 lang/loops benchmarks (contract: u5-competitor-spec.md).
package main

import (
	"math"
	"os"
)

const N int64 = 100000000

// nobN: OPAQUE loop bound for loop-counted-nobounds. Bounding the loop by
// len(a) would let the prove pass delete the check in the plain build too.
var nobN int = 0

func main() {
	initCtx()
	nobN = 1000000 + len(os.Args) - len(os.Args)
	boundsOff := os.Getenv("LANG_BOUNDS") == "off"

	run("loop-counted", N, "for", "stdlib", "", "", func() int64 {
		m := mask
		var s int64
		for i := int64(0); i < N; i++ {
			s += i & m
		}
		return s
	}, func(s int64) bool { return s == 4999999950000000 })

	if selected("loop-counted-nobounds") {
		a := make([]int32, 1000000)
		for k := range a {
			a[k] = int32(k)
		}
		body := func() int64 {
			var s int64
			n := nobN
			for p := 0; p < 100; p++ {
				for k := 0; k < n; k++ {
					s += int64(a[k])
				}
			}
			return s
		}
		chk := func(s int64) bool { return s == 49999950000000 }
		if boundsOff {
			run("loop-counted-nobounds", 100*1000000, "slice index", "stdlib",
				"built with -gcflags=-B", "bounds-off", body, chk)
		} else {
			run("loop-counted-nobounds", 100*1000000, "slice index", "stdlib",
				"checked slice index; loop bound is an opaque var so the prove pass keeps the check", "bounds-on", body, chk)
		}
	}

	run("loop-nested", 100000000, "for", "stdlib", "", "", func() int64 {
		var s int64
		for i := int64(0); i < 10000; i++ {
			for j := int64(0); j < 10000; j++ {
				s += i ^ j
			}
		}
		return s
	}, func(s int64) bool { return s == 642122061696 })

	if selected("loop-foreach-array") || selected("loop-foreach-arraylist") {
		a := make([]int32, 10000000)
		for k := range a {
			a[k] = int32(k % 1000)
		}
		run("loop-foreach-array", 10000000, "for range", "stdlib", "", "", func() int64 {
			var s int64
			for _, x := range a {
				s += int64(x)
			}
			return s
		}, func(s int64) bool { return s == 4995000000 })
		var l []int32
		for k := 0; k < 10000000; k++ {
			l = append(l, int32(k%1000))
		}
		run("loop-foreach-arraylist", 10000000, "append-built slice for range", "stdlib",
			"Go has no list type distinct from a slice; built by append", "", func() int64 {
				var s int64
				for _, x := range l {
					s += int64(x)
				}
				return s
			}, func(s int64) bool { return s == 4995000000 })
	}

	run("loop-range", N, "range over int", "stdlib", "Go 1.22 range-over-int", "", func() int64 {
		m := mask
		var s int64
		for i := range N {
			s += i & m
		}
		return s
	}, func(s int64) bool { return s == 4999999950000000 })

	if selected("loop-reduce") {
		x := make([]float64, 10000000)
		for k := range x {
			x[k] = float64(k%1000) * 0.5
		}
		run("loop-reduce", 10000000, "for", "stdlib", "sequential; Go never reassociates float adds", "", func() float64 {
			s := 0.0
			for k := 0; k < len(x); k++ {
				s += x[k]
			}
			return s
		}, func(s float64) bool { return math.Abs(s-2497500000.0) <= 1e-6*2497500000.0 })
	}

	if selected("loop-early-exit") {
		a := make([]int32, 10000000)
		for k := range a {
			a[k] = int32(k)
		}
		a[5000000] = -1
		run("loop-early-exit", 1000*10000000, "for+break", "stdlib", "", "", func() int64 {
			var s int64
			for r := 0; r < 1000; r++ {
				found := -1
				for k := 0; k < len(a); k++ {
					if a[k] < 0 {
						found = k
						break
					}
				}
				s += int64(found)
			}
			return s
		}, func(s int64) bool { return s == 5000000000 })
	}
}
