// Go competitor: unit-5 lang/values benchmarks (contract: u5-competitor-spec.md).
package main

// optional: Go has no optional type; a (value, ok) pair with map and unwrap helpers.
type optI32 struct {
	v  int32
	ok bool
}

func someOrNone(v int32, present bool) optI32 { return optI32{v, present} }

func (o optI32) mapf(f func(int32) int32) optI32 {
	if o.ok {
		return optI32{f(o.v), true}
	}
	return o
}

func (o optI32) unwrapOr(d int32) int32 {
	if o.ok {
		return o.v
	}
	return d
}

type pair struct {
	first  int32
	second int64
}

func main() {
	initCtx()
	const N int64 = 100000000

	run("optional-map-unwrap", N, "(value ok) pair", "stdlib", "no optional type; struct {v; ok} with map and unwrapOr helpers", "", func() int64 {
		m := mask
		var s int64
		for i := int64(0); i < N; i++ {
			o := someOrNone(int32(i&m), i&1 == 0)
			s += int64(o.mapf(func(x int32) int32 { return x * 2 }).unwrapOr(0))
		}
		return s
	}, func(s int64) bool { return s == 4999999900000000 })

	run("pair-construct", N, "struct", "stdlib", "no pair type: struct {int32; int64}", "", func() int64 {
		m := mask
		var s int64
		for i := int64(0); i < N; i++ {
			p := pair{int32(i & 1023), i & m}
			s += int64(p.first) + p.second
		}
		return s
	}, func(s int64) bool { return s == 5000051099901696 })

	if selected("slice-window") {
		a := make([]int32, 1000000)
		for k := range a {
			a[k] = int32(k)
		}
		run("slice-window", 10000000, "subslice a[s:s+64]", "stdlib", "", "", func() int64 {
			var s int64
			for i := int64(0); i < 10000000; i++ {
				start := int(i % 999936)
				w := a[start : start+64]
				s += int64(w[0])
			}
			return s
		}, func(s int64) bool { return s == 4999355225280 })
	}

	if selected("slice-iterate") || selected("arraystream-iterate") {
		a := make([]int32, 10000000)
		for k := range a {
			a[k] = int32(k % 1000)
		}
		run("slice-iterate", 100000000, "slice for range", "stdlib", "", "", func() int64 {
			w := a[:]
			var s int64
			for p := 0; p < 10; p++ {
				for _, x := range w {
					s += int64(x)
				}
			}
			return s
		}, func(s int64) bool { return s == 49950000000 })
		run("arraystream-iterate", 100000000, "for range", "stdlib", "no stream type: for range over the slice", "", func() int64 {
			var s int64
			for p := 0; p < 10; p++ {
				for _, x := range a {
					s += int64(x)
				}
			}
			return s
		}, func(s int64) bool { return s == 49950000000 })
	}
}
