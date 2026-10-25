// Go JSON competitor for bench/codec/json/Json.md: the six codec/json-* ids
// with encoding/json (stdlib), on the nativejson corpus and the JSON_checker
// set in $PROFILE_DATA_DIR. Contract: the unit-9 competitor contract (one v1
// row per id and variant; competitors/lib/rows.sh lifts it to v2). Errors are
// returned, never panicked; a workload error makes the row invalid.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
)

type corpus struct {
	name          string
	tokens, nodes int64
}

var files = []corpus{
	{"twitter", 29573, 13914},
	{"citm_catalog", 85035, 37778},
	{"canada", 223236, 167179},
}

const errNote = "errors returned"

var sink int64

func nodes(v any) int64 {
	switch x := v.(type) {
	case map[string]any:
		n := int64(1)
		for _, c := range x {
			n += nodes(c)
		}
		return n
	case []any:
		n := int64(1)
		for _, c := range x {
			n += nodes(c)
		}
		return n
	default:
		return 1
	}
}

func tokenize(b []byte) (int64, error) {
	d := json.NewDecoder(bytes.NewReader(b))
	var n int64
	for {
		_, err := d.Token()
		if err == io.EOF {
			return n, nil
		}
		if err != nil {
			return n, err
		}
		n++
	}
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "go/codec/json:", err)
		os.Exit(1)
	}
}

func run() error {
	dir := os.Getenv("PROFILE_DATA_DIR")
	if dir == "" {
		return errors.New("PROFILE_DATA_DIR unset")
	}
	warm := envInt("PROFILE_WARMUP", 3)
	trials := envInt("PROFILE_TRIALS", 10)
	if trials < 1 {
		trials = 1
	}
	lib, ver := "encoding/json", "stdlib "+runtime.Version()

	for _, f := range files {
		b, err := os.ReadFile(filepath.Join(dir, f.name+".json"))
		if err != nil {
			continue // corpus absent: the driver records the gap
		}
		n := int64(len(b))
		base := row{dataset: f.name, variant: f.name, lib: lib, libVer: ver, dsBytes: n, input: n,
			warmup: warm, trials: trials, notes: errNote}

		// json-tokenize: Decoder.Token pulls every delimiter, key and scalar.
		{
			r := base
			r.slug = "json-tokenize"
			var cnt int64
			st, al, err := measure(warm, trials, func() error {
				c, e := tokenize(b)
				cnt = c
				sink += c
				return e
			})
			if err != nil {
				emitErr(r, err)
			} else {
				r.st, r.alloc = st, al
				r.notes = fmt.Sprintf("%s; tokens=%d", errNote, cnt)
				emitOK(r, cnt == f.tokens)
			}
		}
		// json-bind-skip: Unmarshal into a struct with no fields.
		{
			r := base
			r.slug = "json-bind-skip"
			st, al, err := measure(warm, trials, func() error {
				var e struct{}
				return json.Unmarshal(b, &e)
			})
			if err != nil {
				emitErr(r, err)
			} else {
				r.st, r.alloc = st, al
				r.notes = errNote + "; Unmarshal into struct{}"
				emitOK(r, true)
			}
		}
		// json-dom: Unmarshal into any.
		{
			r := base
			r.slug = "json-dom"
			var last any
			st, al, err := measure(warm, trials, func() error {
				var v any
				e := json.Unmarshal(b, &v)
				last = v
				return e
			})
			if err != nil {
				emitErr(r, err)
			} else {
				r.st, r.alloc = st, al
				c := nodes(last)
				r.notes = fmt.Sprintf("%s; nodes=%d", errNote, c)
				emitOK(r, c == f.nodes)
			}
		}
		// json-serialize: Marshal a tree built untimed.
		{
			r := base
			r.slug = "json-serialize"
			var tree any
			if err := json.Unmarshal(b, &tree); err != nil {
				emitErr(r, err)
			} else {
				var out []byte
				st, al, err := measure(warm, trials, func() error {
					o, e := json.Marshal(tree)
					out = o
					sink += int64(len(o))
					return e
				})
				if err != nil {
					emitErr(r, err)
				} else {
					r.st, r.alloc = st, al
					var back any
					ok := json.Unmarshal(out, &back) == nil && nodes(back) == f.nodes
					r.notes = fmt.Sprintf("%s; out=%d bytes", errNote, len(out))
					emitOK(r, ok)
				}
			}
		}
		// json-roundtrip: parse -> serialize -> parse; deep equality.
		{
			r := base
			r.slug = "json-roundtrip"
			var v1, v2 any
			st, al, err := measure(warm, trials, func() error {
				var a, c any
				if e := json.Unmarshal(b, &a); e != nil {
					return e
				}
				o, e := json.Marshal(a)
				if e != nil {
					return e
				}
				if e := json.Unmarshal(o, &c); e != nil {
					return e
				}
				v1, v2 = a, c
				return nil
			})
			if err != nil {
				emitErr(r, err)
			} else {
				r.st, r.alloc = st, al
				r.notes = errNote + "; reflect.DeepEqual"
				emitOK(r, reflect.DeepEqual(v1, v2))
			}
		}
	}

	// json-conformance: the 34 JSON_checker files, held in memory.
	type tc struct {
		name   string
		accept bool
		b      []byte
	}
	var cases []tc
	for i := 1; i <= 3; i++ {
		nm := fmt.Sprintf("pass%02d", i)
		if b, err := os.ReadFile(filepath.Join(dir, "jsonchecker_"+nm+".json")); err == nil {
			cases = append(cases, tc{nm, true, b})
		}
	}
	for i := 2; i <= 33; i++ {
		if i == 18 {
			continue
		}
		nm := fmt.Sprintf("fail%02d", i)
		if b, err := os.ReadFile(filepath.Join(dir, "jsonchecker_"+nm+".json")); err == nil {
			cases = append(cases, tc{nm, false, b})
		}
	}
	r := row{slug: "json-conformance", dataset: "jsonchecker", variant: "", lib: lib, libVer: ver,
		dsBytes: int64(len(cases)), input: int64(len(cases)), warmup: warm, trials: trials, timeOnly: true}
	if len(cases) != 34 {
		emitSkip(r, fmt.Sprintf("jsonchecker set incomplete in PROFILE_DATA_DIR (%d of 34 files; run datasets/fetch.sh)", len(cases)))
		return nil
	}
	verdict := make([]bool, len(cases))
	st, al, _ := measure(warm, trials, func() error {
		for i, c := range cases {
			var v any
			verdict[i] = json.Unmarshal(c.b, &v) == nil
		}
		return nil
	})
	acc, rej := 0, 0
	var mis []string
	for i, c := range cases {
		switch {
		case c.accept && verdict[i]:
			acc++
		case !c.accept && !verdict[i]:
			rej++
		default:
			mis = append(mis, c.name)
		}
	}
	r.st, r.alloc = st, al
	r.notes = fmt.Sprintf("pass %d/3 fail %d/31", acc, rej)
	if len(mis) > 0 {
		r.notes += "; misclassified " + strings.Join(mis, " ")
	}
	emitOK(r, acc == 3 && rej == 31)
	if sink == 42 {
		fmt.Fprintln(os.Stderr, sink)
	}
	return nil
}
