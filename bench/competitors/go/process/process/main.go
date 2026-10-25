// process/ competitor in Go (bench/process/Command.md, Process.md): os/exec.
//
//	command-spawn-wait   exec.Command("/bin/true").Run() R = 1e3 times (smoke 1e2); check: every exit 0
//	process-pipe-100mb   100 MiB (smoke 10 MiB) of data[i] = (i*31+7) & 0xFF through `cat`: cmd.Stdin is a
//	                     bytes.Reader and cmd.Output() collects stdout (os/exec copies both on goroutines);
//	                     check: length and byte sum equal the input's.
//
// Workloads return error; a failed trial makes the row invalid with the error in notes.
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
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

func measure(warmup, trials int, fn func() error) ([4]int64, error) {
	for i := 0; i < warmup; i++ {
		if err := fn(); err != nil {
			return [4]int64{-1, -1, -1, -1}, err
		}
	}
	var s []int64
	for i := 0; i < trials; i++ {
		t := time.Now()
		if err := fn(); err != nil {
			return [4]int64{-1, -1, -1, -1}, err
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
	return [4]int64{s[0], s[n/2], sum / int64(n), s[p]}, nil
}

func emit(slug string, input int64, w, t int, st [4]int64, err error, ok bool, notes string, mbps bool) {
	if err != nil {
		ok = false
		notes += "; error: " + err.Error()
	}
	thr, unit := "", ""
	if mbps && st[1] > 0 {
		thr = fmt.Sprintf("%.1f", float64(input)/float64(st[1])*1e9/1048576.0)
		unit = "MB/s"
	}
	status := "invalid"
	if ok {
		status = "ok"
	}
	fmt.Printf("1,%s,%s,%s,process,,0,,%d,go,%s,os/exec,%s,GOAMD64=%s go build,%d,%d,%d,%d,%d,%d,%s,%s,%d,-1,-1,-1,-1,%s,%t,%s,\n",
		env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, env("PROFILE_LANG_VERSION", runtime.Version()),
		runtime.Version(), env("GOAMD64", "v3"), w, t, st[0], st[1], st[2], st[3], thr, unit, peakRSSKb(), status, ok,
		strings.ReplaceAll(notes, ",", ";"))
}

var sink int64

func main() {
	warmup := envInt("PROFILE_WARMUP", 1)
	trials := envInt("PROFILE_TRIALS", 5)
	if trials < 1 {
		trials = 1
	}
	s := os.Getenv("CAJETA_BENCH_SMOKE")
	sm := s == "1" || s == "true" || s == "yes"

	// ---- command-spawn-wait ----
	r := 1000
	if sm {
		r = 100
	}
	zero := 0
	st, err := measure(warmup, trials, func() error {
		z := 0
		for i := 0; i < r; i++ {
			if err := exec.Command("/bin/true").Run(); err != nil {
				return err
			}
			z++
		}
		zero = z
		sink += int64(z)
		return nil
	})
	n1 := fmt.Sprintf("exec.Command(/bin/true).Run() x%d per trial (Run returns an error on a non-zero exit); returns error; check: zero exits == %d", r, r)
	if sm {
		n1 += "; smoke size 1e2 spawns in place of 1e3"
	}
	emit("command-spawn-wait", int64(r), warmup, trials, st, err, zero == r, n1, false)

	// ---- process-pipe-100mb ----
	n := 100 << 20
	if sm {
		n = 10 << 20
	}
	data := make([]byte, n)
	var want int64
	for i := range data {
		data[i] = byte((i*31 + 7) & 0xFF)
		want += int64(data[i])
	}
	gotLen, gotSum := 0, int64(0)
	sp, err := measure(warmup, trials, func() error {
		cmd := exec.Command("cat")
		cmd.Stdin = bytes.NewReader(data)
		out, err := cmd.Output()
		if err != nil {
			return err
		}
		var sum int64
		for _, b := range out {
			sum += int64(b)
		}
		gotLen, gotSum = len(out), sum
		sink += sum
		return nil
	})
	n2 := fmt.Sprintf("exec.Command(cat) with Stdin = bytes.Reader and Output(); returns error; check: length %d and byte sum %d", n, want)
	if sm {
		n2 += "; smoke size 10 MiB in place of 100 MiB"
	}
	emit("process-pipe-100mb", int64(n), warmup, trials, sp, err, gotLen == n && gotSum == want, n2, true)
}
