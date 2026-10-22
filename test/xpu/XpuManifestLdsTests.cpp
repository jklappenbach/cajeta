// The shared-memory figure a manifest records is read once per registration, not on every
// launch (cajeta-llm 15.2.16.2.7.5).
#include "gtest/gtest.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" void __cajeta_xpu_register_kernel_manifest(const char* kernelName, int32_t backend,
                                                      const char* arch, const void* json,
                                                      uint64_t len);
extern "C" uint64_t __cajeta_xpu_kernel_manifest_lds(const char* kernelName, int32_t backend);

namespace {

// A backend id no real backend uses, so these manifests reach no launch path.
constexpr int32_t kBackend = 97;

std::string manifest(uint64_t lds, size_t padding = 0) {
    return "{\"access\":[{\"param\":\"" + std::string(padding, 'x') +
           "\"}],\"footprint\":{\"ldsStaticBytes\":" + std::to_string(lds) + "}}";
}

} // namespace

TEST(XpuManifestLds, readsTheRecordedFigureForAKernelAndItsVariants) {
    static const std::string json = manifest(4096);
    __cajeta_xpu_register_kernel_manifest("ldsProbeA", kBackend, "probe", json.data(), json.size());
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeA", kBackend), 4096u);
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeA$v64", kBackend), 4096u);
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeA", kBackend + 1), 0u);
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeAbsent", kBackend), 0u);
}

TEST(XpuManifestLds, readsTheFigureFromTheCompilersOwnManifest) {
    cajeta::xpu::KernelManifest m;
    m.kernel = "test.L.ldsProbeEmitted";
    m.target = "spirv/probe";
    m.ldsStaticBytes = 98304;
    static const std::string json = cajeta::xpu::toJson(m);
    __cajeta_xpu_register_kernel_manifest("ldsProbeEmitted", kBackend, "probe", json.data(),
                                          json.size());
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeEmitted", kBackend), 98304u) << json;
}

TEST(XpuManifestLds, aReRegistrationReplacesTheFigureAlreadyRead) {
    static const std::string first = manifest(1024);
    static const std::string second = manifest(98304);
    __cajeta_xpu_register_kernel_manifest("ldsProbeB", kBackend, "probe", first.data(), first.size());
    ASSERT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeB", kBackend), 1024u);
    __cajeta_xpu_register_kernel_manifest("ldsProbeB", kBackend, "probe", second.data(), second.size());
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeB", kBackend), 98304u);
}

TEST(XpuManifestLds, aKernelRegisteredAfterAMissIsFound) {
    ASSERT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeLate", kBackend), 0u);
    static const std::string json = manifest(2048);
    __cajeta_xpu_register_kernel_manifest("ldsProbeLate", kBackend, "probe", json.data(), json.size());
    EXPECT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeLate", kBackend), 2048u);
}

// A 1 MB manifest costs about a millisecond to scan, so 20000 scanning lookups take
// tens of seconds; read once, they take well under one.
TEST(XpuManifestLds, theFigureIsNotScannedFromTheJsonOnEveryLookup) {
    static const std::string json = manifest(512, 1 << 20);
    __cajeta_xpu_register_kernel_manifest("ldsProbeBig", kBackend, "probe", json.data(), json.size());
    ASSERT_EQ(__cajeta_xpu_kernel_manifest_lds("ldsProbeBig", kBackend), 512u);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (int i = 0; i < 20000; i++) sum += __cajeta_xpu_kernel_manifest_lds("ldsProbeBig", kBackend);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(sum, 20000u * 512u);
    EXPECT_LT(s, 1.0) << "20000 lookups took " << s << " s";
}
