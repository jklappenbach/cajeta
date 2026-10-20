// tensor-element-access Unit 4: in an AOT build, line frames are pushed and popped
// inline rather than through __cajeta_line_enter/mark/leave calls. Every behavioural
// test builds the same program twice, with the pass and with CAJETA_INLINE_LINE_FRAMES=0,
// and requires identical traces, so "unchanged" is measured against the call path.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string lineFrameCompiler() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

int exitOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Built { bool ok = false; std::string log; fs::path root; fs::path exe; fs::path dump; };

/** Builds `src` as app.Main.main with `flags`; `inlineFrames` false sets CAJETA_INLINE_LINE_FRAMES=0. */
Built build(const std::string& src, const std::string& flags, bool inlineFrames) {
    static std::mt19937_64 rng(std::random_device{}());
    Built b;
    b.root = fs::temp_directory_path() / ("cajeta_linefr_" + std::to_string(rng()));
    fs::create_directories(b.root / "src" / "app");
    fs::create_directories(b.root / "out");
    fs::create_directories(b.root / "dump");
    std::ofstream(b.root / "src" / "app" / "Main.cajeta") << src;
    b.exe = b.root / "out" / "prog";
    b.dump = b.root / "dump";
    std::string env = cajeta_env_prefix({{"CAJETA_DUMP_CODEGEN_BC", b.dump.string()},
                                         {"CAJETA_INLINE_LINE_FRAMES", inlineFrames ? "1" : "0"}});
    fs::path log = b.root / "build.log";
    std::string cmd = env + "\"" + lineFrameCompiler() + "\" --emit=exe " + flags + " -o \""
        + b.exe.string() + "\" app.Main.main \"" + (b.root / "src").string() + "\" \""
        + (b.root / "out").string() + "\" > \"" + log.string() + "\" 2>&1";
    b.ok = exitOf(std::system(cajeta_shell(cmd).c_str())) == 0;
    b.log = slurp(log);
    return b;
}

struct Ran { int rc = -1; std::string out; std::string err; };

Ran run(const Built& b, const std::string& envPrefix = "") {
    Ran r;
    fs::path o = b.root / "run.out", e = b.root / "run.err";
    std::string cmd = envPrefix + "\"" + cajeta_exe_path(b.exe).string() + "\" > \"" + o.string()
        + "\" 2> \"" + e.string() + "\"";
    r.rc = exitOf(std::system(cajeta_shell(cmd).c_str()));
    r.out = slurp(o);
    r.err = slurp(e);
    return r;
}

/** Concatenated post-optimization IR of every module the build emitted. */
std::string dumpedIr(const Built& b) {
    std::string all;
    for (auto& f : fs::directory_iterator(b.dump))
        if (f.path().extension() == ".ll") all += slurp(f.path());
    return all;
}

/** Bodies of the defined functions whose name contains `needle`. */
std::string bodiesOf(const std::string& ir, const std::string& needle) {
    std::string out;
    size_t p = 0;
    while ((p = ir.find("\ndefine ", p)) != std::string::npos) {
        size_t nl = ir.find('\n', p + 1);
        size_t end = ir.find("\n}", nl);
        if (ir.substr(p, nl - p).find(needle) != std::string::npos)
            out += ir.substr(p, (end == std::string::npos ? ir.size() : end) - p);
        p = nl;
    }
    return out;
}

/** Drops lines carrying a raw address, which differ run to run. */
std::string withoutAddresses(const std::string& s) {
    std::istringstream in(s);
    std::string line, out;
    while (std::getline(in, line))
        if (line.find("0x") == std::string::npos) out += line + "\n";
    return out;
}

void cleanup(const Built& b) { std::error_code ec; fs::remove_all(b.root, ec); }

const char* kTraces =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "import cajeta.error.StackFrame;\n"
    "public class Main {\n"
    "    static int32 leaf(int32[] a, int32 i) {\n"
    "        if (i >= 3) { throw heap Exception(\"leaf\"); }\n"
    "        return a[i];\n"
    "    }\n"
    "    static int32 thrower(int32 n) {\n"
    "        if (n > 0) { throw heap Exception(\"boom\"); }\n"
    "        return n;\n"
    "    }\n"
    "    static int32 middle(int32 n) {\n"
    "        int32 r = Main.thrower(n);\n"
    "        return r + 1;\n"
    "    }\n"
    "    static int32 deep(int32 d) {\n"
    "        if (d == 0) { return Main.middle(1); }\n"
    "        return Main.deep(d - 1) + 1;\n"
    "    }\n"
    "    static #String names(Exception e) {\n"
    "        StackFrame[] fs #= e.getStackTrace();\n"
    "        String out #= \"\" + fs.count() + \":\";\n"
    "        int64 i = 0;\n"
    "        while (i < fs.count() && i < 8) {\n"
    "            out #= out + \" \" + fs[i].method + \"@\" + fs[i].line;\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return #out;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int32[] a = [ 1, 2, 3 ];\n"
    "        try { Main.middle(1); } catch (Exception e) { System.stdout.println(\"A \" + Main.names(e)); }\n"
    "        try { Main.leaf(a, 5); } catch (Exception e) { System.stdout.println(\"L \" + Main.names(e)); }\n"
    "        try { Main.deep(600); } catch (Exception e) { System.stdout.println(\"B \" + Main.names(e)); }\n"
    "        try { Main.middle(1); } catch (Exception e) { System.stdout.println(\"C \" + Main.names(e)); }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

}  // namespace

// 4.1.1: with the pass, no line-frame runtime call is left in the program's own code;
// without it the calls are there, so the check can fire.
TEST(InlineLineFrame, aReleaseBuildHasNoLineFrameCalls) {
    Built on = build(kTraces, "--release", true);
    ASSERT_TRUE(on.ok) << on.log;
    Built off = build(kTraces, "--release", false);
    ASSERT_TRUE(off.ok) << off.log;
    std::string onIr = bodiesOf(dumpedIr(on), "app.Main::");
    std::string offIr = bodiesOf(dumpedIr(off), "app.Main::");
    ASSERT_FALSE(onIr.empty()) << "no app.Main functions in the dumped IR";
    for (const char* fn : {"@__cajeta_line_enter", "@__cajeta_line_leave", "@__cajeta_line_mark"})
        EXPECT_EQ(onIr.find(std::string("call void ") + fn), std::string::npos) << fn << " left in:\n" << onIr;
    EXPECT_NE(offIr.find("call void @__cajeta_line_enter"), std::string::npos)
        << "with CAJETA_INLINE_LINE_FRAMES=0 the calls must remain";
    cleanup(on);
    cleanup(off);
}

// 4.1.2, 4.1.5, 4.6 (at O0 and O2; --release turns trace capture off): exception traces, a throw from a small inlinable method, and a
// throw from below 600 frames, then a shallow trace again; identical with and without.
TEST(InlineLineFrame, tracesMatchTheCallPathAtEveryOptLevel) {
    for (const char* flags : {"", "--opt=O2"}) {
        Built on = build(kTraces, flags, true);
        ASSERT_TRUE(on.ok) << on.log;
        Built off = build(kTraces, flags, false);
        ASSERT_TRUE(off.ok) << off.log;
        Ran a = run(on), b = run(off);
        EXPECT_EQ(a.rc, 0) << a.err;
        EXPECT_EQ(a.out, b.out) << "flags '" << flags << "'";
        EXPECT_NE(a.out.find("A "), std::string::npos) << a.out;
        EXPECT_NE(a.out.find("leaf@"), std::string::npos) << a.out;
        size_t pa = a.out.find("A "), pc = a.out.find("C ");
        ASSERT_NE(pc, std::string::npos) << a.out;
        EXPECT_EQ(a.out.substr(pa + 2, a.out.find('\n', pa) - pa - 2),
                  a.out.substr(pc + 2, a.out.find('\n', pc) - pc - 2))
            << "the shadow stack is unbalanced after recursion past its capacity:\n" << a.out;
        cleanup(on);
        cleanup(off);
    }
}

// 4.2: a fault in a small inlined method prints the same report either way (a SIGSEGV
// prints the native backtrace, not the shadow stack, so addresses are stripped).
TEST(InlineLineFrame, aFaultPrintsTheSameStack) {
    const char* src =
        "package app;\n"
        "public class Main {\n"
        "    static int32 leaf(int32[] a, int32 i) { return a[i]; }\n"
        "    static int32 middle(int32[] a) { return Main.leaf(a, 0) + 1; }\n"
        "    public static int32 main(String[] args) {\n"
        "        int32[] a = null;\n"
        "        return Main.middle(a);\n"
        "    }\n"
        "}\n";
    Built on = build(src, "--opt=O2", true);
    ASSERT_TRUE(on.ok) << on.log;
    Built off = build(src, "--opt=O2", false);
    ASSERT_TRUE(off.ok) << off.log;
    Ran a = run(on), b = run(off);
    EXPECT_NE(a.rc, 0);
    EXPECT_EQ(withoutAddresses(a.err), withoutAddresses(b.err));
    cleanup(on);
    cleanup(off);
}

// 4.3: fibers interleaving on carriers each see their own frames.
TEST(InlineLineFrame, fibersKeepTheirOwnFrames) {
    const char* src =
        "package app;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public class Main {\n"
        "    static void d3() { throw heap Exception(\"x\"); }\n"
        "    static void d2() { Main.d3(); }\n"
        "    static void d1() { Main.d2(); }\n"
        "    static int32 depthOf(boolean deep) {\n"
        "        try { if (deep) { Main.d1(); } else { Main.d3(); } return -1; }\n"
        "        catch (Exception e) { StackFrame[] fs #= e.getStackTrace(); return (int32) fs.count(); }\n"
        "    }\n"
        "    public static async int32 deepFiber() { return Main.depthOf(true); }\n"
        "    public static async int32 shallowFiber() { return Main.depthOf(false); }\n"
        "    public static int32 main(String[] args) {\n"
        "        int32 k = 0;\n"
        "        int32 bad = 0;\n"
        "        while (k < 50) {\n"
        "            Task<int32> a = spawn deepFiber();\n"
        "            Task<int32> b = spawn shallowFiber();\n"
        "            int32 da = await a;\n"
        "            int32 db = await b;\n"
        "            if (da - db != 2) { bad = bad + 1; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        System.stdout.println(\"bad \" + bad);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    Built on = build(src, "--opt=O2", true);
    ASSERT_TRUE(on.ok) << on.log;
    Ran a = run(on);
    EXPECT_EQ(a.rc, 0) << a.err;
    EXPECT_EQ(a.out, "bad 0\n") << a.err;
    cleanup(on);
}


// 4.4: the sampling profiler reads the same chain from inline frames.
TEST(InlineLineFrame, theProfilerSeesTheCallChain) {
    const char* src =
        "package app;\n"
        "public class Main {\n"
        "    static int64 innerSpin(int64 n) { int64 s = 0; int64 i = 0; while (i < n) { s = (s + (i ^ (s >> 3))) & 1048575; i = i + 1; } return s; }\n"
        "    static int64 middleSpin(int64 n) { return Main.innerSpin(n) + 1; }\n"
        "    static int64 outerSpin(int64 n) { return Main.middleSpin(n) + 1; }\n"
        "    public static int32 main(String[] args) {\n"
        "        int64 t = 0;\n"
        "        int32 k = 0;\n"
        "        while (k < 40) { t = t + Main.outerSpin(5000000); k = k + 1; }\n"
        "        System.stdout.println(\"t \" + (t != 0));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    Built on = build(src, "", true);
    ASSERT_TRUE(on.ok) << on.log;
    fs::path trace = on.root / "run.pftrace";
    Ran a = run(on, cajeta_env_prefix({{"CAJETA_PROFILER", "1"}, {"CAJETA_PROFILER_OUT", trace.string()}}));
    EXPECT_EQ(a.rc, 0) << a.err;
    std::string bytes = slurp(trace);
    ASSERT_FALSE(bytes.empty()) << "no trace written:\n" << a.err;
    for (const char* fn : {"innerSpin", "middleSpin", "outerSpin"})
        EXPECT_NE(bytes.find(fn), std::string::npos) << fn << " never sampled";
    cleanup(on);
}
