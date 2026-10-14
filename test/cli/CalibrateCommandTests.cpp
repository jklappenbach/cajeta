// `cajeta calibrate` end to end (xpu-tile-scheduling plan 2.1.5, 2.2.3; manifest §14.7):
// the program's kernel launches are recorded, replayed solo, and written as a
// snapshot; `--with-calibration` builds that snapshot in, and the program then
// reads its own kernel's record with nothing in the cache. Drives the real
// compiler binary on the CPU backend; skips when it is not built.
#include "gtest/gtest.h"

#include "../PortableEnv.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string compilerBinary() { return sourceRoot() + "/build/src/cajeta"; }

int exitOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const char* kApp =
    "package app;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "import cajeta.xpu.Calibration;\n"
    "import cajeta.xpu.CalibrationKey;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Main {\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256)\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        uint32 n = 4096;\n"
    "        float32[] h = heap float32[n];\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);\n"
    "        x.upload(h);\n"
    "        y.upload(h);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        saxpy.launch(s, grid: [16], block: [256])(y, x, 2.0f, n);\n"
    "        s.sync();\n"
    "        KernelManifest m = saxpy.manifest();\n"
    "        CalibrationKey k #= CalibrationKey.live(m.codeHash);\n"
    "        int8[] p #= Calibration.load(k);\n"
    "        if (p == null) {\n"
    "            System.stdout.println(\"calibration: none\");\n"
    "            return 0;\n"
    "        }\n"
    "        int32 c = (int32) p.count();\n"
    "        int8[] t = heap int8[c];\n"
    "        int32 i = 0;\n"
    "        while (i < c) { t[i] = p[i]; i = i + 1; }\n"
    "        String text = heap String(#t, c);\n"
    "        System.stdout.println(\"calibration: \" + text);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

struct World {
    fs::path root;
    World() {
        root = fs::path(sourceRoot()) / "tmp" / ("calibrate-cli-" + std::to_string(getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "src" / "app");
        fs::create_directories(root / "xdg");
        std::ofstream(root / "src" / "app" / "Main.cajeta") << kApp;
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    int sh(const std::string& cmd, std::string& out, const std::string& tag) const {
        fs::path log = root / (tag + ".log");
        std::string full = "cd \"" + root.string() + "\" && XDG_CACHE_HOME=\"" + (root / "xdg").string()
            + "\" " + cmd + " > \"" + log.string() + "\" 2>&1";
        int rc = exitOf(std::system(cajeta_shell(full).c_str()));
        out = readFile(log);
        return rc;
    }
    int build(const std::string& exe, const std::string& extra, std::string& log) const {
        return sh("\"" + compilerBinary() + "\" --emit=exe --xpu-backend=cpu " + extra + " -o \""
                  + (root / exe).string() + "\" app.Main.main \"" + (root / "src").string() + "\" \""
                  + (root / ("arch-" + exe)).string() + "\"", log, "build-" + exe);
    }
};

} // namespace

TEST(CalibrateCommand, aCalibratedSnapshotBuildsInAndTheProgramReadsItsKernelRecord) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary not built";
    World w;
    std::string log;
    ASSERT_EQ(w.build("prog", "", log), 0) << log;
    ASSERT_EQ(w.sh("./prog", log, "plain"), 0) << log;
    EXPECT_NE(log.find("calibration: none"), std::string::npos) << log;

    ASSERT_EQ(w.sh("\"" + compilerBinary() + "\" calibrate ./prog --out=snap", log, "calibrate"), 0) << log;
    std::string snap = readFile(w.root / "snap");
    ASSERT_EQ(snap.rfind("cajeta-calibration-snapshot-1\n", 0), 0u) << snap.substr(0, 200);
    EXPECT_NE(snap.find("saxpy"), std::string::npos) << "the snapshot names the kernel";
    EXPECT_NE(snap.find("launch grid=16x1x1 block=256x1x1"), std::string::npos)
        << "the replayed launch has the recorded shape";

    ASSERT_EQ(w.build("calibrated", "--with-calibration=snap", log), 0) << log;
    ASSERT_EQ(w.sh("./calibrated", log, "calibrated"), 0) << log;
    EXPECT_NE(log.find("calibration: launch grid=16x1x1 block=256x1x1"), std::string::npos) << log;
    EXPECT_NE(log.find(" ns="), std::string::npos) << log;
}

TEST(CalibrateCommand, aMalformedSnapshotFailsTheBuild) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary not built";
    World w;
    std::ofstream(w.root / "bad") << "not a snapshot\n";
    std::string log;
    EXPECT_NE(w.build("prog", "--with-calibration=bad", log), 0) << log;
    EXPECT_NE(log.find("--with-calibration"), std::string::npos) << log;
}
