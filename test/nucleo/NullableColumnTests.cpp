//
// nucleo-column Unit 2 — NullableColumn<T>: the validity bitmap (spec 3.2,
// 3.4, 7.5).
//
// Nullability is a TYPE distinction (spec §3): Column<T> has no bitmap and no
// validity branch; NullableColumn<T> carries a separable 1-bit-per-element
// validity bitmap (LSB-first per Arrow) beside an UNCHANGED value buffer.
// There is no asTensor() on the nullable type — the dense tensor substrate
// is reached only through the explicit fillNulls/dropNulls materializations
// (the developer-decided absent-by-type resolution).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
std::string wrap(const std::string& retTy, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.column.Column;\n"
        "import cajeta.nucleo.column.NullableColumn;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class T {\n"
        "    public static " + retTy + " run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}
float runF32(const std::string& body) {
    auto jit = CajetaJit::compile(wrap("float32", body), "test.T");
    return jit->lookup<float (*)()>("run")();
}
int64_t runI64(const std::string& body) {
    auto jit = CajetaJit::compile(wrap("int64", body), "test.T");
    return jit->lookup<int64_t (*)()>("run")();
}

// values {10,20,30,40}, valid {t,f,t,f} — the shared fixture shape.
const char* kMasked =
    "float32[] va = [ 10.0f, 20.0f, 30.0f, 40.0f ];\n"
    "        boolean[] ok = [ true, false, true, false ];\n"
    "        NullableColumn<float32> c = NullableColumn.of<float32>(va, ok);\n";
} // namespace

// 2.1.1 — the bitmap reflects construction: isValid per element, get on a
// valid element returns the value, the value buffer is unchanged in layout
// (a masked-out slot still holds its physical value — validity is SEPARATE).
TEST(NullableColumn, bitmapReflectsConstruction) {
    EXPECT_EQ(runI64(std::string(kMasked) +
        "        int64 m = 0;\n"
        "        if (c.isValid(0)) { m = m + 1; }\n"
        "        if (c.isValid(1)) { m = m + 10; }\n"
        "        if (c.isValid(2)) { m = m + 100; }\n"
        "        if (c.isValid(3)) { m = m + 1000; }\n"
        "        if (c.get(0) == 10.0f) { m = m + 10000; }\n"
        "        if (c.get(2) == 30.0f) { m = m + 100000; }\n"
        "        return m;"), 110101);
}

// 2.1.1 (mutation) — setNull clears a bit, set(v) writes value + sets the
// bit; both are per-element and do not disturb neighbors.
TEST(NullableColumn, setAndSetNullFlipBitsIndependently) {
    EXPECT_EQ(runI64(std::string(kMasked) +
        "        c.setNull(0);\n"
        "        c.set(1, 25.0f);\n"
        "        int64 m = 0;\n"
        "        if (!c.isValid(0)) { m = m + 1; }\n"
        "        if (c.isValid(1) && c.get(1) == 25.0f) { m = m + 10; }\n"
        "        if (c.isValid(2) && c.get(2) == 30.0f) { m = m + 100; }\n"
        "        if (!c.isValid(3)) { m = m + 1000; }\n"
        "        return m;"), 1111);
}

// 2.1.2 — fillNulls materializes an owned dense Column<T> with nulls
// replaced; dropNulls removes them keeping order. Both leave the source
// untouched.
TEST(NullableColumn, fillNullsAndDropNullsMaterializeDense) {
    EXPECT_FLOAT_EQ(runF32(std::string(kMasked) +
        "        Column<float32> filled = c.fillNulls(-1.0f);\n"
        "        Column<float32> dense = c.dropNulls();\n"
        "        return filled.get(0) + filled.get(1) * 10.0f\n"
        "             + dense.get(1) * 100.0f + ((float32) dense.size()) * 1000.0f\n"
        "             + ((float32) c.size());"),
        // filled {10,-1,30,-1}; dense {10,30} size 2; source size 4:
        // 10 + (-1*10) + 30*100 + 2*1000 + 4 = 5004.
        10.0f - 10.0f + 3000.0f + 2000.0f + 4.0f);
}

// 2.1.3 — asTensor() does not exist on NullableColumn: the §7.5 type-level
// refusal is a compile error, not a runtime surprise.
TEST(NullableColumn, asTensorAbsentByType) {
    try {
        CajetaJit::compile(wrap("float32", std::string(kMasked) +
            "        Tensor<float32> t = c.asTensor();\n"
            "        return 0.0f;"), "test.T");
        FAIL() << "asTensor on NullableColumn must not compile";
    } catch (cajeta::Exception& e) {
        EXPECT_FALSE(std::string(e.getErrorId()).empty());
    }
}

// 2.1.4 — the bitmap is physically real and separable: a NullableColumn
// allocates MORE than a Column of the same length (the bitmap's bytes,
// relational — the FuseSugar allocatedBytes idiom), and the bitmap buffer's
// address is 64-byte aligned (Arrow validity-buffer rules).
TEST(NullableColumn, bitmapIsSeparateAndAligned) {
    int64_t delta = runI64(
        "int64 n = 8192;\n"
        "        int64 base = Cajeta.allocatedBytes();\n"
        "        Column<float32> plain = Column.zeros<float32>(n);\n"
        "        int64 plainBytes = Cajeta.allocatedBytes() - base;\n"
        "        base = Cajeta.allocatedBytes();\n"
        "        NullableColumn<float32> nul = NullableColumn.nulls<float32>(n);\n"
        "        int64 nulBytes = Cajeta.allocatedBytes() - base;\n"
        "        return nulBytes - plainBytes;");
    EXPECT_GE(delta, 8192 / 8) << "no bitmap-sized allocation difference";
    EXPECT_LT(delta, 8192) << "bitmap overhead far larger than 1 bit/element";
    EXPECT_EQ(runI64(
        "NullableColumn<float32> c = NullableColumn.nulls<float32>(1000);\n"
        "        return c.validityAddress() % 64 + c.dataAddress() % 64;"), 0);
}
