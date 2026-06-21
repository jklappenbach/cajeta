// Go codec competitors — parse the nativejson corpus to a generic DOM
// (map[string]any) and emit one schema-conformant CSV row per (file, library),
// matching competitors/columns.txt. Libraries: encoding/json (stdlib) and
// goccy/go-json (fastest drop-in). Cross-check: a known element count per file.
package main

import (
	stdjson "encoding/json"
	"fmt"
	"os"
	"runtime"
	"sort"
	"time"

	gojson "github.com/goccy/go-json"
)

type corpus struct {
	dataset string
	fname   string
	expect  int
}

var files = []corpus{
	{"twitter", "twitter.json", 100},
	{"citm_catalog", "citm_catalog.json", 184},
	{"canada", "canada.json", 1},
}

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func peakRSSKb() int64 {
	// Go has no getrusage in std without syscall; read /proc/self/status VmHWM.
	b, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return -1
	}
	for _, line := range splitLines(string(b)) {
		if len(line) > 6 && line[:6] == "VmHWM:" {
			var kb int64
			fmt.Sscanf(line, "VmHWM: %d", &kb)
			return kb
		}
	}
	return -1
}

func splitLines(s string) []string {
	var out []string
	start := 0
	for i := 0; i < len(s); i++ {
		if s[i] == '\n' {
			out = append(out, s[start:i])
			start = i + 1
		}
	}
	return out
}

func count(v map[string]any, dataset string) int {
	switch dataset {
	case "twitter":
		if a, ok := v["statuses"].([]any); ok {
			return len(a)
		}
	case "citm_catalog":
		if o, ok := v["events"].(map[string]any); ok {
			return len(o)
		}
	case "canada":
		if a, ok := v["features"].([]any); ok {
			return len(a)
		}
	}
	return 0
}

type stats struct{ mn, med, mean, p95 int64 }

func computeStats(s []int64) stats {
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
	return stats{s[0], s[n/2], sum / int64(n), s[idx]}
}

func emit(runID, ts, dataset string, bytes int, lib, ver string, warmup, trials int, s stats, checkOK bool, status string) {
	mbps := 0.0
	if s.med > 0 {
		mbps = float64(bytes) / float64(s.med) * 1e9 / 1048576.0
	}
	fmt.Printf(
		"1,%s,%s,json-dom,codec,%s,%d,,%d,go,%s,%s,%s,-gcflags,%d,%d,%d,%d,%d,%d,%.1f,MB/s,%d,-1,-1,-1,-1,%s,%t,,\n",
		runID, ts, dataset, bytes, bytes, env("PROFILE_LANG_VERSION", ""), lib, ver,
		warmup, trials, s.mn, s.med, s.mean, s.p95, mbps, peakRSSKb(), status, checkOK)
}

func main() {
	dataDir := os.Getenv("PROFILE_DATA_DIR")
	if dataDir == "" {
		fmt.Fprintln(os.Stderr, "go/codec: PROFILE_DATA_DIR unset — skipping")
		return
	}
	runID := env("PROFILE_RUN_ID", "local")
	ts := env("PROFILE_RUN_TS", "")
	warmup := atoiDefault(env("PROFILE_WARMUP", "3"), 3)
	trials := atoiDefault(env("PROFILE_TRIALS", "10"), 10)

	for _, c := range files {
		raw, err := os.ReadFile(dataDir + "/" + c.fname)
		if err != nil {
			continue // dataset absent → skip
		}
		n := len(raw)

		// encoding/json (stdlib)
		runOne(runID, ts, c, raw, n, "encoding/json", runtime.Version(), warmup, trials,
			func(b []byte) (int, bool) {
				var v map[string]any
				if err := stdjson.Unmarshal(b, &v); err != nil {
					return 0, false
				}
				return count(v, c.dataset), true
			})

		// goccy/go-json
		runOne(runID, ts, c, raw, n, "goccy/go-json", "v0.10.5", warmup, trials,
			func(b []byte) (int, bool) {
				var v map[string]any
				if err := gojson.Unmarshal(b, &v); err != nil {
					return 0, false
				}
				return count(v, c.dataset), true
			})
	}
}

func runOne(runID, ts string, c corpus, raw []byte, n int, lib, ver string, warmup, trials int,
	parse func([]byte) (int, bool)) {
	cnt, ok := parse(raw)
	check := ok && cnt == c.expect
	for i := 0; i < warmup; i++ {
		parse(raw)
	}
	samples := make([]int64, 0, trials)
	for i := 0; i < trials; i++ {
		t0 := time.Now()
		parse(raw)
		samples = append(samples, time.Since(t0).Nanoseconds())
	}
	status := "ok"
	if !check {
		status = "invalid"
	}
	emit(runID, ts, c.dataset, n, lib, ver, warmup, trials, computeStats(samples), check, status)
}

func atoiDefault(s string, d int) int {
	var v int
	if _, err := fmt.Sscanf(s, "%d", &v); err != nil {
		return d
	}
	return v
}
