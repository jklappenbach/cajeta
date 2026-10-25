// Go time competitors (benchmark-reference unit 8): every time/ id the Cajeta
// side runs in bench/src/main/cajeta/bench/time/, same sizes, inputs and check
// constants, with the standard library's time package. Go has no
// time-of-day or civil date-time type: localtime-arith and
// localdatetime-arith use time.Time in UTC (no DST) and read the fields back.
// A library error returns error and becomes a `failed` row.
package main

import (
	"errors"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strings"
	"time"
)

const (
	n               = 10000000
	nZone           = 1000000
	nFmt            = 1000000
	instantRef      = int64(22090666995000000)
	localdateRef    = int64(504057395700)
	durationRef     = int64(159999997999999)
	localtimeRef    = int64(432435641193)
	localdatetimeRf = int64(20264337423627200)
	zoneRef         = int64(-25266031177)
	firstISO        = "2024-01-01T00:00:00.000000001Z"
	isoLayout       = "2006-01-02T15:04:05.000000000Z07:00"
)

var sinkI int64
var sinkT time.Time

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
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "VmHWM:") {
			var kb int64
			fmt.Sscanf(l, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

type cfg struct {
	runID, ts, ver string
	warmup, trials int
}

func clean(s string) string { return strings.NewReplacer(",", ";", "\n", " ", "\"", "'").Replace(s) }

func row(c cfg, bench string, size int64, lib, notes string, s []int64, alloc uint64, status string, ok bool) {
	mn, med, mean, p95 := int64(-1), int64(-1), int64(-1), int64(-1)
	if len(s) > 0 {
		sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
		k := len(s)
		var sum int64
		for _, v := range s {
			sum += v
		}
		mn, med, mean, p95 = s[0], s[k/2], sum/int64(k), s[min(k-1, k*95/100)]
	}
	mops := 0.0
	if med > 0 {
		mops = float64(size) / float64(med) * 1e3
	}
	fmt.Printf("1,%s,%s,%s,time,,%d,,%d,go,%s,%s,%s,GOAMD64=v3,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%t,%s,\n",
		c.runID, c.ts, bench, size, size, c.ver, lib, runtime.Version(), c.warmup, c.trials,
		mn, med, mean, p95, mops, peakRSSKb(), alloc, status, ok, clean(notes))
}

func measure(c cfg, bench string, size int64, lib, notes string, f func() (int64, error), check func(int64) bool) {
	run := func() ([]int64, bool, uint64, error) {
		for w := 0; w < c.warmup; w++ {
			if _, err := f(); err != nil {
				return nil, false, 0, err
			}
		}
		s := make([]int64, 0, c.trials)
		ok := true
		for t := 0; t < c.trials; t++ {
			t0 := time.Now()
			r, err := f()
			d := time.Since(t0).Nanoseconds()
			if err != nil {
				return nil, false, 0, err
			}
			sinkI = r
			ok = ok && check(r)
			s = append(s, d)
		}
		var m0, m1 runtime.MemStats
		runtime.ReadMemStats(&m0)
		if _, err := f(); err != nil {
			return nil, false, 0, err
		}
		runtime.ReadMemStats(&m1)
		return s, ok, m1.TotalAlloc - m0.TotalAlloc, nil
	}
	s, ok, a, err := run()
	if err != nil {
		row(c, bench, size, lib, err.Error(), nil, 0, "failed", false)
		return
	}
	st := "ok"
	if !ok {
		st = "invalid"
	}
	row(c, bench, size, lib, notes, s, a, st, ok)
}

func fmtInstant(i int64) time.Time {
	return time.Unix(1704067200+i*997, ((i*1000003)%100000000)*10+1).UTC()
}

func clockNow() (int64, error) {
	first := time.Now()
	prev := first
	back := 0
	for i := 1; i < n; i++ {
		t := time.Now()
		if t.Before(prev) {
			back++
		}
		prev = t
	}
	sinkT = prev
	if back == 0 && prev.After(first) {
		return 1, nil
	}
	return 0, nil
}

func instantArith() (int64, error) {
	half := 500 * time.Millisecond
	var acc, cnt int64
	for i := int64(0); i < n; i++ {
		t0 := time.Unix(1704067200+i, 0)
		t2 := t0.Add(time.Duration(((i*7919)%1000000)*1000)).Add(-half)
		acc += t2.Unix() + int64(t2.Nanosecond())
		if t2.Before(t0) {
			cnt++
		}
	}
	return acc + cnt, nil
}

// AddDate normalizes (Jan 31 + 1 month = Mar 2 or 3); the row clamps to the
// month's last day, as every other library on the page does.
func localdateArith() (int64, error) {
	var acc int64
	for i := int64(0); i < n; i++ {
		a := time.Unix((i%100000)*86400, 0).UTC().AddDate(0, 0, 37)
		y, m, d := a.Date()
		mi := int(m) - 1 + int(i%25)
		ny, nm := y+mi/12, time.Month(mi%12+1)
		if last := time.Date(ny, nm+1, 0, 0, 0, 0, 0, time.UTC).Day(); d > last {
			d = last
		}
		e := time.Date(ny, nm, d, 0, 0, 0, 0, time.UTC)
		acc += e.Unix()/86400 + int64((int(e.Weekday())+6)%7+1)
	}
	return acc, nil
}

func durationArith() (int64, error) {
	three, limit := 3*time.Millisecond, 10*time.Millisecond
	var acc, cnt int64
	for i := int64(0); i < n; i++ {
		d := (time.Duration(i) + three) * 2
		acc += d.Nanoseconds()
		if d > limit {
			cnt++
		}
	}
	return acc + cnt, nil
}

func localtimeArith() (int64, error) {
	midnight := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	step := 18007 * time.Second
	var acc, cnt int64
	for i := int64(0); i < n; i++ {
		t := midnight.Add(time.Duration(i%86400) * time.Second).Add(step)
		h, m, s := t.Clock()
		sod := int64(h*3600 + m*60 + s)
		acc += sod
		if h < 12 {
			cnt++
		}
	}
	return acc + cnt, nil
}

func localdatetimeArith() (int64, error) {
	base := time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC)
	var acc int64
	for i := int64(0); i < n; i++ {
		x := base.Add(time.Duration(i%50000) * time.Hour)
		y, mo, d := x.Date()
		acc += int64(y)*1000000 + int64(mo)*10000 + int64(d)*100 + int64(x.Hour())
	}
	return acc, nil
}

func zonedRegion() (int64, error) {
	la, err := time.LoadLocation("America/Los_Angeles")
	if err != nil {
		return 0, err
	}
	var acc int64
	for i := int64(0); i < nZone; i++ {
		z := time.Unix(1577836800+i*307, 0).In(la)
		_, off := z.Zone()
		acc += int64(off) + int64(z.Hour())*100 + int64(z.Day())
	}
	return acc, nil
}

func formatISO() (int64, error) {
	var l int64
	for i := int64(0); i < nFmt; i++ {
		s := fmtInstant(i).Format(isoLayout)
		if i == 0 && s != firstISO {
			return 0, errors.New("instant 0 formats as " + s)
		}
		l += int64(len(s))
	}
	return l, nil
}

func parseISO(inp []string) (int64, error) {
	var good int64
	for i, s := range inp {
		t, err := time.Parse(time.RFC3339Nano, s)
		if err != nil {
			return 0, err
		}
		if t.Equal(fmtInstant(int64(i))) {
			good++
		}
	}
	return good, nil
}

func main() {
	c := cfg{env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""), 3, 10}
	fmt.Sscanf(env("PROFILE_WARMUP", "3"), "%d", &c.warmup)
	fmt.Sscanf(env("PROFILE_TRIALS", "10"), "%d", &c.trials)
	eq := func(want int64) func(int64) bool { return func(r int64) bool { return r == want } }
	measure(c, "clock-now", n, "time", "time.Now (monotonic reading)", clockNow, eq(1))
	measure(c, "time-instant-arith", n, "time", "time.Time Add", instantArith, eq(instantRef))
	measure(c, "time-localdate-arith", n, "time", "no civil date type: UTC time.Time AddDate with an explicit month clamp", localdateArith, eq(localdateRef))
	measure(c, "duration-arith", n, "time", "time.Duration", durationArith, eq(durationRef))
	measure(c, "localtime-arith", n, "time", "no time-of-day type: time.Time in UTC and Clock()", localtimeArith, eq(localtimeRef))
	measure(c, "localdatetime-arith", n, "time", "no civil date-time type: time.Time in UTC", localdatetimeArith, eq(localdatetimeRf))
	measure(c, "zoneddatetime-region", nZone, "time", "Time.In(LoadLocation)", zonedRegion, eq(zoneRef))
	measure(c, "datetime-format-iso", nFmt, "time", "Time.Format with 9 fixed fraction digits", formatISO, eq(nFmt*30))
	inp := make([]string, nFmt)
	for i := range inp {
		inp[i] = fmtInstant(int64(i)).Format(isoLayout)
	}
	measure(c, "datetime-parse-iso", nFmt, "time", "time.Parse(RFC3339Nano)", func() (int64, error) { return parseISO(inp) }, eq(nFmt))
}
