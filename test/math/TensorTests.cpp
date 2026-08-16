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
    "import cajeta.math.BroadcastException;\n";

} // namespace

// 2a — of/zeros/ones/full/arange (+ _like) produce the right shape/strides/ndim/
// size; C-order (the only layout).

// 2b — multiple Tensors over one Storage (alias) share the buffer (writes show
// through both ways); base() reports the source; the buffer frees exactly once
// (no leak / no double-free — several aliases all drop at scope exit, no crash).

// 2c — itemsize/nbytes/isContiguous/dtype + element read on a freshly-built
// contiguous tensor.

// 3a — reshape(contiguous)/transpose/slice/squeeze/expandDims are VIEWS: share
// the base (writes show through; base() is the source).

// 3b — copy() and a non-contiguous reshape produce INDEPENDENT storage
// (mutation does not show through).

// 3c — in-place writes through a (non-overlapping) structural view are
// well-defined: each element maps to a distinct storage cell, no corruption.

// 4a — broadcastShape matches numpy's right-aligned rule across compatible cases
// and throws BroadcastException on the incompatible ones.

// 4b — broadcastTo yields a stride-0 view (no copy); reads return the stretched
// values; an incompatible target throws.

// 5a — basic indexing: integer index (removes axis), slice with step + negative
// indices, reverseAxis — all VIEWS sharing storage.

// 5b — boolean indexing: masked read is an independent 1-D copy; masked write
// scatters into the source.

// 5c — fancy indexing: take (gather, independent copy, negative wraps); put
// (scatter into the source).

// 7c (capture) — the Tensor<?> airlock: a Tensor<float32> widened to Tensor<?>
// captures back to Tensor<float32> via a reified instanceof guard + cast, sharing
// storage (a write through the captured handle shows through the original); a
// wrong-dtype instanceof is false. Sound because cajeta monomorphizes (see
// documents/cajeta-templates/reified-capture-spec.md).

// 6a — device placement: to-gpu / to-cpu move storage; device()/isOnGpu() report
// it; data survives a host→device→host round-trip; a no-GPU build still works
// (the cajeta.xpu CPU backend backs the buffer).

// 6b — the op-dispatch seam, proven with one elementwise op (add). The op routes
// on operand placement: both-on-device → a cajeta.xpu @Kernel over the device
// buffers; host operands → the CPU loop. The two paths agree (cross-check). The
// op + kernel live in the test program (the op library is numpy-porting-plan
// Phase 3); the Tensor type supplies the seam: placement + deviceBuffer + flat
// access.

// 7a — the interop protocol round-trips a Tensor zero-copy: t.protocol() exports
// { Storage borrow, dtype, shape, strides, device, read-only }; Tensor.fromProtocol
// rebuilds a Tensor<?> sharing the SAME Storage (a write through the rebuilt handle
// shows through the original), with shape/dtype/device/read-only preserved.

// 7c (exact) — a Tensor<?> from fromProtocol captures to the concrete Tensor<T>
// iff the reified dtype matches: an int32 protocol rebuilds a Tensor<int32> (not
// Tensor<float32>), shares storage, and an exact-dtype capture cast succeeds while
// a wrong-dtype instanceof is false. (The bounded Tensor<? extends Floating> form
// is deferred to numeric-bounds-plan — the numeric conformance predicate.)

// 7c (bounded) — a Tensor<?> recovered from fromProtocol admits/rejects a bounded
// NUMERIC wildcard by reified dtype KIND: a float32 tensor ⊨ Tensor<? extends
// Floating>, an int32 tensor ⊭ it; the admitted float captures to the concrete
// Tensor<float32> and shares storage. Backed by the runtime numeric-marker
// conformance in __cajeta_instanceof_bounded (numeric-bounds). Exercises TWO
// fromProtocol tensors in one scope — the regression guard for the interop-drop
// fix (Object-erased TensorProtocol backing, not Storage<?>).
TEST(TensorTests, wildcardBoundedFloatingFromProtocol) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.TensorProtocol;\n"
        "import cajeta.lang.Floating;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tf = Tensor.arange<float32>(4);\n"
        "        TensorProtocol pf = tf.protocol();\n"
        "        Tensor<?> wf = Tensor.fromProtocol(pf);\n"
        "        Tensor<int32> ti = Tensor.arange<int32>(4);\n"
        "        TensorProtocol pi = ti.protocol();\n"
        "        Tensor<?> wi = Tensor.fromProtocol(pi);\n"
        "        int32 r = 0;\n"
        "        if (wf instanceof Tensor<? extends Floating>) { r = r + 1; }\n"    // float ⊨ Floating
        "        if (wi instanceof Tensor<? extends Floating>) { r = r + 10; }\n"   // int ⊭ Floating
        "        if (r != 1) { return -1; }\n"
        "        Tensor<float32> cap = (Tensor<float32>) wf;\n"        // capture admitted float
        "        cap.set1(0, 9.0f);\n"
        "        if (tf.get1(0) != 9.0f) { return -2; }\n"            // zero-copy through the chain
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7b — external-producer import via the protocol with the CONTIGUITY contract
// (fromProtocolContiguous). Two paths in one scope: (1) an AMENABLE (already
// contiguous) layout is consumed zero-copy — the result is a view, writes show
// through to the producer; (2) a NON-AMENABLE (transposed / non-contiguous) layout
// is COPIED into an independent contiguous tensor — not a view, writes don't show
// through. isView() is how the import "says so". Regression guard for the
// rebuildShared/rebuildContiguous borrowed-Storage UAF: the copy path materializes
// directly from the borrow (materializeFrom) and must NOT free the producer buffer.
TEST(TensorTests, externalProducerZeroCopy) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.TensorProtocol;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] data = [ 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f ];\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<float32> producer = Tensor.of<float32>(data, shp);\n"   // [[0,1,2],[3,4,5]]
        // (1) amenable: already contiguous → zero-copy view, writes show through
        "        TensorProtocol pc = producer.protocol();\n"
        "        Tensor<?> wc = Tensor.fromProtocolContiguous(pc);\n"
        "        Tensor<float32> capc = (Tensor<float32>) wc;\n"
        "        if (!capc.isView()) { return -1; }\n"
        "        if (capc.get2(1, 2) != 5.0f) { return -2; }\n"
        "        capc.set2(0, 1, 77.0f);\n"
        "        if (producer.get2(0, 1) != 77.0f) { return -3; }\n"           // shows through (zero-copy)
        // (2) non-amenable: transpose → non-contiguous → independent copy
        "        Tensor<float32> tr = producer.transpose();\n"                  // [3,2], non-contiguous
        "        TensorProtocol pt = tr.protocol();\n"
        "        Tensor<?> wt = Tensor.fromProtocolContiguous(pt);\n"
        "        Tensor<float32> capt = (Tensor<float32>) wt;\n"
        "        if (capt.isView()) { return -4; }\n"
        "        if (!capt.isContiguous()) { return -5; }\n"
        "        if (capt.get2(0, 0) != 0.0f) { return -6; }\n"               // tr[0,0] == producer[0,0]
        "        if (capt.get2(2, 1) != 5.0f) { return -7; }\n"               // tr[2,1] == producer[1,2]
        "        capt.set2(0, 0, 99.0f);\n"
        "        if (producer.get2(0, 0) != 0.0f) { return -8; }\n"           // independent — producer unchanged
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
