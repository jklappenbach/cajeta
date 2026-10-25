// Row plumbing for the Go wire/ competitor (copied from codec/basen; area wire) (v1 rows, 31 columns;
// competitors/lib/rows.sh lifts them to schema v2). Duplicated per program
// because each program is its own module.
package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"sort"
	"strconv"
	"strings"
	"time"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func envInt(k string, d int) int {
	if v, err := strconv.Atoi(os.Getenv(k)); err == nil && v >= 0 {
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

type stats struct{ min, med, mean, p95 int64 }

func statsOf(v []int64) stats {
	s := append([]int64(nil), v...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	n := len(s)
	var sum int64
	for _, x := range s {
		sum += x
	}
	p := n * 95 / 100
	if p > n-1 {
		p = n - 1
	}
	return stats{s[0], s[n/2], sum / int64(n), s[p]}
}

// measure runs warmup untimed rounds, then trials timed rounds of fn; an
// error from any round aborts. alloc is the bytes one extra untimed run
// allocates (runtime.MemStats TotalAlloc delta).
func measure(warmup, trials int, fn func() error) (stats, int64, error) {
	for i := 0; i < warmup; i++ {
		if err := fn(); err != nil {
			return stats{}, -1, err
		}
	}
	samples := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t := time.Now()
		if err := fn(); err != nil {
			return stats{}, -1, err
		}
		samples = append(samples, time.Since(t).Nanoseconds())
	}
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	if err := fn(); err != nil {
		return stats{}, -1, err
	}
	runtime.ReadMemStats(&b)
	return statsOf(samples), int64(b.TotalAlloc - a.TotalAlloc), nil
}

func goVersion() string {
	if v := os.Getenv("PROFILE_LANG_VERSION"); v != "" {
		return v
	}
	return runtime.Version()
}

func flags() string {
	return "GOAMD64=" + env("GOAMD64", "v3") + " go build"
}

func clean(s string) string { return strings.ReplaceAll(s, ",", ";") }

type row struct {
	slug, dataset, variant, lib, libVer, notes string
	dsBytes, input                             int64
	warmup, trials                             int
	st                                         stats
	alloc                                      int64
	timeOnly                                   bool
}

func emitOK(r row, ok bool) {
	status := "invalid"
	if ok {
		status = "ok"
	}
	thr, unit := "", ""
	if !r.timeOnly && r.st.med > 0 {
		thr = fmt.Sprintf("%.1f", float64(r.input)/float64(r.st.med)*1e9/1048576.0)
		unit = "MB/s"
	}
	fmt.Printf("1,%s,%s,%s,wire,%s,%d,,%d,go,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,%s,%d,-1,%d,-1,-1,%s,%t,%s,%s\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), r.slug, r.dataset, r.dsBytes, r.input,
		clean(goVersion()), r.lib, clean(r.libVer), flags(), r.warmup, r.trials,
		r.st.min, r.st.med, r.st.mean, r.st.p95, thr, unit, peakRSSKb(), r.alloc,
		status, ok, clean(r.notes), r.variant)
}

func emitSkip(r row, reason string) {
	fmt.Printf("1,%s,%s,%s,wire,%s,%d,,%d,go,%s,%s,%s,%s,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,%s\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), r.slug, r.dataset, r.dsBytes, r.input,
		clean(goVersion()), r.lib, clean(r.libVer), flags(), clean(reason), r.variant)
}

// emitFailed records a row whose workload returned an error (errors are
// returned, never panicked): status invalid with the error in notes.
func emitErr(r row, err error) {
	r.notes = r.notes + "; error: " + err.Error()
	r.st = stats{-1, -1, -1, -1}
	emitOK(r, false)
}

var _ = debug.SetGCPercent
