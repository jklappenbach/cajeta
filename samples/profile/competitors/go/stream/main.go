// Go stream competitors — the same two workloads the Cajeta side runs over
// xs[i] = i % 1000 (N=1M): stream-filter-map-reduce (filter evens → map +1 → sum,
// a sequential loop) and stream-parallel-reduce (sum fanned out across goroutines,
// one chunk per CPU). One schema-conformant CSV row per benchmark. Cross-check =
// the exact reference sums (250000000 / 499500000).
package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"sync"
	"time"
)

const (
	N      = 1000000
	fmrRef = int64(250000000)
	prRef  = int64(499500000)
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
	start := 0
	for i := 0; i < len(b); i++ {
		if b[i] == '\n' {
			line := string(b[start:i])
			if len(line) > 6 && line[:6] == "VmHWM:" {
				var kb int64
				fmt.Sscanf(line, "VmHWM: %d", &kb)
				return kb
			}
			start = i + 1
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

func emit(runID, ts, bench, lib string, input, warmup, trials int, samples []int64, ok bool) {
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
		"1,%s,%s,%s,stream,,%d,,%d,go,%s,%s,stdlib,-gcflags,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,-1,-1,-1,%s,%t,,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""), lib,
		warmup, trials, mn, med, mean, p95, mops, peakRSSKb(), status, ok)
}

func benchI64(warmup, trials int, f func() int64, check func(int64) bool) ([]int64, bool) {
	for i := 0; i < warmup; i++ {
		_ = f()
	}
	samples := make([]int64, 0, trials)
	ok := true
	for i := 0; i < trials; i++ {
		t0 := time.Now()
		r := f()
		samples = append(samples, time.Since(t0).Nanoseconds())
		ok = check(r)
	}
	return samples, ok
}

func main() {
	runID := env("PROFILE_RUN_ID", "local")
	ts := env("PROFILE_RUN_TS", "")
	warmup := atoiDefault(env("PROFILE_WARMUP", "3"), 3)
	trials := atoiDefault(env("PROFILE_TRIALS", "10"), 10)
	xs := make([]int64, N)
	for i := 0; i < N; i++ {
		xs[i] = int64(i % 1000)
	}

	// filter-map-reduce (sequential)
	s, ok := benchI64(warmup, trials, func() int64 {
		var sum int64
		for _, x := range xs {
			if x%2 == 0 {
				sum += x + 1
			}
		}
		return sum
	}, func(r int64) bool { return r == fmrRef })
	emit(runID, ts, "stream-filter-map-reduce", "hand-loop", N, warmup, trials, s, ok)

	// parallel-reduce (goroutine fan-out, one chunk per CPU)
	workers := runtime.NumCPU()
	s, ok = benchI64(warmup, trials, func() int64 {
		chunk := (N + workers - 1) / workers
		partials := make([]int64, workers)
		var wg sync.WaitGroup
		for w := 0; w < workers; w++ {
			lo := w * chunk
			hi := lo + chunk
			if hi > N {
				hi = N
			}
			if lo >= hi {
				continue
			}
			wg.Add(1)
			go func(w, lo, hi int) {
				defer wg.Done()
				var local int64
				for i := lo; i < hi; i++ {
					local += xs[i]
				}
				partials[w] = local
			}(w, lo, hi)
		}
		wg.Wait()
		var sum int64
		for _, p := range partials {
			sum += p
		}
		return sum
	}, func(r int64) bool { return r == prRef })
	emit(runID, ts, "stream-parallel-reduce", "goroutines", N, warmup, trials, s, ok)
}
