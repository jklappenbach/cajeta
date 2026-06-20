// Go string competitors — the same four workloads the Cajeta side runs over a
// ~360 KB ASCII text (base "the quick brown fox jumps over the lazy dog " × 8192):
// string-search (absent needle), string-replace (fox→feline), string-uppercase,
// string-build-concat (build 4000 chars). One schema-conformant CSV row per
// (benchmark, library). Stdlib strings idioms (Index / ReplaceAll / ToUpper /
// strings.Builder).
package main

import (
	"runtime"
	"fmt"
	"os"
	"sort"
	"strings"
	"time"
)

const (
	base       = "the quick brown fox jumps over the lazy dog "
	repeat     = 8192
	piece      = "abcdefgh"
	buildIters = 500
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

func emit(runID, ts, bench, lib string, bytes, warmup, trials int, samples []int64, ok bool) {
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
	mbps := 0.0
	if med > 0 {
		mbps = float64(bytes) / float64(med) * 1e9 / 1048576.0
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,string,,%d,,%d,go,%s,%s,stdlib,GOAMD64=v4,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%t,,\n",
		runID, ts, bench, bytes, bytes, env("PROFILE_LANG_VERSION", ""), lib,
		warmup, trials, mn, med, mean, p95, mbps, peakRSSKb(), gAlloc, status, ok)
}

func benchInt(warmup, trials int, f func() int, check func(int) bool) ([]int64, bool) {
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

func benchStr(warmup, trials int, f func() string, check func(string) bool) ([]int64, bool) {
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

	text := strings.Repeat(base, repeat)
	nbytes := len(text)
	needle := "zzzzqx-not-present"

	// search — absent ⇒ -1
	s, ok := benchInt(warmup, trials, func() int { return strings.Index(text, needle) },
		func(r int) bool { return r == -1 })
	emit(runID, ts, "string-search", "strings.Index", nbytes, warmup, trials, s, ok)

	// replace — output grows
	s2, ok2 := benchStr(warmup, trials, func() string { return strings.ReplaceAll(text, "fox", "feline") },
		func(r string) bool { return len(r) > nbytes })
	emit(runID, ts, "string-replace", "strings.ReplaceAll", nbytes, warmup, trials, s2, ok2)

	// uppercase — same length (ASCII)
	s3, ok3 := benchStr(warmup, trials, func() string { return strings.ToUpper(text) },
		func(r string) bool { return len(r) == nbytes })
	emit(runID, ts, "string-uppercase", "strings.ToUpper", nbytes, warmup, trials, s3, ok3)

	// build-concat — strings.Builder, 4000 chars
	s4, ok4 := benchStr(warmup, trials, func() string {
		var b strings.Builder
		for i := 0; i < buildIters; i++ {
			b.WriteString(piece)
		}
		return b.String()
	}, func(r string) bool { return len(r) == buildIters*8 })
	emit(runID, ts, "string-build-concat", "strings.Builder", buildIters*8, warmup, trials, s4, ok4)
}
