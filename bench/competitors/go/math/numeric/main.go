// Go math/ numeric competitor (benchmark-reference unit 11): every math/ workload
// the pages name for Go, with gonum v0.15.1 (mat, dsp/fourier, stat, floats),
// math/rand/v2 and plain Go, at the sizes and with the checks of
// scratchpad WORKLOADS.md / the Cajeta side. Prints v1 rows (31 columns);
// competitors/run.sh turns them into schema v2 with catalog ids math/<slug>.
// A workload's error is returned to its row (status failed, message in notes).
package main

import (
	"errors"
	"fmt"
	"math"
	"math/cmplx"
	"math/rand/v2"
	"os"
	"sort"
	"strings"
	"time"

	"gonum.org/v1/gonum/dsp/fourier"
	"gonum.org/v1/gonum/floats"
	"gonum.org/v1/gonum/mat"
	"gonum.org/v1/gonum/stat"
)

const gonumVer = "v0.15.1"

var (
	runID   = env("PROFILE_RUN_ID", "local")
	ts      = env("PROFILE_RUN_TS", "")
	smoke   = os.Getenv("CAJETA_BENCH_SMOKE") == "1" || os.Getenv("CAJETA_BENCH_SMOKE") == "true"
	warmup  int
	trials  int
	langVer = env("PROFILE_LANG_VERSION", "")
	ids     = strings.Fields(os.Getenv("MATH_IDS"))
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func atoi(s string, d int) int {
	var v int
	if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
		return d
	}
	return v
}

func wanted(id string) bool {
	if len(ids) == 0 {
		return true
	}
	for _, x := range ids {
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
	for _, line := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(line, "VmHWM:") {
			var kb int64
			fmt.Sscanf(line, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

func u(i int64) float64 { return float64((i*7919)%10007) / 10007.0 }
func v(i int64) float64 {
	m := i % 10007
	return float64((m*m*7+i*7919)%10007) / 10007.0
}
func f32(x float64) float64 { return float64(float32(x)) }

func clean(s string) string { return strings.ReplaceAll(strings.ReplaceAll(s, ",", ";"), "\n", " ") }

// row prints one v1 row.
func row(slug, variant, lib, libVer string, input int64, samples []int64, status string, okField string, notes string) {
	mn, med, mean, p95 := int64(-1), int64(-1), int64(-1), int64(-1)
	w, t := warmup, trials
	if len(samples) > 0 {
		s := append([]int64(nil), samples...)
		sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
		n := len(s)
		var sum int64
		for _, x := range s {
			sum += x
		}
		idx := n * 95 / 100
		if idx >= n {
			idx = n - 1
		}
		mn, med, mean, p95 = s[0], s[n/2], sum/int64(n), s[idx]
	} else {
		w, t = 0, 0
	}
	fmt.Printf("1,%s,%s,%s,math,,%d,,%d,go,%s,%s,%s,GOAMD64=v3,%d,%d,%d,%d,%d,%d,,,%d,-1,-1,-1,-1,%s,%s,%s,%s\n",
		runID, ts, slug, input, input, langVer, lib, libVer, w, t, mn, med, mean, p95, peakRSSKb(),
		status, okField, clean(notes), variant)
}

// bench runs prep-free work f warmup+trials times; check runs after the last trial.
func bench(slug, variant, lib, libVer string, input int64, notes string, f func() error, check func() (bool, error)) {
	if !wanted(slug) {
		return
	}
	notes = notes + "; errors return error"
	if smoke {
		notes = notes + "; smoke"
	}
	run := func() (err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("panic: %v", r)
			}
		}()
		return f()
	}
	for i := 0; i < warmup; i++ {
		if err := run(); err != nil {
			row(slug, variant, lib, libVer, input, nil, "failed", "", notes+"; "+err.Error())
			return
		}
	}
	samples := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t0 := time.Now()
		err := run()
		d := time.Since(t0).Nanoseconds()
		if err != nil {
			row(slug, variant, lib, libVer, input, nil, "failed", "", notes+"; "+err.Error())
			return
		}
		samples = append(samples, d)
	}
	ok, err := check()
	if err != nil {
		row(slug, variant, lib, libVer, input, samples, "failed", "", notes+"; "+err.Error())
		return
	}
	if ok {
		row(slug, variant, lib, libVer, input, samples, "ok", "true", notes)
	} else {
		row(slug, variant, lib, libVer, input, samples, "invalid", "false", notes)
	}
}

func relOK(got, want, tol float64) bool {
	return math.Abs(got-want) <= tol*math.Max(math.Abs(want), 1e-300)
}

func pick(full, sm int) int {
	if smoke {
		return sm
	}
	return full
}

func sizeNote(name string, full, sm int) string {
	if smoke && full != sm {
		return fmt.Sprintf("smoke size %s=%d in place of %d", name, sm, full)
	}
	return fmt.Sprintf("%s=%d", name, full)
}

// ---------------- tensor ----------------

func tensorBenches() {
	const f32note = "gonum mat is float64-only: the float32 workload runs in float64 over float32-rounded data"
	{
		n := pick(10000000, 1000000)
		a, b, c := make([]float64, n), make([]float64, n), make([]float64, n)
		var ref float64
		for i := 0; i < n; i++ {
			a[i], b[i], c[i] = f32(u(int64(i))), f32(u(int64(i+1))), f32(u(int64(i+2)))
			ref += a[i]*b[i] + c[i]
		}
		va, vb, vc := mat.NewVecDense(n, a), mat.NewVecDense(n, b), mat.NewVecDense(n, c)
		var r *mat.VecDense
		bench("tensor-elementwise", "", "gonum", gonumVer, int64(n),
			sizeNote("n", 10000000, 1000000)+"; VecDense MulElemVec + AddVec fresh result; "+f32note+"; check: rel err <= 1e-5",
			func() error {
				r = mat.NewVecDense(n, nil)
				r.MulElemVec(va, vb)
				r.AddVec(r, vc)
				return nil
			}, func() (bool, error) {
				return relOK(floats.Sum(r.RawVector().Data), ref, 1e-5), nil
			})
	}
	rows, cols := pick(10000, 1000), 1000
	M := mat.NewDense(rows, cols, nil)
	var refM float64
	for r := 0; r < rows; r++ {
		for c := 0; c < cols; c++ {
			x := f32(u(int64(r*cols + c)))
			M.Set(r, c, x)
			refM += x
		}
	}
	{
		row := make([]float64, cols)
		var rowSum float64
		for c := 0; c < cols; c++ {
			row[c] = f32(u(int64(c + 5)))
			rowSum += row[c]
		}
		ref := refM + float64(rows)*rowSum
		var R *mat.Dense
		bench("tensor-broadcast", "", "gonum", gonumVer, int64(rows*cols),
			sizeNote("rows", 10000, 1000)+"; cols=1000; gonum has no broadcast: fresh Dense + floats.AddTo per row; "+f32note+"; check: rel err <= 1e-5",
			func() error {
				R = mat.NewDense(rows, cols, nil)
				for r := 0; r < rows; r++ {
					floats.AddTo(R.RawRowView(r), M.RawRowView(r), row)
				}
				return nil
			}, func() (bool, error) {
				return relOK(floats.Sum(R.RawMatrix().Data), ref, 1e-5), nil
			})
	}
	{
		// float64 reference
		sum0, max0 := make([]float64, cols), make([]float64, cols)
		for c := range max0 {
			max0[c] = math.Inf(-1)
		}
		var ref float64
		for r := 0; r < rows; r++ {
			rv := M.RawRowView(r)
			ref += floats.Sum(rv) + floats.Max(rv) + floats.Sum(rv)/float64(cols)
			for c, x := range rv {
				sum0[c] += x
				if x > max0[c] {
					max0[c] = x
				}
			}
		}
		for c := 0; c < cols; c++ {
			ref += sum0[c] + max0[c] + sum0[c]/float64(rows)
		}
		var s0, m0, a0, s1, m1, a1 []float64
		bench("tensor-reduce", "", "gonum", gonumVer, int64(rows*cols),
			sizeNote("rows", 10000, 1000)+"; cols=1000; sum/max/mean along both axes via floats over Dense rows (gonum has no axis reductions); "+f32note+"; check: rel err <= 1e-3",
			func() error {
				s0, m0, a0 = make([]float64, cols), make([]float64, cols), make([]float64, cols)
				s1, m1, a1 = make([]float64, rows), make([]float64, rows), make([]float64, rows)
				copy(m0, M.RawRowView(0))
				for r := 0; r < rows; r++ {
					rv := M.RawRowView(r)
					s1[r] = floats.Sum(rv)
					m1[r] = floats.Max(rv)
					a1[r] = s1[r] / float64(cols)
					floats.Add(s0, rv)
					for c, x := range rv {
						if x > m0[c] {
							m0[c] = x
						}
					}
				}
				floats.ScaleTo(a0, 1/float64(rows), s0)
				return nil
			}, func() (bool, error) {
				cs := floats.Sum(s0) + floats.Sum(s1) + floats.Sum(m0) + floats.Sum(m1) + floats.Sum(a0) + floats.Sum(a1)
				return relOK(cs, ref, 1e-3), nil
			})
	}
	M = nil
	{
		n := pick(4096, 1024)
		T := mat.NewDense(n, n, nil)
		for i := 0; i < n; i++ {
			for j := 0; j < n; j++ {
				T.Set(i, j, f32(u(int64(i*n+j))))
			}
		}
		var out *mat.Dense
		bench("tensor-transpose", "", "gonum", gonumVer, int64(n*n),
			sizeNote("N", 4096, 1024)+"; mat.DenseCopyOf(m.T()); "+f32note+"; check: exact equality",
			func() error {
				out = mat.DenseCopyOf(T.T())
				return nil
			}, func() (bool, error) {
				for i := 0; i < n; i++ {
					for j := 0; j < n; j++ {
						if out.At(j, i) != T.At(i, j) {
							return false, nil
						}
					}
				}
				return true, nil
			})
	}
	{
		n := pick(10000000, 1000000)
		x := make([]float32, n)
		var ref int64
		for i := 0; i < n; i++ {
			x[i] = float32(u(int64(i))*1000 - 500)
			ref += int64(int32(float64(x[i])))
		}
		var y []float64
		var z []int32
		bench("tensor-cast", "", "go", "stdlib", int64(n),
			sizeNote("n", 10000000, 1000000)+"; gonum has no dtype cast: plain loops float32->float64->int32 (truncation); check: exact int64 checksum",
			func() error {
				y = make([]float64, n)
				for i, e := range x {
					y[i] = float64(e)
				}
				z = make([]int32, n)
				for i, e := range y {
					z[i] = int32(e)
				}
				return nil
			}, func() (bool, error) {
				var s int64
				for _, e := range z {
					s += int64(e)
				}
				return s == ref, nil
			})
	}
}

// ---------------- linalg ----------------

func linMatrix(n int) (*mat.Dense, *mat.VecDense) {
	A := mat.NewDense(n, n, nil)
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			x := u(int64(i*n+j)) - 0.5
			if i == j {
				x += float64(n)
			}
			A.Set(i, j, x)
		}
	}
	b := mat.NewVecDense(n, nil)
	for i := 0; i < n; i++ {
		b.SetVec(i, u(int64(i+3)))
	}
	return A, b
}

func maxAbs(m mat.Matrix) float64 {
	r, c := m.Dims()
	var mx float64
	for i := 0; i < r; i++ {
		for j := 0; j < c; j++ {
			if a := math.Abs(m.At(i, j)); a > mx {
				mx = a
			}
		}
	}
	return mx
}

func linalgBenches() {
	for _, vs := range []struct {
		v        string
		full, sm int
		svdSm    int
	}{{"512", 512, 128, 64}, {"2048", 2048, 256, 128}} {
		n := pick(vs.full, vs.sm)
		note := sizeNote("n", vs.full, vs.sm)
		A, b := linMatrix(n)
		{
			x := mat.NewVecDense(n, nil)
			bench("linalg-solve", vs.v, "gonum", gonumVer, int64(n), note+"; mat.LU Factorize + SolveVecTo; check: ||Ax-b||inf/(||A||inf||x||inf+||b||inf) <= 1e-12",
				func() error {
					var lu mat.LU
					lu.Factorize(A)
					return lu.SolveVecTo(x, false, b)
				}, func() (bool, error) {
					r := mat.NewVecDense(n, nil)
					r.MulVec(A, x)
					r.SubVec(r, b)
					res := mat.Norm(r, math.Inf(1))
					den := mat.Norm(A, math.Inf(1))*mat.Norm(x, math.Inf(1)) + mat.Norm(b, math.Inf(1))
					return res/den <= 1e-12, nil
				})
		}
		{
			var Ai mat.Dense
			bench("linalg-inverse", vs.v, "gonum", gonumVer, int64(n), note+"; mat.Dense.Inverse; check: max|A*Ai-I| <= 1e-10",
				func() error { return Ai.Inverse(A) },
				func() (bool, error) {
					var P mat.Dense
					P.Mul(A, &Ai)
					for i := 0; i < n; i++ {
						P.Set(i, i, P.At(i, i)-1)
					}
					return maxAbs(&P) <= 1e-10, nil
				})
		}
		{
			var Q, R mat.Dense
			bench("linalg-qr", vs.v, "gonum", gonumVer, int64(n), note+"; mat.QR Factorize + QTo + RTo; check: max|QR-A|/max|A| <= 1e-12",
				func() error {
					var qr mat.QR
					qr.Factorize(A)
					qr.QTo(&Q)
					qr.RTo(&R)
					return nil
				}, func() (bool, error) {
					var P mat.Dense
					P.Mul(&Q, &R)
					P.Sub(&P, A)
					return maxAbs(&P)/maxAbs(A) <= 1e-12, nil
				})
		}
		{
			ns := pick(vs.full, vs.svdSm)
			As, _ := linMatrix(ns)
			var U, V mat.Dense
			var S []float64
			bench("linalg-svd", vs.v, "gonum", gonumVer, int64(ns), sizeNote("n", vs.full, vs.svdSm)+"; mat.SVD SVDFull + UTo + VTo; check: max|U*diag(S)*Vt-A|/max|A| <= 1e-10",
				func() error {
					var svd mat.SVD
					if !svd.Factorize(As, mat.SVDFull) {
						return errors.New("svd did not converge")
					}
					svd.UTo(&U)
					svd.VTo(&V)
					S = svd.Values(nil)
					return nil
				}, func() (bool, error) {
					US := mat.DenseCopyOf(&U)
					for j := 0; j < ns; j++ {
						for i := 0; i < ns; i++ {
							US.Set(i, j, US.At(i, j)*S[j])
						}
					}
					var P mat.Dense
					P.Mul(US, V.T())
					P.Sub(&P, As)
					return maxAbs(&P)/maxAbs(As) <= 1e-10, nil
				})
		}
	}
}

// ---------------- fft ----------------

func signal(n int) []complex128 {
	x := make([]complex128, n)
	for k := 0; k < n; k++ {
		x[k] = complex(f32(u(int64(2*k))-0.5), f32(u(int64(2*k+1))-0.5))
	}
	return x
}

func maxAbsDiff(a, b []complex128) float64 {
	var m float64
	for i := range a {
		if d := cmplx.Abs(a[i] - b[i]); d > m {
			m = d
		}
	}
	return m
}

func dftCheck() bool {
	const n = 64
	x := signal(n)
	got := fourier.NewCmplxFFT(n).Coefficients(nil, x)
	want := make([]complex128, n)
	for k := 0; k < n; k++ {
		var s complex128
		for j := 0; j < n; j++ {
			th := -2 * math.Pi * float64(j*k) / n
			s += x[j] * complex(math.Cos(th), math.Sin(th))
		}
		want[k] = s
	}
	return maxAbsDiff(got, want) <= 1e-4
}

func fft2(t *fourier.CmplxFFT, data []complex128, n int, inverse bool) {
	col := make([]complex128, n)
	for r := 0; r < n; r++ {
		rv := data[r*n : (r+1)*n]
		if inverse {
			t.Sequence(rv, rv)
		} else {
			t.Coefficients(rv, rv)
		}
	}
	for c := 0; c < n; c++ {
		for r := 0; r < n; r++ {
			col[r] = data[r*n+c]
		}
		if inverse {
			t.Sequence(col, col)
		} else {
			t.Coefficients(col, col)
		}
		for r := 0; r < n; r++ {
			data[r*n+c] = col[r]
		}
	}
}

func fftBenches() {
	dftOK := dftCheck()
	for _, vs := range []struct {
		v        string
		full, sm int
	}{{"2^16", 65536, 65536}, {"2^20", 1048576, 262144}} {
		n := pick(vs.full, vs.sm)
		x := signal(n)
		t := fourier.NewCmplxFFT(n)
		out := make([]complex128, n)
		bench("fft-1d", vs.v, "gonum/fourier", gonumVer, int64(n),
			sizeNote("N", vs.full, vs.sm)+"; CmplxFFT.Coefficients; complex128 (gonum/fourier is double-only); plan made outside timing; check: direct DFT at N=64 max abs err <= 1e-4 and round trip max abs err <= 1e-4",
			func() error { t.Coefficients(out, x); return nil },
			func() (bool, error) {
				back := t.Sequence(nil, out)
				for i := range back {
					back[i] /= complex(float64(n), 0)
				}
				return dftOK && maxAbsDiff(back, x) <= 1e-4, nil
			})
	}
	{
		n := pick(2048, 512)
		x := signal(n * n)
		t := fourier.NewCmplxFFT(n)
		data := make([]complex128, n*n)
		bench("fft-2d", "", "gonum/fourier", gonumVer, int64(n*n),
			sizeNote("side", 2048, 512)+"; gonum has no 2-D FFT: CmplxFFT over rows then columns; complex128 (double-only); check: round trip max abs err <= 1e-4",
			func() error { copy(data, x); fft2(t, data, n, false); return nil },
			func() (bool, error) {
				back := append([]complex128(nil), data...)
				fft2(t, back, n, true)
				s := complex(1/float64(n*n), 0)
				for i := range back {
					back[i] *= s
				}
				return maxAbsDiff(back, x) <= 1e-4, nil
			})
	}
}

// ---------------- random ----------------

func meanVar(x []float64) (float64, float64) {
	var s float64
	for _, e := range x {
		s += e
	}
	m := s / float64(len(x))
	var q float64
	for _, e := range x {
		d := e - m
		q += d * d
	}
	return m, q / float64(len(x))
}

func randomBenches() {
	n := pick(100000000, 10000000)
	note := sizeNote("n", 100000000, 10000000)
	buf := make([]float64, n)
	{
		rng := rand.New(rand.NewPCG(42, 0))
		bench("random-uniform", "", "math/rand/v2", "stdlib", int64(n), note+"; PCG(42 0) Float64 into a preallocated slice; check: |mean-0.5| <= 1e-3 and |var-1/12| <= 1e-3",
			func() error {
				for i := range buf {
					buf[i] = rng.Float64()
				}
				return nil
			}, func() (bool, error) {
				m, va := meanVar(buf)
				return math.Abs(m-0.5) <= 1e-3 && math.Abs(va-1.0/12) <= 1e-3, nil
			})
	}
	{
		rng := rand.New(rand.NewPCG(42, 0))
		bench("random-normal", "", "math/rand/v2", "stdlib", int64(n), note+"; PCG(42 0) NormFloat64 into a preallocated slice; check: |mean| <= 1e-3 and |var-1| <= 2e-3",
			func() error {
				for i := range buf {
					buf[i] = rng.NormFloat64()
				}
				return nil
			}, func() (bool, error) {
				m, va := meanVar(buf)
				return math.Abs(m) <= 1e-3 && math.Abs(va-1) <= 2e-3, nil
			})
	}
}

// ---------------- poly ----------------

var coef = []float64{0.5, -1.25, 2.0, -0.75, 1.5, -2.25, 0.25, 1.75, -1.0}

func horner(x float64) float64 {
	y := coef[0]
	for _, c := range coef[1:] {
		y = y*x + c
	}
	return y
}

func polyBenches() {
	{
		n := pick(100000000, 10000000)
		x := make([]float64, n)
		for i := range x {
			x[i] = float64(i) / float64(n)
		}
		y := make([]float64, n)
		var integ float64
		for k, c := range coef {
			integ += c / float64(8-k+1)
		}
		ref := float64(n)*integ - (horner(1)-horner(0))/2
		bench("poly-eval", "", "go", "stdlib", int64(n), sizeNote("n", 100000000, 10000000)+"; plain Go Horner into an output slice (gonum has no polyval); check: rel err <= 1e-7 vs Euler-Maclaurin reference",
			func() error {
				for i, e := range x {
					y[i] = horner(e)
				}
				return nil
			}, func() (bool, error) { return relOK(floats.Sum(y), ref, 1e-7), nil })
	}
	{
		m := pick(1000000, 100000)
		fits := pick(100, 10)
		x := make([]float64, m)
		y := make([]float64, m)
		for i := range x {
			x[i] = -1 + 2*float64(i)/float64(m-1)
			y[i] = horner(x[i])
		}
		yv := mat.NewDense(m, 1, y)
		var sol mat.Dense
		note := sizeNote("m", 1000000, 100000)
		if smoke {
			note += "; smoke fits=10 in place of 100"
		}
		bench("poly-fit", "", "gonum", gonumVer, int64(m), note+"; per fit: build the m x 9 Vandermonde + mat.QR SolveTo; check: max|coef-c| <= 1e-6",
			func() error {
				for f := 0; f < fits; f++ {
					V := mat.NewDense(m, 9, nil)
					raw := V.RawMatrix()
					for i, xi := range x {
						p := 1.0
						for k := 8; k >= 0; k-- {
							raw.Data[i*raw.Stride+k] = p
							p *= xi
						}
					}
					var qr mat.QR
					qr.Factorize(V)
					if err := qr.SolveTo(&sol, false, yv); err != nil {
						return err
					}
				}
				return nil
			}, func() (bool, error) {
				for k := 0; k < 9; k++ {
					if math.Abs(sol.At(k, 0)-coef[k]) > 1e-6 {
						return false, nil
					}
				}
				return true, nil
			})
	}
}

// ---------------- stats ----------------

func statsBenches() {
	n := pick(100000000, 10000000)
	note := sizeNote("n", 100000000, 10000000)
	x := make([]float64, n)
	for i := range x {
		x[i] = u(int64(i))
	}
	{
		var mean, va, sk, ku float64
		bench("stats-moments", "", "gonum/stat", gonumVer, int64(n), note+"; stat.PopMeanVariance (population) + stat.Skew + stat.ExKurtosis (gonum's sample-adjusted estimators; differ from biased by O(1/n)); check: |mean-0.49995| |var-1/12| |skew| |kurt+1.2| each <= 1e-3",
			func() error {
				mean, va = stat.PopMeanVariance(x, nil)
				sk = stat.Skew(x, nil)
				ku = stat.ExKurtosis(x, nil)
				return nil
			}, func() (bool, error) {
				return math.Abs(mean-0.49995) <= 1e-3 && math.Abs(va-1.0/12) <= 1e-3 && math.Abs(sk) <= 1e-3 && math.Abs(ku+1.2) <= 1e-3, nil
			})
	}
	{
		ref := make([]int64, 1000)
		for i := 0; i < n; i++ {
			ref[((int64(i)*7919)%10007)*1000/10007]++
		}
		var counts []int64
		bench("stats-histogram", "", "go", "stdlib", int64(n), note+"; 1000 bins over [0 1]; plain loop: stat.Histogram requires sorted input; check: every count equals the integer reference",
			func() error {
				counts = make([]int64, 1000)
				for _, e := range x {
					b := int(e * 1000)
					if b >= 1000 {
						b = 999
					}
					counts[b]++
				}
				return nil
			}, func() (bool, error) {
				for i := range ref {
					if counts[i] != ref[i] {
						return false, nil
					}
				}
				return true, nil
			})
	}
}

// ---------------- geometry ----------------

type vec3 struct{ x, y, z float32 }

func sub(a, b vec3) vec3           { return vec3{a.x - b.x, a.y - b.y, a.z - b.z} }
func dot(a, b vec3) float32        { return a.x*b.x + a.y*b.y + a.z*b.z }
func cross(a, b vec3) vec3         { return vec3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x} }
func scale(a vec3, s float32) vec3 { return vec3{a.x * s, a.y * s, a.z * s} }
func add(a, b vec3) vec3           { return vec3{a.x + b.x, a.y + b.y, a.z + b.z} }

func rays(R int) ([]vec3, []vec3) {
	o, d := make([]vec3, R), make([]vec3, R)
	for i := 0; i < R; i++ {
		ii := int64(i)
		o[i] = vec3{float32(v(3*ii)*2 - 1), float32(v(3*ii+1)*2 - 1), -5}
		dx, dy, dz := v(3*ii+2)*0.2-0.1, v(3*ii+7)*0.2-0.1, 1.0
		l := math.Sqrt(dx*dx + dy*dy + dz*dz)
		d[i] = vec3{float32(dx / l), float32(dy / l), float32(dz / l)}
	}
	return o, d
}

func geometryBenches() {
	const S = 10000
	R := pick(10000, 1000)
	tests := int64(R) * S
	note := fmt.Sprintf("rays=%d x objects=%d", R, S)
	if smoke {
		note = fmt.Sprintf("smoke size rays=%d in place of 10000 (1e7 tests)", R)
	}
	o, d := rays(R)
	c, rr, mn, mx := make([]vec3, S), make([]float32, S), make([]vec3, S), make([]vec3, S)
	for j := 0; j < S; j++ {
		jj := int64(j)
		cx, cy, cz := v(5*jj)*4-2, v(5*jj+1)*4-2, v(5*jj+2)*4
		c[j] = vec3{float32(cx), float32(cy), float32(cz)}
		rr[j] = float32(0.05 + v(5*jj+3)*0.2)
		s := 0.1 + v(5*jj+3)*0.3
		mn[j] = c[j]
		mx[j] = vec3{float32(cx + s), float32(cy + s), float32(cz + s)}
	}
	{
		ref := int64(502607)
		if smoke {
			ref = 51613
		}
		var hits int64
		bench("ray-sphere", "", "go", "stdlib", tests, note+"; plain Go float32 structs; check: |hits-ref| <= 1e-5*tests",
			func() error {
				hits = 0
				for i := 0; i < R; i++ {
					oi, di := o[i], d[i]
					for j := 0; j < S; j++ {
						oc := sub(oi, c[j])
						b := dot(oc, di)
						cc := dot(oc, oc) - rr[j]*rr[j]
						if b*b-cc >= 0 && (b <= 0 || cc <= 0) {
							hits++
						}
					}
				}
				return nil
			}, func() (bool, error) { return math.Abs(float64(hits-ref)) <= 1e-5*float64(tests), nil })
	}
	{
		ref := int64(476570)
		if smoke {
			ref = 49068
		}
		var hits int64
		slab := func(o, inv, lo, hi float32, t0, t1 *float32) bool {
			ta, tb := (lo-o)*inv, (hi-o)*inv
			if ta > tb {
				ta, tb = tb, ta
			}
			if ta > *t0 {
				*t0 = ta
			}
			if tb < *t1 {
				*t1 = tb
			}
			return *t0 <= *t1
		}
		bench("ray-aabb", "", "go", "stdlib", tests, note+"; plain Go float32 slab test; check: |hits-ref| <= 1e-5*tests",
			func() error {
				hits = 0
				for i := 0; i < R; i++ {
					oi := o[i]
					inv := vec3{1 / d[i].x, 1 / d[i].y, 1 / d[i].z}
					for j := 0; j < S; j++ {
						t0, t1 := float32(0), float32(1e8)
						if slab(oi.x, inv.x, mn[j].x, mx[j].x, &t0, &t1) &&
							slab(oi.y, inv.y, mn[j].y, mx[j].y, &t0, &t1) &&
							slab(oi.z, inv.z, mn[j].z, mx[j].z, &t0, &t1) {
							hits++
						}
					}
				}
				return nil
			}, func() (bool, error) { return math.Abs(float64(hits-ref)) <= 1e-5*float64(tests), nil })
	}
	{
		type quat struct{ w, x, y, z float32 }
		Q := make([]quat, 1024)
		V := make([]vec3, 1024)
		for j := 0; j < 1024; j++ {
			jj := int64(j)
			ax, ay, az := v(4*jj)-0.5, v(4*jj+1)-0.5, v(4*jj+2)-0.5
			l := math.Sqrt(ax*ax + ay*ay + az*az)
			a := v(4*jj+3) * 2 * math.Pi
			s := math.Sin(a / 2)
			Q[j] = quat{float32(math.Cos(a / 2)), float32(ax / l * s), float32(ay / l * s), float32(az / l * s)}
			V[j] = vec3{float32(v(3*jj+11) - 0.5), float32(v(3*jj+12) - 0.5), float32(v(3*jj+13) - 0.5)}
		}
		n := pick(100000000, 10000000)
		ref := 386090.57108821085
		if smoke {
			ref = 38600.96393095818
		}
		var sum float64
		bench("rotation-compose", "", "go", "stdlib", int64(n), sizeNote("n", 100000000, 10000000)+"; plain Go float32 Hamilton product + q*v*q' rotation; check: |sum-ref| <= 1e-6*n",
			func() error {
				sum = 0
				for i := 0; i < n; i++ {
					a, b := Q[i&1023], Q[(i>>10)&1023]
					q := quat{a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
						a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
						a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
						a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w}
					p := V[i&1023]
					qv := vec3{q.x, q.y, q.z}
					t := scale(cross(qv, p), 2)
					w := add(add(p, scale(t, q.w)), cross(qv, t))
					sum += float64(w.x) + float64(w.y) + float64(w.z)
				}
				return nil
			}, func() (bool, error) { return math.Abs(sum-ref) <= 1e-6*float64(n), nil })
	}
	{
		const m = 1000000
		passes := pick(100, 10)
		ax, ay, az := 1.0, 2.0, 3.0
		l := math.Sqrt(ax*ax + ay*ay + az*az)
		qs := math.Sin(0.35)
		qw, qx, qy, qz := f32(math.Cos(0.35)), f32(ax/l*qs), f32(ay/l*qs), f32(az/l*qs)
		// 4x4 affine M = [R(q)*2 | t] (rows 0..2; the w row is 0 0 0 1)
		r := [3][3]float64{
			{1 - 2*(qy*qy+qz*qz), 2 * (qx*qy - qw*qz), 2 * (qx*qz + qw*qy)},
			{2 * (qx*qy + qw*qz), 1 - 2*(qx*qx+qz*qz), 2 * (qy*qz - qw*qx)},
			{2 * (qx*qz - qw*qy), 2 * (qy*qz + qw*qx), 1 - 2*(qx*qx+qy*qy)},
		}
		tr := [3]float32{1, -2, 3}
		var M [3][4]float32
		for i := 0; i < 3; i++ {
			for j := 0; j < 3; j++ {
				M[i][j] = float32(r[i][j] * 2)
			}
			M[i][3] = tr[i]
		}
		p := make([]vec3, m)
		out := make([]vec3, m)
		for k := 0; k < m; k++ {
			kk := int64(k)
			p[k] = vec3{float32(v(3*kk)*10 - 5), float32(v(3*kk+1)*10 - 5), float32(v(3*kk+2)*10 - 5)}
		}
		note := "1e8 point applications as 100 passes over 1e6 points"
		if smoke {
			note = "smoke size passes=10 in place of 100 (1e7 point applications over 1e6 points)"
		}
		bench("transform-apply", "", "go", "stdlib", int64(m)*int64(passes), note+"; plain Go 4x4 affine matrix (float32); check: |checksum-ref| <= 2e-5*m",
			func() error {
				for ps := 0; ps < passes; ps++ {
					for k, q := range p {
						out[k] = vec3{
							M[0][0]*q.x + M[0][1]*q.y + M[0][2]*q.z + M[0][3],
							M[1][0]*q.x + M[1][1]*q.y + M[1][2]*q.z + M[1][3],
							M[2][0]*q.x + M[2][1]*q.y + M[2][2]*q.z + M[2][3]}
					}
				}
				return nil
			}, func() (bool, error) {
				var cs float64
				for _, e := range out {
					cs += float64(e.x) + float64(e.y) + float64(e.z)
				}
				return math.Abs(cs-1910391.7256280927) <= 2e-5*m, nil
			})
	}
}

func main() {
	dw, dt := 3, 10
	if smoke {
		dw, dt = 1, 3
	}
	warmup = atoi(os.Getenv("PROFILE_WARMUP"), dw)
	trials = atoi(os.Getenv("PROFILE_TRIALS"), dt)
	if trials < 1 {
		trials = 1
	}
	tensorBenches()
	linalgBenches()
	fftBenches()
	randomBenches()
	polyBenches()
	statsBenches()
	geometryBenches()
}
