// search/distance-* competitors in Go (bench/search/distance/Distance.md):
// agnivade/levenshtein for Levenshtein, xrash/smetrics for Jaro-Winkler (the
// page names agnivade, which has no Jaro-Winkler). Inputs from the shared
// description (bench/scripts/search_ref.py, LCG31 seed 12345); constants are
// search_ref.py's. The workload returns error (none can occur here) rather
// than panicking. v1 rows, area search.
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/agnivade/levenshtein"
	"github.com/xrash/smetrics"
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

func measure(warm, trials int, fn func() error) (stats, error) {
	for i := 0; i < warm; i++ {
		if err := fn(); err != nil {
			return stats{}, err
		}
	}
	if trials < 1 {
		trials = 1
	}
	s := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t := time.Now()
		if err := fn(); err != nil {
			return stats{}, err
		}
		s = append(s, time.Since(t).Nanoseconds())
	}
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
	return stats{s[0], s[n/2], sum / int64(n), s[p]}, nil
}

func clean(s string) string { return strings.ReplaceAll(s, ",", ";") }

func row(slug string, input int, lib, ver string, w, t int, st stats, ok bool, notes string) {
	status := "invalid"
	if ok {
		status = "ok"
	}
	thr := ""
	if st.med > 0 {
		thr = fmt.Sprintf("%.1f", float64(input)/float64(st.med)*1e9)
	}
	lv := env("PROFILE_LANG_VERSION", runtime.Version())
	fmt.Printf("1,%s,%s,%s,search,,0,,%d,go,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,pairs/s,%d,-1,-1,-1,-1,%s,%t,%s,\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, clean(lv), lib, ver,
		"GOAMD64="+env("GOAMD64", "v3")+" go build", w, t, st.min, st.med, st.mean, st.p95, thr, peakRSSKb(),
		status, ok, clean(notes))
}

func errRow(slug string, input int, lib, ver string, err error) {
	row(slug, input, lib, ver, 0, 0, stats{-1, -1, -1, -1}, false, "error: "+err.Error())
}

type lcg struct{ x int64 }

func (g *lcg) rnd(m int64) int64 {
	g.x = (g.x*1103515245 + 12345) & 0x7fffffff
	return ((g.x >> 15) * m) >> 16
}

func randStr(g *lcg, n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte('a' + g.rnd(8))
	}
	return string(b)
}

var sinkI int64
var sinkF float64

const levVer = "v1.2.2"
const smVer = "v0.0.0-20261013231157-39c47d93efba"

func main() {
	smoke := env("CAJETA_BENCH_SMOKE", "") == "1"
	w, t := envInt("PROFILE_WARMUP", 1), envInt("PROFILE_TRIALS", 5)
	m := 1000000
	sn := ""
	if smoke {
		m = 100000
		sn = fmt.Sprintf("; smoke size (%d pairs in place of 1e6)", m)
	}
	g := &lcg{12345}
	a, b := make([]string, m), make([]string, m)
	for i := 0; i < m; i++ {
		a[i] = randStr(g, 16)
		b[i] = randStr(g, 16)
	}

	var sum int64
	st, err := measure(w, t, func() error {
		var s int64
		for i := 0; i < m; i++ {
			s += int64(levenshtein.ComputeDistance(a[i], b[i]))
		}
		sum = s
		sinkI += s
		return nil
	})
	want := int64(12760743)
	if smoke {
		want = 1276356
	}
	if err != nil {
		errRow("distance-levenshtein", m, "agnivade/levenshtein", levVer, err)
	} else {
		row("distance-levenshtein", m, "agnivade/levenshtein", levVer, w, t, st, sum == want,
			fmt.Sprintf("levenshtein.ComputeDistance per pair; sum %d == %d; errors returned%s", sum, want, sn))
	}

	var fs float64
	st, err = measure(w, t, func() error {
		s := 0.0
		for i := 0; i < m; i++ {
			s += smetrics.JaroWinkler(a[i], b[i], 0.7, 4)
		}
		fs = s
		sinkF += s
		return nil
	})
	fwant := 613325.3143160543
	if smoke {
		fwant = 61342.531185943684
	}
	if err != nil {
		errRow("distance-jaro-winkler", m, "xrash/smetrics", smVer, err)
	} else {
		row("distance-jaro-winkler", m, "xrash/smetrics", smVer, w, t, st, math.Abs(fs-fwant) <= 1e-6*float64(m),
			fmt.Sprintf("smetrics.JaroWinkler(a; b; 0.7; 4) per pair (agnivade/levenshtein has no Jaro-Winkler); sum %.6f within 1e-6*M of %v; errors returned%s", fs, fwant, sn))
	}
}
