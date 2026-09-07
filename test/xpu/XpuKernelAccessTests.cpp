// xpu-tile-manifest Unit 3 — access modes and sets (spec §6): the manifest
// records, per buffer-like parameter, what the lowered body does to it
// (read / write / readwrite / accumulate, origin derived), what the author
// declared (`@Access`, origin declared, checked against the body), whether
// loads were lowered non-temporal (`@Streaming`), and the two facts the
// scheduler derives from the sets: restartable (§6.5) and drainsDevice
// (§6.7). Scheduler.submit builds a submission's sets from the manifest and
// the bound handles (§6.6).
//
// Classification runs on the LOWERED IR (pointer provenance back to the
// parameter), so every access shape the lowering emits — scalar, vector,
// coop-matrix, atomic — is one walk; the CPU backend is used where the
// device does not matter (GPU-free) and gfx1151 ISA emission where it does.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"

#include "cajeta/xpu/core/KernelAccess.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"
#include "cajeta/error/Exception.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::MethodPtr;
using cajeta::xpu::KernelAccessEntry;
using cajeta::xpu::KernelAccessSummary;
using cajeta::xpu::KernelManifest;
using cajeta_test::CajetaJit;

namespace fs = std::filesystem;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_access_" + std::to_string(rng()));
    fs::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = fs::temp_directory_path() / ("cajeta_xpu_access_arch_" + std::to_string(rng()));
    fs::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

const char* kImports =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Barrier;\n"
    "public class M {\n";
const char* kEnd = "}\n";

std::string src(const std::string& body) { return std::string(kImports) + body + kEnd; }

// One kernel, four buffers, one shape each: loads only, stores only, both,
// and an atomic.
const char* kModes =
    "    @Kernel\n"
    "    public static void modes(KernelBuffer<float32> r, KernelBuffer<float32> w,\n"
    "                             KernelBuffer<float32> rw, KernelBuffer<int32> acc,\n"
    "                             uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            w[i] = r[i];\n"
    "            rw[i] = rw[i] + 1.0f;\n"
    "            acc.atomicAdd(0, 1);\n"
    "        }\n"
    "    }\n";

const char* kCopy =
    "    @Kernel\n"
    "    public static void copy(KernelBuffer<float32> x, KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i]; }\n"
    "    }\n";

const char* kCopyCount =
    "    @Kernel\n"
    "    public static void copyCount(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                                 KernelBuffer<int32> c, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i]; c.atomicAdd(0, 1); }\n"
    "    }\n";

// A global reduction: every write lands on a compile-time-constant element.
const char* kReduce =
    "    @Kernel\n"
    "    public static void reduce(KernelBuffer<float32> x, KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            float32 s = 0.0f;\n"
    "            uint32 k = 0;\n"
    "            while (k < n) { s = s + x[k]; k = k + 1; }\n"
    "            out[0] = s;\n"
    "        }\n"
    "    }\n";

// The same, accumulating atomically into one element from every work-item.
const char* kReduceAtomic =
    "    @Kernel\n"
    "    public static void reduceAtomic(KernelBuffer<float32> x, KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { out.atomicAdd(0, x[i]); }\n"
    "    }\n";

const char* kStreaming =
    "    @Kernel\n"
    "    public static void stream(@Streaming KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                              uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i] * 2.0f; }\n"
    "    }\n";

const char* kDeclaredOk =
    "    @Kernel\n"
    "    public static void declared(@Access(read) KernelBuffer<float32> x,\n"
    "                                @Access(write) KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i]; }\n"
    "    }\n";

const char* kDeclaredBad =
    "    @Kernel\n"
    "    public static void declaredBad(@Access(write) KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = y[i] + 1.0f; }\n"
    "    }\n";

std::vector<KernelManifest> cpuManifests(const std::string& source,
                                         const std::vector<std::string>& names) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    std::vector<MethodPtr> kernels;
    for (const auto& n : names) {
        auto k = findMethod(module->getStructures()["test.M"], n);
        if (k) kernels.push_back(k);
    }
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_access_host_cpu", ctx);
    std::vector<KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration(kernels, host, "", &out);
    return out;
}

const KernelAccessEntry* entryFor(const KernelManifest& m, const std::string& param) {
    for (const auto& e : m.access)
        if (e.param == param) return &e;
    return nullptr;
}

// Lower `name` for gfx1151 (GPU-free: no lld, no device), classify the lowered
// body, and emit the ISA text.
struct AmdLowered {
    KernelAccessSummary access;
    std::string isa;
    bool ok = false;
};
AmdLowered amdLower(const std::string& source, const std::string& name) {
    AmdLowered out;
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], name);
    if (!k) return out;
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    if (!tm) return out;
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu.dev." + name, ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    llvm::Function* fn = cajeta::xpu::amd::lowerKernel(k, dev);
    if (!fn) return out;
    out.access = cajeta::xpu::classifyKernelAccess(*fn, k);
    out.isa = cajeta::xpu::amd::emitIsa(dev, *tm);
    out.ok = true;
    return out;
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

std::string errorOf(const std::string& source) {
    try {
        CajetaJit::compile(source, "test.M", cpuOptions());
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

} // namespace

// 3.1.1 — modes derived from the body: loads only → read; stores only →
// write; both → readwrite; an atomic → accumulate; every origin `derived`.
TEST(XpuKernelAccess, modesAreDerivedFromTheBody) {
    auto ms = cpuManifests(src(kModes), {"modes"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelManifest& m = ms[0];
    ASSERT_EQ(m.access.size(), 4u) << cajeta::xpu::toJson(m);
    struct Want { const char* param; const char* mode; };
    for (Want w : {Want{"r", "read"}, Want{"w", "write"}, Want{"rw", "readwrite"},
                   Want{"acc", "accumulate"}}) {
        const KernelAccessEntry* e = entryFor(m, w.param);
        ASSERT_NE(e, nullptr) << w.param;
        EXPECT_EQ(e->mode, w.mode) << w.param;
        EXPECT_EQ(e->origin, "derived") << w.param;
        EXPECT_EQ(e->kind, "kernelBuffer") << w.param;
        EXPECT_FALSE(e->streaming) << w.param;
    }
    // Declaration order is preserved; the scalar `n` is not an access entry.
    EXPECT_EQ(m.access[0].param, "r");
    EXPECT_EQ(m.access[3].param, "acc");
    EXPECT_FALSE(m.restartable) << "readwrite + accumulate buffers";
    EXPECT_FALSE(m.drainsDevice) << "rw[i] is a per-item write";
}

// 3.1.4 — disjoint read and write sets, no accumulate → restartable; one
// atomic flips it.
TEST(XpuKernelAccess, restartableFollowsTheSets) {
    auto ms = cpuManifests(src(std::string(kCopy) + kCopyCount), {"copy", "copyCount"});
    ASSERT_EQ(ms.size(), 2u);
    const KernelManifest* copy = &ms[0];
    const KernelManifest* counted = &ms[1];
    if (copy->simpleName() != "copy") std::swap(copy, counted);
    EXPECT_TRUE(copy->restartable) << cajeta::xpu::toJson(*copy);
    EXPECT_FALSE(counted->restartable) << cajeta::xpu::toJson(*counted);
    ASSERT_NE(entryFor(*counted, "c"), nullptr);
    EXPECT_EQ(entryFor(*counted, "c")->mode, "accumulate");
}

// 3.1.5 — a global reduction (every write on a compile-time-constant
// element) is `drainsDevice`; an elementwise kernel is not.
TEST(XpuKernelAccess, drainsDeviceForConstantIndexedWrites) {
    auto ms = cpuManifests(src(std::string(kReduce) + kReduceAtomic + kCopy),
                           {"reduce", "reduceAtomic", "copy"});
    ASSERT_EQ(ms.size(), 3u);
    for (const KernelManifest& m : ms) {
        if (m.simpleName() == "copy") {
            EXPECT_FALSE(m.drainsDevice) << cajeta::xpu::toJson(m);
        } else {
            EXPECT_TRUE(m.drainsDevice) << cajeta::xpu::toJson(m);
        }
    }
}

// 3.1.3 — `@Streaming` on a read buffer: AMD lowers its loads non-temporal
// (the ISA carries the cache-policy bit) and the manifest records streaming.
TEST(XpuKernelAccess, streamingLowersNontemporalLoadsOnAmd) {
    AmdLowered lw = amdLower(src(kStreaming), "stream");
    ASSERT_TRUE(lw.ok) << "gfx1151 lowering unavailable";
    const KernelAccessEntry* x = nullptr;
    const KernelAccessEntry* y = nullptr;
    for (const auto& e : lw.access.entries) {
        if (e.param == "x") x = &e;
        if (e.param == "y") y = &e;
    }
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    EXPECT_TRUE(x->streaming) << "the @Streaming read buffer";
    EXPECT_FALSE(y->streaming) << "the plain write buffer";
    EXPECT_EQ(x->mode, "read");
    // The load of x carries the non-temporal cache policy; the store to y
    // does not. gfx11 spells it `slc` / `nt` depending on the assembler.
    size_t load = lw.isa.find("global_load");
    ASSERT_NE(load, std::string::npos) << lw.isa;
    std::string loadLine = lw.isa.substr(load, lw.isa.find('\n', load) - load);
    EXPECT_TRUE(loadLine.find(" nt") != std::string::npos
                || loadLine.find("slc") != std::string::npos
                || loadLine.find("th:") != std::string::npos)
        << "no non-temporal policy on the streaming load: " << loadLine;
    size_t store = lw.isa.find("global_store");
    ASSERT_NE(store, std::string::npos) << lw.isa;
    std::string storeLine = lw.isa.substr(store, lw.isa.find('\n', store) - store);
    EXPECT_TRUE(storeLine.find(" nt") == std::string::npos
                && storeLine.find("slc") == std::string::npos)
        << "the plain store must not be non-temporal: " << storeLine;
}

// The CPU backend does not lower non-temporal: the manifest must not claim it.
TEST(XpuKernelAccess, streamingIsNotClaimedWhereNotLowered) {
    auto ms = cpuManifests(src(kStreaming), {"stream"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelAccessEntry* x = entryFor(ms[0], "x");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->streaming) << cajeta::xpu::toJson(ms[0]);
    EXPECT_EQ(x->mode, "read");
}

// 3.1.2 — a declared mode the body contradicts is a compile error; a
// consistent one is recorded with origin `declared`.
TEST(XpuKernelAccess, declaredAccessIsCheckedAgainstTheBody) {
    EXPECT_EQ(errorOf(src(kDeclaredBad)), "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED");
    auto ms = cpuManifests(src(kDeclaredOk), {"declared"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelAccessEntry* x = entryFor(ms[0], "x");
    const KernelAccessEntry* y = entryFor(ms[0], "y");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(x->mode, "read");
    EXPECT_EQ(x->origin, "declared");
    EXPECT_EQ(y->mode, "write");
    EXPECT_EQ(y->origin, "declared");
    EXPECT_TRUE(ms[0].restartable);
}

// The JSON carries the access list and the two derived booleans.
TEST(XpuKernelAccess, manifestJsonCarriesAccessAndDerivedFlags) {
    auto ms = cpuManifests(src(kModes), {"modes"});
    ASSERT_EQ(ms.size(), 1u);
    std::string json = cajeta::xpu::toJson(ms[0]);
    auto doc = llvm::json::parse(json);
    ASSERT_TRUE(!!doc) << json;
    auto* root = doc->getAsObject();
    auto* access = root->getArray("access");
    ASSERT_NE(access, nullptr) << json;
    EXPECT_EQ(access->size(), 4u);
    auto* first = (*access)[0].getAsObject();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->getString("param").value_or(""), "r");
    EXPECT_EQ(first->getString("mode").value_or(""), "read");
    EXPECT_EQ(first->getString("origin").value_or(""), "derived");
    EXPECT_EQ(first->getString("kind").value_or(""), "kernelBuffer");
    EXPECT_EQ(root->getBoolean("restartable").value_or(true), false);
    EXPECT_EQ(root->getBoolean("drainsDevice").value_or(true), false);

    KernelManifest back;
    std::string err;
    ASSERT_TRUE(cajeta::xpu::fromJson(json, back, &err)) << err;
    ASSERT_EQ(back.access.size(), 4u);
    EXPECT_EQ(back.access[2].mode, "readwrite");
    EXPECT_EQ(cajeta::xpu::toJson(back), json);
}

// 3.1.6 — Scheduler.submit builds the sets from the manifest and the handles
// bound in parameter order; an author override that disagrees is refused.
TEST(XpuKernelAccess, submitDerivesSetsFromTheManifest) {
    const char* program =
        "package test;\n"
        "import cajeta.error.RecoverableException;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Scheduler;\n"
        "import cajeta.xpu.KernelSubmission;\n"
        "import cajeta.xpu.KernelResourceDescriptor;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void add(KernelBuffer<int32> out, KernelBuffer<int32> a,\n"
        "                           KernelBuffer<int32> b, KernelBuffer<int32> hits) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < 64) { out[i] = a[i] + b[i]; hits.atomicAdd(0, 1); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 n = 64;\n"
        "        KernelBuffer<int32> da = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> db = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> dc = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> dh = heap KernelBuffer<int32>(0, 1);\n"
        "        da.allocate(); db.allocate(); dc.allocate(); dh.allocate();\n"
        "        int64 haH = da.handle();\n"
        "        int64 hbH = db.handle();\n"
        "        int64 hcH = dc.handle();\n"
        "        int64 hhH = dh.handle();\n"
        "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
        "        sub.kernel = \"add\";\n"
        "        sub.bind(dc.handle());\n"
        "        sub.bind(da.handle());\n"
        "        sub.bind(db.handle());\n"
        "        sub.bind(dh.handle());\n"
        "        KernelResourceDescriptor desc #= Scheduler.submit(sub);\n"
        "        int32 code = 0;\n"
        "        if (sub.readSet.count() != 2) { code = 30; }\n"
        "        else if (sub.writeSet.count() != 2) { code = 31; }\n"
        "        else if (sub.accumulateSet.count() != 1) { code = 32; }\n"
        "        else if (sub.readSet.get(0) != haH || sub.readSet.get(1) != hbH) { code = 33; }\n"
        "        else if (sub.writeSet.get(0) != hcH || sub.writeSet.get(1) != hhH) { code = 34; }\n"
        "        else if (sub.accumulateSet.get(0) != hhH) { code = 35; }\n"
        "        da.free(); db.free(); dc.free(); dh.free();\n"
        "        return code;\n"
        "    }\n"
        "    public static int32 runDisagree() {\n"
        "        int32 n = 64;\n"
        "        KernelBuffer<int32> da = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> db = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> dc = heap KernelBuffer<int32>(0, n);\n"
        "        KernelBuffer<int32> dh = heap KernelBuffer<int32>(0, 1);\n"
        "        da.allocate(); db.allocate(); dc.allocate(); dh.allocate();\n"
        "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
        "        sub.kernel = \"add\";\n"
        "        sub.bind(dc.handle());\n"
        "        sub.bind(da.handle());\n"
        "        sub.bind(db.handle());\n"
        "        sub.bind(dh.handle());\n"
        "        sub.reads(dc.handle());\n"   // the body WRITES out — a lie
        "        int32 code = 40;\n"
        "        try {\n"
        "            KernelResourceDescriptor desc #= Scheduler.submit(sub);\n"
        "        } catch (RecoverableException e) {\n"
        "            code = 0;\n"
        "        }\n"
        "        da.free(); db.free(); dc.free(); dh.free();\n"
        "        return code;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(program, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0);
    auto disagree = jit->lookup<int (*)()>("runDisagree");
    ASSERT_NE(disagree, nullptr);
    EXPECT_EQ(disagree(), 0);
}
