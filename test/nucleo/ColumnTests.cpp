//
// nucleo-column Unit 1 — Column<T>: the owned, aligned, tensor-bit-identical
// core (spec 2.1, 2.3, 3.1, 3.3, 6.1, 7.4).
//
// The load-bearing invariant (spec §1.1): a non-null numeric Column<T> IS a
// tensor buffer — same bytes, same Storage, no marshalling. asTensor()/
// fromTensor() are views over the SHARED Storage (mutual write visibility is
// the zero-copy proof), the owned buffer starts at a 64-byte-aligned address
// (aligned start offset inside an over-allocated Storage — plan Q-align), and
// a Column<T> carries no validity bitmap at all (nullability lives in
// NullableColumn<T>, U2).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
std::string wrap(const std::string& retTy, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.column.Column;\n"
        "import cajeta.nucleo.column.ColumnTypeException;\n"
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
} // namespace

// 1.1.1 — Column.of stores values contiguously; get(i) reads them back; size()
// is the element count. {1.5, 2.5, 3.5} probed positionally.
TEST(Column, ofStoresAndReadsValues) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] fa = [ 1.5f, 2.5f, 3.5f ];\n"
        "        Column<float32> c = Column.of<float32>(fa);\n"
        "        return c.get(0) + c.get(1) * 10.0f + c.get(2) * 100.0f\n"
        "             + ((float32) c.size()) * 1000.0f;"),
        1.5f + 25.0f + 350.0f + 3000.0f);
}

// 1.1.2 — asTensor() is a ZERO-COPY view: a write through the tensor is
// visible through the column, and a write through the column is visible
// through the tensor. Shared bytes, not a copy (spec 7.4, §1.1).
TEST(Column, asTensorIsZeroCopyView) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] fa = [ 1.0f, 2.0f, 4.0f ];\n"
        "        Column<float32> c = Column.of<float32>(fa);\n"
        "        Tensor<float32> t = c.asTensor();\n"
        "        t.set1(0, 7.0f);\n"
        "        c.set(1, 9.0f);\n"
        "        return c.get(0) + t.get1(1) * 10.0f + t.get1(2) * 100.0f;"),
        7.0f + 90.0f + 400.0f);
}

// 1.1.3 — fromTensor adopts a contiguous rank-1 tensor's Storage zero-copy,
// and round-trips: writes through the column reach the source tensor.
TEST(Column, fromTensorSharesTheBuffer) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] fa = [ 1.0f, 2.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        Column<float32> c = Column.fromTensor<float32>(x);\n"
        "        c.set(2, 8.0f);\n"
        "        Tensor<float32> t2 = c.asTensor();\n"
        "        return x.get1(2) + t2.get1(0) * 10.0f;"),
        8.0f + 10.0f);
}

// 1.1.4 — every owned buffer's data address is 64-byte aligned, across element
// widths (aligned start offset; spec 6.1). Sum of (addr % 64) over dtypes = 0.
TEST(Column, ownedBuffersAre64ByteAligned) {
    EXPECT_EQ(runI64(
        "Column<float32> a = Column.zeros<float32>(1000);\n"
        "        Column<float64> b = Column.zeros<float64>(1000);\n"
        "        Column<int8> c = Column.zeros<int8>(1000);\n"
        "        Column<int64> d = Column.zeros<int64>(1000);\n"
        "        return a.dataAddress() % 64 + b.dataAddress() % 64\n"
        "             + c.dataAddress() % 64 + d.dataAddress() % 64;"), 0);
}

// 1.1.5 — a non-null Column allocates NO validity bitmap: allocation for
// zeros(n) is the value buffer plus small constant overhead. Relational bound
// (the FuseSugar allocatedBytes idiom): extra-over-values < n/8 bits' worth,
// which is exactly what a bitmap would add. n = 8192 -> bitmap would be 1 KiB.
TEST(Column, nonNullColumnAllocatesNoBitmap) {
    int64_t extra = runI64(
        "int64 base = Cajeta.allocatedBytes();\n"
        "        Column<float32> c = Column.zeros<float32>(8192);\n"
        "        int64 grew = Cajeta.allocatedBytes() - base;\n"
        "        return grew - 8192 * 4;");
    EXPECT_GE(extra, 0) << "value buffer not allocated?";
    EXPECT_LT(extra, 8192 / 8) << "allocation beyond values + slack: a bitmap?";
}

// 1.1.6 — an excluded dtype is a NAMED error at construction, never a mislaid
// buffer: boolean (Arrow bit-packs bools; byte-backed buffers would break
// bit-identity) rejects with ColumnTypeException.
TEST(Column, excludedDtypeIsNamedError) {
    EXPECT_EQ(runI64(
        "try {\n"
        "            Column<boolean> c = Column.zeros<boolean>(4);\n"
        "            return 0;\n"
        "        } catch (ColumnTypeException e) {\n"
        "            return 1;\n"
        "        }"), 1);
}
