// A value-type release function is defined in the object that calls it. Defined
// only in the stdlib's object, a ThinLTO build (cvm's release flavor) wrote the
// stdlib before its callers added the function, and the link left
// `__cajeta_vrel_cajeta_lang_Slice_int8_` undefined.

#include <gtest/gtest.h>

#include "../PortableEnv.h"

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.lang.Slice;\n"
    "import cajeta.lang.System;\n"
    "public class V {\n"
    "    int8[] data = heap int8[8];\n"
    "    Slice<int8> body() { return this.data[2:6]; }\n"
    "    static int64 total(Slice<int8> s) {\n"
    "        int64 t = 0;\n"
    "        for (int64 i = 0; i < s.count(); i = i + 1) { t = t + (int64) s[i]; }\n"
    "        return t;\n"
    "    }\n"
    "    public static int32 main() {\n"
    "        V v = heap V();\n"
    "        v.data[3] = (int8) 7;\n"
    "        System.stdout.println(\"t=\" + V.total(v.body()));\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r = (envRoot && *envRoot) ? envRoot : ".";
    return r + "/build/src/cajeta";
}

// 1 defined, 0 undefined, -1 absent or unreadable.
int symbolState(const fs::path& obj, const std::string& name) {
    auto buf = llvm::MemoryBuffer::getFile(obj.string());
    if (!buf) return -1;
    auto file = llvm::object::ObjectFile::createObjectFile((*buf)->getMemBufferRef());
    if (!file) { llvm::consumeError(file.takeError()); return -1; }
    for (const auto& sym : (*file)->symbols()) {
        auto n = sym.getName();
        if (!n) { llvm::consumeError(n.takeError()); continue; }
        if (*n != name) continue;
        auto flags = sym.getFlags();
        if (!flags) { llvm::consumeError(flags.takeError()); return -1; }
        return (*flags & llvm::object::SymbolRef::SF_Undefined) ? 0 : 1;
    }
    return -1;
}

}  // namespace

TEST(ValueReleaseHomeTests, theCallersObjectDefinesTheReleaseItCalls) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path dir = fs::temp_directory_path() / ("cajeta_vrelhome_" + std::to_string(rng()));
    fs::create_directories(dir / "src" / "test");
    fs::create_directories(dir / "out");
    std::ofstream(dir / "src" / "test" / "V.cajeta") << kSource;
    std::string cmd = "\"" + compilerBinary() + "\" --emit=exe test.V.main \""
                    + (dir / "src").string() + "\" \"" + (dir / "out").string()
                    + "\" > \"" + (dir / "log").string() + "\" 2>&1";
    ASSERT_EQ(0, std::system(cajeta_shell(cmd).c_str())) << "the build failed, see " << dir / "log";
    EXPECT_EQ(1, symbolState(dir / "out" / "test" / "V.o", "__cajeta_vrel_cajeta_lang_Slice_int8_"))
        << "test/V.o releases its Slice<int8> temporary but does not define the release";
    std::error_code ec;
    fs::remove_all(dir, ec);
}
