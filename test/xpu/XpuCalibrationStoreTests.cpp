// The calibration store (xpu-tile-scheduling plan 2.1.2 to 2.1.4, 2.3.1).
//
// Memory is the calibration set's source of truth; records leave it only
// through a CalibrationStore. The default module serves embedded records,
// then the platform cache, and never writes under /tmp or the artifact
// store. A custom module replaces it entirely; a faulting one is disabled
// once. A record keyed to another driver, compiler or power mode, or a
// corrupt one, is ignored. Each test points XDG_CACHE_HOME at its own
// repo-local directory.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
using cajeta_test::CajetaJit;

namespace {

std::string repoRoot() {
    std::string f = __FILE__;
    std::filesystem::path p(f);
    return p.parent_path().parent_path().parent_path().string();
}

std::string testDir() {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    auto d = std::filesystem::path(repoRoot()) / "tmp" /
             ("calibration-test-" + std::to_string(getpid()) + "-" + (info ? info->name() : "none"));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d.string();
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* kHelpers =
    "    static #String str(int8[] b) {\n"
    "        if (b == null) { return \"<null>\"; }\n"
    "        int32 n = (int32) b.count();\n"
    "        int8[] c = heap int8[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { c[i] = b[i]; i = i + 1; }\n"
    "        return heap String(#c, n);\n"
    "    }\n"
    "    static boolean keep(CalibrationKey k, String s) {\n"
    "        int8[] b #= s.toBytes();\n"
    "        return Calibration.store(k, b, (int32) b.count());\n"
    "    }\n"
    "    static #int8[] framed(CalibrationKey k, String s) {\n"
    "        int8[] b #= s.toBytes();\n"
    "        return CalibrationRecord.encode(k, b, (int32) b.count());\n"
    "    }\n";

const char* kStores =
    "class MapStore implements CalibrationStore {\n"
    "    HashMap<String, int8[]> map;\n"
    "    int32 loads;\n"
    "    int32 stores;\n"
    "    public MapStore() { this.map = heap HashMap<String, int8[]>(); this.loads = 0; this.stores = 0; }\n"
    "    public #int8[] load(String slot) {\n"
    "        this.loads = this.loads + 1;\n"
    "        if (!this.map.containsKey(slot)) { return null; }\n"
    "        int8[] b = this.map.get(slot);\n"
    "        int32 n = (int32) b.count();\n"
    "        int8[] c = heap int8[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { c[i] = b[i]; i = i + 1; }\n"
    "        return c;\n"
    "    }\n"
    "    public boolean store(String slot, int8[] bytes) {\n"
    "        this.stores = this.stores + 1;\n"
    "        int32 n = (int32) bytes.count();\n"
    "        int8[] c = heap int8[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { c[i] = bytes[i]; i = i + 1; }\n"
    "        this.map.put(\"\" + slot, #c);\n"
    "        return true;\n"
    "    }\n"
    "    public boolean writable() { return true; }\n"
    "}\n"
    "class FaultStore implements CalibrationStore {\n"
    "    int32 calls;\n"
    "    public FaultStore() { this.calls = 0; }\n"
    "    public #int8[] load(String slot) {\n"
    "        this.calls = this.calls + 1;\n"
    "        throw heap RecoverableException(\"load\");\n"
    "    }\n"
    "    public boolean store(String slot, int8[] bytes) {\n"
    "        this.calls = this.calls + 1;\n"
    "        throw heap RecoverableException(\"store\");\n"
    "    }\n"
    "    public boolean writable() {\n"
    "        this.calls = this.calls + 1;\n"
    "        throw heap RecoverableException(\"writable\");\n"
    "    }\n"
    "}\n";

std::string program(const std::string& body) {
    return std::string(
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "import cajeta.error.RecoverableException;\n"
        "import cajeta.io.file.File;\n"
        "import cajeta.io.file.Path;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.xpu.Calibration;\n"
        "import cajeta.xpu.CalibrationKey;\n"
        "import cajeta.xpu.CalibrationRecord;\n"
        "import cajeta.xpu.CalibrationStore;\n"
        "import cajeta.xpu.DefaultCalibrationStore;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Scheduler;\n"
        "public final class T {\n"
        "    @Kernel\n"
        "    public static void storeTestKernel(KernelBuffer<float32> y, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { y[i] = 1.0f; }\n"
        "    }\n") + kHelpers + body + "}\n" + kStores;
}

std::unique_ptr<CajetaJit> compile(const std::string& body) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return CajetaJit::compile(program(body), "test.T", o);
}

int call(CajetaJit* jit, const char* name) {
    auto fn = jit->lookup<int (*)()>(name);
    return fn ? fn() : -101;
}

bool emptyDir(const std::filesystem::path& p) {
    return !std::filesystem::exists(p) || std::filesystem::is_empty(p);
}

} // namespace

// 2.1.2 — with no module installed, the default keeps records in the platform cache.
TEST(XpuCalibrationStore, theDefaultKeepsRecordsInThePlatformCache) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    auto jit = compile(
        "    public static int32 run() {\n"
        "        CalibrationKey k #= CalibrationKey.live(\"device\");\n"
        "        if (!Calibration.persistent()) { return 1; }\n"
        "        if (!T.keep(k, \"alpha=7\")) { return 2; }\n"
        "        DefaultCalibrationStore d = heap DefaultCalibrationStore();\n"
        "        String slot #= k.slot();\n"
        "        int8[] raw #= d.load(slot);\n"
        "        int8[] p #= CalibrationRecord.decode(k, raw);\n"
        "        String s #= T.str(p);\n"
        "        if (!s.equals(\"alpha=7\")) { return 3; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
    auto cal = std::filesystem::path(dir) / "cajeta" / "calibration";
    EXPECT_TRUE(std::filesystem::exists(cal)) << "records go under <XDG_CACHE_HOME>/cajeta/calibration";
    std::filesystem::remove_all(dir);
}

// 2.1.2 — an embedded record is served before the platform cache.
TEST(XpuCalibrationStore, anEmbeddedRecordIsServedFirst) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    std::string out = dir + "/out";
    auto jit = compile(
        "    public static int32 prepare() {\n"
        "        CalibrationKey k #= CalibrationKey.live(\"embedded-first\");\n"
        "        String slot #= k.slot();\n"
        "        DefaultCalibrationStore d = heap DefaultCalibrationStore();\n"
        "        int8[] cached #= T.framed(k, \"from=cache\");\n"
        "        if (!d.store(slot, cached)) { return 1; }\n"
        "        int8[] emb #= T.framed(k, \"from=build\");\n"
        "        File.writeAllBytes(\"" + out + ".rec\", emb, (int64) emb.count());\n"
        "        int8[] sb #= slot.toBytes();\n"
        "        File.writeAllBytes(\"" + out + ".slot\", sb, (int64) sb.count());\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 check() {\n"
        "        CalibrationKey k #= CalibrationKey.live(\"embedded-first\");\n"
        "        int8[] p #= Calibration.load(k);\n"
        "        String s #= T.str(p);\n"
        "        if (!s.equals(\"from=build\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    ASSERT_EQ(call(jit.get(), "prepare"), 0);
    static std::string rec, slot;
    rec = slurp(out + ".rec");
    slot = slurp(out + ".slot");
    ASSERT_FALSE(rec.empty());
    auto reg = reinterpret_cast<void (*)(const char*, const uint8_t*, int64_t)>(
        jit->lookupRawSymbol("__cajeta_xpu_register_calibration_record"));
    ASSERT_NE(reg, nullptr);
    reg(slot.c_str(), reinterpret_cast<const uint8_t*>(rec.data()), (int64_t) rec.size());
    EXPECT_EQ(call(jit.get(), "check"), 0) << "the embedded record outranks the cache";
    std::filesystem::remove_all(dir);
}

// 2.1.2, 2.1.4 — with nothing writable the store reports read-only and memory serves.
TEST(XpuCalibrationStore, nothingWritableRunsFromMemory) {
    std::string dir = testDir();
    { std::ofstream blocker(dir + "/blocker"); blocker << "a file, not a directory"; }
    std::string xdg = dir + "/blocker/cache";
    setenv("XDG_CACHE_HOME", xdg.c_str(), 1);
    auto jit = compile(
        "    public static int32 run() {\n"
        "        CalibrationKey k #= CalibrationKey.live(\"device\");\n"
        "        if (Calibration.persistent()) { return 1; }\n"
        "        if (T.keep(k, \"beta=3\")) { return 2; }\n"
        "        int8[] p #= Calibration.load(k);\n"
        "        String s #= T.str(p);\n"
        "        if (!s.equals(\"beta=3\")) { return 3; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
    std::filesystem::remove_all(dir);
}

// 2.1.3 — an installed module is the only store; it can be installed only before first use.
TEST(XpuCalibrationStore, anInstalledModuleIsTheOnlyStore) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    auto jit = compile(
        "    public static int32 run() {\n"
        "        MapStore m = heap MapStore();\n"
        "        MapStore alias = m;\n"
        "        if (!Scheduler.calibrationStore(#m)) { return 1; }\n"
        "        CalibrationKey k #= CalibrationKey.live(\"device\");\n"
        "        if (!T.keep(k, \"gamma=5\")) { return 2; }\n"
        "        CalibrationKey other #= CalibrationKey.live(\"never-stored\");\n"
        "        int8[] none #= Calibration.load(other);\n"
        "        if (none != null) { return 3; }\n"
        "        if (alias.stores != 1 || alias.loads != 1) { return 4; }\n"
        "        String slot #= k.slot();\n"
        "        if (!alias.map.containsKey(slot)) { return 5; }\n"
        "        MapStore late = heap MapStore();\n"
        "        if (Scheduler.calibrationStore(#late)) { return 6; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
    EXPECT_TRUE(emptyDir(dir)) << "the default module must not have touched the platform cache";
    std::filesystem::remove_all(dir);
}

// 2.1.3 — a module that faults is disabled once and the run continues from memory.
TEST(XpuCalibrationStore, aFaultingModuleIsDisabledOnce) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    auto jit = compile(
        "    public static int32 run() {\n"
        "        FaultStore f = heap FaultStore();\n"
        "        FaultStore alias = f;\n"
        "        if (!Scheduler.calibrationStore(#f)) { return 1; }\n"
        "        CalibrationKey k #= CalibrationKey.live(\"device\");\n"
        "        T.keep(k, \"delta=9\");\n"
        "        CalibrationKey other #= CalibrationKey.live(\"absent\");\n"
        "        int8[] none #= Calibration.load(other);\n"
        "        T.keep(other, \"epsilon=1\");\n"
        "        if (Calibration.persistent()) { return 2; }\n"
        "        if (Calibration.faultCount() != 1) { return 3; }\n"
        "        if (alias.calls != 1) { return 4; }\n"
        "        int8[] p #= Calibration.load(k);\n"
        "        String s #= T.str(p);\n"
        "        if (!s.equals(\"delta=9\")) { return 5; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
    std::filesystem::remove_all(dir);
}

// 2.1.4 — a record for another driver, compiler or power mode, or a corrupt one, is ignored.
// Each case is its own program: the calibration statics outlive a call.
static int servePlanted(const std::string& driver, const std::string& compiler, const std::string& power,
                 bool corrupt) {
    auto jit = compile(
        "    public static int32 run() {\n"
        "        MapStore m = heap MapStore();\n"
        "        MapStore alias = m;\n"
        "        if (!Scheduler.calibrationStore(#m)) { return 2; }\n"
        "        CalibrationKey live #= CalibrationKey.live(\"device\");\n"
        "        CalibrationKey planted = heap CalibrationKey(\"\" + live.subject, \"\" + live.device,\n"
        "            " + driver + ", " + compiler + ", " + power + ");\n"
        "        int8[] rec #= T.framed(planted, \"zeta=2\");\n"
        "        int32 at = (int32) rec.count() - 20;\n"
        + std::string(corrupt ? "        rec[at] = (int8) (rec[at] + 1);\n" : "") +
        "        String slot #= live.slot();\n"
        "        alias.store(slot, rec);\n"
        "        int8[] p #= Calibration.load(live);\n"
        "        if (p == null) { return 0; }\n"
        "        String s #= T.str(p);\n"
        "        if (s.equals(\"zeta=2\")) { return 1; }\n"
        "        return 3;\n"
        "    }\n");
    if (!jit) return -100;
    return call(jit.get(), "run");
}

TEST(XpuCalibrationStore, aMismatchedOrCorruptRecordIsIgnored) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    const std::string d = "\"\" + live.driver", c = "\"\" + live.compiler", p = "\"\" + live.powerMode";
    EXPECT_EQ(servePlanted(d, c, p, false), 1) << "control: the live key's intact record is served";
    EXPECT_EQ(servePlanted("\"999999\"", c, p, false), 0) << "a record for another driver must be ignored";
    EXPECT_EQ(servePlanted(d, "\"0.0.0-other\"", p, false), 0) << "a record for another compiler must be ignored";
    EXPECT_EQ(servePlanted(d, c, "\"low-power/manual\"", false), 0) << "a record for another power mode must be ignored";
    EXPECT_EQ(servePlanted(d, c, p, true), 0) << "a corrupt record must be ignored";
    std::filesystem::remove_all(dir);
}

// 2.3.1 — the default module never writes under /tmp or the artifact store.
TEST(XpuCalibrationStore, neverWritesTmpOrTheArtifactStore) {
    std::string dir = testDir();
    std::string olla = dir + "/home/.olla/cache";
    std::string tmp = "/tmp/cajeta-calibration-test-" + std::to_string(getpid());
    ASSERT_FALSE(std::filesystem::exists(tmp));
    auto jit = compile(
        "    public static int32 run() {\n"
        "        if (!DefaultCalibrationStore.refused(\"/tmp\")) { return 1; }\n"
        "        if (!DefaultCalibrationStore.refused(\"/tmp/x/cajeta/calibration\")) { return 2; }\n"
        "        if (!DefaultCalibrationStore.refused(\"/home/u/.olla/cache/cajeta/calibration\")) { return 3; }\n"
        "        if (DefaultCalibrationStore.refused(\"/home/u/.cache/cajeta/calibration\")) { return 4; }\n"
        "        DefaultCalibrationStore d = heap DefaultCalibrationStore();\n"
        "        if (d.writable()) { return 5; }\n"
        "        CalibrationKey k #= CalibrationKey.live(\"device\");\n"
        "        int8[] rec #= T.framed(k, \"eta=4\");\n"
        "        String slot #= k.slot();\n"
        "        if (d.store(slot, rec)) { return 6; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    setenv("XDG_CACHE_HOME", tmp.c_str(), 1);
    EXPECT_EQ(call(jit.get(), "run"), 0) << "an XDG cache under /tmp is refused";
    EXPECT_FALSE(std::filesystem::exists(tmp)) << "nothing may be created under /tmp";
    setenv("XDG_CACHE_HOME", olla.c_str(), 1);
    EXPECT_EQ(call(jit.get(), "run"), 0) << "an XDG cache inside the artifact store is refused";
    EXPECT_FALSE(std::filesystem::exists(olla)) << "nothing may be created inside .olla";
    std::filesystem::remove_all(dir);
}

// 2.1.4 — a slot and a checksum are the same in every process, or no stored record is ever found.
TEST(XpuCalibrationStore, aSlotAndChecksumAreTheSameInEveryProcess) {
    auto jit = compile(
        "    public static int32 run() {\n"
        "        CalibrationKey k = heap CalibrationKey(\"device\", \"cpu/x/\", \"0\", \"0.41.0\", \"performance/auto\");\n"
        "        String slot #= k.slot();\n"
        "        if (!slot.equals(\"cal-c78dfe403a912db5\")) { return 1; }\n"
        "        int8[] r #= T.framed(k, \"a=1\");\n"
        "        int32 n = (int32) r.count();\n"
        "        int8[] tail = heap int8[16];\n"
        "        int32 i = 0;\n"
        "        while (i < 16) { tail[i] = r[n - 16 + i]; i = i + 1; }\n"
        "        String sum = heap String(#tail, 16);\n"
        "        if (!sum.equals(\"43a167bd898621c9\")) { return 2; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0) << "the slot and checksum must not depend on the process seed";
}
