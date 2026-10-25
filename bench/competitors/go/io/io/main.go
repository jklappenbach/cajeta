// io/ competitor, Go (bench/io/*.md): []byte, os, bufio, path/filepath,
// fsnotify, net with the peer on a goroutine (a goroutine per connection in
// the Server rows), encoding/binary, sync.Pool and net/url. Sizes, inputs,
// ports (61500+) and checks are those of competitors/drivers/io/io.sh; every
// row's notes carry check=<value>. Workloads return error (an invalid row
// with the error), never panic. Usage: io_bench [id ...].
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"runtime/debug"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/fsnotify/fsnotify"
)

const portBase = 61500
const noteErr = "errors returned as error"

var only = map[string]bool{}
var dir, scale, dev string

func want(id string) bool { return len(only) == 0 || only[id] }
func p(n string) string  { return dir + "/" + n }
func pidNote(slot int) string {
	return fmt.Sprintf("; port %d; pid %d", portBase+slot, os.Getpid())
}
func seqBytes() int64  { return pick(1073741824, 67108864) }
func lineCount() int64 { return pick(10000000, 1000000) }

var sink int64

// ------------------------------------------------------------ buffer
func bufferRows() {
	const N = 16777216
	if want("buffer-fill") {
		passes := pick(100, 4)
		var exp, acc int64
		for q := int64(0); q < passes; q++ {
			exp += ((q*7919)%N + q) & 255
		}
		exp += (N / 256) * 32640
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			d := make([]byte, N)
			var a int64
			for q := int64(0); q < passes; q++ {
				for i := range d {
					d[i] = byte((int64(i) + q) & 255)
				}
				a += int64(d[(q*7919)%N])
			}
			for _, b := range d {
				a += int64(b)
			}
			acc = a
			return nil
		})
		emit("buffer-fill", N*passes/1048576, "[]byte", "", w, t, st, acc == exp,
			fmt.Sprintf("check=%d; 16 MiB x %d passes%s", acc, passes, sm("100 passes")), "MB/s")
	}
	if want("buffer-read-primitives") {
		reads := pick(50000000, 5000000)
		d := make([]byte, N+8)
		for k := uint32(0); k < 2097152; k++ {
			binary.LittleEndian.PutUint32(d[k*4:], k*2654435761)
		}
		for k := 0; k < 1048576; k++ {
			binary.LittleEndian.PutUint64(d[8388608+k*8:], math.Float64bits(float64(k&1023)*0.5))
		}
		var ei int64
		var ef float64
		for j := int64(0); j < reads; j++ {
			ei += int64(int32(uint32(j&2097151) * 2654435761))
			ef += float64((j&1048575)&1023) * 0.5
		}
		var si int64
		var sf float64
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			var a int64
			var b float64
			for j := int64(0); j < reads; j++ {
				a += int64(int32(binary.LittleEndian.Uint32(d[(j&2097151)*4:])))
				b += math.Float64frombits(binary.LittleEndian.Uint64(d[8388608+(j&1048575)*8:]))
			}
			si, sf = a, b
			return nil
		})
		emit("buffer-read-primitives", reads*2, "[]byte + encoding/binary", "", w, t, st, si == ei && sf == ef,
			fmt.Sprintf("check=%d/%d; %d values%s", si, int64(sf*2), reads*2, sm("1e8")), "values/s")
	}
	if want("buffer-slice") {
		views := pick(10000000, 1000000)
		span := int64(N - 64)
		d := make([]byte, N)
		for i := range d {
			d[i] = byte((i*31 + 7) & 255)
		}
		var exp, sum int64
		for j := int64(0); j < views; j++ {
			exp += (((j * 4099) % span) * 31 + 7) & 255
		}
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			var a int64
			for j := int64(0); j < views; j++ {
				s := (j * 4099) % span
				v := d[s : s+64]
				a += int64(v[0])
			}
			sum = a
			return nil
		})
		emit("buffer-slice", views, "[]byte", "", w, t, st, sum == exp,
			fmt.Sprintf("check=%d; %d views of 64 bytes%s", sum, views, sm("1e7")), "views/s")
	}
}

// ------------------------------------------------------------ files
func fileRows() {
	dv := "; device " + dev
	mib := seqBytes() / 1048576
	seq := p("io-seq-" + scale + ".bin")
	if want("file-read-sequential") {
		buf := make([]byte, 1048576)
		var bytes, sum int64
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			f, err := os.Open(seq)
			if err != nil {
				return err
			}
			defer f.Close()
			var pos, a int64
			for {
				n, err := f.Read(buf)
				for i := (4096 - (pos & 4095)) & 4095; i < int64(n); i += 4096 {
					a += int64(buf[i])
				}
				pos += int64(n)
				if err == io.EOF {
					break
				}
				if err != nil {
					return err
				}
			}
			bytes, sum = pos, a
			return nil
		})
		if err != nil {
			emitErr("file-read-sequential", "os.File", err)
		} else {
			emit("file-read-sequential", mib, "os.File", "", w, t, st, bytes == seqBytes() && sum == mib*32640,
				fmt.Sprintf("check=%d/%d%s; %d MiB in 1 MiB reads%s; page cache warm (no drop without root); %s", bytes, sum, dv, mib, sm("1 GiB"), noteErr), "MB/s")
		}
	}
	if want("file-write-sequential") {
		chunk := make([]byte, 1048576)
		for j := range chunk {
			chunk[j] = byte(((j >> 12) * 31 + j) & 255)
		}
		out := p("io-write.bin")
		var size int64
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			f, err := os.Create(out)
			if err != nil {
				return err
			}
			for k := int64(0); k < mib; k++ {
				if _, err := f.Write(chunk); err != nil {
					f.Close()
					return err
				}
			}
			if err := f.Sync(); err != nil {
				f.Close()
				return err
			}
			if err := f.Close(); err != nil {
				return err
			}
			fi, err := os.Stat(out)
			if err != nil {
				return err
			}
			size = fi.Size()
			return nil
		})
		if err != nil {
			emitErr("file-write-sequential", "os.File", err)
		} else {
			emit("file-write-sequential", mib, "os.File", "", w, t, st, size == seqBytes(),
				fmt.Sprintf("check=%d%s; %d MiB in 1 MiB writes + fsync%s; %s", size, dv, mib, sm("1 GiB"), noteErr), "MB/s")
		}
	}
	if want("file-random-read") {
		reads := pick(100000, 10000)
		blocks := uint64(seqBytes() / 4096)
		x := uint64(42)
		var exp, sum int64
		for r := int64(0); r < reads; r++ {
			x = x*6364136223846793005 + 1442695040888963407
			m := int64((x >> 33) % blocks)
			exp += ((m * 31) & 255) + ((m*31 + 4095) & 255)
		}
		buf := make([]byte, 4096)
		w, t := warm(2), trials(10)
		st, err := measure(w, t, func() error {
			f, err := os.Open(seq)
			if err != nil {
				return err
			}
			defer f.Close()
			y := uint64(42)
			var a int64
			for r := int64(0); r < reads; r++ {
				y = y*6364136223846793005 + 1442695040888963407
				m := int64((y >> 33) % blocks)
				if _, err := f.ReadAt(buf, m*4096); err != nil {
					return err
				}
				a += int64(buf[0]) + int64(buf[4095])
			}
			sum = a
			return nil
		})
		if err != nil {
			emitErr("file-random-read", "os.File", err)
		} else {
			emit("file-random-read", reads, "os.File ReadAt", "", w, t, st, sum == exp,
				fmt.Sprintf("check=%d%s; %d reads of 4 KiB over %d MiB%s; %s", sum, dv, reads, mib, sm("1e5 over 1 GiB"), noteErr), "reads/s")
		}
	}
	if want("file-stat") {
		n := pick(1000000, 100000)
		sp := p("io-stat.bin")
		var sum int64
		w, t := warm(2), trials(10)
		st, err := measure(w, t, func() error {
			var a int64
			for i := int64(0); i < n; i++ {
				fi, err := os.Stat(sp)
				if err != nil {
					return err
				}
				a += fi.Size()
			}
			sum = a
			return nil
		})
		if err != nil {
			emitErr("file-stat", "os.Stat", err)
		} else {
			emit("file-stat", n, "os.Stat", "", w, t, st, sum == n*4096,
				fmt.Sprintf("check=%d%s; %d size queries%s; %s", sum, dv, n, sm("1e6"), noteErr), "stats/s")
		}
	}
	if want("filereader-lines") {
		lp := p("io-lines-" + scale + ".txt")
		var lines int64
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			f, err := os.Open(lp)
			if err != nil {
				return err
			}
			defer f.Close()
			sc := bufio.NewScanner(f)
			var c int64
			for sc.Scan() {
				c++
			}
			lines = c
			return sc.Err()
		})
		if err != nil {
			emitErr("filereader-lines", "bufio.Scanner", err)
		} else {
			emit("filereader-lines", lineCount(), "bufio.Scanner", "", w, t, st, lines == lineCount(),
				fmt.Sprintf("check=%d%s; %d lines of 50 bytes%s; %s", lines, dv, lineCount(), sm("1e7"), noteErr), "lines/s")
		}
	}
	if want("filewriter-lines") {
		op := p("io-lines-out.txt")
		n := lineCount()
		var size int64
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			f, err := os.Create(op)
			if err != nil {
				return err
			}
			bw := bufio.NewWriter(f)
			var ln [50]byte
			for k := int64(0); k < n; k++ {
				v := k
				for d := 9; d >= 0; d-- {
					ln[d] = byte('0' + v%10)
					v /= 10
				}
				c := byte('a' + k%26)
				for i := 10; i < 49; i++ {
					ln[i] = c
				}
				ln[49] = '\n'
				if _, err := bw.Write(ln[:]); err != nil {
					f.Close()
					return err
				}
			}
			if err := bw.Flush(); err != nil {
				f.Close()
				return err
			}
			if err := f.Close(); err != nil {
				return err
			}
			fi, err := os.Stat(op)
			if err != nil {
				return err
			}
			size = fi.Size()
			return nil
		})
		if err != nil {
			emitErr("filewriter-lines", "bufio.Writer", err)
		} else {
			emit("filewriter-lines", n, "bufio.Writer", "", w, t, st, size == n*50,
				fmt.Sprintf("check=%d%s; %d lines%s; %s", size, dv, n, sm("1e7"), noteErr), "lines/s")
		}
	}
	if want("directory-walk") {
		root := p("tree-" + scale)
		depth := 3
		if smoke() {
			depth = 2
		}
		var dirs, lvl int64 = 0, 1
		for l := 0; l < depth; l++ {
			lvl *= 10
			dirs += lvl
		}
		per := dirs + (dirs+1)*5
		var entries int64
		w, t := warm(2), trials(10)
		st, err := measure(w, t, func() error {
			var c int64
			for r := 0; r < 10; r++ {
				err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
					if err != nil {
						return err
					}
					if path != root {
						c++
					}
					return nil
				})
				if err != nil {
					return err
				}
			}
			entries = c
			return nil
		})
		tree := ""
		if smoke() {
			tree = " (smoke tree: depth 2; the page's depth-3 tree is the full run)"
		}
		if err != nil {
			emitErr("directory-walk", "filepath.WalkDir", err)
		} else {
			emit("directory-walk", per*10, "filepath.WalkDir", "", w, t, st, entries == per*10,
				fmt.Sprintf("check=%d%s; %d entries x 10%s; %s", entries, dv, per, tree, noteErr), "entries/s")
		}
	}
}

// ------------------------------------------------------------ path
func pathRows() {
	if !want("path-join-normalize") {
		return
	}
	n := pick(10000000, 1000000)
	var A [16]string
	var B [8]string
	var C [32]string
	for k := range A {
		A[k] = "d" + strconv.Itoa(k)
	}
	for k := range B {
		if k%2 == 0 {
			B[k] = "../e" + strconv.Itoa(k)
		} else {
			B[k] = "f" + strconv.Itoa(k) + "/.."
		}
	}
	for k := range C {
		C[k] = "file" + strconv.Itoa(k) + ".txt"
	}
	var exp, sum int64
	for i := int64(0); i < n; i++ {
		seg := int64(2)
		if (i&7)%2 != 0 && (i&15) >= 10 {
			seg = 3
		}
		f := int64(10)
		if (i & 31) < 10 {
			f = 9
		}
		exp += 9 + seg + 1 + f
	}
	w, t := warm(2), trials(10)
	st, _ := measure(w, t, func() error {
		var a int64
		for i := int64(0); i < n; i++ {
			a += int64(len(filepath.Join("/srv/app", A[i&15], B[i&7], C[i&31])))
		}
		sum = a
		return nil
	})
	emit("path-join-normalize", n, "filepath.Join", "", w, t, st, sum == exp,
		fmt.Sprintf("check=%d; %d joins%s; Join cleans (normalizes) the result", sum, n, sm("1e7")), "joins/s")
}

// ------------------------------------------------------------ watcher
func watcherRows() {
	if !want("watcher-event-latency") {
		return
	}
	n := int(pick(1000, 100))
	wd := p("watch-go")
	os.RemoveAll(wd)
	err := func() error {
		if err := os.MkdirAll(wd, 0o755); err != nil {
			return err
		}
		wt, err := fsnotify.NewWatcher()
		if err != nil {
			return err
		}
		defer wt.Close()
		if err := wt.Add(wd); err != nil {
			return err
		}
		lat := make([]int64, 0, n)
		for k := 0; k < n; k++ {
			target := fmt.Sprintf("%s/f%d", wd, k)
			t0 := time.Now()
			f, err := os.Create(target)
			if err != nil {
				return err
			}
			f.Close()
			timeout := time.After(2 * time.Second)
		wait:
			for {
				select {
				case ev := <-wt.Events:
					if ev.Has(fsnotify.Create) && ev.Name == target {
						lat = append(lat, time.Since(t0).Nanoseconds())
						break wait
					}
				case err := <-wt.Errors:
					return err
				case <-timeout:
					break wait
				}
			}
		}
		st := stats{}
		if len(lat) > 0 {
			st = statsOf(lat)
		}
		emit("watcher-event-latency", int64(n), "fsnotify", fsnotifyVersion(), 0, len(lat), st, len(lat) == n,
			fmt.Sprintf("check=%d; %d file creations; median create-to-event latency%s; inotify backend; %s", len(lat), n, sm("1e3"), noteErr), "")
		return nil
	}()
	os.RemoveAll(wd)
	if err != nil {
		emitErr("watcher-event-latency", "fsnotify", err)
	}
}

// ------------------------------------------------------------ binary, pool
func bytesRows() {
	if want("bytebuffer-put-get") {
		pairs := pick(25000000, 2500000)
		var exp, sum int64
		for j := int64(0); j < pairs; j++ {
			exp += int64(int32(uint32(uint64(j)*2654435761))) + int64(uint64(j)*0x9E3779B97F4A7C15)
		}
		buf := make([]byte, 65536)
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			var a int64
			for j := int64(0); j < pairs; {
				end := j + 5000
				if end > pairs {
					end = pairs
				}
				o := 0
				for k := j; k < end; k++ {
					binary.BigEndian.PutUint32(buf[o:], uint32(uint64(k)*2654435761))
					binary.BigEndian.PutUint64(buf[o+4:], uint64(k)*0x9E3779B97F4A7C15)
					o += 12
				}
				o = 0
				for k := j; k < end; k++ {
					a += int64(int32(binary.BigEndian.Uint32(buf[o:]))) + int64(binary.BigEndian.Uint64(buf[o+4:]))
					o += 12
				}
				j = end
			}
			sum = a
			return nil
		})
		emit("bytebuffer-put-get", pairs*4, "encoding/binary BigEndian", "", w, t, st, sum == exp,
			fmt.Sprintf("check=%d; %d int32+int64 pairs put and got (big-endian)%s", sum, pairs, sm("1e8 ops")), "ops/s")
	}
	if want("bufferpool-acquire-release") {
		n := pick(10000000, 1000000)
		var allocs atomic.Int64
		pool := sync.Pool{New: func() any { allocs.Add(1); b := make([]byte, 0, 65536); return &b }}
		var good int64
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			var g int64
			for i := int64(0); i < n; i++ {
				bp := pool.Get().(*[]byte)
				b := (*bp)[:0]
				x := byte(i & 127)
				b = append(b, x)
				if cap(b) == 65536 && b[0] == x {
					g++
				}
				*bp = b
				pool.Put(bp)
			}
			good = g
			return nil
		})
		emit("bufferpool-acquire-release", n, "sync.Pool", "", w, t, st, good == n,
			fmt.Sprintf("check=%d; allocations=%d; %d acquire/release pairs%s; *[]byte in a sync.Pool (GC may drop idle buffers)", good, allocs.Load(), n, sm("1e7")), "acquires/s")
	}
}

// ------------------------------------------------------------ net
func lo(port int) string { return "127.0.0.1:" + strconv.Itoa(port) }

func netRows() {
	if want("tcp-echo-latency") {
		rounds := int(pick(100000, 1000))
		port := portBase
		rtt := make([]int64, rounds)
		var okc int
		w, t := warm(1), trials(3)
		st, err := measure(w, t, func() error {
			l, err := net.Listen("tcp", lo(port))
			if err != nil {
				return err
			}
			defer l.Close()
			done := make(chan error, 1)
			go func() {
				c, err := l.Accept()
				if err != nil {
					done <- err
					return
				}
				defer c.Close()
				c.(*net.TCPConn).SetNoDelay(true)
				b := make([]byte, 65536)
				for {
					n, err := c.Read(b)
					if n > 0 {
						if _, werr := c.Write(b[:n]); werr != nil {
							done <- werr
							return
						}
					}
					if err == io.EOF {
						done <- nil
						return
					}
					if err != nil {
						done <- err
						return
					}
				}
			}()
			c, err := net.Dial("tcp", lo(port))
			if err != nil {
				return err
			}
			c.(*net.TCPConn).SetNoDelay(true)
			msg, back := make([]byte, 64), make([]byte, 64)
			ok := 0
			for r := 0; r < rounds; r++ {
				for i := range msg {
					msg[i] = byte((r + i) & 255)
				}
				t0 := time.Now()
				if _, err := c.Write(msg); err != nil {
					return err
				}
				if _, err := io.ReadFull(c, back); err != nil {
					return err
				}
				rtt[r] = time.Since(t0).Nanoseconds()
				if string(back) == string(msg) {
					ok++
				}
			}
			c.Close()
			if err := <-done; err != nil {
				return err
			}
			okc = ok
			return nil
		})
		if err != nil {
			emitErr("tcp-echo-latency", "net", err)
		} else {
			r := statsOf(rtt)
			emit("tcp-echo-latency", int64(rounds), "net", "", w, t, st, okc == rounds,
				fmt.Sprintf("check=%d; rtt_median_ns=%d; rtt_p95_ns=%d; %d round trips%s%s; peer on a goroutine; %s", okc, r.med, r.p95, rounds, sm("1e5"), pidNote(0), noteErr), "round trips/s")
		}
	}
	if want("tcp-bulk-throughput") {
		bytes := pick(4294967296, 268435456)
		port := portBase + 1
		chunk := make([]byte, 65536)
		for i := range chunk {
			chunk[i] = byte(i & 255)
		}
		var got int64
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			l, err := net.Listen("tcp", lo(port))
			if err != nil {
				return err
			}
			defer l.Close()
			res := make(chan int64, 1)
			go func() {
				c, err := l.Accept()
				if err != nil {
					res <- -1
					return
				}
				defer c.Close()
				b := make([]byte, 65536)
				var tot int64
				for {
					n, err := c.Read(b)
					tot += int64(n)
					if err != nil {
						break
					}
				}
				res <- tot
			}()
			c, err := net.Dial("tcp", lo(port))
			if err != nil {
				return err
			}
			for sent := int64(0); sent < bytes; sent += 65536 {
				if _, err := c.Write(chunk); err != nil {
					return err
				}
			}
			c.Close()
			got = <-res
			return nil
		})
		if err != nil {
			emitErr("tcp-bulk-throughput", "net", err)
		} else {
			emit("tcp-bulk-throughput", bytes/1048576, "net", "", w, t, st, got == bytes,
				fmt.Sprintf("check=%d; %d MiB in 64 KiB writes%s%s; peer on a goroutine; %s", got, bytes/1048576, sm("4 GiB"), pidNote(1), noteErr), "MB/s")
		}
	}
	if want("tcp-accept-rate") {
		n := int(pick(100000, 10000))
		port := portBase + 2
		var accepted int
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			l, err := net.Listen("tcp", lo(port))
			if err != nil {
				return err
			}
			defer l.Close()
			made := make(chan int, 1)
			go func() {
				m := 0
				for i := 0; i < n; i++ {
					c, err := net.Dial("tcp", lo(port))
					if err != nil {
						break
					}
					c.(*net.TCPConn).SetLinger(0)
					c.Close()
					m++
				}
				made <- m
			}()
			a := 0
			for i := 0; i < n; i++ {
				c, err := l.Accept()
				if err != nil {
					return err
				}
				c.Close()
				a++
			}
			if m := <-made; m != a {
				return errors.New("connected and accepted counts differ")
			}
			accepted = a
			return nil
		})
		if err != nil {
			emitErr("tcp-accept-rate", "net", err)
		} else {
			emit("tcp-accept-rate", int64(n), "net", "", w, t, st, accepted == n,
				fmt.Sprintf("check=%d; %d connections%s%s; connector on a goroutine; client close with SO_LINGER 0; %s", accepted, n, sm("1e5"), pidNote(2), noteErr), "connections/s")
		}
	}
	if want("udp-send-rate") {
		n := int(pick(1000000, 100000))
		port := portBase + 3
		var got int
		w, t := warm(1), trials(5)
		st, err := measure(w, t, func() error {
			ra, _ := net.ResolveUDPAddr("udp", lo(port))
			ta, _ := net.ResolveUDPAddr("udp", lo(port+1))
			rx, err := net.ListenUDP("udp", ra)
			if err != nil {
				return err
			}
			defer rx.Close()
			tx, err := net.ListenUDP("udp", ta)
			if err != nil {
				return err
			}
			defer tx.Close()
			res := make(chan int, 1)
			go func() {
				b := make([]byte, 2048)
				ack := []byte{1}
				rec := 0
				for rec < n {
					k, _, err := rx.ReadFromUDP(b)
					if err != nil {
						break
					}
					if k == 512 {
						rec++
						if rec%64 == 0 || rec == n {
							rx.WriteToUDP(ack, ta)
						}
					}
				}
				res <- rec
			}()
			d := make([]byte, 512)
			ack := make([]byte, 8)
			for i := 0; i < n; {
				d[0] = byte(i & 255)
				if _, err := tx.WriteToUDP(d, ra); err != nil {
					return err
				}
				i++
				if i%64 == 0 || i == n {
					if _, _, err := tx.ReadFromUDP(ack); err != nil {
						return err
					}
				}
			}
			got = <-res
			return nil
		})
		if err != nil {
			emitErr("udp-send-rate", "net", err)
		} else {
			emit("udp-send-rate", int64(n), "net", "", w, t, st, got == n,
				fmt.Sprintf("check=%d; %d datagrams of 512 bytes acked every 64%s; ports %d-%d; pid %d; receiver on a goroutine; %s", got, n, sm("1e6"), portBase+3, portBase+4, os.Getpid(), noteErr), "datagrams/s")
		}
	}
	if want("server-echo-throughput") {
		const clients = 64
		dur := time.Duration(pick(10000000000, 2000000000))
		port := portBase + 6
		var msgs, bad, conns int64
		w, t := warm(1), trials(3)
		st, err := measure(w, t, func() error {
			l, err := net.Listen("tcp", lo(port))
			if err != nil {
				return err
			}
			var served atomic.Int64
			var hw sync.WaitGroup
			go func() {
				for {
					c, err := l.Accept()
					if err != nil {
						return
					}
					served.Add(1)
					hw.Add(1)
					go func(c net.Conn) {
						defer hw.Done()
						defer c.Close()
						c.(*net.TCPConn).SetNoDelay(true)
						b := make([]byte, 16384)
						for {
							n, err := c.Read(b)
							if n > 0 {
								if _, err := c.Write(b[:n]); err != nil {
									return
								}
							}
							if err != nil {
								return
							}
						}
					}(c)
				}
			}()
			deadline := time.Now().Add(dur)
			var ok, wrong atomic.Int64
			var cw sync.WaitGroup
			errs := make(chan error, clients)
			for id := 0; id < clients; id++ {
				cw.Add(1)
				go func(id int) {
					defer cw.Done()
					c, err := net.Dial("tcp", lo(port))
					if err != nil {
						errs <- err
						return
					}
					defer c.Close()
					c.(*net.TCPConn).SetNoDelay(true)
					msg, back := make([]byte, 1024), make([]byte, 1024)
					for m := 0; time.Now().Before(deadline); m++ {
						for i := range msg {
							msg[i] = byte((i + id + m) & 255)
						}
						if _, err := c.Write(msg); err != nil {
							errs <- err
							return
						}
						if _, err := io.ReadFull(c, back); err != nil {
							errs <- err
							return
						}
						if string(back) == string(msg) {
							ok.Add(1)
						} else {
							wrong.Add(1)
						}
					}
				}(id)
			}
			cw.Wait()
			l.Close()
			hw.Wait()
			close(errs)
			for e := range errs {
				return e
			}
			msgs, bad, conns = ok.Load(), wrong.Load(), served.Load()
			return nil
		})
		if err != nil {
			emitErr("server-echo-throughput", "net", err)
		} else {
			emit("server-echo-throughput", msgs, "net", "", w, t, st, msgs > 0 && bad == 0 && conns == clients,
				fmt.Sprintf("messages=%d; mismatches=%d; connections=%d; %d clients x %d ms%s%s; a goroutine per connection; clients on goroutines in-process; %s", msgs, bad, conns, clients, dur.Milliseconds(), sm("10 s"), pidNote(6), noteErr), "messages/s")
		}
	}
	if want("server-1k-idle") {
		const n = 1000
		hold := time.Duration(pick(10000000000, 2000000000))
		port := portBase + 5
		var held, rss, cpu int64
		w, t := warm(1), trials(3)
		st, err := measure(w, t, func() error {
			l, err := net.Listen("tcp", lo(port))
			if err != nil {
				return err
			}
			var conns atomic.Int64
			var hw sync.WaitGroup
			go func() {
				for {
					c, err := l.Accept()
					if err != nil {
						return
					}
					conns.Add(1)
					hw.Add(1)
					go func(c net.Conn) {
						defer hw.Done()
						defer c.Close()
						b := make([]byte, 64)
						for {
							if _, err := c.Read(b); err != nil {
								return
							}
						}
					}(c)
				}
			}()
			cs := make([]net.Conn, 0, n)
			for i := 0; i < n; i++ {
				c, err := net.Dial("tcp", lo(port))
				if err != nil {
					return err
				}
				cs = append(cs, c)
			}
			until := time.Now().Add(10 * time.Second)
			for conns.Load() < n && time.Now().Before(until) {
				time.Sleep(time.Millisecond)
			}
			c0 := cpuMs()
			time.Sleep(hold / 2)
			r1 := rssKb()
			time.Sleep(hold - hold/2)
			cpu, rss, held = cpuMs()-c0, r1, conns.Load()
			for _, c := range cs {
				c.Close()
			}
			l.Close()
			hw.Wait()
			return nil
		})
		if err != nil {
			emitErr("server-1k-idle", "net", err)
		} else {
			emit("server-1k-idle", n, "net", "", w, t, st, held == n,
				fmt.Sprintf("check=%d; rss_hold_kb=%d; cpu_ms=%d; %d connections held %d ms%s%s; a goroutine per connection; clients in-process; %s", held, rss, cpu, n, hold.Milliseconds(), sm("10 s"), pidNote(5), noteErr), "")
		}
	}
	_ = syscall.Getpid
}

// ------------------------------------------------------------ url
var uris = [16]string{
	"https://example.com:443/a/b/c?x=1&y=two#frag",
	"http://api.example.org:8080/v1/users/42?fields=name%2Cemail&limit=10",
	"https://cdn.example.net:443/assets/img%20one.png?w=640&h=480",
	"http://localhost:3000/search?q=caf%C3%A9&page=2#results",
	"https://user@host.example.com:8443/path/to/resource?a=%2F%3F&b=c%26d",
	"ftp://files.example.com:21/pub/file.tar.gz",
	"https://www.example.com:443/?utm_source=news&utm_medium=email&utm_campaign=fall%202026",
	"http://10.0.0.1:8000/metrics?name=cpu%5Ftotal&le=0.5",
	"https://shop.example.com:443/cart/items/9?coupon=SAVE%2010&ref=home#top",
	"http://example.com:80/a%2Fb/c%3Bd?k=v",
	"https://docs.example.com:443/guide/intro.html#section-2",
	"http://svc.internal:9090/rpc?method=get&id=%7B1%7D",
	"https://maps.example.com:443/place?ll=37.77%2C-122.41&z=12",
	"http://example.com:8081/~user/index.html?lang=en-US",
	"https://auth.example.com:443/oauth/authorize?client_id=abc&redirect_uri=https%3A%2F%2Fapp.example.com%2Fcb&scope=read%20write",
	"http://example.com:8082/path;params?x=%41%42%43#f%20g",
}
var segs = [8]string{"report 2026", "café menu", "a<b>c", "plain", "say \"hi\"", "x{y}z", "100% done", "über"}
var vals = [8]string{"alpha", "a<b", "50%", "café", "x\"y", "{id}", "beta-2", "naïve"}

func uriChecksum(u *url.URL) int64 {
	port, _ := strconv.Atoi(u.Port())
	c := int64(len(u.Scheme)+len(u.Hostname())+port+len(u.EscapedPath())+len(u.RawQuery)) + int64(len(u.EscapedFragment()))
	for _, vs := range u.Query() {
		for _, v := range vs {
			c += int64(len(v))
		}
	}
	return c
}

func uriRows() {
	if want("uri-parse") {
		n := pick(1000000, 100000)
		var per [16]int64
		for k := range uris {
			u, err := url.Parse(uris[k])
			if err != nil {
				emitErr("uri-parse", "net/url", err)
				return
			}
			per[k] = uriChecksum(u)
		}
		var exp, sum int64
		for i := int64(0); i < n; i++ {
			exp += per[i&15]
		}
		w, t := warm(2), trials(10)
		st, err := measure(w, t, func() error {
			var a int64
			for i := int64(0); i < n; i++ {
				u, err := url.Parse(uris[i&15])
				if err != nil {
					return err
				}
				a += uriChecksum(u)
			}
			sum = a
			return nil
		})
		if err != nil {
			emitErr("uri-parse", "net/url", err)
		} else {
			emit("uri-parse", n, "net/url", "", w, t, st, sum == exp,
				fmt.Sprintf("check=%d; %d URIs%s; url.Parse + Query(); %s", sum, n, sm("1e6"), noteErr), "URIs/s")
		}
	}
	if want("uri-build") {
		n := pick(1000000, 100000)
		var hosts [8]string
		for k := range hosts {
			hosts[k] = "api" + strconv.Itoa(k) + ".example.com"
		}
		one := func(i int64) int64 {
			u := url.URL{Scheme: "https", Host: hosts[i&7] + ":" + strconv.Itoa(8000+int(i&15)),
				Path: "/v1/" + segs[i&7], RawQuery: "q=" + url.QueryEscape(vals[(i>>3)&7])}
			return int64(len(u.String()))
		}
		var per [64]int64
		for k := range per {
			per[k] = one(int64(k))
		}
		var exp, sum int64
		for i := int64(0); i < n; i++ {
			exp += per[i&63]
		}
		w, t := warm(2), trials(10)
		st, _ := measure(w, t, func() error {
			var a int64
			for i := int64(0); i < n; i++ {
				a += one(i)
			}
			sum = a
			return nil
		})
		emit("uri-build", n, "net/url", "", w, t, st, sum == exp,
			fmt.Sprintf("check=%d; %d URIs%s; url.URL{...}.String() escapes the path; QueryEscape the value", sum, n, sm("1e6")), "URIs/s")
	}
}

func fsnotifyVersion() string {
	if bi, ok := debug.ReadBuildInfo(); ok {
		for _, d := range bi.Deps {
			if d.Path == "github.com/fsnotify/fsnotify" {
				return d.Version
			}
		}
	}
	return ""
}

func main() {
	for _, a := range os.Args[1:] {
		only[a] = true
	}
	dir = env("CAJETA_BENCH_IO_DIR", "build/io-data")
	scale = "full"
	if smoke() {
		scale = "smoke"
	}
	dev = env("IO_DEVICE", "unknown")
	bufferRows()
	fileRows()
	pathRows()
	watcherRows()
	bytesRows()
	netRows()
	uriRows()
	_ = sink
}
