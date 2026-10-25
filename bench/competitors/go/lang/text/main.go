// Go competitor: unit-5 lang/text benchmarks (contract: u5-competitor-spec.md).
// Every text is built once, untimed.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

type cpResult struct{ count, sum int64 }

var sinkStr string

func main() {
	initCtx()

	if selected("string-codepoint-walk") {
		base := "Hello, wörld! Καλημέρα κόσμε, こんにちは世界 \U0001F680✨ "
		if len(base) != 74 {
			fmt.Fprintf(os.Stderr, "base size %d != 74\n", len(base))
			os.Exit(1)
		}
		text := strings.Repeat(base, 14170)
		run("string-codepoint-walk", int64(len(text)), "for range string", "stdlib", "", "", func() cpResult {
			var r cpResult
			for _, c := range text {
				r.count++
				r.sum += int64(c)
			}
			return r
		}, func(r cpResult) bool { return r.count == 580970 && r.sum == 3748489290 })
	}

	if selected("string-byteat-scan") {
		text := []byte(strings.Repeat("the quick brown fox jumps over the lazy dog\n", 23832))
		run("string-byteat-scan", int64(len(text)), "[]byte index", "stdlib", "", "", func() int64 {
			var c int64
			for k := 0; k < len(text); k++ {
				if text[k] == '\n' {
					c++
				}
			}
			return c
		}, func(c int64) bool { return c == 23832 })
	}

	if selected("string-split") {
		text := strings.Repeat("alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa,", 18397) + "end"
		run("string-split", 100*int64(len(text)), "strings.Split", "stdlib", "", "", func() int64 {
			var total int64
			for r := 0; r < 100; r++ {
				total += int64(len(strings.Split(text, ",")))
			}
			return total
		}, func(t int64) bool { return t == 18397100 })
	}

	if selected("string-compare") {
		var a, b [64]string
		for k := 0; k < 64; k++ {
			s := "abcdefghijklmnopqrstuvwxyz0123" + string([]byte{byte('0' + k/10), byte('0' + k%10)})
			a[k] = s
			bs := []byte(s) // a fresh copy
			if k&1 == 1 {
				bs[len(bs)-1] = 'x'
			}
			b[k] = string(bs)
		}
		run("string-compare", 10000000, "string ==", "stdlib", "", "", func() int64 {
			var eq int64
			for i := int64(0); i < 10000000; i++ {
				if a[i&63] == b[i&63] {
					eq++
				}
			}
			return eq
		}, func(e int64) bool { return e == 5000000 })
	}

	run("stringbuilder-append", 1000000, "strings.Builder", "stdlib", "", "", func() int64 {
		var sb strings.Builder
		for i := 0; i < 1000000; i++ {
			sb.WriteString("0123456789abcdef")
		}
		sinkStr = sb.String()
		return int64(len(sinkStr))
	}, func(n int64) bool { return n == 16000000 })

	run("stringbuilder-append-mixed", 1000000, "strings.Builder + strconv.Append*", "stdlib",
		"AppendInt / AppendFloat('f' -1 64) into a scratch buffer then Write", "", func() int64 {
			var sb strings.Builder
			buf := make([]byte, 0, 64)
			for i := int32(0); i < 1000000; i++ {
				switch i % 3 {
				case 0:
					buf = strconv.AppendInt(buf[:0], int64(i), 10)
					sb.Write(buf)
				case 1:
					buf = strconv.AppendFloat(buf[:0], float64(i%1000)+0.5, 'f', -1, 64)
					sb.Write(buf)
				default:
					sb.WriteString("abc")
				}
			}
			sinkStr = sb.String()
			return int64(len(sinkStr))
		}, func(n int64) bool { return n == 4592962 })

	if selected("stringbuilder-tostring") {
		var builder strings.Builder
		for r := 0; r < 1048576; r++ {
			builder.WriteString("0123456789abcdef")
		}
		run("stringbuilder-tostring", 100, "strings.Builder.String", "stdlib", "zero-copy: no bytes are copied", "", func() int64 {
			var total int64
			for r := 0; r < 100; r++ {
				sinkStr = builder.String()
				total += int64(len(sinkStr))
			}
			return total
		}, func(t int64) bool { return t == 1677721600 })
	}
	sinkStr = ""
}
