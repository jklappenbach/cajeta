// Go concurrency competitors — the same two workloads the Cajeta side runs:
// atomic-fetchadd (1M single-threaded atomic.Int64.Add(1) → counter==N) and
// task-spawn-await (20K: spawn a goroutine computing i+1, receive its result,
// accumulate → sum == N*(N+1)/2). One schema-conformant CSV row per benchmark.
// Cross-check = the exact closed forms (1000000 / 200010000). The task primitive
// is a goroutine — the natural lightweight task, contrast with OS threads.
package main

import (
	"fmt"
	"os"
	"sort"
	"sync/atomic"
	"time"
)

const (
	nAtomic  = 1000000
	nSpawn   = 20000
	spawnRef = int64(200010000)
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
		"1,%s,%s,%s,concurrent,,%d,,%d,go,%s,%s,stdlib,-gcflags,%d,%d,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,-1,-1,-1,%s,%t,,\n",
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

	// atomic-fetchadd
	s, ok := benchI64(warmup, trials, func() int64 {
		var a atomic.Int64
		for i := 0; i < nAtomic; i++ {
			a.Add(1)
		}
		return a.Load()
	}, func(r int64) bool { return r == nAtomic })
	emit(runID, ts, "atomic-fetchadd", "atomic.Int64", nAtomic, warmup, trials, s, ok)

	// task-spawn-await (goroutine + channel, spawn then await each)
	s, ok = benchI64(warmup, trials, func() int64 {
		var sum int64
		for i := int64(0); i < nSpawn; i++ {
			ch := make(chan int64, 1)
			go func(i int64) { ch <- i + 1 }(i)
			sum += <-ch
		}
		return sum
	}, func(r int64) bool { return r == spawnRef })
	emit(runID, ts, "task-spawn-await", "goroutine", nSpawn, warmup, trials, s, ok)
}
