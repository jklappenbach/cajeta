// `cajeta lower` gives the session-install symbols weak definitions.
//
// `cajeta_rt_session.c` declares __cajeta_install_hook / _ctx / _out extern
// ON PURPOSE — they are defined in the HOST, and that file is compiled twice
// (into the compiler, and into the bitcode every JIT session carries), so a
// definition there would give JIT'd cell code a second copy and make the
// host's registration invisible to it.
//
// An object lowered for an AOT link has no host, so those references have
// nothing to resolve to. `70ef31ae` fixed that for `--emit=exe` by adding a
// generated weak stub to the link line — which cannot reach a caller who
// builds their OWN link line. cajeta-coco does exactly that: its instrument
// pass lowers the stdlib and links it, and it failed on all three symbols
// with only coco's tour gate to notice.
//
// The failure was intermittent by nature: whether __cajeta_session_install
// survives DCE depends on the debug-info level, so a build that linked was a
// build that got lucky. These tests pin both directions so a fix that trades
// one broken configuration for another fails here rather than in a consumer.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() {
    const char* env = std::getenv("CAJETA_SOURCE_ROOT");
    if (env && *env) return fs::path(env);
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return fs::path(CAJETA_SOURCE_ROOT_DEFAULT);
#else
    return fs::path(".");
#endif
}

fs::path compilerPath() { return repoRoot() / "build" / "src" / "cajeta"; }

// An `.ll` that references all three session symbols plus an unrelated
// extern, every one of them USED so nothing is dropped before lowering.
const char* kSource = R"(target triple = "x86_64-unknown-linux-gnu"

@__cajeta_install_hook = external global ptr
@__cajeta_install_ctx = external global ptr
@__cajeta_install_out = external global [2048 x i8]
@__unrelated_host_symbol = external global ptr

define ptr @touch() {
  %a = load ptr, ptr @__cajeta_install_hook
  %b = load ptr, ptr @__unrelated_host_symbol
  store ptr %a, ptr @__cajeta_install_ctx
  store ptr %b, ptr @__cajeta_install_ctx
  ret ptr @__cajeta_install_out
}
)";

// `nm` output for the lowered object, or empty when the tools are missing.
std::string symbolsOf(const fs::path& obj) {
    const std::string cmd = "nm '" + obj.string() + "' 2>/dev/null";
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
    ::pclose(p);
    return out;
}

// A symbol line whose type letter is `V` or `W` — a WEAK DEFINITION, which is
// what lets a host's strong definition still win.
bool weaklyDefined(const std::string& nm, const std::string& symbol) {
    std::istringstream in(nm);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(symbol) == std::string::npos) continue;
        // "<addr> <type> <name>" — the type letter precedes the name.
        const size_t at = line.find(symbol);
        if (at < 2) continue;
        const char type = line[at - 2];
        return type == 'V' || type == 'W';
    }
    return false;
}

bool undefined(const std::string& nm, const std::string& symbol) {
    std::istringstream in(nm);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(symbol) == std::string::npos) continue;
        const size_t at = line.find(symbol);
        if (at < 2) continue;
        return line[at - 2] == 'U';
    }
    return false;
}

}  // namespace

TEST(IrLowerSessionSymbols, loweredObjectDefinesTheSessionSymbolsWeakly) {
    if (!fs::is_regular_file(compilerPath())) {
        GTEST_SKIP() << "no built compiler at " << compilerPath();
    }

    const fs::path dir = fs::temp_directory_path() / "cajeta-lower-session";
    fs::create_directories(dir);
    const fs::path ll = dir / "session.ll";
    const fs::path obj = dir / "session.o";
    { std::ofstream(ll) << kSource; }
    fs::remove(obj);

    const std::string cmd =
        "'" + compilerPath().string() + "' lower '" + ll.string() +
        "' -o '" + obj.string() + "' >/dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "cajeta lower failed";
    ASSERT_TRUE(fs::is_regular_file(obj)) << "no object produced";

    const std::string nm = symbolsOf(obj);
    if (nm.empty()) GTEST_SKIP() << "nm produced no output on this host";

    for (const char* s : {"__cajeta_install_hook", "__cajeta_install_ctx",
                          "__cajeta_install_out"}) {
        EXPECT_TRUE(weaklyDefined(nm, s))
            << s << " is not weakly defined; an AOT link that includes this "
                    "object has nothing to resolve it to:\n" << nm;
    }

    // The negative arm, and the one that keeps this honest: the fix must
    // define exactly these three and invent nothing else. A pass that
    // defined every undefined symbol would make every link succeed and every
    // missing dependency silent.
    EXPECT_TRUE(undefined(nm, "__unrelated_host_symbol"))
        << "an unrelated extern was given a definition it never had:\n" << nm;
}
