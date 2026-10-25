// Go math competitors — the same three scalar f64 kernels the Cajeta side runs:
// saxpy (z = 2.5*x + y over 1M), dot-product (sum x*y over 1M), matmul (200×200,
// A identity · B). Same init ⇒ exact reference checksums. Throughput in GFLOP/s.
// Scalar loops only; numpy (BLAS) is the Python competitor.
package main

import (
	"runtime"
	"fmt"
	"math"
	"os"
	"sort"
	"time"
)

const (
	N         = 1000000
	nDim      = 200
	saxpyRef  = 148250000.0
	dotRef    = 148499899.0
	matmulRef = 1980000.0
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

func approx(a, b float64) bool { return math.Abs(a-b) <= 1e-6*math.Max(math.Abs(b), 1.0) }

func emit(runID, ts, bench string, input int, flops float64, warmup, trials int, samples []int64, ok bool) {
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
	gflops := 0.0
	if med > 0 {
		gflops = flops / float64(med)
	}
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,math,,%d,,%d,go,%s,scalar,stdlib,GOAMD64=v4,%d,%d,%d,%d,%d,%d,%.3f,GFLOP/s,%d,-1,%d,-1,-1,%s,%t,,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""),
		warmup, trials, mn, med, mean, p95, gflops, peakRSSKb(), gAlloc, status, ok)
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

	x := make([]float64, N)
	ys := make([]float64, N)
	yd := make([]float64, N)
	for i := 0; i < N; i++ {
		x[i] = float64(i % 100)
		ys[i] = float64(i % 50)
		yd[i] = float64(i % 7)
	}

	// saxpy
	{
		const a = 2.5
		z := make([]float64, N)
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			for i := 0; i < N; i++ {
				z[i] = a*x[i] + ys[i]
			}
		}
		for t := 0; t < trials; t++ {
			t0 := time.Now()
			sum := 0.0
			for i := 0; i < N; i++ {
				z[i] = a*x[i] + ys[i]
				sum += z[i]
			}
			samples = append(samples, time.Since(t0).Nanoseconds())
			ok = approx(sum, saxpyRef)
		}
		emit(runID, ts, "saxpy", N, 2.0*float64(N), warmup, trials, samples, ok)
	}
	// dot
	{
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			s := 0.0
			for i := 0; i < N; i++ {
				s += x[i] * yd[i]
			}
			_ = s
		}
		for t := 0; t < trials; t++ {
			t0 := time.Now()
			acc := 0.0
			for i := 0; i < N; i++ {
				acc += x[i] * yd[i]
			}
			samples = append(samples, time.Since(t0).Nanoseconds())
			ok = approx(acc, dotRef)
		}
		emit(runID, ts, "dot-product", N, 2.0*float64(N), warmup, trials, samples, ok)
	}
	// matmul
	{
		a := make([]float64, nDim*nDim)
		b := make([]float64, nDim*nDim)
		c := make([]float64, nDim*nDim)
		for i := 0; i < nDim; i++ {
			for j := 0; j < nDim; j++ {
				idx := i*nDim + j
				if i == j {
					a[idx] = 1.0
				}
				b[idx] = float64(idx % 100)
			}
		}
		mm := func() {
			for i := 0; i < nDim; i++ {
				for j := 0; j < nDim; j++ {
					acc := 0.0
					for k := 0; k < nDim; k++ {
						acc += a[i*nDim+k] * b[k*nDim+j]
					}
					c[i*nDim+j] = acc
				}
			}
		}
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			mm()
		}
		for t := 0; t < trials; t++ {
			t0 := time.Now()
			mm()
			samples = append(samples, time.Since(t0).Nanoseconds())
			sum := 0.0
			for _, v := range c {
				sum += v
			}
			ok = approx(sum, matmulRef)
		}
		emit(runID, ts, "matmul", nDim*nDim, 2.0*math.Pow(float64(nDim), 3), warmup, trials, samples, ok)
	}
}
