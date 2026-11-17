//
// cajeta-ml-gpu 23.2.2.1.3: offline gfx1151 builds on a box with no ROCm spilled
// attnFlashPrefillF16 (144 B), attnFlashPrefillTile (48 B) and qkPrep (80 B, at 47
// VGPRs) where proton's builds did not. The arch and target features were the same
// on both paths. The ROCm device bitcode was missing, so Math.sin, cos, exp and
// sqrt stayed external calls, whose frames are the scratch. ROCm 7.2's
// rocm-device-libs package puts the bitcode under lib/llvm/lib/clang/<N>/lib/
// amdgcn/bitcode, a layout findRocmBitcodeDir did not probe.
//

#include <gtest/gtest.h>
#include "cajeta/xpu/amd/AmdgpuBackend.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// A fake ROCm root holding an ockl.bc at `sub`, ROCM_PATH pointed at it.
struct FakeRocm {
    fs::path root;
    std::string oldPath;
    bool hadOld = false;
    explicit FakeRocm(const std::string& tag) {
        root = fs::temp_directory_path() / ("cajeta-rocm-" + tag + "-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);
        if (const char* o = std::getenv("ROCM_PATH")) { oldPath = o; hadOld = true; }
        ::setenv("ROCM_PATH", root.string().c_str(), 1);
    }
    std::string add(const std::string& sub) {
        fs::path d = root / sub;
        fs::create_directories(d);
        std::ofstream(d / "ockl.bc") << "";
        return d.string();
    }
    ~FakeRocm() {
        if (hadOld) ::setenv("ROCM_PATH", oldPath.c_str(), 1); else ::unsetenv("ROCM_PATH");
        fs::remove_all(root);
    }
};

} // namespace

// ROCm 7.2's device-libs package: the bitcode under the bundled clang's resource dir.
TEST(AmdgpuRocmBitcode, theClangResourceLayoutIsFound) {
    FakeRocm r("clang");
    std::string want = r.add("lib/llvm/lib/clang/22/lib/amdgcn/bitcode");
    EXPECT_EQ(want, cajeta::xpu::amd::findRocmBitcodeDir());
}

// With two clang versions installed, the newer one's bitcode is taken.
TEST(AmdgpuRocmBitcode, theNewestClangResourceDirWins) {
    FakeRocm r("two");
    r.add("lib/llvm/lib/clang/9/lib/amdgcn/bitcode");
    std::string want = r.add("lib/llvm/lib/clang/22/lib/amdgcn/bitcode");
    EXPECT_EQ(want, cajeta::xpu::amd::findRocmBitcodeDir());
}

// The classic layout is still found first.
TEST(AmdgpuRocmBitcode, theClassicLayoutIsStillFound) {
    FakeRocm r("classic");
    std::string want = r.add("amdgcn/bitcode");
    r.add("lib/llvm/lib/clang/22/lib/amdgcn/bitcode");
    EXPECT_EQ(want, cajeta::xpu::amd::findRocmBitcodeDir());
}
