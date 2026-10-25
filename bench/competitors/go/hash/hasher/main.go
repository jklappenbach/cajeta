// Go hasher competitors (benchmark-reference unit 8), same inputs and folds as
// bench/src/main/cajeta/bench/hash/, with hash/maphash (the exported form of
// the runtime's map hash; one Seed per run, as a map holds one):
//   defaulthasher-string: 1e7 maphash.String over 1024 keys "k-" + (1e13 + k);
//   defaulthasher-int:    1e8 int64 keys 0 .. 1e8-1, maphash.Bytes of the 8
//                         little-endian bytes (Go 1.22 has no maphash.Comparable;
//                         a native map hashes int64 with runtime memhash64);
//   hash-struct:          1e7 hashes cycling 4096 structs (id int64 = k*7919,
//                         x int32 = k, y int32 = k ^ 0x5bd1), a maphash.Hash
//                         Reset and fed the three fields.
// Fold: acc ^= h; low += h & 0xFFFF. The seed is random per process, so the
// check is a second pass reproducing (acc, low) and neighbours hashing apart.
package main

import (
	"encoding/binary"
	"fmt"
	"hash/maphash"
	"os"
	"runtime"
	"sort"
	"strings"
	"time"
)

type u2 struct{ a, b uint64 }
type key3 struct {
	id   int64
	x, y int32
}

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

func measure(bench string, n int64, notes string, f func() u2, extraOK bool) {
	first := f()
	for w := 0; w < warmup; w++ {
		sink = f()
	}
	s := make([]int64, 0, trials)
	ok := extraOK && first.b > 0
	for t := 0; t < trials; t++ {
		t0 := time.Now()
		r := f()
		s = append(s, time.Since(t0).Nanoseconds())
		sink = r
		ok = ok && r == first
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
	mops := 0.0
	if med > 0 {
		mops = float64(n) / float64(med) * 1e3
	}
	st := "ok"
	if !ok {
		st = "invalid"
	}
	fmt.Printf("1,%s,%s,%s,hash,,%d,,%d,go,%s,maphash,%s,GOAMD64=v3,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%t,%s,\n",
		runID, ts, bench, n, n, ver, runtime.Version(), warmup, trials, s[0], med, sum/int64(k), s[min(k-1, k*95/100)],
		mops, peakRSSKb(), m1.TotalAlloc-m0.TotalAlloc, st, ok, notes)
}

func main() {
	runID, ts, ver = env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", "")
	fmt.Sscanf(env("PROFILE_WARMUP", "3"), "%d", &warmup)
	fmt.Sscanf(env("PROFILE_TRIALS", "10"), "%d", &trials)
	seed := maphash.MakeSeed()

	pool := make([]string, 1024)
	for k := range pool {
		pool[k] = fmt.Sprintf("k-%d", 10000000000000+k)
	}
	sOK := len(pool[0]) == 16 && maphash.String(seed, pool[0]) != maphash.String(seed, pool[1])
	measure("defaulthasher-string", 10000000, "maphash.String", func() u2 {
		var a, l uint64
		for i := 0; i < 10000000; i++ {
			h := maphash.String(seed, pool[i&1023])
			a ^= h
			l += h & 0xFFFF
		}
		return u2{a, l}
	}, sOK)

	hashInt := func(v int64) uint64 {
		var b [8]byte
		binary.LittleEndian.PutUint64(b[:], uint64(v))
		return maphash.Bytes(seed, b[:])
	}
	measure("defaulthasher-int", 100000000, "maphash.Bytes of the 8 LE bytes (Go 1.22: no maphash.Comparable)", func() u2 {
		var a, l uint64
		for i := int64(0); i < 100000000; i++ {
			h := hashInt(i)
			a ^= h
			l += h & 0xFFFF
		}
		return u2{a, l}
	}, hashInt(0) != hashInt(1))

	keys := make([]key3, 4096)
	for k := range keys {
		keys[k] = key3{int64(k) * 7919, int32(k), int32(k) ^ 0x5bd1}
	}
	var mh maphash.Hash
	mh.SetSeed(seed)
	var fb [16]byte
	hashKey := func(p *key3) uint64 {
		mh.Reset()
		binary.LittleEndian.PutUint64(fb[0:8], uint64(p.id))
		binary.LittleEndian.PutUint32(fb[8:12], uint32(p.x))
		binary.LittleEndian.PutUint32(fb[12:16], uint32(p.y))
		mh.Write(fb[:])
		return mh.Sum64()
	}
	kOK := true
	for k := 1; k < len(keys); k++ {
		kOK = kOK && hashKey(&keys[k]) != hashKey(&keys[k-1])
	}
	same := key3{7 * 7919, 7, 7 ^ 0x5bd1}
	kOK = kOK && hashKey(&keys[7]) == hashKey(&same)
	measure("hash-struct", 10000000, "maphash.Hash over the three fields", func() u2 {
		var a, l uint64
		for i := 0; i < 10000000; i++ {
			h := hashKey(&keys[i&4095])
			a ^= h
			l += h & 0xFFFF
		}
		return u2{a, l}
	}, kOK)
}
