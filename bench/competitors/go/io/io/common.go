// Row plumbing for the Go io competitor (v1 rows, 31 columns, area io;
// competitors/lib/rows.sh lifts them to schema v2) and the shared sizes.
package main

import (
	"fmt"
	"os"
	"runtime"
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

func smoke() bool {
	s := os.Getenv("CAJETA_BENCH_SMOKE")
	return s == "1" || s == "true" || s == "yes"
}

func pick(full, sm int64) int64 {
	if smoke() {
		return sm
	}
	return full
}

func warm(d int) int {
	if smoke() {
		return 1
	}
	return envInt("PROFILE_WARMUP", d)
}

func trials(d int) int {
	if smoke() {
		return envInt("PROFILE_TRIALS", 3)
	}
	return envInt("PROFILE_TRIALS", d)
}

func sm(what string) string {
	if smoke() {
		return " (smoke; the page's " + what + " is the full run)"
	}
	return ""
}

func statusKb(key string) int64 {
	b, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return -1
	}
	for _, line := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(line, key) {
			f := strings.Fields(line)
			if len(f) > 1 {
				v, _ := strconv.ParseInt(f[1], 10, 64)
				return v
			}
		}
	}
	return -1
}

func peakRSSKb() int64 { return statusKb("VmHWM:") }
func rssKb() int64     { return statusKb("VmRSS:") }

// cpuMs is utime + stime of this process in milliseconds (USER_HZ = 100).
func cpuMs() int64 {
	b, err := os.ReadFile("/proc/self/stat")
	if err != nil {
		return -1
	}
	s := string(b)
	i := strings.LastIndexByte(s, ')')
	f := strings.Fields(s[i+1:])
	u, _ := strconv.ParseInt(f[11], 10, 64)
	st, _ := strconv.ParseInt(f[12], 10, 64)
	return (u + st) * 10
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

// measure runs warmup untimed rounds then trials timed rounds of fn; the
// first error aborts and is returned (a Go row returns error, never panics).
func measure(warmup, trials int, fn func() error) (stats, error) {
	for i := 0; i < warmup; i++ {
		if err := fn(); err != nil {
			return stats{}, err
		}
	}
	if trials < 1 {
		trials = 1
	}
	samples := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t := time.Now()
		if err := fn(); err != nil {
			return stats{}, err
		}
		samples = append(samples, time.Since(t).Nanoseconds())
	}
	return statsOf(samples), nil
}

func goVersion() string {
	if v := os.Getenv("PROFILE_LANG_VERSION"); v != "" {
		return v
	}
	return runtime.Version()
}

func flags() string { return "GOAMD64=" + env("GOAMD64", "v3") + " go build" }

func clean(s string) string { return strings.NewReplacer(",", ";", "\n", " ").Replace(s) }

// emit writes one row; input is the throughput numerator (MiB for MB/s).
func emit(slug string, input int64, lib, ver string, w, t int, st stats, ok bool, notes, unit string) {
	status := "invalid"
	if ok {
		status = "ok"
	}
	thr, u := "", ""
	if unit != "" && st.med > 0 && input >= 0 {
		thr = fmt.Sprintf("%.3f", float64(input)*1e9/float64(st.med))
		u = unit
	}
	if smoke() {
		notes += "; smoke"
	}
	fmt.Printf("1,%s,%s,%s,io,,-1,,%d,go,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,%s,%d,-1,-1,-1,-1,%s,%t,%s,\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, clean(goVersion()), lib, clean(ver), flags(),
		w, t, st.min, st.med, st.mean, st.p95, thr, u, peakRSSKb(), status, ok, clean(notes))
}

// emitErr records a row whose workload returned an error.
func emitErr(slug, lib string, err error) {
	fmt.Printf("1,%s,%s,%s,io,,-1,,-1,go,%s,%s,,%s,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,invalid,false,%s,\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, clean(goVersion()), lib, flags(), clean("error: "+err.Error()))
}
