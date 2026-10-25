// Go wire/ competitor (bench/wire/CompressStream.md): compressstream-roundtrip
// through compress/flate (raw DEFLATE, the standard library) and
// github.com/klauspost/compress/zstd. Data and constants from
// bench/scripts/gen_wire.py (expect.tsv at $WIRE_EXPECT). Errors are returned,
// never panicked: a failed stream is an invalid row with the error in notes.
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"math"
	"os"
	"strconv"
	"strings"

	"compress/flate"

	"github.com/klauspost/compress/zstd"
)

const pool = 1024

func expect() (map[string]int64, error) {
	b, err := os.ReadFile(env("WIRE_EXPECT", "expect.tsv"))
	if err != nil {
		return nil, err
	}
	m := map[string]int64{}
	for _, l := range strings.Split(string(b), "\n") {
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		p := strings.Split(l, "\t")
		v, err := strconv.ParseInt(p[1], 10, 64)
		if err != nil {
			return nil, err
		}
		m[p[0]] = v
	}
	return m, nil
}

func streamData(size int) []byte {
	b := make([]byte, 0, size+720)
	var t [8]byte
	for r := 0; len(b) < size; r++ {
		p := int64(r % pool)
		for k := int64(1); k <= 100; k++ {
			switch {
			case k%5 == 0:
				binary.LittleEndian.PutUint64(t[:], math.Float64bits(float64(p)*0.5+float64(k)*0.25))
				b = append(b, t[:8]...)
			case k%5 == 1:
				binary.LittleEndian.PutUint32(t[:], uint32(int32((p*31+k*17)%65536)))
				b = append(b, t[:4]...)
			default:
				binary.LittleEndian.PutUint64(t[:], uint64((p+1)*(k*k*k+17)))
				b = append(b, t[:8]...)
			}
		}
	}
	return b[:size]
}

type flusher interface {
	io.Writer
	Flush() error
	Close() error
}

// roundtrip writes data in 64 KiB pieces, flushing every 1 MiB, closes, then reads it all back.
func roundtrip(data []byte, mk func(io.Writer) (flusher, error), rd func(io.Reader) (io.Reader, func(), error)) ([]byte, int, error) {
	const piece, flushEvery = 65536, 1 << 20
	var comp bytes.Buffer
	comp.Grow(len(data) / 2)
	w, err := mk(&comp)
	if err != nil {
		return nil, 0, err
	}
	for off := 0; off < len(data); off += piece {
		n := min(piece, len(data)-off)
		if _, err := w.Write(data[off : off+n]); err != nil {
			return nil, 0, err
		}
		if (off+n)%flushEvery == 0 && off+n < len(data) {
			if err := w.Flush(); err != nil {
				return nil, 0, err
			}
		}
	}
	if err := w.Close(); err != nil {
		return nil, 0, err
	}
	clen := comp.Len()
	r, done, err := rd(&comp)
	if err != nil {
		return nil, 0, err
	}
	defer done()
	out := bytes.NewBuffer(make([]byte, 0, len(data)))
	if _, err := io.Copy(out, r); err != nil {
		return nil, 0, err
	}
	return out.Bytes(), clen, nil
}

func run() error {
	smoke := env("CAJETA_BENCH_SMOKE", "") == "1"
	tag := "full"
	size := 64 << 20
	ssz := ""
	if smoke {
		tag, size, ssz = "smoke", 8<<20, "; smoke size 8 MiB in place of 64 MiB"
	}
	warmup := envInt("PROFILE_WARMUP", 2)
	trials := max(1, envInt("PROFILE_TRIALS", 5))
	ex, err := expect()
	if err != nil {
		return err
	}
	data := streamData(size)
	crcOK := int64(crc32.ChecksumIEEE(data)) == ex["stream_crc32_"+tag]

	cases := []struct {
		variant, lib, ver, api string
		mk                     func(io.Writer) (flusher, error)
		rd                     func(io.Reader) (io.Reader, func(), error)
	}{
		{"zlib", "compress/flate", goVersion(), "flate.NewWriter(level 1; raw DEFLATE); Flush (sync flush) every 1 MiB; Close; flate.NewReader + io.Copy",
			func(w io.Writer) (flusher, error) { return flate.NewWriter(w, 1) },
			func(r io.Reader) (io.Reader, func(), error) { fr := flate.NewReader(r); return fr, func() { fr.Close() }, nil }},
		{"zstd", "klauspost/compress/zstd", "v1.17.11", "zstd.NewWriter(SpeedFastest = level 1); Flush every 1 MiB; Close; zstd.NewReader + io.Copy",
			func(w io.Writer) (flusher, error) {
				return zstd.NewWriter(w, zstd.WithEncoderLevel(zstd.SpeedFastest), zstd.WithEncoderConcurrency(1))
			},
			func(r io.Reader) (io.Reader, func(), error) {
				d, err := zstd.NewReader(r, zstd.WithDecoderConcurrency(1))
				if err != nil {
					return nil, nil, err
				}
				return d, d.Close, nil
			}},
	}
	for _, c := range cases {
		var back []byte
		var clen int
		st, alloc, err := measure(warmup, trials, func() error {
			var e error
			back, clen, e = roundtrip(data, c.mk, c.rd)
			return e
		})
		r := row{slug: "compressstream-roundtrip", variant: c.variant, lib: c.lib, libVer: c.ver, dsBytes: -1,
			input: int64(size), warmup: warmup, trials: trials, st: st, alloc: alloc}
		r.notes = fmt.Sprintf("%s; 64 KiB writes; single-goroutine encoder/decoder; compressed %d B; check: byte equality; errors returned (Go)%s", c.api, clen, ssz)
		if err != nil {
			emitErr(r, err)
			continue
		}
		emitOK(r, crcOK && bytes.Equal(back, data))
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "wire:", err)
		os.Exit(1)
	}
}
