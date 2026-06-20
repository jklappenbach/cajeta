// Go collection competitors — the same four workloads the Cajeta side runs:
// hashmap-int / hashmap-string (Ankerl insert+find shape), hashset-dedup,
// arraylist-append. One schema-conformant CSV row per (benchmark, library).
// Cross-checks are the exact closed forms. Go's builtin map / slice (the
// idiomatic containers).
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"time"
)

const (
	nInt   = 50000
	nStr   = 30000
	nSet   = 100000
	setHalf = 50000
	nVec   = 100000
	intRef = int64(2499950000)
	strRef = int64(449985000)
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
		"1,%s,%s,%s,collection,,%d,,%d,go,%s,%s,stdlib,-gcflags,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,-1,-1,-1,%s,%t,,\n",
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

	// hashmap-int
	s, ok := benchI64(warmup, trials, func() int64 {
		m := make(map[int64]int64, nInt)
		for i := int64(0); i < nInt; i++ {
			m[i] = i * 2
		}
		var sum int64
		for j := int64(0); j < nInt; j++ {
			sum += m[j]
		}
		return sum
	}, func(r int64) bool { return r == intRef })
	emit(runID, ts, "hashmap-int", "map", nInt, warmup, trials, s, ok)

	// hashmap-string
	s, ok = benchI64(warmup, trials, func() int64 {
		m := make(map[string]int64, nStr)
		for i := int64(0); i < nStr; i++ {
			m["key"+strconv.FormatInt(i, 10)] = i
		}
		var sum int64
		for j := int64(0); j < nStr; j++ {
			sum += m["key"+strconv.FormatInt(j, 10)]
		}
		return sum
	}, func(r int64) bool { return r == strRef })
	emit(runID, ts, "hashmap-string", "map", nStr, warmup, trials, s, ok)

	// hashset-dedup
	s, ok = benchI64(warmup, trials, func() int64 {
		set := make(map[int64]struct{})
		for i := int64(0); i < nSet; i++ {
			set[i%setHalf] = struct{}{}
		}
		return int64(len(set))
	}, func(r int64) bool { return r == setHalf })
	emit(runID, ts, "hashset-dedup", "map-set", nSet, warmup, trials, s, ok)

	// arraylist-append
	s, ok = benchI64(warmup, trials, func() int64 {
		var v []int64
		for i := int64(0); i < nVec; i++ {
			v = append(v, i)
		}
		return int64(len(v))
	}, func(r int64) bool { return r == nVec })
	emit(runID, ts, "arraylist-append", "slice", nVec, warmup, trials, s, ok)
}
