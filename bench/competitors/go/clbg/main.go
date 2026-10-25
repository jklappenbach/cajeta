// Go CLBG competitors — the same three Computer Language Benchmarks Game classics
// the Cajeta side runs, at the same sizes, verified against the canonical
// checksums: clbg-mandelbrot (800², 254099), clbg-fannkuch-redux (N=10, 38 /
// 73196), clbg-spectral-norm (N=100, ≈1.274224). Pure scalar stdlib. One
// schema-conformant CSV row per benchmark.
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
	mandelN    = 800
	mandelRef  = int64(254099)
	fannN      = 10
	spectralN  = 100
	spectralRef = 1.274224
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

func emit(runID, ts, bench string, input, warmup, trials int, samples []int64, ok bool) {
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
	status := "ok"
	if !ok {
		status = "invalid"
	}
	fmt.Printf(
		"1,%s,%s,%s,clbg,,%d,,%d,go,%s,scalar,stdlib,GOAMD64=v4,%d,%d,%d,%d,%d,%d,,,%d,-1,%d,-1,-1,%s,%t,,\n",
		runID, ts, bench, input, input, env("PROFILE_LANG_VERSION", ""),
		warmup, trials, mn, med, mean, p95, peakRSSKb(), gAlloc, status, ok)
}

func mandelbrot(n int) int64 {
	var count int64
	for py := 0; py < n; py++ {
		ci := 2.0*float64(py)/float64(n) - 1.0
		for px := 0; px < n; px++ {
			cr := 2.0*float64(px)/float64(n) - 1.5
			zr, zi := 0.0, 0.0
			member := true
			for it := 0; it < 50; it++ {
				nzr := zr*zr - zi*zi + cr
				nzi := 2.0*zr*zi + ci
				zr, zi = nzr, nzi
				if zr*zr+zi*zi > 4.0 {
					member = false
					break
				}
			}
			if member {
				count++
			}
		}
	}
	return count
}

func fannkuch(n int) (int, int) {
	perm := make([]int, n)
	perm1 := make([]int, n)
	count := make([]int, n)
	for i := range perm1 {
		perm1[i] = i
	}
	maxFlips, checksum, permCount, r := 0, 0, 0, n
	for {
		for r != 1 {
			count[r-1] = r
			r--
		}
		copy(perm, perm1)
		flips, k := 0, perm[0]
		for k != 0 {
			for i, j := 0, k; i < j; i, j = i+1, j-1 {
				perm[i], perm[j] = perm[j], perm[i]
			}
			flips++
			k = perm[0]
		}
		if flips > maxFlips {
			maxFlips = flips
		}
		if permCount%2 == 0 {
			checksum += flips
		} else {
			checksum -= flips
		}
		permCount++
		for {
			if r == n {
				return maxFlips, checksum
			}
			p0 := perm1[0]
			for i := 0; i < r; i++ {
				perm1[i] = perm1[i+1]
			}
			perm1[r] = p0
			count[r]--
			if count[r] > 0 {
				break
			}
			r++
		}
	}
}

func spectralNorm(n int) float64 {
	a := func(i, j int) float64 { return 1.0 / float64((i+j)*(i+j+1)/2+i+1) }
	mulAv := func(v, out []float64) {
		for i := 0; i < n; i++ {
			s := 0.0
			for j := 0; j < n; j++ {
				s += a(i, j) * v[j]
			}
			out[i] = s
		}
	}
	mulAtv := func(v, out []float64) {
		for i := 0; i < n; i++ {
			s := 0.0
			for j := 0; j < n; j++ {
				s += a(j, i) * v[j]
			}
			out[i] = s
		}
	}
	u := make([]float64, n)
	v := make([]float64, n)
	t := make([]float64, n)
	for i := range u {
		u[i] = 1.0
	}
	for it := 0; it < 10; it++ {
		mulAv(u, t)
		mulAtv(t, v)
		mulAv(v, t)
		mulAtv(t, u)
	}
	vbv, vv := 0.0, 0.0
	for i := 0; i < n; i++ {
		vbv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	return math.Sqrt(vbv / vv)
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

	// mandelbrot
	{
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			mandelbrot(mandelN)
		}
		for tr := 0; tr < trials; tr++ {
			t0 := time.Now()
			r := mandelbrot(mandelN)
			samples = append(samples, time.Since(t0).Nanoseconds())
			ok = r == mandelRef
		}
		measAlloc(func() { mandelbrot(mandelN) })
		emit(runID, ts, "clbg-mandelbrot", mandelN*mandelN, warmup, trials, samples, ok)
	}
	// fannkuch
	{
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			fannkuch(fannN)
		}
		for tr := 0; tr < trials; tr++ {
			t0 := time.Now()
			mf, cs := fannkuch(fannN)
			samples = append(samples, time.Since(t0).Nanoseconds())
			ok = mf == 38 && cs == 73196
		}
		measAlloc(func() { fannkuch(fannN) })
		emit(runID, ts, "clbg-fannkuch-redux", fannN, warmup, trials, samples, ok)
	}
	// spectral-norm
	{
		var samples []int64
		ok := true
		for w := 0; w < warmup; w++ {
			spectralNorm(spectralN)
		}
		for tr := 0; tr < trials; tr++ {
			t0 := time.Now()
			r := spectralNorm(spectralN)
			samples = append(samples, time.Since(t0).Nanoseconds())
			ok = math.Abs(r-spectralRef) < 1e-3
		}
		measAlloc(func() { spectralNorm(spectralN) })
		emit(runID, ts, "clbg-spectral-norm", spectralN, warmup, trials, samples, ok)
	}
}
