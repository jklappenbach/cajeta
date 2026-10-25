// Go "hello-metric" runner — emits one schema-conformant CSV row (columns.txt
// order). Real per-area Go runners (encoding/json, sonic, goccy/go-json) extend
// this contract in Units 5+.
package main

import (
	"fmt"
	"os"
	"time"
)

func main() {
	runID := os.Getenv("PROFILE_RUN_ID")
	if runID == "" {
		runID = "local"
	}
	ts := os.Getenv("PROFILE_RUN_TS")
	ver := os.Getenv("PROFILE_LANG_VERSION")

	t0 := time.Now()
	var acc uint64
	for i := uint64(0); i < 1000000; i++ {
		acc += i
	}
	ns := time.Since(t0).Nanoseconds()
	_ = acc

	fmt.Printf(
		"1,%s,%s,hello,scaffold,,-1,,1000000,go,%s,stdlib,,-gcflags,0,1,%d,%d,%d,%d,,,-1,-1,-1,-1,-1,ok,true,,\n",
		runID, ts, ver, ns, ns, ns, ns)
}
