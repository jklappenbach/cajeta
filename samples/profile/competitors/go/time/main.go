// Go time competitors — the same two workloads the Cajeta side runs:
// time-instant-arith (1M) and time-localdate-arith (100K), using the stdlib time
// package. One schema-conformant CSV row per benchmark. Cross-check = the exact
// closed-form sum (500006500000 / 5000050000). All math in UTC (no DST), so days
// are exactly 86400 s.
package main

import (
	"runtime"
	"fmt"
	"os"
	"sort"
	"time"
)

const (
	nInstant   = 1000000
	nDate      = 100000
	instantRef = int64(500006500000)
	dateRef    = int64(5000050000)
	secPerDay  = int64(86400)
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

func emit(runID, ts, bench string, input, warmup, trials int, samples []int64, ok bool) {
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
		"1,%s,%s,%s,time,,%d,,%d,go,%s,time,stdlib,-gcflags,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%t,,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""),
		warmup, trials, mn, med, mean, p95, mops, peakRSSKb(), gAlloc, status, ok)
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
	measAlloc(func() { f() })
	return samples, ok
}

var gAlloc uint64

func measAlloc(run func()) {
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	run()
	runtime.ReadMemStats(&b)
	gAlloc = b.TotalAlloc - a.TotalAlloc
}

func main() {
	runID := env("PROFILE_RUN_ID", "local")
	ts := env("PROFILE_RUN_TS", "")
	warmup := atoiDefault(env("PROFILE_WARMUP", "3"), 3)
	trials := atoiDefault(env("PROFILE_TRIALS", "10"), 10)

	// instant-arith: time.Unix(i,0).Add(7s).Unix()
	s, ok := benchI64(warmup, trials, func() int64 {
		var sum int64
		for i := int64(0); i < nInstant; i++ {
			t := time.Unix(i, 0).UTC()
			t2 := t.Add(7 * time.Second)
			sum += t2.Unix()
		}
		return sum
	}, func(r int64) bool { return r == instantRef })
	emit(runID, ts, "time-instant-arith", nInstant, warmup, trials, s, ok)

	// localdate-arith: epoch day i → +1 day → epoch day (UTC, exact 86400s/day)
	s, ok = benchI64(warmup, trials, func() int64 {
		var sum int64
		for i := int64(0); i < nDate; i++ {
			t := time.Unix(i*secPerDay, 0).UTC()
			t2 := t.AddDate(0, 0, 1)
			sum += t2.Unix() / secPerDay
		}
		return sum
	}, func(r int64) bool { return r == dateRef })
	emit(runID, ts, "time-localdate-arith", nDate, warmup, trials, s, ok)
}
