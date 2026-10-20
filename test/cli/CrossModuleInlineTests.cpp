// tensor-element-access Unit 6: a --release exe without LTO inlines small stdlib methods
// into user code, through an available_externally copy in the caller's module.

#include <gtest/gtest.h>

#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

const char* kLoop =
    "package app;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.npio.Npy;\n"
    "public class Main {\n"
    "    static float64 sumGet(Tensor<float64> t, int64 n) {\n"
    "        float64 s = 0.0;\n"
    "        int64 i = 0;\n"
    "        while (i < n) { s = s + t.flatGet(i); i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int64[] shape = heap int64[1];\n"
    "        shape[0] = 8;\n"
    "        Tensor<float64> t #= Tensor.zeros<float64>(shape);\n"
    "        int64 i = 0;\n"
    "        while (i < 8) { t.flatSet(i, (float64) i); i = i + 1; }\n"
    "        int8[] img #= Npy.encodeF64(t);\n"
    "        Tensor<float64> back #= Npy.decodeF64(img);\n"
    "        System.stdout.println(\"sum \" + (int64) Main.sumGet(back, 8));\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

const std::string kFlatGet = "cajeta.math.Tensor<float64>::flatGet(this:pointer,i:int64)";

/** The IR of modules other than the stdlib's. */
std::string userIr(const Built& b) {
    std::string all;
    for (auto& f : fs::directory_iterator(b.dump))
        if (f.path().extension() == ".ll" && f.path().filename().string().find("__stdlib__") == std::string::npos)
            all += slurp(f.path());
    return all;
}

}  // namespace

// 6.1.1: in --release, the loop calling flatGet has no call left to it; without the copies it does.
TEST(CrossModuleInline, aReleaseLoopInlinesFlatGet) {
    Workspace w;
    w.write("src/app/Main.cajeta", kLoop);
    Built on = compile(w, "on", "--emit=exe --release", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    Built off = compile(w, "off", "--emit=exe --release", "app.Main.main", "src", {{"CAJETA_XMODULE_INLINE", "0"}});
    ASSERT_TRUE(off.ok) << off.log;
    std::string onLoop = bodiesOf(userIr(on), "app.Main::sumGet");
    std::string offLoop = bodiesOf(userIr(off), "app.Main::sumGet");
    ASSERT_FALSE(onLoop.empty()) << "sumGet not found in the user IR";
    EXPECT_EQ(onLoop.find("call double @\"" + kFlatGet), std::string::npos) << onLoop;
    EXPECT_NE(offLoop.find("call double @\"" + kFlatGet), std::string::npos)
        << "with CAJETA_XMODULE_INLINE=0 the call must remain:\n" << offLoop;
    EXPECT_EQ(run(on).out, "sum 28\n");
    EXPECT_EQ(run(off).out, "sum 28\n");
}

// 6.1.2: the copies are discardable, so O0 and --release builds link with one definition each.
TEST(CrossModuleInline, copiesNeverDuplicateADefinition) {
    Workspace w;
    w.write("src/app/Main.cajeta", kLoop);
    for (const char* flags : {"--emit=exe", "--emit=exe --release"}) {
        Built b = compile(w, std::string(flags).size() > 10 ? "rel" : "o0", flags, "app.Main.main", "src");
        ASSERT_TRUE(b.ok) << flags << "\n" << b.log;
        EXPECT_EQ(run(b).out, "sum 28\n") << flags;
    }
}

// 7.1.1 (spec 5.2): with flatGet and flatSet inlined, a device-resident tensor still throws
// PlacementException with Storage's message, at O0 and in --release.
TEST(CrossModuleInline, inlinedAccessorsStillRefuseDeviceResidentData) {
    Workspace w;
    w.write("src/app/Main.cajeta",
        "package app;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.PlacementException;\n"
        "public class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        float32[] d = [ 3.0f, 4.0f, 5.0f, 6.0f ];\n"
        "        int64[] s = heap int64[1];\n"
        "        s[0] = 4;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        t.gpu();\n"
        "        try { float32 v = t.flatGet(2); System.stdout.println(\"no throw \" + v); }\n"
        "        catch (PlacementException ex) { System.stdout.println(ex.getMessage()); }\n"
        "        try { t.flatSet(1, 9.0f); System.stdout.println(\"no throw\"); }\n"
        "        catch (PlacementException ex) { System.stdout.println(ex.getMessage()); }\n"
        "        t.cpu();\n"
        "        System.stdout.println(\"back \" + t.flatGet(2));\n"
        "        return 0;\n"
        "    }\n}\n");
    for (const char* flags : {"--emit=exe --xpu-backend=cpu", "--emit=exe --xpu-backend=cpu --release"}) {
        Built b = compile(w, std::string(flags).find("release") != std::string::npos ? "rel" : "o0", flags,
                          "app.Main.main", "src");
        ASSERT_TRUE(b.ok) << flags << "\n" << b.log;
        Ran r = run(b, cajeta_env_prefix({{"CAJETA_XPU_BACKEND", "cpu"}}));
        EXPECT_EQ(r.rc, 0) << r.err;
        EXPECT_NE(r.out.find("Storage.get: the data is device-resident"), std::string::npos) << flags << "\n" << r.out;
        EXPECT_NE(r.out.find("Storage.set: the data is device-resident"), std::string::npos) << flags << "\n" << r.out;
        EXPECT_NE(r.out.find("back 5"), std::string::npos) << flags << "\n" << r.out;
    }
}
