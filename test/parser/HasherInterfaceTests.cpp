// Tests for the cajeta.hash.Hasher interface. The interface itself
// is a shape contract — no behavior to verify beyond "a class
// implementing it parses, registers in the vtable, and dispatches
// through an interface-typed variable." The real algorithm classes
// (MD5, XXHash3, SipHash, RapidHash, DefaultHasher) get their own
// behavioral suites against published test vectors.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// Smoke: a class can implement Hasher's whole surface and finish()
// returns a value driven by what was written. The accumulator here
// XORs each input slice — adequate to prove the dispatch wires up;
// real algorithms cover the actual mixing logic.
TEST(HasherInterfaceTests, classImplementsFullSurface) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Hasher;\n"
        "import cajeta.lang.Object;\n"
        "import cajeta.lang.String;\n"
        "public class XorHasher implements Hasher {\n"
        "    public int64 acc;\n"
        "    public XorHasher() { this.acc = 0; }\n"
        "    public void writeBoolean(boolean v) { if (v) { this.acc = this.acc ^ 1; } }\n"
        "    public void writeInt8(int8 v)       { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeInt16(int16 v)     { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeInt32(int32 v)     { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeInt64(int64 v)     { this.acc = this.acc ^ v; }\n"
        "    public void writeUInt8(uint8 v)     { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeUInt16(uint16 v)   { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeUInt32(uint32 v)   { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeUInt64(uint64 v)   { this.acc = this.acc ^ ((int64) v); }\n"
        "    public void writeFloat32(float32 v) { this.acc = this.acc ^ 32; }\n"
        "    public void writeFloat64(float64 v) { this.acc = this.acc ^ 64; }\n"
        "    public void writeBytes(int8[] data) { this.acc = this.acc ^ 100; }\n"
        "    public void writeBytesRange(int8[] data, int64 offset, int64 length) {\n"
        "        this.acc = this.acc ^ (length + offset);\n"
        "    }\n"
        "    public void writeString(String s) { this.acc = this.acc ^ 200; }\n"
        "    public void writeObject(Object obj) { this.acc = this.acc ^ 300; }\n"
        "    public int64 finish() { return this.acc; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        XorHasher h = heap XorHasher();\n"
        "        h.writeInt32(5);\n"
        "        h.writeInt64(10);\n"
        "        return h.finish();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 5L ^ 10L);
}

// Dispatch through Hasher-typed variable. Confirms the interface
// vtable lookup lands on the concrete writeInt32 + finish.
TEST(HasherInterfaceTests, dispatchThroughInterfaceVar) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Hasher;\n"
        "import cajeta.lang.Object;\n"
        "import cajeta.lang.String;\n"
        "public class Counter implements Hasher {\n"
        "    public int64 acc;\n"
        "    public Counter() { this.acc = 0; }\n"
        "    public void writeBoolean(boolean v) { this.acc = this.acc + 1; }\n"
        "    public void writeInt8(int8 v)       { this.acc = this.acc + 1; }\n"
        "    public void writeInt16(int16 v)     { this.acc = this.acc + 1; }\n"
        "    public void writeInt32(int32 v)     { this.acc = this.acc + 1; }\n"
        "    public void writeInt64(int64 v)     { this.acc = this.acc + 1; }\n"
        "    public void writeUInt8(uint8 v)     { this.acc = this.acc + 1; }\n"
        "    public void writeUInt16(uint16 v)   { this.acc = this.acc + 1; }\n"
        "    public void writeUInt32(uint32 v)   { this.acc = this.acc + 1; }\n"
        "    public void writeUInt64(uint64 v)   { this.acc = this.acc + 1; }\n"
        "    public void writeFloat32(float32 v) { this.acc = this.acc + 1; }\n"
        "    public void writeFloat64(float64 v) { this.acc = this.acc + 1; }\n"
        "    public void writeBytes(int8[] data) { this.acc = this.acc + 1; }\n"
        "    public void writeBytesRange(int8[] data, int64 offset, int64 length) {\n"
        "        this.acc = this.acc + 1;\n"
        "    }\n"
        "    public void writeString(String s) { this.acc = this.acc + 1; }\n"
        "    public void writeObject(Object obj) { this.acc = this.acc + 1; }\n"
        "    public int64 finish() { return this.acc; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Hasher h = heap Counter();\n"
        "        h.writeInt32(1);\n"
        "        h.writeInt32(2);\n"
        "        h.writeInt32(3);\n"
        "        return h.finish();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 3L);
}
