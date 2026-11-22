// A cpu kernel whose work-item loop the vectorizer does not widen runs one work-item at a time.
// The build names it with `[xpu-cpu-scalar]` and the reason (gpu-kernel-parity 19.1.1, spec 13.2).
#include "XpuCpuBarrierFissionShapes.h"
#include <cstdlib>
#include <string>

using namespace cajeta_fission_shapes;

namespace {

const char* RUN = "    public static int32 run() { return 1; }\n";

const char* ELEMENTWISE =
    "    @Kernel\n"
    "    public static void scaleAdd(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i] * 2.0f + 1.0f; }\n"
    "    }\n";

const char* ROW_SUM =
    "    @Kernel\n"
    "    public static void rowSum(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 rows, uint32 cols) {\n"
    "        uint32 r = KernelThread.globalIdX();\n"
    "        if (r < rows) {\n"
    "            float32 acc = 0.0f;\n"
    "            for (uint32 k = 0; k < cols; k = k + 1) { acc = acc + x[r * cols + k]; }\n"
    "            y[r] = acc;\n"
    "        }\n"
    "    }\n";

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuScalarNote, anElementwiseKernelVectorizesSilently) {
    std::string err = compileCpuCapturingStderr(std::string(PRE) + ELEMENTWISE + RUN + END);
    EXPECT_EQ(err.find("[xpu-cpu-scalar]"), std::string::npos)
        << "a straight-line elementwise kernel's work-item loop vectorizes; no note expected:\n" << err;
}

TEST(XpuCpuScalarNote, aKernelWithItsOwnLoopIsNamedWithTheReason) {
    std::string err = compileCpuCapturingStderr(std::string(PRE) + ROW_SUM + RUN + END);
    const size_t at = err.find("[xpu-cpu-scalar] rowSum");
    ASSERT_NE(at, std::string::npos)
        << "a kernel whose body has its own loop keeps a scalar work-item loop; the build must say so:\n"
        << err;
    const std::string line = err.substr(at, err.find('\n', at) - at);
    EXPECT_NE(line.find("("), std::string::npos) << "the note must give a reason: " << line;
}

TEST(XpuCpuScalarNote, vectorizationForcedOffNamesEveryKernel) {
    ScopedEnv off("CAJETA_XPU_CPU_NO_VECTORIZE", "1");
    std::string err = compileCpuCapturingStderr(std::string(PRE) + ELEMENTWISE + RUN + END);
    EXPECT_NE(err.find("[xpu-cpu-scalar] scaleAdd"), std::string::npos)
        << "with CAJETA_XPU_CPU_NO_VECTORIZE no work-item loop is widened, and the note fires:\n" << err;
    EXPECT_NE(err.find("CAJETA_XPU_CPU_NO_VECTORIZE"), std::string::npos)
        << "the reason names the switch:\n" << err;
}
