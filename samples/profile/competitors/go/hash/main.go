// Go hash competitors — bulk throughput over a deterministic 1 MiB buffer
// (buf[i] = i & 0xFF), the SMHasher bulk metric. One schema-conformant CSV row
// per algorithm (xxhash3 via zeebo/xxh3; sha256 + md5 via the stdlib crypto
// packages). Cross-check: the digest matches the cross-language reference.
package main

import (
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"runtime"
	"sort"
	"time"

	"github.com/zeebo/xxh3"
)

const N = 1048576

const (
	sha256Ref = "fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83"
	md5Ref    = "c35cc7d8d91728a0cb052831bc4ef372"
	xxh3Ref   = uint64(0xd36c0e13a3df139e)
)

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

func emit(runID, ts, bench, lib, ver string, warmup, trials int, samples []int64, checkOK bool) {
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
	mbps := 0.0
	if med > 0 {
		mbps = float64(N) / float64(med) * 1e9 / 1048576.0
	}
	status := "ok"
	if !checkOK {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,hash,,%d,,%d,go,%s,%s,%s,-gcflags,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%t,,\n",
		runID, ts, bench, N, N, env("PROFILE_LANG_VERSION", ""), lib, ver,
		warmup, trials, mn, med, mean, p95, mbps, peakRSSKb(), gAlloc, status, checkOK)
}

func benchU64(warmup, trials int, f func() uint64) []int64 {
	for i := 0; i < warmup; i++ {
		f()
	}
	s := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t0 := time.Now()
		sink := f()
		s = append(s, time.Since(t0).Nanoseconds())
		_ = sink
	}
	measAlloc(func() { f() })
	return s
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
	buf := make([]byte, N)
	for i := range buf {
		buf[i] = byte(i & 0xFF)
	}

	// xxhash3
	s := benchU64(warmup, trials, func() uint64 { return xxh3.Hash(buf) })
	emit(runID, ts, "xxhash3", "zeebo/xxh3", "v1.0.2", warmup, trials, s, xxh3.Hash(buf) == xxh3Ref)

	// sha256
	s = benchU64(warmup, trials, func() uint64 { h := sha256.Sum256(buf); return uint64(h[0]) })
	d := sha256.Sum256(buf)
	emit(runID, ts, "sha256", "crypto/sha256", runtime.Version(), warmup, trials, s,
		hex.EncodeToString(d[:]) == sha256Ref)

	// md5
	s = benchU64(warmup, trials, func() uint64 { h := md5.Sum(buf); return uint64(h[0]) })
	m := md5.Sum(buf)
	emit(runID, ts, "md5", "crypto/md5", runtime.Version(), warmup, trials, s,
		hex.EncodeToString(m[:]) == md5Ref)
}
