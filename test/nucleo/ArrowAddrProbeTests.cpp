//
// Probe: is Storage/Tensor/Column.hostAddress()/dataAddress() the REAL,
// STABLE data buffer address — or a transient (per-call marshalling copy)?
// The U3 export failures showed export-time buffers[1] != a later
// dataAddress() read, with garbage values through both — which fits a
// forwarder passing a TEMP rather than the live buffer. This pins the
// contract the whole C-Data seam stands on.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
const char* kProbe =
    "package test;\n"
    "import cajeta.nucleo.column.Column;\n"
    "public final class T {\n"
    "    static Column<float32> held;\n"
    "    public static int64 diff() {\n"
    "        float32[] fa = { 1.5f, 2.5f, 3.5f };\n"
    "        Column<float32> c = Column.of<float32>(fa);\n"
    "        T.held = c;\n"
    "        int64 a1 = T.held.dataAddress();\n"
    "        int64 a2 = T.held.dataAddress();\n"
    "        return a1 - a2;\n"
    "    }\n"
    "    public static int64 addr() { return T.held.dataAddress(); }\n"
    "    public static float32 val(int64 i) { return T.held.get(i); }\n"
    "}\n";
} // namespace

TEST(ArrowAddrProbe, addressStableAndReadable) {
    auto jit = CajetaJit::compile(kProbe, "test.T");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("diff")(), 0)
        << "dataAddress() differs across calls — transient marshalling temp";
    int64_t a = jit->lookup<int64_t (*)()>("addr")();
    float v0 = jit->lookup<float (*)(int64_t)>("val")(0);
    EXPECT_FLOAT_EQ(v0, 1.5f);
    EXPECT_FLOAT_EQ(reinterpret_cast<float*>(a)[0], v0)
        << "the address does not point at the live element bytes";
}
