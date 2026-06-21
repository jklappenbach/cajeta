// Go sort competitors — sort 50K int64 across the same four input patterns the
// Cajeta side uses (random / ascending / descending / dups), plus a 50K float64
// random sort. One schema-conformant CSV row per (benchmark, variant, library).
// Same splitmix64 input sequence as every other language. Cross-check: output
// non-decreasing and (for int64) wrapping sum preserved. Libraries: slices.Sort
// (the stdlib generic pattern-defeating sort) and slices.SortStableFunc.
package main

import (
	"cmp"
	"fmt"
	"os"
	"runtime"
	"slices"
	"sort"
	"time"
)

const N = 50000

const seed = uint64(0x123456789ABCDEF0)

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

type split struct{ s uint64 }

func (g *split) next() uint64 {
	g.s += 0x9E3779B97F4A7C15
	z := g.s
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

func makeI64(variant string) []int64 {
	g := split{seed}
	v := make([]int64, N)
	for i := 0; i < N; i++ {
		switch variant {
		case "random":
			v[i] = int64(g.next())
		case "ascending":
			v[i] = int64(i)
		case "descending":
			v[i] = int64(N - 1 - i)
		default: // dups
			v[i] = int64(i % 16)
		}
	}
	return v
}

func makeF64() []float64 {
	g := split{seed}
	v := make([]float64, N)
	for i := 0; i < N; i++ {
		v[i] = float64(g.next()%100000000) / 7.0
	}
	return v
}

func wsum(v []int64) uint64 {
	var a uint64
	for _, x := range v {
		a += uint64(x)
	}
	return a
}

func emit(runID, ts, bench, variant, lib string, warmup, trials int, samples []int64, ok bool) {
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
	mels := 0.0
	if med > 0 {
		mels = float64(N) / float64(med) * 1e9 / 1e6
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,sort,,%d,,%d,go,%s,%s,stdlib,GOAMD64=v4,%d,%d,%d,%d,%d,%d,%.2f,Melem/s,%d,-1,%d,-1,-1,%s,%t,,%s\n",
		runID, ts, bench, N, N, env("PROFILE_LANG_VERSION", ""), lib,
		warmup, trials, mn, med, mean, p95, mels, peakRSSKb(), gAlloc, status, ok, variant)
}

func benchI64(input []int64, warmup, trials int, sorter func([]int64)) ([]int64, bool) {
	want := wsum(input)
	for i := 0; i < warmup; i++ {
		w := slices.Clone(input)
		sorter(w)
	}
	samples := make([]int64, 0, trials)
	ok := true
	for i := 0; i < trials; i++ {
		w := slices.Clone(input)
		t0 := time.Now()
		sorter(w)
		samples = append(samples, time.Since(t0).Nanoseconds())
		ok = slices.IsSorted(w) && wsum(w) == want
	}
	measAlloc(func() { sorter(slices.Clone(input)) })
	return samples, ok
}

func benchF64(input []float64, warmup, trials int, sorter func([]float64)) ([]int64, bool) {
	for i := 0; i < warmup; i++ {
		w := slices.Clone(input)
		sorter(w)
	}
	samples := make([]int64, 0, trials)
	ok := true
	for i := 0; i < trials; i++ {
		w := slices.Clone(input)
		t0 := time.Now()
		sorter(w)
		samples = append(samples, time.Since(t0).Nanoseconds())
		ok = slices.IsSorted(w)
	}
	measAlloc(func() { sorter(slices.Clone(input)) })
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
	_ = runtime.Version

	for _, variant := range []string{"random", "ascending", "descending", "dups"} {
		input := makeI64(variant)
		s, ok := benchI64(input, warmup, trials, func(w []int64) { slices.Sort(w) })
		emit(runID, ts, "sort-int64", variant, "slices.Sort", warmup, trials, s, ok)
	}
	// stable (random only)
	{
		input := makeI64("random")
		s, ok := benchI64(input, warmup, trials, func(w []int64) {
			slices.SortStableFunc(w, func(a, b int64) int { return cmp.Compare(a, b) })
		})
		emit(runID, ts, "sort-stable-int64", "", "slices.SortStableFunc", warmup, trials, s, ok)
	}
	// f64 random
	{
		input := makeF64()
		s, ok := benchF64(input, warmup, trials, func(w []float64) { slices.Sort(w) })
		emit(runID, ts, "sort-f64", "", "slices.Sort", warmup, trials, s, ok)
	}
}
