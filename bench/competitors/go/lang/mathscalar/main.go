// Go competitor: unit-5 lang/mathscalar benchmarks (contract: u5-competitor-spec.md).
package main

import "math"

func main() {
	initCtx()
	const N int64 = 100000000

	if selected("math-transcendental") {
		x := make([]float64, 10000000)
		for k := range x {
			x[k] = float64(k%1000) * 0.001
		}
		run("math-transcendental", 10000000, "math", "stdlib", "math.Sin/Exp/Sqrt; sequential sum", "", func() float64 {
			s := 0.0
			for k := 0; k < len(x); k++ {
				s += math.Sin(x[k]) + math.Exp(x[k]) + math.Sqrt(x[k])
			}
			return s
		}, func(s float64) bool { return math.Abs(s-28428598.85459962) <= 1e-9*28428598.85459962 })
	}

	run("math-intdiv", N, "operator / %", "stdlib", "", "", func() int64 {
		var s int64
		for i := int64(0); i < N; i++ {
			d := (i & 7) + 1
			s += i/d + i%d
		}
		return s
	}, func(s int64) bool { return s == 1698660847083334 })

	run("math-intrinsics", N, "math.Abs/Min/Max/FMA", "stdlib", "", "", func() float64 {
		s := 0.0
		for i := int64(0); i < N; i++ {
			a := float64((i & 1023) - 512)
			s += math.Abs(a) + math.Min(a, 100.0) + math.Max(a, -100.0) + math.FMA(a, 0.5, 1.0)
		}
		return s
	}, func(s float64) bool { return s == 25615094240.0 })
}
