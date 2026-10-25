// Go competitor: unit-5 lang/exceptions benchmarks (contract: u5-competitor-spec.md).
// Go has no exceptions: every "throw" allocates a *BenchError and returns it
// as an error through the same call depth; the caller checks and counts.
package main

const noteErr = "Go has no exceptions: error returned"

type BenchError struct{ code int64 }

func (e *BenchError) Error() string { return "bench error" }

//go:noinline
func fMaybeThrow(i, m int64) (int64, error) {
	if i < 0 {
		return 0, &BenchError{i}
	}
	return i & m, nil
}

// depth frames of non-inlined recursion; the innermost "throws" when i >= 0 (always).
//
//go:noinline
func thrower(depth int, i int64) (int64, error) {
	if depth <= 1 {
		if i >= 0 {
			return 0, &BenchError{i}
		}
		return i, nil
	}
	r, err := thrower(depth-1, i)
	if err != nil {
		return 0, err
	}
	return r + 1, nil
}

func throwCatch(depth int, iters int64) int64 {
	var count int64
	for i := int64(0); i < iters; i++ {
		r, err := thrower(depth, i)
		if err != nil {
			count++
		} else {
			count -= r // never reached
		}
	}
	return count
}

func main() {
	initCtx()
	const N int64 = 100000000
	const lib = "error return"

	run("try-no-throw", N, lib, "stdlib", noteErr+"; nil error checked per call", "", func() int64 {
		m := mask
		var s int64
		for i := int64(0); i < N; i++ {
			v, err := fMaybeThrow(i, m)
			if err != nil {
				s -= 1
			} else {
				s += v
			}
		}
		return s
	}, func(s int64) bool { return s == 4999999950000000 })

	notes := noteErr + "; a new *BenchError allocated per throw"
	run("throw-catch-1", 1000000, lib, "stdlib", notes, "", func() int64 { return throwCatch(1, 1000000) },
		func(c int64) bool { return c == 1000000 })
	run("throw-catch-10", 1000000, lib, "stdlib", notes, "", func() int64 { return throwCatch(10, 1000000) },
		func(c int64) bool { return c == 1000000 })
	run("throw-catch-100", 100000, lib, "stdlib", notes, "", func() int64 { return throwCatch(100, 100000) },
		func(c int64) bool { return c == 100000 })

	emitSkipped("throw-recoverable", 1000000, lib, "stdlib",
		"no recoverable/unrecoverable distinction; the page's competitor is Java", "")

	run("throw-in-fiber", 100000, "goroutine + error return", "stdlib",
		noteErr+"; one goroutine returns its count over a channel", "", func() int64 {
			ch := make(chan int64)
			go func() { ch <- throwCatch(1, 100000) }()
			return <-ch
		}, func(c int64) bool { return c == 100000 })
}
