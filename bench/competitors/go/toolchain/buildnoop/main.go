// toolchain/startup-exe and binary-size-hello (bench/toolchain/Startup.md, BinarySize.md).
package main

import "os"

func main() {
	os.Stdout.WriteString("hello, world\n")
}
