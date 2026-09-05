//
// `System.args` — the ambient argument vector (script-units; the argv half of
// the System.<ns> intrinsics beside System.env and System.property).
//
// The property these tests exist to defend is not "argv is readable". It is
// that there is ONE store. A class entry receives a `String[]`; any code
// anywhere reads `System.args`. Both are built from the same installed
// vector, so they cannot report different arguments — an arrangement chosen
// after the alternative was measured to break: the host wrote its own copy of
// the runtime's statics while the JIT'd program read the module's copy, and
// `System.args.count()` returned 0 while the program ran with four arguments.
// A silent wrong answer, because "no arguments" is a legitimate result.
//
// So `noArgumentsIsStated` below is deliberately NOT the only coverage: a
// suite that only checked the empty case would pass against a host that never
// installed anything at all.
//

#include "gtest/gtest.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string argsCompilerBinary() {
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
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

fs::path argsTempDir() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_sysargs_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "demo");
    fs::create_directories(base / "build");
    return base;
}

std::string argsCapture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
#ifdef _WIN32
    _pclose(p);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
#else
    pclose(p);
#endif
    return out;
}

// Write a script unit — no class, no entry method — and run it.
std::string runScript(const fs::path& base, const std::string& body,
                      const std::string& argsTail) {
    auto file = base / "s.cajeta";
    { std::ofstream out(file); out << body; }
    return argsCapture(argsCompilerBinary() + " run " + file.string()
                       + " " + argsTail);
}

} // namespace

// A script unit has no parameter to receive argv through — this is the shape
// the ambient accessor exists for.
TEST(SystemArgsTests, aScriptUnitReadsTheArgumentVector) {
    auto base = argsTempDir();
    auto out = runScript(base,
        "int64 n = System.args.count();\n"
        "System.stdout.println(\"n=\" + n);\n"
        "int64 i = 0;\n"
        "while (i < n) { System.stdout.println(System.args.get(i)); i = i + 1; }\n",
        "run --model m.gguf");
    EXPECT_EQ("n=3\nrun\n--model\nm.gguf\n", out);
    fs::remove_all(base);
}

// AMBIENT: a method several frames below the entry reads argv without it
// being threaded through. This is the whole reason for the accessor over an
// injected `args` parameter.
TEST(SystemArgsTests, aHelperReadsArgvWithoutItBeingThreadedThrough) {
    auto base = argsTempDir();
    auto out = runScript(base,
        "System.stdout.println(depth1());\n"
        "String depth1() { return depth2(); }\n"
        "String depth2() { return System.args.get(0); }\n",
        "deep");
    EXPECT_EQ("deep\n", out);
    fs::remove_all(base);
}

// Past the end is NULL, matching System.env.get for an unset variable, so one
// idiom covers both. Clamping to "" would make a typo'd index look like an
// empty argument.
TEST(SystemArgsTests, pastTheEndIsNull) {
    auto base = argsTempDir();
    auto out = runScript(base,
        "String past = System.args.get(System.args.count());\n"
        "if (past == null) { System.stdout.println(\"null\"); }\n"
        "else { System.stdout.println(\"got:\" + past); }\n",
        "only");
    EXPECT_EQ("null\n", out);
    fs::remove_all(base);
}

// Zero is a real answer, and every host states it rather than leaving the
// store cold. On its own this test cannot tell a working install from a
// missing one — the tests above are what give it meaning.
TEST(SystemArgsTests, noArgumentsIsStated) {
    auto base = argsTempDir();
    auto out = runScript(base,
        "System.stdout.println(\"n=\" + System.args.count());\n", "");
    EXPECT_EQ("n=0\n", out);
    fs::remove_all(base);
}

// THE INVARIANT. A compiled binary takes the path where the two spellings
// could most easily diverge: the exe shim must slice argv[0] (the program
// name) while the JIT never had it. Both now read one store, so they agree by
// construction — and this fails the moment anyone reintroduces a second
// vector.
TEST(SystemArgsTests, theStringArrayAndSystemArgsAgreeInACompiledBinary) {
    auto base = argsTempDir();
    {
        std::ofstream out(base / "src" / "demo" / "Agree.cajeta");
        out << "package demo;\n"
            << "public final class Agree {\n"
            << "    public static int32 main(String[] args) {\n"
            << "        boolean same = args.count() == System.args.count();\n"
            << "        int64 i = 0;\n"
            << "        while (i < args.count()) {\n"
            << "            if (!args[(int32) i].equals(System.args.get(i))) { same = false; }\n"
            << "            i = i + 1;\n"
            << "        }\n"
            << "        System.stdout.println(\"n=\" + args.count()"
            << " + \" agree=\" + same);\n"
            << "        return 0;\n"
            << "    }\n"
            << "}\n";
    }
    auto bin = base / "build" / "agree";
    std::string cmd = argsCompilerBinary() + " --emit=exe -o " + bin.string()
        + " demo.Agree.main " + (base / "src").string() + " "
        + (base / "build").string() + " > /dev/null 2>&1";
    bool built = std::system(cmd.c_str()) == 0;
    if (built && !fs::exists(bin)) {
        fs::path withExe = bin; withExe += ".exe";
        if (fs::exists(withExe)) bin = withExe;
    }
    ASSERT_TRUE(built && fs::exists(bin)) << "exe build failed";

    EXPECT_EQ("n=3 agree=true\n", argsCapture(bin.string() + " run --model m"));
    EXPECT_EQ("n=0 agree=true\n", argsCapture(bin.string()));
    fs::remove_all(base);
}

// argv reports how the process was invoked; a program that could rewrite it
// would be lying to anything that read it afterwards.
TEST(SystemArgsTests, systemArgsIsReadOnly) {
    auto base = argsTempDir();
    auto file = base / "s.cajeta";
    { std::ofstream out(file); out << "System.args.set(\"0\", \"hacked\");\n"; }
#ifdef _WIN32
    FILE* p = _popen((argsCompilerBinary() + " run " + file.string()
                      + " 2>&1").c_str(), "r");
#else
    FILE* p = popen((argsCompilerBinary() + " run " + file.string()
                     + " 2>&1").c_str(), "r");
#endif
    ASSERT_NE(p, nullptr);
    std::string out; std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    EXPECT_NE(out.find("CAJETA_ERROR_ARGS_READ_ONLY"), std::string::npos)
        << "actual: " << out;
    fs::remove_all(base);
}

// Regression, and it was NOT introduced by System.args — the same fault hit
// System.env.get for as long as the intrinsic existed. The System.<ns>
// intrinsics never stamped a resolved type on the call, so a context that
// asks what the expression is — string concatenation, most visibly —
// formatted the raw pointer. Assigning to a local first hid it, because the
// declaration supplied the type instead.
TEST(SystemArgsTests, anIntrinsicGetConcatenatesAsAString) {
    auto base = argsTempDir();
    auto out = runScript(base,
        "String viaLocal = System.args.get(0);\n"
        "System.stdout.println(\"local:\" + viaLocal);\n"
        "System.stdout.println(\"inline:\" + System.args.get(0));\n",
        "value");
    EXPECT_EQ("local:value\ninline:value\n", out);
    fs::remove_all(base);
}
