// Package collgen holds the collection/ competitors' shared inputs and row
// format (Go side). It mirrors bench/src/main/cajeta/bench/collection/
// CollGen.cajeta exactly: the same draws in the same order give the same keys,
// queries and checks in all six languages.
//
//	next(): s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s >> 33
//	        (xorshift64 on uint64, seed 88172645463325252; a 31-bit draw)
//	key(i): three xorshift64 rounds of i ^ 0x5851F42D4C957F2D, as int64
package collgen

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

func xs(x uint64) uint64 {
	x ^= x << 13
	x ^= x >> 7
	x ^= x << 17
	return x
}

// Key is the bijective pseudo-random int64 key of index i.
func Key(i int64) int64 { return int64(xs(xs(xs(uint64(i) ^ 0x5851F42D4C957F2D)))) }

// Gen is the shared xorshift64 draw stream.
type Gen struct{ s uint64 }

func NewGen() *Gen { return &Gen{s: 88172645463325252} }

func (g *Gen) Next() int32 { g.s = xs(g.s); return int32(g.s >> 33) }

func (g *Gen) Indices(count int, rng int32) []int32 {
	out := make([]int32, count)
	for i := range out {
		out[i] = g.Next() % rng
	}
	return out
}

func Env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func Num(k string, d int) int {
	if v, err := strconv.Atoi(Env(k, "")); err == nil {
		return v
	}
	return d
}

func Params() (int, int) { return Num("PROFILE_WARMUP", 3), Num("PROFILE_TRIALS", 10) }

// Selected reports whether PROFILE_BENCH (a space list) is empty or names id.
func Selected(id string) bool {
	s := strings.Fields(os.Getenv("PROFILE_BENCH"))
	if len(s) == 0 {
		return true
	}
	for _, x := range s {
		if x == id {
			return true
		}
	}
	return false
}

func peakRSSKb() int64 {
	b, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return -1
	}
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "VmHWM:") {
			f := strings.Fields(l)
			if len(f) > 1 {
				v, _ := strconv.ParseInt(f[1], 10, 64)
				return v
			}
		}
	}
	return -1
}

// Measured is one id's trials, its last check and one run's allocated bytes.
type Measured struct {
	Samples []int64
	OK      bool
	Alloc   uint64
}

// Bench warms up, times `trials` calls of f, checks the last result (untimed),
// then measures one more call's allocated bytes (runtime.MemStats.TotalAlloc).
func Bench[T any](f func() T, check func(T) bool) Measured {
	w, t := Params()
	for i := 0; i < w; i++ {
		f()
	}
	m := Measured{OK: true}
	for i := 0; i < t; i++ {
		t0 := time.Now()
		r := f()
		m.Samples = append(m.Samples, time.Since(t0).Nanoseconds())
		m.OK = check(r)
	}
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	f()
	runtime.ReadMemStats(&b)
	m.Alloc = b.TotalAlloc - a.TotalAlloc
	return m
}

func clean(s string) string { return strings.ReplaceAll(s, ",", " ") }

// Row prints one v1 row.
func Row(id, lib, libver string, input int64, m Measured, notes string) {
	w, t := Params()
	s := append([]int64(nil), m.Samples...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	n := len(s)
	var sum int64
	for _, v := range s {
		sum += v
	}
	mn, med, mean, p95 := s[0], s[n/2], sum/int64(n), s[min(n-1, n*95/100)]
	mops := 0.0
	if med > 0 {
		mops = float64(input) / float64(med) * 1e3
	}
	status := "invalid"
	if m.OK {
		status = "ok"
	}
	fmt.Printf("1,%s,%s,%s,collection,,%d,,%d,go,%s,%s,%s,GOAMD64=v4,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%t,%s,\n",
		Env("PROFILE_RUN_ID", "local"), Env("PROFILE_RUN_TS", ""), id, input, input, Env("PROFILE_LANG_VERSION", ""),
		lib, libver, w, t, mn, med, mean, p95, mops, peakRSSKb(), m.Alloc, status, m.OK, clean(notes))
}

// Skip prints one skipped v1 row.
func Skip(id, lib string, input int64, reason string) {
	fmt.Printf("1,%s,%s,%s,collection,,%d,,%d,go,%s,%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,\n",
		Env("PROFILE_RUN_ID", "local"), Env("PROFILE_RUN_TS", ""), id, input, input, Env("PROFILE_LANG_VERSION", ""), lib, clean(reason))
}
