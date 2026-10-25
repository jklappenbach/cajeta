// Go competitor: unit-5 lang/guid benchmarks (contract: u5-competitor-spec.md).
// github.com/google/uuid v1.6.0; the random pool stays at its default (off),
// so every uuid.New reads crypto/rand.
package main

import (
	"bytes"
	"sort"

	"github.com/google/uuid"
)

const uuidVer = "v1.6.0"

func distinct(ids []uuid.UUID) int64 {
	v := make([]uuid.UUID, len(ids))
	copy(v, ids)
	sort.Slice(v, func(i, j int) bool { return bytes.Compare(v[i][:], v[j][:]) < 0 })
	var n int64
	for k := range v {
		if k == 0 || v[k] != v[k-1] {
			n++
		}
	}
	return n
}

func main() {
	initCtx()
	const N = 1000000

	if selected("guid-generate") {
		ids := make([]uuid.UUID, N) // preallocated 128-bit values
		run("guid-generate", N, "google/uuid", uuidVer, "uuid.New into a preallocated []uuid.UUID; rand pool off", "", func() []uuid.UUID {
			for k := 0; k < N; k++ {
				ids[k] = uuid.New()
			}
			return ids
		}, func(p []uuid.UUID) bool {
			ok := distinct(p) == N // untimed distinct-count check
			clear(ids)
			return ok
		})
	}

	if selected("guid-parse") || selected("guid-format") {
		ids := make([]uuid.UUID, N)
		strs := make([]string, N)
		for k := 0; k < N; k++ {
			ids[k] = uuid.New()
			strs[k] = ids[k].String()
		}
		parsed := make([]uuid.UUID, N)
		run("guid-parse", N, "google/uuid", uuidVer, "uuid.Parse", "", func() []uuid.UUID {
			for k := 0; k < N; k++ {
				u, err := uuid.Parse(strs[k])
				if err != nil {
					continue // leaves a zero value: the check fails
				}
				parsed[k] = u
			}
			return parsed
		}, func(p []uuid.UUID) bool {
			var eq int64
			for k := 0; k < N; k++ {
				if p[k] == ids[k] {
					eq++
				}
			}
			clear(parsed) // the next trial must parse again
			return eq == N
		})

		out := make([]string, N)
		run("guid-format", N, "google/uuid", uuidVer, "UUID.String into a preallocated []string", "", func() []string {
			for k := 0; k < N; k++ {
				out[k] = ids[k].String()
			}
			return out
		}, func(o []string) bool {
			var total int64
			for _, s := range o {
				total += int64(len(s))
			}
			clear(out)
			return total == 36000000
		})
	}
}
