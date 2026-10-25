// Go competitor: unit-5 lang/dispatch benchmarks (contract: u5-competitor-spec.md).
// Receivers and func values live in package-level vars so the compiler
// cannot devirtualize the calls.
package main

//go:noinline
func direct(x int64) int64 { return x + 1 }

func inc(x int64) int64 { return x + 1 }

// call-virtual: no inheritance; an interface with two implementations.
type VBase interface{ F(x int64) int64 }
type VA struct{}
type VB struct{}

func (VA) F(x int64) int64 { return x + 1 }
func (VB) F(x int64) int64 { return x + 2 }

// call-interface: the same shape through a second interface.
type IFace interface{ F(x int64) int64 }
type IA struct{ pad int64 }
type IB struct{ pad int64 }

func (*IA) F(x int64) int64 { return x + 1 }
func (*IB) F(x int64) int64 { return x + 2 }

func combine[T ~int32 | ~int64](a, b T) T { return a + b }

// instanceof: Base <- Mid <- Leaf by embedding.
type Base struct{ tag int }
type Mid struct{ Base }
type Leaf struct{ Mid }

var (
	vobjs [2]VBase
	iobjs [2]IFace
	gref  func(int64) int64
	kobjs [3]any
)

func main() {
	initCtx()
	const N int64 = 100000000
	vobjs = [2]VBase{VA{}, VB{}}
	iobjs = [2]IFace{&IA{}, &IB{}}
	gref = inc
	kobjs = [3]any{&Base{}, &Mid{}, &Leaf{}}

	run("call-direct", N, "go:noinline function", "stdlib", "", "", func() int64 {
		m := mask
		var s int64
		for i := int64(0); i < N; i++ {
			s += direct(i & m)
		}
		return s
	}, func(s int64) bool { return s == 5000000050000000 })

	run("call-virtual", N, "interface", "stdlib", "no inheritance: interface", "", func() int64 {
		var s int64
		for i := int64(0); i < N; i++ {
			s += vobjs[i&1].F(i)
		}
		return s
	}, func(s int64) bool { return s == 5000000100000000 })

	run("call-interface", N, "interface", "stdlib", "pointer receivers", "", func() int64 {
		var s int64
		for i := int64(0); i < N; i++ {
			s += iobjs[i&1].F(i)
		}
		return s
	}, func(s int64) bool { return s == 5000000100000000 })

	run("call-template", N, "generic [T ~int32|~int64]", "stdlib", "GC-shape stenciled; inlined", "", func() int64 {
		var s int64
		for i := int64(0); i < N; i++ {
			s += int64(combine[int32](int32(i&1023), 1))
		}
		return s
	}, func(s int64) bool { return s == 51249901696 })

	run("call-lambda", N, "func literal", "stdlib", "closure captures opaque k; held in a local func variable", "", func() int64 {
		k := one
		f := func(x int64) int64 { return x + k }
		var s int64
		for i := int64(0); i < N; i++ {
			s += f(i)
		}
		return s
	}, func(s int64) bool { return s == 5000000050000000 })

	run("call-method-ref", N, "func value", "stdlib", "func value held in a package var: an indirect call", "", func() int64 {
		g := gref
		var s int64
		for i := int64(0); i < N; i++ {
			s += g(i)
		}
		return s
	}, func(s int64) bool { return s == 5000000050000000 })

	run("instanceof", N, "type switch", "stdlib",
		"no inheritance: type switch case *Mid; *Leaf over embedded structs", "", func() int64 {
			var count int64
			for i := int64(0); i < N; i++ {
				switch kobjs[i%3].(type) {
				case *Mid, *Leaf:
					count++
				}
			}
			return count
		}, func(c int64) bool { return c == 66666666 })
}
