// Go CSV competitor for bench/codec/csv/Csv.md: encoding/csv plus strconv
// into typed rows (int64, string, int32, float64, string), and csv.Writer
// back out with "\n" endings and strconv.FormatFloat(p, 'f', -1, 64). Input
// is datasets/gen_csv.py's file in $PROFILE_DATA_DIR (the smoke file under
// CAJETA_BENCH_SMOKE=1). Errors are returned, never panicked.
package main

import (
	"bytes"
	"encoding/csv"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
)

type rec struct {
	id    int64
	name  string
	qty   int32
	price float64
	note  string
}

var sink int64

func parse(b []byte) ([]rec, error) {
	rd := csv.NewReader(bytes.NewReader(b))
	rd.FieldsPerRecord = 5
	rd.ReuseRecord = true
	if _, err := rd.Read(); err != nil { // header
		return nil, err
	}
	out := make([]rec, 0, 1024)
	for {
		f, err := rd.Read()
		if err == io.EOF {
			return out, nil
		}
		if err != nil {
			return nil, err
		}
		id, err := strconv.ParseInt(f[0], 10, 64)
		if err != nil {
			return nil, err
		}
		q, err := strconv.ParseInt(f[2], 10, 32)
		if err != nil {
			return nil, err
		}
		p, err := strconv.ParseFloat(f[3], 64)
		if err != nil {
			return nil, err
		}
		out = append(out, rec{id, f[1], int32(q), p, f[4]})
	}
}

func write(rows []rec) ([]byte, error) {
	var buf bytes.Buffer
	w := csv.NewWriter(&buf)
	w.UseCRLF = false
	if err := w.Write([]string{"id", "name", "qty", "price", "note"}); err != nil {
		return nil, err
	}
	f := make([]string, 5)
	for i := range rows {
		r := &rows[i]
		f[0] = strconv.FormatInt(r.id, 10)
		f[1] = r.name
		f[2] = strconv.FormatInt(int64(r.qty), 10)
		f[3] = strconv.FormatFloat(r.price, 'f', -1, 64)
		f[4] = r.note
		if err := w.Write(f); err != nil {
			return nil, err
		}
	}
	w.Flush()
	return buf.Bytes(), w.Error()
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "go/codec/csv:", err)
		os.Exit(1)
	}
}

func run() error {
	dir := os.Getenv("PROFILE_DATA_DIR")
	if dir == "" {
		return errors.New("PROFILE_DATA_DIR unset")
	}
	scale, note := "full", "errors returned"
	switch os.Getenv("CAJETA_BENCH_SMOKE") {
	case "1", "true", "yes":
		scale, note = "smoke", "errors returned; smoke input 4 MB"
	}
	ds := "codec-csv-" + scale
	warm := envInt("PROFILE_WARMUP", 3)
	trials := envInt("PROFILE_TRIALS", 10)
	if trials < 1 {
		trials = 1
	}
	ver := "stdlib " + runtime.Version()
	base := row{dataset: ds, variant: "", lib: "encoding/csv", libVer: ver, warmup: warm, trials: trials, notes: note}

	b, err := os.ReadFile(filepath.Join(dir, ds+".csv"))
	mb, err2 := os.ReadFile(filepath.Join(dir, ds+".meta"))
	if err != nil || err2 != nil {
		for _, s := range []string{"csv-parse", "csv-write"} {
			r := base
			r.slug = s
			emitSkip(r, ds+".csv absent (run datasets/gen_csv.py)")
		}
		return nil
	}
	var wantRows, wantQty, wantBytes int64
	if _, err := fmt.Sscanf(string(mb), "%d %d %d", &wantRows, &wantQty, &wantBytes); err != nil {
		return fmt.Errorf("bad meta: %w", err)
	}
	base.dsBytes, base.input = int64(len(b)), int64(len(b))

	r := base
	r.slug = "csv-parse"
	var rows []rec
	st, al, err := measure(warm, trials, func() error {
		x, e := parse(b)
		rows = x
		sink += int64(len(x))
		return e
	})
	if err != nil {
		emitErr(r, err)
	} else {
		var q int64
		for i := range rows {
			q += int64(rows[i].qty)
		}
		r.st, r.alloc = st, al
		emitOK(r, int64(len(rows)) == wantRows && q == wantQty)
	}

	r = base
	r.slug = "csv-write"
	typed, err := parse(b)
	if err != nil {
		emitErr(r, err)
		return nil
	}
	var outLen int
	st, al, err = measure(warm, trials, func() error {
		o, e := write(typed)
		outLen = len(o)
		sink += int64(outLen)
		return e
	})
	if err != nil {
		emitErr(r, err)
		return nil
	}
	r.st, r.alloc = st, al
	r.notes += fmt.Sprintf("; out=%d bytes", outLen)
	emitOK(r, int64(outLen) == wantBytes)
	if sink == 42 {
		fmt.Fprintln(os.Stderr, sink)
	}
	return nil
}
