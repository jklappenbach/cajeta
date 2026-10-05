//
// The kernel conformance corpus (xpu-kernel-independence plan Unit 1, spec
// §2.1, §2.2, §2.4).
//
// A backend's launches are recorded as they run (CAJETA_XPU_RECORD), and the
// corpus runner replays each one through the reference interpreter and
// compares what the backend wrote. These tests record real cpu launches,
// including one from a lowering broken on purpose, and hand-made recordings
// whose answers are known, and check what the runner reports.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/reference/Conformance.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void corpusDot(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                                 KernelBuffer<int8> a, KernelBuffer<int32> zero) {
        uint32 g = KernelThread.globalIdX();
        Vector<uint8,16> wv = w.vload<16>((int64) g * 16L);
        Vector<int8,16> av = a.vload<16>((int64) g * 16L);
        Vector<int32,4> acc = zero.vload<4>((int64) g * 4L);
        out.vstore((int64) g * 4L, wv.dotAccum(av, acc));
    }
    @Kernel
    public static void corpusScale(KernelBuffer<float32> y, KernelBuffer<float32> x,
                                   float32 s) {
        uint32 i = KernelThread.globalIdX();
        y[i] = x[i] * s;
    }
    public static int32 run() {
        uint8[] hw = heap uint8[128];
        int8[] ha = heap int8[128];
        for (int32 i = 0; i < 128; i = i + 1) {
            hw[i] = (uint8) (i * 29 + 3);
            ha[i] = (int8) (0 - (i % 100));
        }
        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(128);
        KernelBuffer<int8> a = heap KernelBuffer<int8>(128);
        KernelBuffer<int32> zero = heap KernelBuffer<int32>(32);
        KernelBuffer<int32> out = heap KernelBuffer<int32>(32);
        w.upload(hw);
        a.upload(ha);
        int32[] hz = heap int32[32];
        zero.upload(hz);
        out.upload(hz);
        KernelStream s #= KernelStream.current();
        corpusDot.launch(s, grid: [1], block: [8])(out, w, a, zero);
        s.sync();
        int32[] got = heap int32[32];
        out.download(got);
        return got[0];
    }
}
)CJ";

fs::path freshDir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() /
                 ("cajeta-corpus-" + std::to_string(cajeta_getpid()) + "-" + tag);
    fs::remove_all(d);
    return d;
}

// Compile and run the host program on the cpu backend with launches recorded
// into `dir`. `fault` sets CAJETA_XPU_FAULT for the compile.
void recordOnCpu(const fs::path& dir, const char* fault) {
    if (fault) setenv("CAJETA_XPU_FAULT", fault, 1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (fault) unsetenv("CAJETA_XPU_FAULT");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    fn();
    unsetenv("CAJETA_XPU_RECORD");
}

// The kernels the runner replays against, from a separate compile of the
// same source: the corpus is matched by the name each kernel registers under.
struct Kernels {
    cajeta::Compiler compiler;
    cajeta::CajetaModulePtr module;
    std::vector<cajeta::MethodPtr> all;
    Kernels() {
        module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
        for (const char* n : {"corpusDot", "corpusScale"})
            if (auto k = findKernel(module, "test.M", n)) all.push_back(k);
    }
};

void writeHeld(const fs::path& p, const std::string& text) {
    std::ofstream(p) << text;
}

const ref::CorpusResult* only(const ref::CorpusRun& run, const std::string& kernel) {
    const ref::CorpusResult* hit = nullptr;
    for (auto& r : run.results)
        if (r.kernel.find(kernel) != std::string::npos) hit = &r;
    return hit;
}

// A hand-made recording of corpusScale: y = x * s over 4 elements, where the
// backend's y is `got`.
void handRecording(const fs::path& dir, const std::vector<float>& x, float s,
                   const std::vector<float>& got) {
    fs::path d = dir / "test.M.corpusScale.1";
    fs::create_directories(d);
    auto put = [&](const std::string& f, const void* p, size_t n) {
        std::ofstream o(d / f, std::ios::binary);
        o.write((const char*) p, (std::streamsize) n);
    };
    std::vector<float> y0(x.size(), 0.0f);
    put("a0.in", y0.data(), y0.size() * 4);
    put("a0.out", got.data(), got.size() * 4);
    put("a1.in", x.data(), x.size() * 4);
    put("a1.out", x.data(), x.size() * 4);
    uint32_t sb;
    std::memcpy(&sb, &s, 4);
    char hex[9];
    std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x", sb & 0xFF, (sb >> 8) & 0xFF,
                  (sb >> 16) & 0xFF, sb >> 24);
    std::ofstream(d / "launch.json")
        << "{\"kernel\":\"corpusScale\",\"backend\":\"cpu\",\"grid\":[1,1,1],"
           "\"block\":[" << x.size() << ",1,1],\"sharedBytes\":0,\"waveWidth\":0,"
           "\"allocs\":[{\"bytes\":" << x.size() * 4 << "},{\"bytes\":" << x.size() * 4
        << "}],\"args\":[{\"kind\":\"buffer\",\"alloc\":0,\"offset\":0},"
           "{\"kind\":\"buffer\",\"alloc\":1,\"offset\":0},{\"kind\":\"scalar\",\"hex\":\""
        << hex << "\"}]}\n";
}

} // namespace

// The recorder writes a real cpu launch, and a clean lowering passes.
TEST(XpuConformanceCorpus, aRecordedCpuLaunchReplaysAndPasses) {
    fs::path dir = freshDir("clean");
    recordOnCpu(dir, nullptr);
    ASSERT_TRUE(fs::exists(dir / "corpusDot.1" / "launch.json"))
        << "the cpu launch was not recorded under " << dir;
    Kernels k;
    ref::CorpusRun run = ref::runCorpus(k.all, dir.string(), "");
    const ref::CorpusResult* r = only(run, "corpusDot");
    ASSERT_NE(r, nullptr) << ref::toTsv(run);
    EXPECT_EQ(r->outcome, "pass") << r->detail;
    EXPECT_EQ(r->backend, "cpu");
    EXPECT_EQ(run.failures(), 0u) << ref::toTsv(run);
    fs::remove_all(dir);
}

// 4.1.1.1: a lowering broken on purpose (3ba58bda's signedness) fails the
// corpus, naming the kernel and the backend.
TEST(XpuConformanceCorpus, aBrokenLoweringFailsNamingKernelAndBackend) {
    fs::path dir = freshDir("fault");
    recordOnCpu(dir, "dot-activation-sign");
    Kernels k;
    ref::CorpusRun run = ref::runCorpus(k.all, dir.string(), "");
    const ref::CorpusResult* r = only(run, "corpusDot");
    ASSERT_NE(r, nullptr) << ref::toTsv(run);
    EXPECT_EQ(r->outcome, "fail");
    EXPECT_EQ(r->kernel, "corpusDot");
    EXPECT_EQ(r->backend, "cpu");
    EXPECT_NE(r->detail.find("out["), std::string::npos) << r->detail;
    EXPECT_EQ(run.failures(), 1u);
    fs::remove_all(dir);
}

// 4.1.1.2: a held kernel is reported held, not passed; an unheld one that
// disagrees fails; a hold on a kernel that agrees now is stale and fails.
TEST(XpuConformanceCorpus, aHeldKernelIsReportedHeldAndAStaleHoldFails) {
    fs::path dir = freshDir("held");
    recordOnCpu(dir, "dot-activation-sign");
    fs::path held = dir / "held.tsv";
    writeHeld(held, "# kernel\tbackend\tstatus\tnote\ncorpusDot\tcpu\theld\tplan 9.9.9\n");
    Kernels k;
    ref::CorpusRun run = ref::runCorpus(k.all, dir.string(), held.string());
    const ref::CorpusResult* r = only(run, "corpusDot");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->outcome, "held") << r->detail;
    EXPECT_NE(r->detail.find("plan 9.9.9"), std::string::npos) << r->detail;
    EXPECT_EQ(run.failures(), 0u);

    // Held on another backend only: the cpu disagreement fails.
    writeHeld(held, "corpusDot\tcuda\theld\tplan 9.9.9\n");
    run = ref::runCorpus(k.all, dir.string(), held.string());
    EXPECT_EQ(only(run, "corpusDot")->outcome, "fail");

    fs::path clean = freshDir("held-clean");
    recordOnCpu(clean, nullptr);
    writeHeld(clean / "held.tsv", "corpusDot\t*\theld\tplan 9.9.9\n");
    run = ref::runCorpus(k.all, clean.string(), (clean / "held.tsv").string());
    EXPECT_EQ(only(run, "corpusDot")->outcome, "stale");
    EXPECT_EQ(run.failures(), 1u);
    fs::remove_all(dir);
    fs::remove_all(clean);
}

// 4.1.1.3: integers bit for bit; floats within the stated bound, 0 ulp when
// none is stated.
TEST(XpuConformanceCorpus, floatsCompareWithinTheirStatedBound) {
    std::vector<float> x = {1.0f, 3.0f, -2.5f, 0.1f};
    float s = 1.75f;
    std::vector<float> exact(x.size()), twoOff(x.size());
    for (size_t i = 0; i < x.size(); ++i) exact[i] = twoOff[i] = x[i] * s;
    twoOff[3] = std::nextafter(std::nextafter(exact[3], 1.0f), 1.0f);
    Kernels k;

    fs::path dir = freshDir("bound");
    handRecording(dir, x, s, exact);
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), ""), "corpusScale")->outcome, "pass");
    fs::remove_all(dir);

    handRecording(dir, x, s, twoOff);
    auto none = ref::runCorpus(k.all, dir.string(), "");
    EXPECT_EQ(only(none, "corpusScale")->outcome, "fail");
    EXPECT_NE(only(none, "corpusScale")->detail.find("2 ulp, bound 0"), std::string::npos)
        << only(none, "corpusScale")->detail;
    writeHeld(dir / "held.tsv", "corpusScale\tcpu\tulps=1\tone rounding\n");
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), (dir / "held.tsv").string()),
                   "corpusScale")->outcome, "fail");
    writeHeld(dir / "held.tsv", "corpusScale\tcpu\tulps=2\ttwo roundings\n");
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), (dir / "held.tsv").string()),
                   "corpusScale")->outcome, "pass");
    fs::remove_all(dir);
}

// A float failure names the first element that disagrees, and also the worst
// distance and how many elements are out of bound, so a bound can be stated
// from one run.
TEST(XpuConformanceCorpus, aFloatFailureReportsTheWorstDistance) {
    std::vector<float> x = {1.0f, 3.0f, -2.5f, 0.1f};
    float s = 1.75f;
    std::vector<float> got(x.size());
    for (size_t i = 0; i < x.size(); ++i) got[i] = x[i] * s;
    got[1] = std::nextafter(got[1], 100.0f);
    for (int n = 0; n < 5; ++n) got[3] = std::nextafter(got[3], 100.0f);
    Kernels k;
    fs::path dir = freshDir("worst");
    handRecording(dir, x, s, got);
    auto run = ref::runCorpus(k.all, dir.string(), "");
    const ref::CorpusResult* r = only(run, "corpusScale");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->outcome, "fail");
    EXPECT_NE(r->detail.find("y[1]"), std::string::npos) << r->detail;
    EXPECT_NE(r->detail.find("worst 5 ulp"), std::string::npos) << r->detail;
    EXPECT_NE(r->detail.find("2 of 4 out of bound"), std::string::npos) << r->detail;
    fs::remove_all(dir);
}

// A relative bound measures an element's error against the largest reference
// magnitude in its view, so a result near zero that is many ulps away but a
// tiny fraction of the buffer's scale passes. Either bound admits an element.
TEST(XpuConformanceCorpus, aRelativeBoundIsMeasuredAgainstTheLargestValue) {
    std::vector<float> x = {1000.0f, 3.0f, -2.5f, 1e-6f};
    float s = 1.75f;
    std::vector<float> got(x.size());
    for (size_t i = 0; i < x.size(); ++i) got[i] = x[i] * s;
    got[3] *= 2.0f; // millions of ulps, about 1e-9 of the largest value
    Kernels k;
    fs::path dir = freshDir("rel");
    handRecording(dir, x, s, got);
    auto none = ref::runCorpus(k.all, dir.string(), "");
    const ref::CorpusResult* r = only(none, "corpusScale");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->outcome, "fail");
    EXPECT_NE(r->detail.find("relative"), std::string::npos) << r->detail;

    writeHeld(dir / "held.tsv", "corpusScale\tcpu\trel=1e-12\ttoo tight\n");
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), (dir / "held.tsv").string()),
                   "corpusScale")->outcome, "fail");
    writeHeld(dir / "held.tsv", "corpusScale\tcpu\trel=1e-6\tfast math\n");
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), (dir / "held.tsv").string()),
                   "corpusScale")->outcome, "pass");
    writeHeld(dir / "held.tsv", "corpusScale\tcpu\tulps=2,rel=1e-6\tboth\n");
    EXPECT_EQ(only(ref::runCorpus(k.all, dir.string(), (dir / "held.tsv").string()),
                   "corpusScale")->outcome, "pass");
    fs::remove_all(dir);
}
