// concurrent/tasks-1m-footprint, Go (Tasks.md): park 1e6 goroutines on one
// unbuffered channel, read the resident set while all are parked, then close
// the channel and wait for all of them. Own process, so peak_rss_kb (the row's
// memory metric) is this workload's. One trial: the row is memory; the time is
// spawn + park + release. Skips under 16 GB MemAvailable. CAJETA_BENCH_SMOKE=1
// parks 1e4 goroutines and says so in notes. Errors are returned, never panics.
package main

import (
	"bufio"
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func kb(path, key string) int64 {
	f, err := os.Open(path)
	if err != nil {
		return -1
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if l := sc.Text(); strings.HasPrefix(l, key) {
			var v int64
			fmt.Sscanf(strings.TrimSpace(l[len(key):]), "%d", &v)
			return v
		}
	}
	return -1
}

func run(n int64) (parked, done, before, parkedRSS int64, err error) {
	ch := make(chan struct{})
	var started, finished atomic.Int64
	var wg sync.WaitGroup
	before = kb("/proc/self/status", "VmRSS:")
	for i := int64(0); i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			started.Add(1)
			<-ch
			finished.Add(1)
		}()
	}
	for started.Load() < n {
		runtime.Gosched()
	}
	time.Sleep(10 * time.Millisecond) // let the last ones reach <-ch
	parkedRSS = kb("/proc/self/status", "VmRSS:")
	parked = started.Load()
	close(ch)
	wg.Wait()
	done = finished.Load()
	if done != n {
		err = fmt.Errorf("finished %d of %d", done, n)
	}
	return
}

func main() {
	smoke := env("CAJETA_BENCH_SMOKE", "") == "1" || env("CAJETA_BENCH_SMOKE", "") == "true"
	n := int64(1000000)
	if smoke {
		n = 10000
	}
	head := fmt.Sprintf("1,%s,%s,tasks-1m-footprint,concurrent,,%d,,%d,go,%s,goroutine", env("PROFILE_RUN_ID", "local"),
		env("PROFILE_RUN_TS", ""), n, n, env("PROFILE_LANG_VERSION", ""))
	if avail := kb("/proc/meminfo", "MemAvailable:"); avail >= 0 && avail < 16*1024*1024 {
		fmt.Printf("%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,MemAvailable %d MiB is under the 16 GB this row needs,\n", head, avail/1024)
		return
	}
	t0 := time.Now()
	parked, done, before, parkedRSS, err := run(n)
	ns := time.Since(t0).Nanoseconds()
	ok := err == nil && parked == n && done == n
	grew := parkedRSS - before
	notes := fmt.Sprintf("one unbuffered chan; %d goroutines parked in receive; RSS grew %d KiB while parked = %d B/task; one trial", n, grew, grew*1024/n)
	if smoke {
		notes += "; smoke size: 1e4 tasks"
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf("%s,stdlib,GOAMD64=v4,0,1,%d,%d,%d,%d,%.3f,Mop/s,%d,-1,-1,-1,%d,%s,%t,%s,\n",
		head, ns, ns, ns, ns, float64(n)/float64(ns)*1e3, kb("/proc/self/status", "VmHWM:"), grew, status, ok, notes)
}
