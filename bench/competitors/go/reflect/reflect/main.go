// reflect/ competitor in Go (bench/reflect/Class.md): package reflect.
//
//	class-lookup     reflect.TypeOf(v) 1e7 times (smoke 1e6), cycling over values of 4 struct types. Go has no
//	                 lookup of a type by its name at run time (no registry), so the row is the page's
//	                 reflect.TypeOf on a value; check: struct types found == n.
//	field-enumerate  t := reflect.TypeOf(obj); t.NumField(); t.Field(i).Name on a 20-field struct (A0..A19: 10 int64,
//	                 5 int32, 5 float64) 1e6 times (smoke 1e5); check: fields == 20n and name bytes == 50n
//	                 (Go names must be exported to be read portably, hence A0 rather than a0: same lengths).
//
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
package main

import (
	"fmt"
	"os"
	"reflect"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

type RT0 struct{ V int }
type RT1 struct{ V int }
type RT2 struct{ V int }
type RT3 struct{ V int }

type Target20 struct {
	A0  int64
	A1  int64
	A2  int64
	A3  int64
	A4  int64
	A5  int64
	A6  int64
	A7  int64
	A8  int64
	A9  int64
	A10 int32
	A11 int32
	A12 int32
	A13 int32
	A14 int32
	A15 float64
	A16 float64
	A17 float64
	A18 float64
	A19 float64
}

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
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "VmHWM:") {
			var kb int64
			fmt.Sscanf(l, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

func measure(warmup, trials int, fn func()) [4]int64 {
	for i := 0; i < warmup; i++ {
		fn()
	}
	var s []int64
	for i := 0; i < trials; i++ {
		t := time.Now()
		fn()
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
	return [4]int64{s[0], s[n/2], sum / int64(n), s[p]}
}

func emit(slug string, input int64, w, t int, st [4]int64, ok bool, notes string) {
	status := "invalid"
	if ok {
		status = "ok"
	}
	fmt.Printf("1,%s,%s,%s,reflect,,0,,%d,go,%s,reflect,%s,GOAMD64=%s go build,%d,%d,%d,%d,%d,%d,,,%d,-1,-1,-1,-1,%s,%t,%s,\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, env("PROFILE_LANG_VERSION", runtime.Version()),
		runtime.Version(), env("GOAMD64", "v3"), w, t, st[0], st[1], st[2], st[3], peakRSSKb(), status, ok,
		strings.ReplaceAll(notes, ",", ";"))
}

var sink int64

func main() {
	warmup := envInt("PROFILE_WARMUP", 1)
	trials := envInt("PROFILE_TRIALS", 5)
	if trials < 1 {
		trials = 1
	}
	sv := os.Getenv("CAJETA_BENCH_SMOKE")
	sm := sv == "1" || sv == "true" || sv == "yes"

	n := 10000000
	if sm {
		n = 1000000
	}
	vals := []any{RT0{}, RT1{}, RT2{}, RT3{}}
	var found int64
	st := measure(warmup, trials, func() {
		var f int64
		for i := 0; i < n; i++ {
			if reflect.TypeOf(vals[i&3]).Kind() == reflect.Struct {
				f++
			}
		}
		found = f
		sink += f
	})
	n1 := fmt.Sprintf("reflect.TypeOf(v).Kind() x%d over 4 struct types (Go has no by-name type lookup); check: found == %d", n, n)
	if sm {
		n1 += "; smoke size 1e6 in place of 1e7"
	}
	emit("class-lookup", int64(n), warmup, trials, st, found == int64(n), n1)

	m := 1000000
	if sm {
		m = 100000
	}
	var obj any = &Target20{}
	var fields, nbytes int64
	sf := measure(warmup, trials, func() {
		var fc, bc int64
		for i := 0; i < m; i++ {
			t := reflect.TypeOf(obj).Elem()
			k := t.NumField()
			for j := 0; j < k; j++ {
				fc++
				bc += int64(len(t.Field(j).Name))
			}
		}
		fields, nbytes = fc, bc
		sink += bc
	})
	n2 := fmt.Sprintf("reflect.TypeOf(obj).Elem(); NumField(); Field(i).Name on a 20-field struct x%d; check: fields == %d and name bytes == %d", m, 20*m, 50*m)
	if sm {
		n2 += "; smoke size 1e5 in place of 1e6"
	}
	emit("field-enumerate", int64(m), warmup, trials, sf, fields == int64(20*m) && nbytes == int64(50*m), n2)
}
