//
// script-units U6 (spec 7.1-7.4, 5.6 script half) — `cajeta run <file>.cajeta`.
//
// Drives the real built binary end-to-end (the IncrementalBuildTests
// pattern): a script file compiles as a script unit, runs under the JIT
// host, session bindings drop after the entry returns (plan 3.2.3 — the
// host lifecycle U3 deferred here), an explicit `return N` is the exit
// code, a trailing expression is NOT (spec 8.2), manifest dependencies of
// an enclosing project resolve onto the classpath, and an uncaught throw
// exits non-zero with the message on stderr. The compile-error case pins
// U5's 5.3.1 at the CLI: --diag-format=json diagnostics carry the script's
// path in `file`.
//
// NOTE (stale-CLI trap): these tests exec build/src/cajeta — the `cajeta`
// target must be rebuilt alongside cajeta_test or they test yesterday's
// binary.
//

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include "../PortableEnv.h"

#ifndef _WIN32
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
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

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_runcmd_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << content;
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

struct RunResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

// Run `cajeta run <script>` from `cwd` with HOME pinned into the temp tree
// (dependency resolution write-through must never touch the real ~).
RunResult runScript(const fs::path& cwd, const std::string& args) {
    fs::path outLog = cwd / "run.out";
    fs::path errLog = cwd / "run.err";
    fs::path home = cwd / "home";
    fs::create_directories(home);
    std::ostringstream cmd;
    cmd << CAJETA_PORTABLE_CD << cwd.string() << " && "
        << cajeta_env_prefix({{"HOME", home.string()}}) << compilerBinary()
        << " run " << args << " > " << outLog << " 2> " << errLog;
    int status = std::system(cajeta_shell(cmd.str()).c_str());
    return {exitCodeOf(status), readAll(outLog), readAll(errLog)};
}

}  // namespace

// 6.1.1 / spec 7.1 + plan 3.2.3 — a hello script compiles, runs, exits 0,
// and its session bindings drop AFTER the entry returns (destructor output
// ordered after the entry's last print).
TEST(RunCommandTests, helloScriptRunsExitsZero) {
    auto dir = freshTempDir("hello");
    writeFile(dir / "hello.cajeta",
        "public class D {\n"
        "    public D() { }\n"
        "    public ~D() { System.stdout.println(\"dropped\"); }\n"
        "}\n"
        "D d = heap D();\n"
        "int32 x = 40 + 2;\n"
        "System.stdout.println(\"hello \" + x);\n");
    RunResult r = runScript(dir, "hello.cajeta");
    EXPECT_EQ(0, r.exitCode) << r.err;
    size_t hello = r.out.find("hello 42");
    size_t dropped = r.out.find("dropped");
    EXPECT_NE(std::string::npos, hello) << r.out;
    EXPECT_NE(std::string::npos, dropped) << r.out;
    EXPECT_LT(hello, dropped);   // session drop_all fires after the entry
}

// 6.1.2 / spec 7.2 — explicit `return 3;` is the process exit code.
TEST(RunCommandTests, explicitReturnIsExitCode) {
    auto dir = freshTempDir("exitcode");
    writeFile(dir / "three.cajeta",
        "int32 v = 1;\n"
        "return v + 2;\n");
    RunResult r = runScript(dir, "three.cajeta");
    EXPECT_EQ(3, r.exitCode) << r.err;
}

// 6.1.2 / spec 5.6 + 8.2 — a trailing expression is the unit RESULT, not
// the exit code: the script host ignores it and exits 0.
TEST(RunCommandTests, trailingExpressionIsNotExitCode) {
    auto dir = freshTempDir("trailing");
    writeFile(dir / "tail.cajeta",
        "int32 v = 40;\n"
        "v + 2;\n");
    RunResult r = runScript(dir, "tail.cajeta");
    EXPECT_EQ(0, r.exitCode) << r.err;
}

// 6.1.3 / spec 7.3 — a script inside a project directory resolves the
// manifest's dependencies onto the classpath. The dependency is a real
// library built by `cajeta build` and served from a filesystem repository.
TEST(RunCommandTests, projectClasspathResolves) {
    // 1. Build the dependency library.
    auto lib = freshTempDir("deplib");
    writeFile(lib / "cajeta.json",
        "{\n"
        "  \"details\": { \"name\": \"dev.example.greet\","
        " \"version\": \"0.1.0\", \"cajeta-lang-version\": \"1.0\" },\n"
        "  \"tasks\": { \"build\": { \"actions\": [\n"
        "      { \"action\": \"build\", \"flavor\": \"debug\","
        " \"id\": \"lib\" } ] } }\n"
        "}\n");
    writeFile(lib / "src" / "main" / "cajeta" / "dev" / "example" / "greet"
                  / "Greeter.cajeta",
        "package dev.example.greet;\n"
        "public class Greeter {\n"
        "    public static int32 answer() { return 42; }\n"
        "}\n");
    {
        fs::path home = lib / "home";
        fs::create_directories(home);
        std::ostringstream cmd;
        cmd << CAJETA_PORTABLE_CD << lib.string() << " && "
            << cajeta_env_prefix({{"HOME", home.string()}}) << compilerBinary()
            << " build > build.log 2>&1";
        ASSERT_EQ(0, exitCodeOf(std::system(cajeta_shell(cmd.str()).c_str())))
            << readAll(lib / "build.log");
    }
    // 2. Lay the archive into a filesystem-repository layout.
    fs::path archive;
    for (auto& e : fs::directory_iterator(lib / "build" / "archive")) {
        if (e.path().extension() == ".cja") archive = e.path();
    }
    ASSERT_FALSE(archive.empty());
    auto repo = freshTempDir("repo");
    auto slot = repo / "dev.example.greet" / "0.1.0";
    fs::create_directories(slot);
    fs::copy_file(archive, slot / "dev.example.greet-0.1.0.cja");
    writeFile(slot / "cajeta.json",
        "{\"details\":{\"name\":\"dev.example.greet\",\"version\":\"0.1.0\"}}");
    // 3. The script project declares the dependency; the script imports it.
    auto proj = freshTempDir("proj");
    writeFile(proj / "cajeta.json",
        "{\n"
        "  \"details\": { \"name\": \"app.tool\", \"version\": \"0.1.0\","
        " \"cajeta-lang-version\": \"1.0\" },\n"
        "  \"settings\": {\n"
        "    \"repositories\": [ { \"name\": \"localfs\","
        " \"type\": \"filesystem\", \"path\": \"" + repo.generic_string()
            + "\" } ],\n"
        "    \"dependencies\": { \"dev.example.greet\": \"0.1.*\" }\n"
        "  }\n"
        "}\n");
    writeFile(proj / "tool.cajeta",
        "import dev.example.greet.Greeter;\n"
        "return Greeter.answer() - 42;\n");
    RunResult r = runScript(proj, "tool.cajeta");
    EXPECT_EQ(0, r.exitCode) << r.err;
}

// 6.1.4 / spec 7.4 — an uncaught throw prints the message (and a 6.2-style
// trace) and exits non-zero.
TEST(RunCommandTests, uncaughtThrowNonZeroWithTrace) {
    auto dir = freshTempDir("throws");
    writeFile(dir / "boom.cajeta",
        "import cajeta.error.Exception;\n"
        "throw heap Exception(\"kaboom-7\");\n");
    RunResult r = runScript(dir, "boom.cajeta");
    EXPECT_NE(0, r.exitCode);
    EXPECT_NE(std::string::npos, (r.err + r.out).find("kaboom-7"))
        << "stdout: " << r.out << "\nstderr: " << r.err;
}

// 6.3.3(a) regression — variable obscures type. A single-letter file stem
// makes the implicit class `t`, which collides with stdlib method LOCALS
// named `t`; before the obscuring fix, stdlib codegen under the JIT host
// died with "no member 'scheme' on 't'". The whole stdlib compiles behind
// this run, so it pins the fix at full breadth.
TEST(RunCommandTests, singleLetterScriptStemRuns) {
    auto dir = freshTempDir("stem");
    writeFile(dir / "t.cajeta",
        "int32 x = 40;\n"
        "System.stdout.println(\"t=\" + (x + 2));\n");
    RunResult r = runScript(dir, "t.cajeta");
    EXPECT_EQ(0, r.exitCode) << r.err;
    EXPECT_NE(std::string::npos, r.out.find("t=42")) << r.out;
}

// U5 5.3.1 at the CLI — a compile error under --diag-format=json carries
// the script's path in the record's `file` field and a host line.
TEST(RunCommandTests, compileErrorJsonCarriesHostFile) {
    auto dir = freshTempDir("jsondiag");
    writeFile(dir / "bad.cajeta",
        "public class P { public int32 v; public P(int32 v) { this.v = v; } }\n"
        "P p = heap P(1);\n"
        "P q = p;\n");
    RunResult r = runScript(dir, "--diag-format=json bad.cajeta");
    EXPECT_NE(0, r.exitCode);
    std::string all = r.err + r.out;
    EXPECT_NE(std::string::npos, all.find("CAJETA_ERROR_SESSION_BORROW_ESCAPE"))
        << all;
    EXPECT_NE(std::string::npos, all.find("bad.cajeta")) << all;
}

// ─── slot cache (--cache-dir) ──────────────────────────────────────
//
// `cajeta run --cache-dir=DIR` persists the compiled script as a cache
// slot (program.meta + the module dylibs) and reuses it on a warm run.
// None of that machinery — the meta writer, the strict v2 meta reader,
// the digest check, invalidation — had a consumer.

TEST(RunCommandTests, cacheDirIsPopulatedAndWarmRunReusesIt) {
    auto dir = freshTempDir("cachewarm");
    writeFile(dir / "s.cajeta",
        "int32 x = 20 + 3;\n"
        "System.stdout.println(\"value \" + x);\n"
        "return x;\n");
    fs::path cache = dir / "slotcache";

    RunResult cold = runScript(dir, "--cache-dir=" + cache.string()
                                    + " s.cajeta");
    EXPECT_EQ(23, cold.exitCode) << cold.err;
    EXPECT_NE(cold.out.find("value 23"), std::string::npos) << cold.out;
    ASSERT_TRUE(fs::exists(cache)) << cold.err;
    EXPECT_FALSE(fs::is_empty(cache)) << "cache dir left empty";

    // Warm: same source, same slot — identical observable behaviour.
    RunResult warm = runScript(dir, "--cache-dir=" + cache.string()
                                    + " s.cajeta");
    EXPECT_EQ(23, warm.exitCode) << warm.err;
    EXPECT_NE(warm.out.find("value 23"), std::string::npos) << warm.out;
}

TEST(RunCommandTests, editingTheScriptInvalidatesTheSlot) {
    auto dir = freshTempDir("cacheinval");
    fs::path cache = dir / "slotcache";
    writeFile(dir / "s.cajeta", "return 11;\n");
    EXPECT_EQ(11, runScript(dir, "--cache-dir=" + cache.string()
                                 + " s.cajeta").exitCode);

    // A different program must not serve the old slot.
    writeFile(dir / "s.cajeta", "return 22;\n");
    RunResult second = runScript(dir, "--cache-dir=" + cache.string()
                                      + " s.cajeta");
    EXPECT_EQ(22, second.exitCode) << second.err;
}

// The meta reader is strict by design: any anomaly is a MISS, so a
// truncated / garbled slot re-compiles instead of misbehaving.
TEST(RunCommandTests, corruptSlotMetaFallsBackToCompiling) {
    auto dir = freshTempDir("cachecorrupt");
    fs::path cache = dir / "slotcache";
    writeFile(dir / "s.cajeta", "return 7;\n");
    ASSERT_EQ(7, runScript(dir, "--cache-dir=" + cache.string()
                                + " s.cajeta").exitCode);

    // Garble every meta file the cold run wrote.
    int clobbered = 0;
    for (auto& e : fs::recursive_directory_iterator(cache)) {
        if (e.is_regular_file() && e.path().filename() == "program.meta") {
            std::ofstream(e.path(), std::ios::trunc) << "not-jitmeta\n";
            ++clobbered;
        }
    }
    EXPECT_GT(clobbered, 0) << "no program.meta written by the cold run";

    RunResult after = runScript(dir, "--cache-dir=" + cache.string()
                                     + " s.cajeta");
    EXPECT_EQ(7, after.exitCode) << after.err;
}

// A wrong meta VERSION line is the other strict-reject arm.
TEST(RunCommandTests, wrongMetaVersionIsAMiss) {
    auto dir = freshTempDir("cacheversion");
    fs::path cache = dir / "slotcache";
    writeFile(dir / "s.cajeta", "return 5;\n");
    ASSERT_EQ(5, runScript(dir, "--cache-dir=" + cache.string()
                                + " s.cajeta").exitCode);

    for (auto& e : fs::recursive_directory_iterator(cache)) {
        if (e.is_regular_file() && e.path().filename() == "program.meta") {
            std::string body = readAll(e.path());
            auto nl = body.find('\n');
            if (nl != std::string::npos) {
                std::ofstream(e.path(), std::ios::trunc)
                    << "cajeta-jitmeta-v1" << body.substr(nl);
            }
        }
    }
    EXPECT_EQ(5, runScript(dir, "--cache-dir=" + cache.string()
                                + " s.cajeta").exitCode);
}

TEST(RunCommandTests, cacheDirIsCreatedWhenAbsent) {
    auto dir = freshTempDir("cachemk");
    writeFile(dir / "s.cajeta", "return 4;\n");
    fs::path nested = dir / "a" / "b" / "slots";
    RunResult r = runScript(dir, "--cache-dir=" + nested.string()
                                 + " s.cajeta");
    EXPECT_EQ(4, r.exitCode) << r.err;
    EXPECT_TRUE(fs::exists(nested)) << r.err;
}
