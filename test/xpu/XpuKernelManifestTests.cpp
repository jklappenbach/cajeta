// xpu-tile-manifest Unit 1 — the per-(kernel, target) manifest record: identity
// (§2), footprint from the assembled artifact (§3), the `k.manifest()` host
// accessor (§12.1) and the JSON copy in the build output (§12.4).
//
// The manifest is built where the SHIPPED device code is assembled — each
// backend's registration emitter — so these tests drive
// `emitKernelRegistration` into a throwaway host module and read the records it
// hands back, exactly what the compiler and the JIT host consume. The AMD tests
// need `ld.lld` to produce a code object, so they gate on HIP hardware (the
// XpuDeviceTestUtil rule for AOT artifacts); the CPU tests are GPU-free.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"

#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/core/DeviceProfile.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuRegistration.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::MethodPtr;
using cajeta::xpu::KernelManifest;
using cajeta_test::CajetaJit;

namespace fs = std::filesystem;

namespace {

// ---- fixtures ------------------------------------------------------------ //

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xpu_manifest_" + std::to_string(rng()));
    fs::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = fs::temp_directory_path()
                 / ("cajeta_xpu_manifest_arch_" + std::to_string(rng()));
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
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n";

// A plain streaming kernel: the launch block is an argument (no pin).
const char* kSaxpy =
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n";

// The same kernel with ONE instruction changed (an add became a subtract).
const char* kSaxpyMinus =
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        if (i < n) { y[i] = a * x[i] - y[i]; }\n"
    "    }\n";

// A block pinned by @Occupancy: the manifest records the pin, not candidates.
const char* kPinned =
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256)\n"
    "    public static void pinned(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                              uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        if (i < n) { y[i] = x[i] * 2.0f; }\n"
    "    }\n";

// A kernel with static LDS, so the footprint carries ldsStaticBytes > 0.
const char* kLds =
    "    @Kernel\n"
    "    public static void withLds(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                               uint32 n) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        Shared<float32> s = shared float32[256];\n"
    "        s[t] = x[t];\n"
    "        Barrier.workgroup();\n"
    "        y[t] = s[255 - t];\n"
    "    }\n";

// A deliberate spiller: a 320-value forward recurrence consumed in REVERSE
// order. The recurrence cannot run backwards and floating-point sums cannot
// reassociate, so every value is live at the peak — 320 > the 256-VGPR file,
// let alone the 192-VGPR budget at the default 1024-thread workgroup. (A
// 32-accumulator WMMA kernel did NOT spill: independent fragments let the
// scheduler interleave mma and store per accumulator; measured 103 VGPRs.)
// Measured 2026-09-06 through the production hsaco path: 520 scratch bytes,
// 129 spilled VGPRs. Built programmatically so the count is one constant.
std::string spillerSource() {
    const int kValues = 320;
    std::string s =
        "    @Kernel\n"
        "    public static void spiller(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
        "                               float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.x();\n"
        "        float32 v0 = x[i];\n"
        "        float32 v1 = x[i + 1];\n";
    for (int k = 2; k < kValues; ++k)
        s += "        float32 v" + std::to_string(k) + " = v" + std::to_string(k - 1)
           + " * a + v" + std::to_string(k - 2) + ";\n";
    s += "        float32 s = v" + std::to_string(kValues - 1) + ";\n";
    for (int k = kValues - 2; k >= 0; --k)
        s += "        s = s * a + v" + std::to_string(k) + ";\n";
    s += "        y[i] = s;\n"
         "    }\n";
    return s;
}

const char* kEnd = "}\n";

std::string src(std::initializer_list<const char*> bodies) {
    std::string s = kImports;
    for (const char* b : bodies) s += b;
    return s + kEnd;
}

// Drive the AMD registration emitter for `names` in `source` against a
// throwaway host module; return the manifests it produced (one per kernel,
// gfx1151 only). This is the production path — the same call the compiler's
// emitXpuKernels and the JIT host make.
std::vector<KernelManifest> amdManifests(
        const std::string& source, const std::vector<std::string>& names,
        const cajeta::xpu::amd::KernelMaxThreads& maxThreads = {}) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    std::vector<MethodPtr> kernels;
    for (const auto& n : names) {
        auto k = findMethod(module->getStructures()["test.M"], n);
        if (k) kernels.push_back(k);
    }
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_manifest_host", ctx);
    std::vector<KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::amd::emitKernelRegistration(kernels, host, "gfx1151", maxThreads,
                                             &out);
    testing::internal::GetCapturedStderr();
    return out;
}

// The same for the CPU backend (GPU-free).
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
    llvm::Module host("xpu_manifest_host_cpu", ctx);
    std::vector<KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration(kernels, host, "", &out);
    return out;
}

// Assemble `name` from `source` into a gfx1151 code object through the same
// pipeline the registration uses (fresh device module, arch-list flag, lower,
// no workgroup pin) — the bytes the manifest's hash and footprint describe.
std::vector<uint8_t> amdCodeObject(const std::string& source, const std::string& name) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], name);
    if (!k) return {};
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu.dev." + name, ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    dev.addModuleFlag(llvm::Module::Warning, "cajeta.amdgpu.archlist",
                      llvm::MDString::get(ctx, "gfx1151"));
    if (!cajeta::xpu::amd::lowerKernel(k, dev)) return {};
    return cajeta::xpu::amd::assembleHsaco(dev, *tm, "gfx1151");
}

std::string findReadelf() {
    if (auto p = llvm::sys::findProgramByName("llvm-readelf")) return *p;
    if (const char* rocm = std::getenv("ROCM_PATH")) {
        std::string p = std::string(rocm) + "/llvm/bin/llvm-readelf";
        if (llvm::sys::fs::exists(p)) return p;
    }
    return {};
}

// `llvm-readelf --notes` renders the code object's AMDGPU metadata as YAML;
// pull `key: value` integers out of it (first occurrence — one kernel per
// object in these tests).
std::map<std::string, long> readelfNotes(const std::vector<uint8_t>& hsaco) {
    std::map<std::string, long> out;
    std::string readelf = findReadelf();
    if (readelf.empty()) return out;
    static std::mt19937_64 rng(std::random_device{}());
    auto dir = fs::temp_directory_path()
             / ("cajeta_xpu_manifest_readelf_" + std::to_string(rng()));
    fs::create_directories(dir);
    auto obj = dir / "k.hsaco";
    auto txt = dir / "notes.txt";
    {
        std::ofstream o(obj, std::ios::binary);
        o.write(reinterpret_cast<const char*>(hsaco.data()),
                (std::streamsize) hsaco.size());
    }
    // StringRefs must point at storage that outlives the call — a temporary
    // `path.string()` here once handed readelf a garbage path.
    std::string notesFlag = "--notes";
    std::string objPath = obj.string();
    std::string txtPath = txt.string();
    llvm::SmallVector<llvm::StringRef, 4> args = {readelf, notesFlag, objPath};
    std::optional<llvm::StringRef> redirects[3] = {std::nullopt, llvm::StringRef(txtPath),
                                                   std::nullopt};
    std::string err;
    int rc = llvm::sys::ExecuteAndWait(readelf, args, std::nullopt, redirects, 0, 0, &err);
    if (rc != 0) return out;
    std::ifstream in(txt);
    std::string line;
    static const std::regex kv(R"(^\s*(\.[a-z_]+):\s*(-?\d+)\s*$)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, kv) && !out.count(m[1]))
            out[m[1]] = std::stol(m[2]);
    }
    fs::remove_all(dir);
    return out;
}

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---- a small JSON-schema checker ---------------------------------------- //
// Covers the constructs tile-manifest-v1.schema.json uses: type, required,
// properties, const, enum, pattern, minimum/maximum, items, minItems/maxItems
// and $ref into #/$defs. Unknown properties are allowed (the schema declares
// no additionalProperties). No python in this repository, so the check is
// in-tree and runs with the suite.
struct SchemaCheck {
    const llvm::json::Object& root;
    std::vector<std::string> errors;

    const llvm::json::Object* deref(const llvm::json::Object& s) {
        auto ref = s.getString("$ref");
        if (!ref) return &s;
        llvm::StringRef r = *ref;
        if (!r.consume_front("#/")) return nullptr;
        const llvm::json::Object* cur = &root;
        llvm::SmallVector<llvm::StringRef, 4> parts;
        r.split(parts, '/');
        for (auto p : parts) {
            cur = cur->getObject(p);
            if (!cur) return nullptr;
        }
        return cur;
    }

    void fail(const std::string& path, const std::string& what) {
        errors.push_back(path + ": " + what);
    }

    void check(const llvm::json::Value& v, const llvm::json::Object& schemaIn,
               const std::string& path) {
        const llvm::json::Object* sp = deref(schemaIn);
        if (!sp) { fail(path, "unresolvable $ref"); return; }
        const llvm::json::Object& s = *sp;
        if (auto c = s.get("const")) {
            if (*c != v) fail(path, "const mismatch");
        }
        if (auto t = s.getString("type")) {
            bool ok = true;
            if (*t == "object")       ok = v.getAsObject() != nullptr;
            else if (*t == "array")   ok = v.getAsArray() != nullptr;
            else if (*t == "string")  ok = v.getAsString().has_value();
            else if (*t == "boolean") ok = v.getAsBoolean().has_value();
            else if (*t == "integer") ok = v.getAsInteger().has_value();
            else if (*t == "number")  ok = v.getAsNumber().has_value();
            if (!ok) fail(path, "expected type " + t->str());
        }
        if (auto e = s.getArray("enum")) {
            bool hit = false;
            for (auto& opt : *e) if (opt == v) hit = true;
            if (!hit) fail(path, "not in enum");
        }
        if (auto pat = s.getString("pattern")) {
            if (auto str = v.getAsString()) {
                if (!std::regex_search(str->str(), std::regex(pat->str())))
                    fail(path, "pattern " + pat->str() + " failed on '" + str->str() + "'");
            }
        }
        if (auto n = v.getAsInteger()) {
            if (auto mn = s.getInteger("minimum")) if (*n < *mn) fail(path, "below minimum");
            if (auto mx = s.getInteger("maximum")) if (*n > *mx) fail(path, "above maximum");
        }
        if (auto* obj = v.getAsObject()) {
            if (auto req = s.getArray("required")) {
                for (auto& r : *req) {
                    auto key = r.getAsString();
                    if (key && !obj->get(*key)) fail(path, "missing required '" + key->str() + "'");
                }
            }
            if (auto props = s.getObject("properties")) {
                for (auto& [key, sub] : *obj) {
                    if (auto ps = props->getObject(key))
                        check(sub, *ps, path + "." + key.str());
                }
            }
        }
        if (auto* arr = v.getAsArray()) {
            if (auto mn = s.getInteger("minItems")) if ((long) arr->size() < *mn) fail(path, "too few items");
            if (auto mx = s.getInteger("maxItems")) if ((long) arr->size() > *mx) fail(path, "too many items");
            if (auto items = s.getObject("items")) {
                size_t i = 0;
                for (auto& el : *arr) check(el, *items, path + "[" + std::to_string(i++) + "]");
            }
        }
    }
};

std::vector<std::string> validateAgainstSchema(const std::string& jsonText) {
    std::string schemaText = readFile(
        fs::path(sourceRoot()) / "specs" / "schemas" / "tile-manifest-v1.schema.json");
    auto schema = llvm::json::parse(schemaText);
    auto doc = llvm::json::parse(jsonText);
    std::vector<std::string> errors;
    if (!schema) { errors.push_back("schema does not parse"); return errors; }
    if (!doc) { errors.push_back("manifest JSON does not parse"); return errors; }
    SchemaCheck sc{*schema->getAsObject(), {}};
    sc.check(*doc, *schema->getAsObject(), "$");
    return sc.errors;
}

const KernelManifest* byName(const std::vector<KernelManifest>& ms, const std::string& simple) {
    for (auto& m : ms)
        if (m.kernel.size() >= simple.size()
                && m.kernel.compare(m.kernel.size() - simple.size(), simple.size(), simple) == 0)
            return &m;
    return nullptr;
}

} // namespace

// 1.1.1 — identity: qualified kernel name, target, sha256 code hash, compiler
// version, ABI 3, schema 1 (§2.1, §2.3).
TEST(XpuKernelManifest, amdManifestCarriesIdentity) {
    CAJETA_SKIP_IF_NO_HIP();
    auto ms = amdManifests(src({kSaxpy}), {"saxpy"});
    ASSERT_EQ(ms.size(), 1u) << "one manifest per (kernel, target)";
    const KernelManifest& m = ms[0];
    EXPECT_EQ(m.kernel, "test.M.saxpy");
    EXPECT_EQ(m.target, "amdgpu/gfx1151");
    EXPECT_TRUE(std::regex_match(m.codeHash, std::regex("^sha256:[0-9a-f]{64}$")))
        << "codeHash = " << m.codeHash;
    EXPECT_FALSE(m.compilerVersion.empty());
    EXPECT_EQ(m.compilerVersion, cajeta::xpu::compilerVersionString());
    EXPECT_EQ(m.xpuAbiVersion, 3);
    EXPECT_EQ(KernelManifest::kSchemaVersion, 1);

    auto doc = llvm::json::parse(cajeta::xpu::toJson(m));
    ASSERT_TRUE(!!doc) << "manifest JSON parses";
    auto* obj = doc->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getInteger("schemaVersion").value_or(-1), 1);
    auto* id = obj->getObject("identity");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->getInteger("xpuAbiVersion").value_or(-1), 3);
    EXPECT_EQ(id->getString("kernel").value_or(""), "test.M.saxpy");
}

// 1.1.2 — one instruction changed: the hash changes, nothing else in identity.
TEST(XpuKernelManifest, oneInstructionChangesOnlyTheHash) {
    CAJETA_SKIP_IF_NO_HIP();
    auto a = amdManifests(src({kSaxpy}), {"saxpy"});
    auto b = amdManifests(src({kSaxpyMinus}), {"saxpy"});
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_NE(a[0].codeHash, b[0].codeHash);
    EXPECT_EQ(a[0].kernel, b[0].kernel);
    EXPECT_EQ(a[0].target, b[0].target);
    EXPECT_EQ(a[0].compilerVersion, b[0].compilerVersion);
    EXPECT_EQ(a[0].xpuAbiVersion, b[0].xpuAbiVersion);
    EXPECT_EQ(a[0].instrumented, b[0].instrumented);

    // Same source twice is the same hash: the hash is over the code object,
    // and the code object is a function of the source.
    auto a2 = amdManifests(src({kSaxpy}), {"saxpy"});
    ASSERT_EQ(a2.size(), 1u);
    EXPECT_EQ(a[0].codeHash, a2[0].codeHash);
}

// 1.1.3a — footprint equals what llvm-readelf reports for the same code object
// (§3.1): vgpr, sgpr, spill (scratch) bytes, static LDS, wave width.
TEST(XpuKernelManifest, amdFootprintMatchesReadelf) {
    CAJETA_SKIP_IF_NO_HIP();
    if (findReadelf().empty()) GTEST_SKIP() << "llvm-readelf not found";
    std::string source = src({kLds});
    auto ms = amdManifests(source, {"withLds"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelManifest& m = ms[0];
    std::vector<uint8_t> hsaco = amdCodeObject(source, "withLds");
    ASSERT_FALSE(hsaco.empty()) << "ld.lld produced no code object";
    // The hash is over exactly these bytes.
    EXPECT_EQ(m.codeHash, cajeta::xpu::sha256Hex(hsaco.data(), hsaco.size()));

    auto notes = readelfNotes(hsaco);
    ASSERT_TRUE(notes.count(".vgpr_count")) << "readelf printed no AMDGPU metadata";
    ASSERT_TRUE(m.vgpr.has_value());
    ASSERT_TRUE(m.sgpr.has_value());
    ASSERT_TRUE(m.spillBytes.has_value());
    ASSERT_TRUE(m.ldsStaticBytes.has_value());
    ASSERT_TRUE(m.waveWidth.has_value());
    EXPECT_EQ((long) *m.vgpr,           notes[".vgpr_count"]);
    EXPECT_EQ((long) *m.sgpr,           notes[".sgpr_count"]);
    EXPECT_EQ((long) *m.spillBytes,     notes[".private_segment_fixed_size"]);
    EXPECT_EQ((long) *m.ldsStaticBytes, notes[".group_segment_fixed_size"]);
    EXPECT_EQ((long) *m.waveWidth,      notes[".wavefront_size"]);
    EXPECT_EQ(*m.ldsStaticBytes, 256u * 4u) << "shared float32[256]";
    EXPECT_EQ(*m.spillBytes, 0u) << "a streaming kernel must not touch scratch";
}

// 1.1.3b — a spilling kernel records non-zero spill and the build prints a
// warning naming it (§3.2). Never silently accepted as tuned.
TEST(XpuKernelManifest, spillingKernelRecordsSpillAndWarns) {
    CAJETA_SKIP_IF_NO_HIP();
    Compiler compiler;
    auto module = compileForInspection(compiler,
                                       std::string(kImports) + spillerSource() + kEnd);
    auto k = findMethod(module->getStructures()["test.M"], "spiller");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_manifest_host", ctx);
    std::vector<KernelManifest> out;
    testing::internal::CaptureStderr();
    // No workgroup pin: at the default 1024-thread budget this kernel spills.
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx1151", {}, &out);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].spillBytes.has_value());
    EXPECT_GT(*out[0].spillBytes, 0u) << cajeta::xpu::toJson(out[0]);
    EXPECT_NE(err.find("[xpu-kernel-spill]"), std::string::npos) << err;
    EXPECT_NE(err.find("spiller"), std::string::npos) << err;
    EXPECT_NE(err.find("amdgpu/gfx1151"), std::string::npos) << err;

    // Control: the non-spilling kernel prints no such warning.
    auto quiet = amdManifests(src({kSaxpy}), {"saxpy"});
    ASSERT_EQ(quiet.size(), 1u);
    EXPECT_EQ(quiet[0].spillBytes.value_or(1), 0u);
}

TEST(XpuKernelManifest, noSpillNoWarning) {
    CAJETA_SKIP_IF_NO_HIP();
    Compiler compiler;
    auto module = compileForInspection(compiler, src({kSaxpy}));
    auto k = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_manifest_host", ctx);
    std::vector<KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx1151", {}, &out);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[xpu-kernel-spill]"), std::string::npos) << err;
}

// 1.1.4 — the CPU manifest has no vgpr / lds keys AT ALL (absent, not zero);
// identity is still complete (§2.4).
TEST(XpuKernelManifest, cpuManifestOmitsDeviceOnlyFields) {
    auto ms = cpuManifests(src({kSaxpy, kLds}), {"saxpy", "withLds"});
    ASSERT_EQ(ms.size(), 2u);
    for (const KernelManifest& m : ms) {
        EXPECT_EQ(m.target.rfind("cpu/", 0), 0u) << m.target;
        EXPECT_TRUE(std::regex_match(m.codeHash, std::regex("^sha256:[0-9a-f]{64}$")));
        EXPECT_EQ(m.xpuAbiVersion, 3);
        EXPECT_FALSE(m.vgpr.has_value());
        EXPECT_FALSE(m.sgpr.has_value());
        EXPECT_FALSE(m.ldsStaticBytes.has_value());
        EXPECT_FALSE(m.spillBytes.has_value());
        std::string json = cajeta::xpu::toJson(m);
        EXPECT_EQ(json.find("\"vgpr\""), std::string::npos) << json;
        EXPECT_EQ(json.find("\"sgpr\""), std::string::npos) << json;
        EXPECT_EQ(json.find("\"ldsStaticBytes\""), std::string::npos) << json;
        EXPECT_EQ(json.find("\"spillBytes\""), std::string::npos) << json;
    }
    EXPECT_NE(byName(ms, ".saxpy"), nullptr);
    EXPECT_NE(byName(ms, ".withLds"), nullptr);
    // Two kernels differ in code, so they differ in hash.
    EXPECT_NE(ms[0].codeHash, ms[1].codeHash);
}

// 1.1.5a — a pinned block records threadsPerGroup and residentGroupsPerCu equal
// to DeviceProfile::occupancy's answer for that footprint (§3.3).
TEST(XpuKernelManifest, pinnedBlockRecordsOccupancy) {
    CAJETA_SKIP_IF_NO_HIP();
    auto ms = amdManifests(src({kPinned}), {"pinned"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelManifest& m = ms[0];
    ASSERT_TRUE(m.threadsPerGroup.has_value());
    EXPECT_EQ(*m.threadsPerGroup, 256u);
    EXPECT_TRUE(m.feasibleBlocks.empty()) << "a pinned block enumerates no candidates";
    ASSERT_TRUE(m.vgpr.has_value());
    cajeta::xpu::DeviceModel model;
    ASSERT_TRUE(cajeta::xpu::lookupArch("gfx1151", model));
    unsigned waves = cajeta::xpu::occupancy(model, 256, *m.vgpr,
                                            m.ldsStaticBytes.value_or(0));
    unsigned wavesPerBlock = (256 + model.waveSize - 1) / model.waveSize;
    ASSERT_TRUE(m.residentGroupsPerCu.has_value());
    EXPECT_EQ(*m.residentGroupsPerCu, waves / wavesPerBlock);
    EXPECT_GT(*m.residentGroupsPerCu, 0u);
    ASSERT_TRUE(m.occupancyLimiter.has_value());
    EXPECT_TRUE(*m.occupancyLimiter == "registers" || *m.occupancyLimiter == "lds"
                || *m.occupancyLimiter == "waveSlots")
        << *m.occupancyLimiter;
}

// The launch-site constant block is a pin too: the AMDGPU backend compiles the
// code object for exactly that workgroup size (amdgpu-flat-work-group-size), so
// the manifest reports the size the artifact was budgeted for.
TEST(XpuKernelManifest, launchSiteConstantBlockIsAPin) {
    CAJETA_SKIP_IF_NO_HIP();
    cajeta::xpu::amd::KernelMaxThreads pins;
    pins["saxpy"] = 128;
    auto ms = amdManifests(src({kSaxpy}), {"saxpy"}, pins);
    ASSERT_EQ(ms.size(), 1u);
    ASSERT_TRUE(ms[0].threadsPerGroup.has_value());
    EXPECT_EQ(*ms[0].threadsPerGroup, 128u);
    EXPECT_TRUE(ms[0].feasibleBlocks.empty());
}

// 1.1.5b — an argument-block kernel records feasibleBlocks in candidateBlocks
// order (best first) and no threadsPerGroup (§3.3).
TEST(XpuKernelManifest, argumentBlockRecordsFeasibleBlocksBestFirst) {
    CAJETA_SKIP_IF_NO_HIP();
    auto ms = amdManifests(src({kSaxpy}), {"saxpy"});
    ASSERT_EQ(ms.size(), 1u);
    const KernelManifest& m = ms[0];
    EXPECT_FALSE(m.threadsPerGroup.has_value());
    EXPECT_FALSE(m.residentGroupsPerCu.has_value());
    ASSERT_TRUE(m.vgpr.has_value());
    cajeta::xpu::DeviceModel model;
    ASSERT_TRUE(cajeta::xpu::lookupArch("gfx1151", model));
    std::vector<unsigned> expect = cajeta::xpu::candidateBlocks(
        model, *m.vgpr, m.ldsStaticBytes.value_or(0), /*clamp=*/0);
    ASSERT_FALSE(expect.empty());
    EXPECT_EQ(m.feasibleBlocks, expect);
}

// 1.1.6 — `k.manifest()` in a cajeta program returns the record for the active
// target; absent fields read as absent (§12.1). CPU backend, so GPU-free.
TEST(XpuKernelManifest, accessorReturnsRecordForActiveTarget) {
    const char* program =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.KernelManifest;\n"
        "import cajeta.lang.Optional;\n"
        "public final class P {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
        "                             float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.x();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        KernelManifest m = saxpy.manifest();\n"
        "        if (m == null) { return 1; }\n"
        "        if (!m.target.startsWith(\"cpu/\")) { return 2; }\n"
        "        if (m.schemaVersion != 1) { return 3; }\n"
        "        if (m.xpuAbiVersion != 3) { return 4; }\n"
        "        Optional<int32> v = m.vgpr();\n"
        "        if (v.isPresent()) { return 5; }\n"
        "        Optional<int32> l = m.ldsStaticBytes();\n"
        "        if (l.isPresent()) { return 6; }\n"
        "        if (!m.kernel.equals(\"test.P.saxpy\")) { return 7; }\n"
        "        if (!m.codeHash.startsWith(\"sha256:\")) { return 8; }\n"
        "        if (m.compilerVersion == null) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.P", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 1.1.7 — the JSON copy an AOT build writes beside the artifact validates
// against tile-manifest-v1.schema.json (§12.4). Drives the real compiler binary
// with --emit=obj --xpu-backend=cpu (GPU-free).
TEST(XpuKernelManifest, buildOutputJsonValidatesAgainstSchema) {
    std::string bin = sourceRoot() + "/build/src/cajeta";
    if (!fs::exists(bin)) GTEST_SKIP() << "compiler binary not at " << bin;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path()
                  / ("cajeta_xpu_manifest_aot_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "test");
    fs::create_directories(root / "out");
    std::ofstream(root / "src" / "test" / "M.cajeta")
        << "package test;\n"
           "import cajeta.xpu.KernelBuffer;\n"
           "import cajeta.xpu.KernelThread;\n"
           "public class M {\n"
        << kSaxpy
        << "    public static void main(String[] args) { }\n"
           "}\n";
    std::string cmd = "\"" + bin + "\" --emit=obj --xpu-backend=cpu test.M.main \""
        + (root / "src").string() + "\" \"" + (root / "out").string() + "\" > \""
        + (root / "build.log").string() + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << readFile(root / "build.log");

    std::vector<fs::path> copies;
    for (const auto& e : fs::recursive_directory_iterator(root / "out")) {
        std::string n = e.path().filename().string();
        if (n.size() > 14 && n.substr(n.size() - 14) == ".manifest.json")
            copies.push_back(e.path());
    }
    ASSERT_EQ(copies.size(), 1u) << "one JSON copy per (kernel, target):\n"
                                 << readFile(root / "build.log");
    EXPECT_NE(copies[0].filename().string().find("saxpy"), std::string::npos)
        << copies[0];
    std::string text = readFile(copies[0]);
    auto errors = validateAgainstSchema(text);
    EXPECT_TRUE(errors.empty()) << [&] {
        std::string s = text + "\n";
        for (auto& e : errors) s += e + "\n";
        return s;
    }();
    // Identity from the file, not the process: the copy is self-describing.
    auto doc = llvm::json::parse(text);
    ASSERT_TRUE(!!doc);
    auto* id = doc->getAsObject()->getObject("identity");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->getString("kernel").value_or(""), "test.M.saxpy");
    EXPECT_EQ(id->getString("target").value_or("").substr(0, 4), "cpu/");
    fs::remove_all(root);
}

// 1.3.2 — the schema and the C++ record agree field for field: a fully
// populated record round-trips through JSON unchanged, every identity /
// footprint key it emits is a schema property, and every schema property in
// those two objects is a key the record can emit.
TEST(XpuKernelManifest, schemaAndRecordAgreeFieldForField) {
    KernelManifest m;
    m.kernel = "dev.example.K.k";
    m.target = "amdgpu/gfx1151";
    m.codeHash = "sha256:" + std::string(64, 'a');
    m.compilerVersion = "0.0.0-test";
    m.xpuAbiVersion = 3;
    m.instrumented = true;
    m.waveWidth = 32;
    m.vgpr = 40;
    m.sgpr = 24;
    m.spillBytes = 0;
    m.ldsStaticBytes = 4096;
    m.ldsDynamicParam = "sharedBytes";
    m.threadsPerGroup = 256;
    m.residentGroupsPerCu = 4;
    m.feasibleBlocks = {256, 512, 128};
    m.occupancyLimiter = "registers";
    m.restartable = true;
    m.captureSafe = true;

    std::string json = cajeta::xpu::toJson(m);
    EXPECT_TRUE(validateAgainstSchema(json).empty()) << json;

    KernelManifest back;
    std::string err;
    ASSERT_TRUE(cajeta::xpu::fromJson(json, back, &err)) << err;
    EXPECT_EQ(back.kernel, m.kernel);
    EXPECT_EQ(back.target, m.target);
    EXPECT_EQ(back.codeHash, m.codeHash);
    EXPECT_EQ(back.compilerVersion, m.compilerVersion);
    EXPECT_EQ(back.xpuAbiVersion, m.xpuAbiVersion);
    EXPECT_EQ(back.instrumented, m.instrumented);
    EXPECT_EQ(back.waveWidth, m.waveWidth);
    EXPECT_EQ(back.vgpr, m.vgpr);
    EXPECT_EQ(back.sgpr, m.sgpr);
    EXPECT_EQ(back.spillBytes, m.spillBytes);
    EXPECT_EQ(back.ldsStaticBytes, m.ldsStaticBytes);
    EXPECT_EQ(back.ldsDynamicParam, m.ldsDynamicParam);
    EXPECT_EQ(back.threadsPerGroup, m.threadsPerGroup);
    EXPECT_EQ(back.residentGroupsPerCu, m.residentGroupsPerCu);
    EXPECT_EQ(back.feasibleBlocks, m.feasibleBlocks);
    EXPECT_EQ(back.occupancyLimiter, m.occupancyLimiter);
    EXPECT_EQ(back.restartable, m.restartable);
    EXPECT_EQ(back.captureSafe, m.captureSafe);
    EXPECT_EQ(cajeta::xpu::toJson(back), json) << "second serialization differs";

    // Key sets: emitted == declared, for the two objects Unit 1 owns.
    auto schema = llvm::json::parse(readFile(
        fs::path(sourceRoot()) / "specs" / "schemas" / "tile-manifest-v1.schema.json"));
    ASSERT_TRUE(!!schema);
    auto doc = llvm::json::parse(json);
    ASSERT_TRUE(!!doc);
    for (const char* section : {"identity", "footprint"}) {
        auto* declared = schema->getAsObject()->getObject("properties")
                             ->getObject(section)->getObject("properties");
        auto* emitted = doc->getAsObject()->getObject(section);
        ASSERT_NE(declared, nullptr) << section;
        ASSERT_NE(emitted, nullptr) << section;
        for (auto& [k, v] : *emitted)
            EXPECT_NE(declared->get(k), nullptr) << section << "." << k.str() << " not in schema";
        for (auto& [k, v] : *declared)
            EXPECT_NE(emitted->get(k), nullptr) << section << "." << k.str() << " never emitted";
    }

    // An EMPTY footprint (CPU) still validates: the required keys are the
    // identity, the access list and the two derived booleans.
    KernelManifest cpu;
    cpu.kernel = "x.Y.z";
    cpu.target = "cpu/znver4";
    cpu.codeHash = "sha256:" + std::string(64, '0');
    cpu.compilerVersion = "0.0.0-test";
    EXPECT_TRUE(validateAgainstSchema(cajeta::xpu::toJson(cpu).c_str()).empty())
        << cajeta::xpu::toJson(cpu);
}

// NVPTX footprint source: `ptxas -v` text parsed into per-kernel stats. The
// parser is exercised on canned output here (no NVIDIA toolchain on this box);
// the wiring runs on the NVIDIA leg.
TEST(XpuKernelManifest, ptxasVerboseParserReadsRegistersSmemAndSpill) {
    const char* text =
        "ptxas info    : 0 bytes gmem\n"
        "ptxas info    : Compiling entry function 'saxpy' for 'sm_89'\n"
        "ptxas info    : Function properties for saxpy\n"
        "    16 bytes stack frame, 8 bytes spill stores, 8 bytes spill loads\n"
        "ptxas info    : Used 24 registers, used 0 barriers, 2048 bytes smem, 380 bytes cmem[0]\n"
        "ptxas info    : Compiling entry function 'dot' for 'sm_89'\n"
        "ptxas info    : Function properties for dot\n"
        "    0 bytes stack frame, 0 bytes spill stores, 0 bytes spill loads\n"
        "ptxas info    : Used 12 registers, used 1 barriers, 380 bytes cmem[0]\n";
    auto stats = cajeta::xpu::nvidia::parsePtxasVerbose(text);
    ASSERT_EQ(stats.size(), 2u);
    EXPECT_EQ(stats[0].name, "saxpy");
    EXPECT_EQ(stats[0].registers, 24u);
    EXPECT_EQ(stats[0].smemBytes, 2048u);
    EXPECT_EQ(stats[0].spillStoreBytes, 8u);
    EXPECT_EQ(stats[0].spillLoadBytes, 8u);
    EXPECT_EQ(stats[0].stackBytes, 16u);
    EXPECT_EQ(stats[1].name, "dot");
    EXPECT_EQ(stats[1].registers, 12u);
    EXPECT_EQ(stats[1].smemBytes, 0u);
    EXPECT_EQ(stats[1].spillStoreBytes, 0u);
}
