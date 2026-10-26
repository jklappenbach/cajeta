//
// CoffTreeShakeTests — an --emit=exe build leaves no runtime symbol undefined.
//
// On COFF the tree-shake erases every external definition its reachability
// walk missed (pruneUnreachable's IR-level --gc-sections). Codegen expands the
// AOT line frames AFTER that walk, and the expansion calls
// __cajeta_shadow_cur_slow, which nothing referenced yet: the walk erased its
// definition, the expansion re-declared it, and every Windows --emit=exe link
// failed on "undefined symbol: __cajeta_shadow_cur_slow" (2309 references in
// the stdlib object) from a52d1e8c through v0.47.0. ELF never erases at IR
// level, so Linux linked.
//
// The check reads the stdlib object an exe build writes, on a COFF target and
// on the host: every __cajeta_* symbol it leaves undefined must be one the
// build's own stub sources define. The link itself is not part of the check —
// a COFF target cannot link on a non-Windows host.
//
#include <gtest/gtest.h>

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string treeShakeCompilerBinary() {
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

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** The __cajeta_* symbols the stdlib object of an exe build for `triple` leaves
 *  undefined and no stub source of the build names. Empty `triple` = the host. */
std::set<std::string> unresolvedRuntimeSymbols(const std::string& triple,
                                               std::string& why) {
    fs::path root = fs::temp_directory_path() /
        ("cajeta-coff-shake-" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root / "src" / "demo");
    fs::create_directories(root / "out");
    {
        std::ofstream src(root / "src" / "demo" / "Hello.cajeta");
        src << "package demo;\n"
               "public class Hello {\n"
               "    public static int32 main() {\n"
               "        System.stdout.println(\"hello\");\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
    }
    std::string cmd = "\"" + treeShakeCompilerBinary() + "\" --emit=exe"
        + (triple.empty() ? std::string() : " --target=" + triple)
        + " -o \"" + (root / "out" / "hello.exe").string() + "\" demo.Hello.main \""
        + (root / "src").string() + "\" \"" + (root / "out").string() + "\" > \""
        + (root / "build.log").string() + "\" 2>&1";
    (void) std::system(cajeta_shell(cmd).c_str());   // a COFF link fails off Windows

    std::set<std::string> out;
    fs::path obj = root / "out" / "cajeta.runtime.__stdlib__.o";
    if (!fs::exists(obj)) {
        why = "no stdlib object was written; build log:\n" + readAll(root / "build.log");
        out.insert("<no object>");
        return out;
    }
    std::string stubs;
    for (auto& e : fs::directory_iterator(root / "out"))
        if (e.path().extension() == ".c") stubs += readAll(e.path());

    auto buf = llvm::MemoryBuffer::getFile(obj.string());
    if (!buf) { out.insert("<unreadable object>"); return out; }
    auto file = llvm::object::ObjectFile::createObjectFile((*buf)->getMemBufferRef());
    if (!file) {
        llvm::consumeError(file.takeError());
        out.insert("<not an object>");
        return out;
    }
    for (const llvm::object::SymbolRef& sym : (*file)->symbols()) {
        auto flags = sym.getFlags();
        if (!flags) { llvm::consumeError(flags.takeError()); continue; }
        if (!(*flags & llvm::object::SymbolRef::SF_Undefined)) continue;
        auto name = sym.getName();
        if (!name) { llvm::consumeError(name.takeError()); continue; }
        if (!name->starts_with("__cajeta_")) continue;
        if (stubs.find(name->str()) != std::string::npos) continue;
        out.insert(name->str());
    }
    fs::remove_all(root);
    return out;
}

std::string joined(const std::set<std::string>& s) {
    std::string r;
    for (auto& n : s) r += "  " + n + "\n";
    return r;
}

}  // namespace

TEST(CoffTreeShake, aWindowsExeLeavesNoRuntimeSymbolUndefined) {
    std::string why;
    auto missing = unresolvedRuntimeSymbols("x86_64-w64-windows-gnu", why);
    EXPECT_TRUE(missing.empty())
        << "the stdlib object of a COFF exe build needs runtime symbols no "
           "object defines:\n" << joined(missing) << why;
}

// Does not fire: the host's exe build, which links today.
TEST(CoffTreeShake, aHostExeLeavesNoRuntimeSymbolUndefined) {
    std::string why;
    auto missing = unresolvedRuntimeSymbols("", why);
    EXPECT_TRUE(missing.empty()) << joined(missing) << why;
}
