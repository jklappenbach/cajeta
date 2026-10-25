// toolchain/debugger-attach debuggee (bench/toolchain/Debugger.md): a running
// process that calls tick() every millisecond, for delve to attach to and break in.
// PR_SET_PTRACER_ANY lets a non-parent debugger attach under Yama ptrace_scope=1.
package main

import (
	"fmt"
	"syscall"
	"time"
)

//go:noinline
func tick(n int64) int64 {
	return n*3 + 1
}

func main() {
	syscall.Syscall6(syscall.SYS_PRCTL, 0x59616d61, ^uintptr(0), 0, 0, 0, 0)
	fmt.Println("ready")
	var acc int64
	for i := int64(0); ; i++ {
		acc += tick(i)
		time.Sleep(time.Millisecond)
	}
}
