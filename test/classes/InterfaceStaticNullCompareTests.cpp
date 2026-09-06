//
// Comparing a STATIC field of interface type against null.
//
// THE CRASH: the interface-vs-null path in BinaryOpExpression only fired when
// the interface operand arrived as a POINTER to its fat struct. Locals and
// instance fields do arrive that way; a static field loads as the fat VALUE
// itself. So for statics the whole block was skipped and the struct reached
// CreateICmp against a null pointer, killing the compiler in LLVM:
//
//   ICmpInst::AssertOK(): Assertion `getOperand(0)->getType() ==
//   getOperand(1)->getType() && "Both operands to ICmp instruction are not of
//   the same type!"' failed.
//
// Found 2026-08-27 adding an opt-in diagnostics hook to cajeta-llm, whose
// whole design is "a static sink field, null when nobody is listening" — the
// single most obvious way to write that shape. The workaround (carry the armed
// state in a parallel boolean) is exactly the sort of thing that should not be
// necessary, hence this fix.
//
// The tests cover BOTH directions, because a fix that always reported "null"
// would satisfy a one-sided test while breaking every guard written this way:
// the field must read null before assignment and non-null after, through `==`
// and `!=` alike. The local/instance shapes are pinned too — they always
// worked, and a fix to the static path must not regress them.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include "../PortableEnv.h"

using cajeta_test::CajetaJit;

namespace {
// Shared preamble: one interface and one implementation.
const char* kIface =
    "package test;\n"
    "public interface Sink { int64 tag(); }\n"
    "public final class Impl implements Sink {\n"
    "    public Impl() { return; }\n"
    "    public int64 tag() { return 7; }\n"
    "}\n";
}  // namespace

// The crash itself: `!=` against a static interface field. Compiling at all
// used to be impossible.
TEST(InterfaceStaticNullCompareTests, staticInterfaceFieldComparesToNull) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        if (D.held == null) { score = score + 1; }\n"   // 1
        "        if (D.held != null) { score = score + 100; }\n" // not taken
        "        D.held = heap Impl();\n"
        "        if (D.held != null) { score = score + 10; }\n"  // 11
        "        if (D.held == null) { score = score + 100; }\n" // not taken
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// Null on the LEFT — the operand-order arm of the same path.
TEST(InterfaceStaticNullCompareTests, nullOnTheLeftAlsoWorks) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        if (null == D.held) { score = score + 1; }\n"
        "        D.held = heap Impl();\n"
        "        if (null != D.held) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// A guarded dispatch through the static field — the actual shape the engine
// wanted: check, then call. Proves the non-null value is still usable, not
// merely comparable.
TEST(InterfaceStaticNullCompareTests, staticFieldGuardThenDispatch) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 total = 0;\n"
        "        if (D.held != null) { total = total + D.held.tag(); }\n"
        "        D.held = heap Impl();\n"
        "        if (D.held != null) { total = total + D.held.tag(); }\n"
        "        return total;\n"                                  // 0 + 7
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Controls for the shapes that ALWAYS worked, pinned so the static fix cannot
// regress them. Split in two so a failure names the shape rather than the pair.
TEST(InterfaceStaticNullCompareTests, localInterfaceShapeStillCompares) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        Sink local = null;\n"
        "        if (local == null) { score = score + 1; }\n"
        "        Sink live #= heap Impl();\n"
        "        if (live != null) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

TEST(InterfaceStaticNullCompareTests, instanceFieldInterfaceShapeStillCompares) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class Holder {\n"
        "    public Sink field;\n"
        "    public Holder() { this.field = null; }\n"
        "    public void set(#Sink s) { this.field #= s; return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        Holder h = heap Holder();\n"
        "        if (h.field == null) { score = score + 1; }\n"
        "        Sink live #= heap Impl();\n"
        "        h.set(#live);\n"
        "        if (h.field != null) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// ── the --release arm ───────────────────────────────────────────────────
//
// The JIT tests above all run in ONE codegen configuration, and that is
// exactly how the worst half of this defect survived them: the global for a
// static interface field was declared `ptr` (8 bytes) while the emitted code
// memcpy'd the 24-byte fat struct into it and loaded the struct back out.
// Unoptimized builds tolerate the overrun and read back what was written;
// under --release LLVM knows the object's size, folds the out-of-bounds
// reads, and an ASSIGNED field compares EQUAL TO NULL. A silent miscompile in
// the SHIPPING configuration, invisible to every test that only builds one
// way. It surfaced in cajeta-llm, whose suite runs twice — the second pass
// under `--release --live-set=bounded` — and only the second pass failed.
//
// So this arm drives the real compiler binary end to end in each mode and
// runs the program. Both assignment shapes are covered: direct
// (`Held.field = obj`) and through an interface-typed PARAMETER, since the
// two take different paths to the same store.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace {
namespace rfs = std::filesystem;

std::string releaseCompilerBinary() {
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

// Compile + run the fixture under `mode`; returns the program's stdout.
std::string buildAndRun(const std::string& mode, int& exitCode) {
    static std::mt19937_64 rng(std::random_device{}());
    rfs::path root = rfs::temp_directory_path()
        / ("cajeta_ifacerel_" + std::to_string(rng()));
    rfs::create_directories(root / "src" / "pkg");
    rfs::create_directories(root / "out");

    std::ofstream(root / "src" / "pkg" / "Sink.cajeta")
        << "package pkg;\npublic interface Sink { public int64 tag(); }\n";
    std::ofstream(root / "src" / "pkg" / "Impl.cajeta")
        << "package pkg;\n"
           "public final class Impl implements Sink {\n"
           "    public Impl() { return; }\n"
           "    public int64 tag() { return 7; }\n"
           "}\n";
    std::ofstream(root / "src" / "pkg" / "Main.cajeta")
        << "package pkg;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.lang.System;\n"
           "public final class Main {\n"
           "    static Sink direct;\n"
           "    static Sink viaParam;\n"
           "    static void install(Sink s) { Main.viaParam = s; return; }\n"
           "    public static int32 main(String[] args) {\n"
           "        Impl a = heap Impl();\n"
           "        Main.direct = a;\n"
           "        if (Main.direct == null) { System.stdout.println(\"DIRECT-NULL\"); }\n"
           "        else { System.stdout.println(\"DIRECT-OK\"); }\n"
           "        Impl b = heap Impl();\n"
           "        Main.install(b);\n"
           "        if (Main.viaParam == null) { System.stdout.println(\"PARAM-NULL\"); }\n"
           "        else { System.stdout.println(\"PARAM-OK\"); }\n"
           "        return 0;\n"
           "    }\n"
           "}\n";

    rfs::path prog = root / "prog";
    rfs::path log  = root / "run.log";
    std::string cmd = "\"" + releaseCompilerBinary() + "\" --emit=exe " + mode
        + " -o \"" + prog.string() + "\" pkg.Main.main \""
        + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > " CAJETA_PORTABLE_DEVNULL " 2>&1 && \"" + prog.string() + "\" > \""
        + log.string() + "\" 2>&1";
    exitCode = std::system(cmd.c_str());
    std::ifstream in(log);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::error_code ec;
    rfs::remove_all(root, ec);
    return out;
}
}  // namespace

TEST(InterfaceStaticNullCompareTests, assignedStaticIsNonNullInEveryCodegenMode) {
    for (const std::string& mode :
         {std::string(""), std::string("--release"),
          std::string("--release --live-set=bounded")}) {
        int rc = 0;
        std::string out = buildAndRun(mode, rc);
        EXPECT_NE(std::string::npos, out.find("DIRECT-OK"))
            << "mode[" << mode << "]: an assigned static interface field read "
               "as NULL (direct assignment). Output:\n" << out;
        EXPECT_NE(std::string::npos, out.find("PARAM-OK"))
            << "mode[" << mode << "]: an assigned static interface field read "
               "as NULL (assigned through an interface-typed parameter). "
               "Output:\n" << out;
        EXPECT_EQ(std::string::npos, out.find("-NULL"))
            << "mode[" << mode << "]: something read null. Output:\n" << out;
    }
}
