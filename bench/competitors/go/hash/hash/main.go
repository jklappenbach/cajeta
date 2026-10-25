// Go hash competitors (benchmark-reference unit 8): the hash/ ids of
// bench/hash/XXHash3.md and SipHash.md, same inputs, folds and checks as
// bench/src/main/cajeta/bench/hash/:
//   xxhash3 / xxhash3_128 / siphash: 1e7 hashes of a 32-byte key whose first 8
//     bytes are i little-endian (byte j = (j*31+7) & 0xFF);
//   xxhash3-256k / xxhash3_128-256k: 1e4 hashes of a 256 KB buffer, same rule.
// Libraries: zeebo/xxh3 v1.0.2, dchest/siphash v1.2.3 (SipHash-2-4).
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strings"
	"time"

	"github.com/dchest/siphash"
	"github.com/zeebo/xxh3"
)

const (
	nKeys = 10000000
	nBig  = 10000
	big   = 262144
)

type u2 struct{ a, b uint64 }

var sink u2

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
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "VmHWM:") {
			var kb int64
			fmt.Sscanf(l, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

var runID, ts, ver string
var warmup, trials = 3, 10

func measure(bench string, bytes int64, lib, libver, notes string, f func() u2, want u2) {
	for w := 0; w < warmup; w++ {
		sink = f()
	}
	s := make([]int64, 0, trials)
	ok := true
	for t := 0; t < trials; t++ {
		t0 := time.Now()
		r := f()
		s = append(s, time.Since(t0).Nanoseconds())
		sink = r
		ok = ok && r == want
	}
	var m0, m1 runtime.MemStats
	runtime.ReadMemStats(&m0)
	sink = f()
	runtime.ReadMemStats(&m1)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	k := len(s)
	var sum int64
	for _, v := range s {
		sum += v
	}
	med := s[k/2]
	mbps := 0.0
	if med > 0 {
		mbps = float64(bytes) / float64(med) * 1e9 / 1048576.0
	}
	st := "ok"
	if !ok {
		st = "invalid"
	}
	fmt.Printf("1,%s,%s,%s,hash,,%d,,%d,go,%s,%s,%s,GOAMD64=v3,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,%d,-1,-1,%s,%t,%s,\n",
		runID, ts, bench, bytes, bytes, ver, lib, libver, warmup, trials, s[0], med, sum/int64(k), s[min(k-1, k*95/100)],
		mbps, peakRSSKb(), m1.TotalAlloc-m0.TotalAlloc, st, ok, notes)
}

func buf(n int) []byte {
	b := make([]byte, n)
	for j := range b {
		b[j] = byte((j*31 + 7) & 0xFF)
	}
	return b
}

func main() {
	runID, ts, ver = env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", "")
	fmt.Sscanf(env("PROFILE_WARMUP", "3"), "%d", &warmup)
	fmt.Sscanf(env("PROFILE_TRIALS", "10"), "%d", &trials)
	key, bb := buf(32), buf(big)

	measure("xxhash3", nKeys*32, "zeebo/xxh3", "v1.0.2", "xxh3.Hash", func() u2 {
		var a uint64
		for i := uint64(0); i < nKeys; i++ {
			binary.LittleEndian.PutUint64(key, i)
			a ^= xxh3.Hash(key)
		}
		return u2{a, 0}
	}, u2{0x15ae4a024a0e82de, 0})
	measure("xxhash3_128", nKeys*32, "zeebo/xxh3", "v1.0.2", "xxh3.Hash128", func() u2 {
		var lo, hi uint64
		for i := uint64(0); i < nKeys; i++ {
			binary.LittleEndian.PutUint64(key, i)
			h := xxh3.Hash128(key)
			lo ^= h.Lo
			hi ^= h.Hi
		}
		return u2{lo, hi}
	}, u2{0x7f4adb2d9f387c7f, 0xb318fb3ec50537ef})
	measure("xxhash3-256k", nBig*big, "zeebo/xxh3", "v1.0.2", "xxh3.Hash", func() u2 {
		var a uint64
		for i := uint64(0); i < nBig; i++ {
			binary.LittleEndian.PutUint64(bb, i)
			a ^= xxh3.Hash(bb)
		}
		return u2{a, 0}
	}, u2{0x4f52c915522f9fe0, 0})
	measure("xxhash3_128-256k", nBig*big, "zeebo/xxh3", "v1.0.2", "xxh3.Hash128", func() u2 {
		var lo, hi uint64
		for i := uint64(0); i < nBig; i++ {
			binary.LittleEndian.PutUint64(bb, i)
			h := xxh3.Hash128(bb)
			lo ^= h.Lo
			hi ^= h.Hi
		}
		return u2{lo, hi}
	}, u2{0x4f52c915522f9fe0, 0x3c843c6da63e2dc3})
	const k0, k1 = uint64(0x0706050403020100), uint64(0x0f0e0d0c0b0a0908)
	vec := make([]byte, 15)
	for j := range vec {
		vec[j] = byte(j)
	}
	vecOK := siphash.Hash(k0, k1, vec) == 0xa129ca6149be45e5
	measure("siphash", nKeys*32, "dchest/siphash", "v1.2.3", "siphash.Hash (key 00..0f)", func() u2 {
		var a uint64
		for i := uint64(0); i < nKeys; i++ {
			binary.LittleEndian.PutUint64(key, i)
			a ^= siphash.Hash(k0, k1, key)
		}
		if !vecOK {
			a = 0
		}
		return u2{a, 0}
	}, u2{0x1a6999cca3276432, 0})
}
