//
// TensorTests — tensor-plan.md Phase 2: the Tensor<T> core. A strided view over
// an owning Storage<T>: construction/factories (zeros/ones/full/empty/arange/of
// + _like), C-order + F-order strides, accessors (ndim/size/shape/strides/
// itemsize/nbytes/isContiguous/dtype), element read/write, and the storage
// drop-chain under aliased (shared-Storage) tensors. cajeta.math is lazily
// parsed; importing cajeta.math.Tensor triggers it.
//
// Shapes are built explicitly (int64[] via heap + assign). Factories are
// method-templated statics: Tensor.zeros<float32>(shape), etc. The Storage a
// factory allocates is #-moved into the tensor (it owns it); a view from
// alias() borrows that Storage (shared, freed once via the live-set claim).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.DType;\n"
    "import cajeta.math.MemoryOrder;\n";

} // namespace

// 2a — of/zeros/ones/full/arange (+ _like) produce the right shape/strides/ndim/
// size; C-order default + F-order.
TEST(TensorTests, tensorConstructionAndShape) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<float32> z = Tensor.zeros<float32>(shp);\n"
        "        if (z.ndim() != 2) { return -1; }\n"
        "        if (z.shapeAt(0) != 2) { return -2; }\n"
        "        if (z.shapeAt(1) != 3) { return -3; }\n"
        "        if (z.size() != 6) { return -4; }\n"
        "        if (z.strideAt(0) != 3) { return -5; }\n"   // C-order: [3, 1]
        "        if (z.strideAt(1) != 1) { return -6; }\n"
        "        if (z.get2(0, 0) != 0.0f) { return -7; }\n"
        "        Tensor<int32> o = Tensor.ones<int32>(shp);\n"
        "        if (o.get2(1, 2) != 1) { return -8; }\n"
        "        Tensor<int32> f = Tensor.full<int32>(shp, 7);\n"
        "        if (f.get2(0, 1) != 7) { return -9; }\n"
        "        Tensor<int32> r = Tensor.arange<int32>(5);\n"
        "        if (r.ndim() != 1) { return -10; }\n"
        "        if (r.size() != 5) { return -11; }\n"
        "        if (r.get1(0) != 0) { return -12; }\n"
        "        if (r.get1(4) != 4) { return -13; }\n"
        "        int32[] data = { 10, 20, 30, 40, 50, 60 };\n"
        "        Tensor<int32> t = Tensor.of<int32>(data, shp);\n"
        "        if (t.get2(1, 0) != 40) { return -14; }\n"   // row1,col0 = flat idx 3
        "        if (t.get2(0, 2) != 30) { return -15; }\n"
        "        Tensor<int32> zl = Tensor.zerosLike<int32>(t);\n"
        "        if (zl.ndim() != 2) { return -16; }\n"
        "        if (zl.shapeAt(1) != 3) { return -17; }\n"
        "        if (zl.get2(1, 0) != 0) { return -18; }\n"
        "        Tensor<int32> ol = Tensor.onesLike<int32>(t);\n"
        "        if (ol.get2(1, 2) != 1) { return -19; }\n"
        "        Tensor<float32> ff = Tensor.emptyOrdered<float32>(shp, MemoryOrder.F);\n"
        "        if (ff.strideAt(0) != 1) { return -20; }\n"  // F-order: [1, 2]
        "        if (ff.strideAt(1) != 2) { return -21; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2b — multiple Tensors over one Storage (alias) share the buffer (writes show
// through both ways); base() reports the source; the buffer frees exactly once
// (no leak / no double-free — several aliases all drop at scope exit, no crash).
TEST(TensorTests, storageRefcountDropChain) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<int32> a = Tensor.arange<int32>(4);\n"   // [0,1,2,3]
        "        Tensor<int32> b = a.alias();\n"
        "        if (!b.isView()) { return -1; }\n"
        "        if (a.isView()) { return -2; }\n"
        "        if (b.base() == null) { return -3; }\n"
        "        if (a.base() != null) { return -4; }\n"
        "        a.set1(2, 99);\n"                                // write via a
        "        if (b.get1(2) != 99) { return -5; }\n"          // read via b
        "        b.set1(0, 77);\n"                                // write via b
        "        if (a.get1(0) != 77) { return -6; }\n"          // read via a
        "        Tensor<int32> c = a.alias();\n"
        "        Tensor<int32> d = b.alias();\n"
        "        c.set1(3, 55);\n"
        "        if (d.get1(3) != 55) { return -7; }\n"
        "        int32 s = d.get1(0) + d.get1(1) + d.get1(2) + d.get1(3);\n"  // 77+1+99+55
        "        if (s != 232) { return -8; }\n"
        "        return 1;\n"                                     // a,b,c,d all drop here
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2c — itemsize/nbytes/isContiguous/dtype + element read on a freshly-built
// contiguous tensor; F-order is not C-contiguous.
TEST(TensorTests, tensorAccessors) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<float32> t = Tensor.zeros<float32>(shp);\n"
        "        if (t.itemsize() != 4) { return -1; }\n"        // float32 = 4 bytes
        "        if (t.nbytes() != 24) { return -2; }\n"         // 6 * 4
        "        if (!t.isContiguous()) { return -3; }\n"
        "        DType dt = t.dtype();\n"
        "        if (!dt.isFloating()) { return -4; }\n"
        "        if (dt.bits() != 32) { return -5; }\n"
        "        t.set2(1, 2, 3.5f);\n"
        "        if (t.get2(1, 2) != 3.5f) { return -6; }\n"
        "        if (t.get2(0, 0) != 0.0f) { return -7; }\n"
        "        int64[] shp2 = heap int64[1];\n"
        "        shp2[0] = 8;\n"
        "        Tensor<int16> s = Tensor.zeros<int16>(shp2);\n"
        "        if (s.itemsize() != 2) { return -8; }\n"
        "        if (s.nbytes() != 16) { return -9; }\n"
        "        Tensor<float32> ff = Tensor.emptyOrdered<float32>(shp, MemoryOrder.F);\n"
        "        if (ff.isContiguous()) { return -10; }\n"       // F-order, ndim>1 → not C-contig
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
