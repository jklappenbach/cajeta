// Go concurrency competitors for bench/concurrent/*.md — every id, the same
// workload, size and check as the Cajeta side. One v1 row per id (lib/rows.sh
// lifts it to v2). The contended rows use goroutines on GOMAXPROCS OS threads
// (Go has no other thread primitive) where Cajeta uses fibers. Under
// CAJETA_BENCH_SMOKE=1 the multi-goroutine rows run 1e4 operations each
// (channels: 1e4 messages), the 1e8 single-goroutine rows 1e6 and the sleep row
// 100 sleeps; those rows say "smoke size" in notes. Every workload returns
// (value, error); an error turns the row's check false. Results are consumed
// (returned and checked).
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func peakRSSKb() int64 {
	b, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return -1
	}
	for _, line := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(line, "VmHWM:") {
			var kb int64
			fmt.Sscanf(line, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

func atoiDefault(s string, d int) int {
	var v int
	if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
		return d
	}
	return v
}

var (
	runID, ts      string
	warmup, trials int
	smoke          bool
)

func smokeNote(what string) string {
	if smoke {
		return "smoke size: " + what
	}
	return ""
}

func join(a, b string) string {
	if b == "" {
		return a
	}
	return a + "; " + b
}

func emit(bench, lib, libver string, input int64, warm int, samples []int64, ok bool, alloc uint64, notes string) {
	sort.Slice(samples, func(i, j int) bool { return samples[i] < samples[j] })
	n := len(samples)
	var sum int64
	for _, x := range samples {
		sum += x
	}
	idx := n * 95 / 100
	if idx >= n {
		idx = n - 1
	}
	mn, med, mean, p95 := samples[0], samples[n/2], sum/int64(n), samples[idx]
	mops := 0.0
	if med > 0 {
		mops = float64(input) / float64(med) * 1e9 / 1e6
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,concurrent,,%d,,%d,go,%s,%s,%s,GOAMD64=v4,%d,%d,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,%d,-1,-1,%s,%t,%s,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib, libver,
		warm, n, mn, med, mean, p95, mops, peakRSSKb(), alloc, status, ok, notes)
}

func skip(bench, lib string, input int64, reason string) {
	fmt.Printf("1,%s,%s,%s,concurrent,,%d,,%d,go,%s,%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib, reason)
}

func bench(id, lib string, input int64, notes string, f func() (int64, error), want int64) {
	for i := 0; i < warmup; i++ {
		_, _ = f()
	}
	samples := make([]int64, 0, trials)
	ok := true
	for i := 0; i < trials; i++ {
		t0 := time.Now()
		r, err := f()
		samples = append(samples, time.Since(t0).Nanoseconds())
		ok = ok && err == nil && r == want
	}
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	_, _ = f()
	runtime.ReadMemStats(&b)
	emit(id, lib, "stdlib", input, warmup, samples, ok, b.TotalAlloc-a.TotalAlloc, notes)
}

func par(t int, body func(k int) (int64, error)) ([]int64, error) {
	var wg sync.WaitGroup
	out := make([]int64, t)
	errs := make([]error, t)
	for k := 0; k < t; k++ {
		wg.Add(1)
		go func(k int) { defer wg.Done(); out[k], errs[k] = body(k) }(k)
	}
	wg.Wait()
	return out, errors.Join(errs...)
}

type flKey struct{}

func main() {
	runID = env("PROFILE_RUN_ID", "local")
	ts = env("PROFILE_RUN_TS", "")
	warmup = atoiDefault(env("PROFILE_WARMUP", "3"), 3)
	trials = atoiDefault(env("PROFILE_TRIALS", "10"), 10)
	smoke = env("CAJETA_BENCH_SMOKE", "") == "1" || env("CAJETA_BENCH_SMOKE", "") == "true"

	n1, per, nch, nsp, nsleep := int64(100000000), int64(1000000), int64(5000000), int64(100000), 10000
	if smoke {
		n1, per, nch, nsp, nsleep = 1000000, 10000, 10000, 10000, 100
	}
	s1, sc := smokeNote("1e6 ops"), smokeNote("1e4 ops per goroutine")

	bench("atomic-fetchadd", "atomic.Int64", n1, join("Add (lock xadd)", s1), func() (int64, error) {
		var a atomic.Int64
		for i := int64(0); i < n1; i++ {
			a.Add(1)
		}
		return a.Load(), nil
	}, n1)
	bench("atomic-fetchadd-32", "atomic.Int32", n1, join("Add (lock xadd)", s1), func() (int64, error) {
		var a atomic.Int32
		for i := int64(0); i < n1; i++ {
			a.Add(1)
		}
		return int64(a.Load()), nil
	}, n1)
	bench("atomic-cas", "atomic.Int64", n1, join("CompareAndSwap; uncontended", s1), func() (int64, error) {
		var a atomic.Int64
		for i := int64(0); i < n1; i++ {
			if !a.CompareAndSwap(i, i+1) {
				return 0, fmt.Errorf("cas failed at %d", i)
			}
		}
		return a.Load(), nil
	}, n1)
	bench("atomic-contended-16", "atomic.Int64+goroutines", 16*per, join("16 goroutines on GOMAXPROCS threads", sc), func() (int64, error) {
		var a atomic.Int64
		_, err := par(16, func(int) (int64, error) {
			for i := int64(0); i < per; i++ {
				a.Add(1)
			}
			return 0, nil
		})
		return a.Load(), err
	}, 16*per)
	bench("mutex-uncontended", "sync.Mutex", n1, join("Lock; v++; Unlock on one goroutine", s1), func() (int64, error) {
		var m sync.Mutex
		var v int64
		for i := int64(0); i < n1; i++ {
			m.Lock()
			v++
			m.Unlock()
		}
		return v, nil
	}, n1)
	for _, t := range []int{4, 16} {
		t := t
		bench(fmt.Sprintf("mutex-contended-%d", t), "sync.Mutex+goroutines", int64(t)*per,
			join(fmt.Sprintf("%d goroutines on GOMAXPROCS threads", t), sc), func() (int64, error) {
				var m sync.Mutex
				var v int64
				_, err := par(t, func(int) (int64, error) {
					for i := int64(0); i < per; i++ {
						m.Lock()
						v++
						m.Unlock()
					}
					return 0, nil
				})
				return v, err
			}, int64(t)*per)
	}
	bench("rwlock-read-heavy", "sync.RWMutex", 16*per, join("16 goroutines 90/10 read/write", sc), func() (int64, error) {
		var m sync.RWMutex
		var v int64
		reads, err := par(16, func(int) (int64, error) {
			var s int64
			for i := int64(0); i < per; i++ {
				if i%10 == 0 {
					m.Lock()
					v++
					m.Unlock()
				} else {
					m.RLock()
					s += v
					m.RUnlock()
				}
			}
			return s, nil
		})
		if len(reads) != 16 {
			return 0, errors.New("missing readers")
		}
		return v, err
	}, 16*(per/10))
	bench("lock-uncontended", "sync.Mutex", n1, join("Lock/Unlock pair per pass on one goroutine", s1), func() (int64, error) {
		var m sync.Mutex
		var c int64
		for i := int64(0); i < n1; i++ {
			m.Lock()
			c++
			m.Unlock()
		}
		return c, nil
	}, n1)
	bench("semaphore-acquire-release", "buffered chan struct{} (cap 4)", 16*per,
		join("16 goroutines through a 4-slot buffered channel", smokeNote("1e4 passes per goroutine")), func() (int64, error) {
			sem := make(chan struct{}, 4)
			var inside, viol atomic.Int64
			passes, err := par(16, func(int) (int64, error) {
				for i := int64(0); i < per; i++ {
					sem <- struct{}{}
					if inside.Add(1) > 4 {
						viol.Add(1)
					}
					inside.Add(-1)
					<-sem
				}
				return per, nil
			})
			if viol.Load() != 0 {
				return 0, errors.New("more than 4 holders")
			}
			var t int64
			for _, p := range passes {
				t += p
			}
			return t, err
		}, 16*per)

	wantCh := nch * (nch + 1) / 2
	for _, sh := range []struct {
		id   string
		p, c int
	}{{"channel-spsc", 1, 1}, {"channel-mpsc", 4, 1}, {"channel-mpmc", 4, 4}} {
		sh := sh
		bench(sh.id, "chan int64", nch, join("make(chan int64 1000); goroutines", smokeNote("1e4 messages")), func() (int64, error) {
			ch := make(chan int64, 1000)
			pp := nch / int64(sh.p)
			var sum, got atomic.Int64
			var cw sync.WaitGroup
			for c := 0; c < sh.c; c++ {
				cw.Add(1)
				go func() {
					defer cw.Done()
					var s, k int64
					for v := range ch {
						s += v
						k++
					}
					sum.Add(s)
					got.Add(k)
				}()
			}
			_, err := par(sh.p, func(p int) (int64, error) {
				for i := int64(0); i < pp; i++ {
					ch <- int64(p)*pp + i + 1
				}
				return 0, nil
			})
			close(ch)
			cw.Wait()
			if got.Load() != nch {
				return 0, fmt.Errorf("received %d", got.Load())
			}
			return sum.Load(), err
		}, wantCh)
	}

	// fiber-sleep-wake: one sample per sleep, the overshoot past 1 ms.
	{
		warm := 10
		samples := make([]int64, 0, nsleep)
		for k := 0; k < warm+nsleep; k++ {
			t0 := time.Now()
			time.Sleep(time.Millisecond)
			over := time.Since(t0) - time.Millisecond
			if over < 0 {
				over = 0
			}
			if k >= warm {
				samples = append(samples, over.Nanoseconds())
			}
		}
		emit("fiber-sleep-wake", "time.Sleep", "stdlib", 1, warm, samples, len(samples) == nsleep, 0,
			join("row = overshoot past 1 ms per sleep over the trials; on the main goroutine", smokeNote("100 sleeps")))
	}

	bench("fiberlocal-get-set", "context.WithValue", n1,
		join("Go has no goroutine-local: set = context.WithValue(background; key; i) and get = ctx.Value(key) per pass", s1), func() (int64, error) {
			base := context.Background()
			var sum int64
			for i := int64(0); i < n1; i++ {
				ctx := context.WithValue(base, flKey{}, i)
				v, ok := ctx.Value(flKey{}).(int64)
				if !ok {
					return 0, errors.New("context value missing")
				}
				sum += v
			}
			return sum, nil
		}, n1*(n1-1)/2)

	bench("task-spawn-await", "goroutine", nsp, join("go + buffered chan per task", smokeNote("1e4 tasks")), func() (int64, error) {
		var sum int64
		for i := int64(0); i < nsp; i++ {
			ch := make(chan int64, 1)
			go func(i int64) { ch <- i + 1 }(i)
			sum += <-ch
		}
		return sum, nil
	}, nsp*(nsp+1)/2)
	_ = skip
}
