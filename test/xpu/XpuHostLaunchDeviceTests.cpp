//
// CajetaXPU step 11 — host-source launch on a real NVIDIA device.
//
// The milestone the C++-orchestrated XpuSaxpyDeviceTests set up for: here
// BOTH the @Kernel AND the host driver are Cajeta source, compiled through
// the LLJIT, running on the GPU. No C++ touches the device — the entire
// orchestration (allocate, upload, launch, sync, download, free) is written
// in Cajeta and lowered by the compiler:
//
//   Buffer.alloc      -> __cajeta_xpu_buffer_alloc -> cuMemAlloc
//   buf.upload        -> __cajeta_xpu_buffer_upload -> cuMemcpyHtoD
//   kernel.launch(..) -> __cajeta_xpu_launch        -> cuLaunchKernel
//   stream.sync       -> __cajeta_xpu_stream_sync    -> cuCtxSynchronize
//   buf.download      -> __cajeta_xpu_buffer_download -> cuMemcpyDtoH
//   buf.free          -> __cajeta_xpu_buffer_free     -> cuMemFree
//
// The kernel cubin is built + registered by the device-cubin pass
// (NvptxRegistration) via an llvm.global_ctors entry that LLJIT runs at
// initialize() time, before run() executes.
//
// This is the variance-discipline payoff in full: one source, one memory
// model — the launch borrows each Buffer until stream.sync(), and freeing
// before the sync would be a compile error (XPU-K02) — compiling to a real
// device launch. Skips cleanly when no CUDA device/driver is present.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"

using cajeta_test::CajetaJit;
using cajeta::xpu::nvidia::CudaDriver;

// The step-11 milestone, live: BOTH the @Kernel AND the host driver are Cajeta
// source, compiled through the LLJIT, running SAXPY on the real GPU. The full
// NVPTX host-launch infrastructure backs it — the CUDA-backed runtime
// (cajeta_runtime.c), the Buffer<T>.elementBytes() intrinsic, the
// cubin-registration global-ctor pass (NvptxRegistration) wired into
// JitTestHelper, and the device kernel host stub.
//
// Lighting this up required one frontend codegen fix, surfaced here because
// this is the first code to actually orchestrate a kernel from Cajeta source:
// Stream.current() returns a NULL handle (the CUDA default-stream sentinel),
// but `s.sync()` was virtually dispatched, loading the vtable from the null
// receiver and segfaulting. @Native methods are leaf forwarders to a fixed C
// symbol — inherently non-polymorphic — so CajetaClass::invokeMethod now
// direct-dispatches them, passing the null handle through to the forwarder
// (which ignores `self`) instead of dereferencing it. See CajetaClass.cpp.
TEST(XpuHostLaunchDeviceTests, saxpyHostSourceOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    auto src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Saxpy {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                             float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 1024;\n"
        "        float32[] hx = heap float32[n];\n"
        "        float32[] hy = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            hx[i] = 1.0f;\n"
        "            hy[i] = 2.0f;\n"
        "        }\n"
        "        Buffer<float32> x = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> y = heap Buffer<float32>(0, n);\n"
        "        x.allocate();\n"
        "        y.allocate();\n"
        "        x.upload(hx);\n"
        "        y.upload(hy);\n"
        "        Stream s = Stream.current();\n"
        "        saxpy.launch(s, grid: [4], block: [256])(y, x, 2.0f, n);\n"
        "        s.sync();\n"
        "        y.download(hy);\n"
        "        x.free();\n"
        "        y.free();\n"
        "        float32 sum = 0.0f;\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            sum = sum + hy[i];\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.Saxpy");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // 1024 elements, each 2*1 + 2 = 4  ->  sum = 4096.
    EXPECT_FLOAT_EQ(fn(), 4096.0f);
}
