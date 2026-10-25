// toolchain/startup-exe and binary-size-hello (bench/toolchain/Startup.md, BinarySize.md):
// hello-world, the same line the Cajeta program prints.
#include <cstdio>

int main() {
    std::puts("hello, world");
    return 0;
}
