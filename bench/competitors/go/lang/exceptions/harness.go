// ---- common harness (identical in every unit-5 go lang program) ----
// One v1-shape CSV row (32 columns) per (benchmark, variant, library). Each
// workload returns its result; the check is applied to every timed trial's
// result (and warmup results feed a package sink), so nothing can be elided.
// OPAQUE values are package-level vars assigned in main.
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

const flags = "GOAMD64=v3"

var (
	mask     int64 = 0 // OPAQUE: set to -1 in initOpaque
	one      int64 = 0 // OPAQUE: set to 1 in initOpaque
	warmSink int
)

func initOpaque() {
	z := int64(len(os.Args) - len(os.Args))
	mask = z - 1
	one = z + 1
}

func env(k, d string) string {
	if v, ok := os.LookupEnv(k); ok && v != "" {
		return v
	}
	return d
}

func atoiDefault(s string, d int) int {
	v, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil {
		return d
	}
	return v
}

func peakRSSKb() int64 {
	b, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return -1
	}
	for _, line := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(line, "VmHWM:") {
			f := strings.Fields(line)
			if len(f) >= 2 {
				if kb, err := strconv.ParseInt(f[1], 10, 64); err == nil {
					return kb
				}
			}
		}
	}
	return -1
}

type ctxT struct {
	runID, ts, langVersion string
	warmup, trials         int
	only                   []string
}

var ctx ctxT

func initCtx() {
	ctx.runID = env("PROFILE_RUN_ID", "local")
	ctx.ts = env("PROFILE_RUN_TS", "")
	ctx.langVersion = env("PROFILE_LANG_VERSION", "")
	ctx.warmup = atoiDefault(env("PROFILE_WARMUP", "3"), 3)
	ctx.trials = atoiDefault(env("PROFILE_TRIALS", "10"), 10)
	if ctx.trials < 1 {
		ctx.trials = 1
	}
	if ctx.warmup < 0 {
		ctx.warmup = 0
	}
	for _, t := range strings.Split(env("LANG_ONLY", ""), ",") {
		if t = strings.TrimSpace(t); t != "" {
			ctx.only = append(ctx.only, t)
		}
	}
	initOpaque()
}

func selected(slug string) bool {
	if len(ctx.only) == 0 {
		return true
	}
	for _, o := range ctx.only {
		if o == slug {
			return true
		}
	}
	return false
}

func field(s string) string { return strings.ReplaceAll(s, ",", ";") }

func emit(slug string, input int64, lib, libver string, s []int64, ok bool, notes, variant string) {
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	n := len(s)
	var sum int64
	for _, x := range s {
		sum += x
	}
	idx := n * 95 / 100
	if idx > n-1 {
		idx = n - 1
	}
	mn, med, mean, p95 := s[0], s[n/2], sum/int64(n), s[idx]
	mops := 0.0
	if med > 0 {
		mops = float64(input) / float64(med) * 1e9 / 1e6
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf("1,%s,%s,%s,lang,,%d,,%d,go,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,-1,-1,-1,%s,%t,%s,%s,cpu\n",
		ctx.runID, ctx.ts, slug, input, input, field(ctx.langVersion), field(lib), field(libver), flags,
		ctx.warmup, ctx.trials, mn, med, mean, p95, mops, peakRSSKb(), status, ok, field(notes), field(variant))
}

func emitSkipped(slug string, input int64, lib, libver, reason, variant string) {
	if !selected(slug) {
		return
	}
	fmt.Printf("1,%s,%s,%s,lang,,%d,,%d,go,%s,%s,%s,%s,%d,%d,-1,-1,-1,-1,,Mop/s,%d,-1,-1,-1,-1,skipped,,%s,%s,cpu\n",
		ctx.runID, ctx.ts, slug, input, input, field(ctx.langVersion), field(lib), field(libver), flags,
		ctx.warmup, ctx.trials, peakRSSKb(), field(reason), field(variant))
}

// run: warmup untimed runs, then timed trials; check every trial's result.
func run[T any](slug string, input int64, lib, libver, notes, variant string, f func() T, check func(T) bool) {
	if !selected(slug) {
		return
	}
	for i := 0; i < ctx.warmup; i++ {
		if check(f()) {
			warmSink++
		}
	}
	s := make([]int64, 0, ctx.trials)
	ok := true
	for i := 0; i < ctx.trials; i++ {
		t0 := time.Now()
		r := f()
		s = append(s, time.Since(t0).Nanoseconds())
		if !check(r) {
			ok = false
		}
	}
	emit(slug, input, lib, libver, s, ok, notes, variant)
}

// ---- end common harness ----
