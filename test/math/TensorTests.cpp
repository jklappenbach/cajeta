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
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Phase 6 device tests need the XPU backend enabled so @Kernel lowers + launches
// (on the portable CPU backend in-process — the same discipline as
// XpuComputeProbeTests; on-device Vulkan/CUDA validation rides separate device
// gates). The seam routes on placement; the kernel runs through the real launch
// FFI either way.
int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.DType;\n"
    "import cajeta.math.MemoryOrder;\n"
    "import cajeta.math.BroadcastException;\n";

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

// 3a — reshape(contiguous)/transpose/slice/squeeze/expandDims are VIEWS: share
// the base (writes show through; base() is the source).
TEST(TensorTests, structuralOpsAreViews) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] data = { 0, 1, 2, 3, 4, 5 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"   // [[0,1,2],[3,4,5]]
        // reshape (contiguous → view)
        "        int64[] rs = heap int64[2];\n"
        "        rs[0] = 3;\n"
        "        rs[1] = 2;\n"
        "        Tensor<int32> r = a.reshape(rs);\n"
        "        if (!r.isView()) { return -1; }\n"
        "        if (r.base() == null) { return -2; }\n"
        "        if (r.shapeAt(0) != 3 || r.shapeAt(1) != 2) { return -3; }\n"
        "        r.set2(0, 0, 100);\n"                                // shares a
        "        if (a.get2(0, 0) != 100) { return -4; }\n"
        // transpose → view
        "        Tensor<int32> t = a.transpose();\n"                 // shape [3,2], strides [1,3]
        "        if (!t.isView()) { return -5; }\n"
        "        if (t.shapeAt(0) != 3 || t.shapeAt(1) != 2) { return -6; }\n"
        "        if (t.get2(0, 1) != 3) { return -7; }\n"            // a[1,0] = 3
        "        t.set2(0, 1, 99);\n"                                // sets a[1,0]
        "        if (a.get2(1, 0) != 99) { return -8; }\n"
        // slice axis 0 rows [1,2) → shape [1,3]
        "        Tensor<int32> sl = a.slice(0, 1, 2);\n"
        "        if (!sl.isView()) { return -9; }\n"
        "        if (sl.shapeAt(0) != 1 || sl.shapeAt(1) != 3) { return -10; }\n"
        "        if (sl.get2(0, 0) != 99) { return -11; }\n"        // a[1,0] = 99
        // expandDims at 0 → [1,2,3]; squeeze → [2,3]
        "        Tensor<int32> e = a.expandDims(0);\n"
        "        if (!e.isView()) { return -12; }\n"
        "        if (e.ndim() != 3 || e.shapeAt(0) != 1) { return -13; }\n"
        "        Tensor<int32> sq = e.squeeze();\n"
        "        if (!sq.isView()) { return -14; }\n"
        "        if (sq.ndim() != 2 || sq.shapeAt(0) != 2 || sq.shapeAt(1) != 3) { return -15; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3b — copy() and a non-contiguous reshape produce INDEPENDENT storage
// (mutation does not show through).
TEST(TensorTests, copyIsIndependent) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] data = { 0, 1, 2, 3, 4, 5 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"
        "        Tensor<int32> c = a.copy();\n"
        "        if (c.isView()) { return -1; }\n"
        "        if (c.base() != null) { return -2; }\n"
        "        c.set2(0, 0, 500);\n"
        "        if (a.get2(0, 0) == 500) { return -3; }\n"          // independent
        "        if (c.get2(1, 2) != 5) { return -4; }\n"            // copied the data
        // non-contiguous reshape → copy
        "        Tensor<int32> t = a.transpose();\n"                 // non-contiguous
        "        if (t.isContiguous()) { return -5; }\n"
        "        int64[] rs = heap int64[1];\n"
        "        rs[0] = 6;\n"
        "        Tensor<int32> r = t.reshape(rs);\n"                 // non-contig → copy
        "        if (r.isView()) { return -6; }\n"
        // t is transpose of a: logical C-order of t is a[0,0],a[1,0],a[0,1],a[1,1],a[0,2],a[1,2]
        "        if (r.get1(0) != 0 || r.get1(1) != 3 || r.get1(2) != 1) { return -7; }\n"
        "        r.set1(0, 777);\n"
        "        if (a.get2(0, 0) == 777) { return -8; }\n"          // copy independent of a
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3c — in-place writes through a (non-overlapping) structural view are
// well-defined: each element maps to a distinct storage cell, no corruption.
TEST(TensorTests, aliasingDefined) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] data = { 0, 1, 2, 3, 4, 5 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"
        "        Tensor<int32> t = a.transpose();\n"                 // [3,2]
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 j = 0;\n"
        "            while (j < 2) {\n"
        "                t.set2(i, j, (int32) (i * 10 + j));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // read back through t: consistent
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 j = 0;\n"
        "            while (j < 2) {\n"
        "                if (t.get2(i, j) != (int32) (i * 10 + j)) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // and a reflects t (a[j,i] == t[i,j]): t[0,1]=1 → a[1,0]; t[2,0]=20 → a[0,2]
        "        if (a.get2(1, 0) != 1) { return -2; }\n"
        "        if (a.get2(0, 2) != 20) { return -3; }\n"
        "        if (a.get2(0, 0) != 0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4a — broadcastShape matches numpy's right-aligned rule across compatible cases
// and throws BroadcastException on the incompatible ones.
TEST(TensorTests, broadcastShapeRules) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [2,1,3] vs [4,3] → [2,4,3]
        "        int64[] a = heap int64[3];\n"
        "        a[0] = 2; a[1] = 1; a[2] = 3;\n"
        "        int64[] b = heap int64[2];\n"
        "        b[0] = 4; b[1] = 3;\n"
        "        int64[] r = Tensor.broadcastShape<int32>(a, b);\n"
        "        if (r.count() != 3) { return -1; }\n"
        "        if (r[0] != 2 || r[1] != 4 || r[2] != 3) { return -2; }\n"
        // [5] vs [1] → [5]
        "        int64[] c = heap int64[1];\n"
        "        c[0] = 5;\n"
        "        int64[] d = heap int64[1];\n"
        "        d[0] = 1;\n"
        "        int64[] r2 = Tensor.broadcastShape<int32>(c, d);\n"
        "        if (r2[0] != 5) { return -3; }\n"
        // incompatible: [3] vs [4] → throws
        "        int64[] e = heap int64[1];\n"
        "        e[0] = 3;\n"
        "        int64[] f = heap int64[1];\n"
        "        f[0] = 4;\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            int64[] bad = Tensor.broadcastShape<int32>(e, f);\n"
        "        } catch (BroadcastException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4b — broadcastTo yields a stride-0 view (no copy); reads return the stretched
// values; an incompatible target throws.
TEST(TensorTests, broadcastIsZeroCopyView) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // a = [[10],[20],[30]] shape [3,1]; broadcast to [3,4]
        "        int32[] data = { 10, 20, 30 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 3; shp[1] = 1;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"
        "        int64[] tgt = heap int64[2];\n"
        "        tgt[0] = 3; tgt[1] = 4;\n"
        "        Tensor<int32> bc = a.broadcastTo(tgt);\n"
        "        if (!bc.isView()) { return -1; }\n"
        "        if (bc.shapeAt(0) != 3 || bc.shapeAt(1) != 4) { return -2; }\n"
        "        if (bc.strideAt(1) != 0) { return -3; }\n"          // stretched axis stride 0
        // every column reads the row value
        "        if (bc.get2(0, 0) != 10 || bc.get2(0, 3) != 10) { return -4; }\n"
        "        if (bc.get2(2, 1) != 30 || bc.get2(2, 3) != 30) { return -5; }\n"
        // it shares storage: mutating the source row shows through all columns
        "        a.set2(1, 0, 99);\n"
        "        if (bc.get2(1, 0) != 99 || bc.get2(1, 3) != 99) { return -6; }\n"
        // broadcast a 1-D [4] up to [2,4] (missing leading axis → stride 0)
        "        int32[] row = { 1, 2, 3, 4 };\n"
        "        int64[] rshp = heap int64[1];\n"
        "        rshp[0] = 4;\n"
        "        Tensor<int32> v = Tensor.of<int32>(row, rshp);\n"
        "        int64[] t2 = heap int64[2];\n"
        "        t2[0] = 2; t2[1] = 4;\n"
        "        Tensor<int32> vb = v.broadcastTo(t2);\n"
        "        if (vb.strideAt(0) != 0) { return -7; }\n"
        "        if (vb.get2(0, 2) != 3 || vb.get2(1, 2) != 3) { return -8; }\n"
        // incompatible target throws
        "        boolean threw = false;\n"
        "        int64[] bad = heap int64[2];\n"
        "        bad[0] = 3; bad[1] = 5;\n"                          // a is [3,1] → col ok, but try [3,1]→[2,4]
        "        int64[] bad2 = heap int64[2];\n"
        "        bad2[0] = 2; bad2[1] = 4;\n"
        "        try {\n"
        "            Tensor<int32> x = a.broadcastTo(bad2);\n"        // a[3,1] vs [2,4]: 3 vs 2 incompatible
        "        } catch (BroadcastException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5a — basic indexing: integer index (removes axis), slice with step + negative
// indices, reverseAxis — all VIEWS sharing storage.
TEST(TensorTests, basicIndexingViews) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] data = { 0, 1, 2, 3, 4, 5 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"   // [[0,1,2],[3,4,5]]
        // integer index row 1 → [3,4,5], view
        "        Tensor<int32> row = a.index(0, 1);\n"
        "        if (!row.isView()) { return -1; }\n"
        "        if (row.ndim() != 1 || row.shapeAt(0) != 3) { return -2; }\n"
        "        if (row.get1(0) != 3 || row.get1(2) != 5) { return -3; }\n"
        "        row.set1(0, 99);\n"                                 // a[1,0]
        "        if (a.get2(1, 0) != 99) { return -4; }\n"
        // negative integer index → last row [99,4,5]
        "        Tensor<int32> last = a.index(0, -1);\n"
        "        if (last.get1(0) != 99) { return -5; }\n"
        // slice with step on a 1-D arange(6): [0,2,4]
        "        Tensor<int32> r = Tensor.arange<int32>(6);\n"
        "        Tensor<int32> ev = r.sliceAxis(0, 0, 6, 2);\n"
        "        if (!ev.isView()) { return -6; }\n"
        "        if (ev.shapeAt(0) != 3) { return -7; }\n"
        "        if (ev.get1(0) != 0 || ev.get1(1) != 2 || ev.get1(2) != 4) { return -8; }\n"
        // negative-index slice [-3, 6) step 1 → [3,4,5]
        "        Tensor<int32> tail = r.sliceAxis(0, -3, 6, 1);\n"
        "        if (tail.shapeAt(0) != 3 || tail.get1(0) != 3 || tail.get1(2) != 5) { return -9; }\n"
        // reverse
        "        Tensor<int32> rev = r.reverseAxis(0);\n"
        "        if (!rev.isView()) { return -10; }\n"
        "        if (rev.get1(0) != 5 || rev.get1(5) != 0) { return -11; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5b — boolean indexing: masked read is an independent 1-D copy; masked write
// scatters into the source.
TEST(TensorTests, booleanIndexing) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] data = { 0, 1, 2, 3, 4, 5 };\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(data, shp);\n"   // [[0,1,2],[3,4,5]]
        "        boolean[] md = heap boolean[6];\n"
        "        md[0] = true; md[1] = false; md[2] = true;\n"       // even-value mask
        "        md[3] = false; md[4] = true; md[5] = false;\n"
        "        Tensor<boolean> mask = Tensor.of<boolean>(md, shp);\n"
        "        Tensor<int32> sel = a.maskedSelect(mask);\n"
        "        if (sel.ndim() != 1 || sel.shapeAt(0) != 3) { return -1; }\n"
        "        if (sel.get1(0) != 0 || sel.get1(1) != 2 || sel.get1(2) != 4) { return -2; }\n"
        // masked read is a copy (independent)
        "        sel.set1(0, 100);\n"
        "        if (a.get2(0, 0) == 100) { return -3; }\n"
        // masked write scatters into a
        "        a.maskedAssign(mask, 7);\n"
        "        if (a.get2(0, 0) != 7 || a.get2(0, 2) != 7 || a.get2(1, 1) != 7) { return -4; }\n"
        "        if (a.get2(0, 1) != 1 || a.get2(1, 0) != 3) { return -5; }\n"   // unmasked intact
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5c — fancy indexing: take (gather, independent copy, negative wraps); put
// (scatter into the source).
TEST(TensorTests, fancyIndexing) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<int32> r = Tensor.arange<int32>(10);\n"      // [0..9]
        "        int64[] idx = heap int64[4];\n"
        "        idx[0] = 2; idx[1] = 5; idx[2] = 8; idx[3] = -1;\n" // -1 → 9
        "        Tensor<int32> g = r.take(idx);\n"
        "        if (g.ndim() != 1 || g.shapeAt(0) != 4) { return -1; }\n"
        "        if (g.get1(0) != 2 || g.get1(1) != 5 || g.get1(2) != 8 || g.get1(3) != 9) { return -2; }\n"
        // gather is a copy
        "        g.set1(0, 50);\n"
        "        if (r.get1(2) == 50) { return -3; }\n"
        // put scatters
        "        int64[] pidx = heap int64[3];\n"
        "        pidx[0] = 0; pidx[1] = 3; pidx[2] = 9;\n"
        "        int32[] pvals = { 100, 200, 300 };\n"
        "        r.put(pidx, pvals);\n"
        "        if (r.get1(0) != 100 || r.get1(3) != 200 || r.get1(9) != 300) { return -4; }\n"
        "        if (r.get1(1) != 1) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6a — device placement: to-gpu / to-cpu move storage; device()/isOnGpu() report
// it; data survives a host→device→host round-trip; a no-GPU build still works
// (the cajeta.gpu CPU backend backs the buffer).
TEST(TensorTests, devicePlacement) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] da = { 1.0f, 2.0f, 3.0f, 4.0f };\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 4;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, shp);\n"
        "        if (a.device() != 0) { return -1; }\n"        // starts on host
        "        if (a.isOnGpu()) { return -2; }\n"
        "        a.gpu();\n"
        "        if (a.device() != 1) { return -3; }\n"        // now device-resident
        "        if (!a.isOnGpu()) { return -4; }\n"
        "        a.cpu();\n"
        "        if (a.device() != 0) { return -5; }\n"        // back on host
        "        if (a.get1(0) != 1.0f || a.get1(3) != 4.0f) { return -6; }\n"  // data survived
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 6b — the op-dispatch seam, proven with one elementwise op (add). The op routes
// on operand placement: both-on-device → a cajeta.gpu @Kernel over the device
// buffers; host operands → the CPU loop. The two paths agree (cross-check). The
// op + kernel live in the test program (the op library is numpy-porting-plan
// Phase 3); the Tensor type supplies the seam: placement + deviceBuffer + flat
// access.
TEST(TensorTests, seamElementwiseCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuStream;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void addF32(GpuBuffer<float32> out, GpuBuffer<float32> a,\n"
        "                              GpuBuffer<float32> b, uint32 n) {\n"
        "        uint32 i = GpuThread.globalIdX();\n"
        "        if (i < n) { out[i] = a[i] + b[i]; }\n"
        "    }\n"
        // the elementwise-add op, routed through the placement seam
        "    public static #Tensor<float32> add(Tensor<float32> a, Tensor<float32> b) {\n"
        "        Tensor<float32> out = Tensor.zerosLike<float32>(a);\n"
        "        int64 n = a.size();\n"
        "        if (a.isOnGpu() && b.isOnGpu()) {\n"
        "            out.gpu();\n"
        "            uint32 un = (uint32) n;\n"
        "            GpuStream s = GpuStream.current();\n"
        "            addF32.launch(s, grid: [(un + 63) / 64], block: [64])"
        "(out.deviceBuffer(), a.deviceBuffer(), b.deviceBuffer(), un);\n"
        "            s.sync();\n"
        "        } else {\n"
        "            int64 i = 0;\n"
        "            while (i < n) {\n"
        "                float32 v = a.flatGet(i) + b.flatGet(i);\n"
        "                out.flatSet(i, v);\n"
        "                i = i + 1;\n"
        "            }\n"
        "        }\n"
        "        return #out;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] da = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };\n"
        "        float32[] db = { 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f };\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 8;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, shp);\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, shp);\n"
        "        Tensor<float32> cCpu = D.add(a, b);\n"           // both host → CPU path
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float32> cGpu = D.add(a, b);\n"           // both device → GPU path
        "        cGpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 want = da[(int32) i] + db[(int32) i];\n"
        "            if (cCpu.get1(i) != want) { return -1; }\n"
        "            if (cGpu.get1(i) != want) { return -2; }\n"  // GPU agrees with CPU + oracle
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}
