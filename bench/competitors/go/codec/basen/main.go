// Go base-n competitor for bench/codec/Base64.md and Base32.md:
// encoding/base64 StdEncoding and encoding/base32 StdEncoding (padded) over
// 1 MiB, data[i] = (i*31+7)&0xFF. Default 1000 trials (the pages' "1e3
// times"); PROFILE_TRIALS overrides. Errors are returned, never panicked.
package main

import (
	"bytes"
	"encoding/base32"
	"encoding/base64"
	"fmt"
	"os"
	"runtime"
)

const size = 1048576

var sink int64

func main() {
	warm := envInt("PROFILE_WARMUP", 3)
	trials := envInt("PROFILE_TRIALS", 1000)
	if trials < 1 {
		trials = 1
	}
	data := make([]byte, size)
	for i := range data {
		data[i] = byte((i*31 + 7) & 0xFF)
	}
	ver := "stdlib " + runtime.Version()
	type codec struct {
		pfx, lib string
		enc      interface {
			EncodedLen(int) int
			Encode([]byte, []byte)
			DecodedLen(int) int
			Decode([]byte, []byte) (int, error)
		}
		wantLen int
	}
	for _, c := range []codec{
		{"base64", "encoding/base64", base64.StdEncoding, 1398104},
		{"base32", "encoding/base32", base32.StdEncoding, 1677728},
	} {
		base := row{lib: c.lib, libVer: ver, dsBytes: size, input: size, warmup: warm, trials: trials,
			notes: "errors returned"}
		// encode: a fresh output buffer per call, as the stdlib's EncodeToString does.
		var enc []byte
		r := base
		r.slug = c.pfx + "-encode"
		st, al, _ := measure(warm, trials, func() error {
			out := make([]byte, c.enc.EncodedLen(len(data)))
			c.enc.Encode(out, data)
			enc = out
			sink += int64(out[len(out)-1])
			return nil
		})
		r.st, r.alloc = st, al
		emitOK(r, len(enc) == c.wantLen)

		r = base
		r.slug = c.pfx + "-decode"
		var dec []byte
		st, al, err := measure(warm, trials, func() error {
			out := make([]byte, c.enc.DecodedLen(len(enc)))
			n, e := c.enc.Decode(out, enc)
			dec = out[:n]
			sink += int64(n)
			return e
		})
		if err != nil {
			emitErr(r, err)
			continue
		}
		r.st, r.alloc = st, al
		emitOK(r, bytes.Equal(dec, data))
	}
	if sink == 42 {
		fmt.Fprintln(os.Stderr, sink)
	}
}
