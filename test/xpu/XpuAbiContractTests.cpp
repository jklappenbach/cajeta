//
// Cajeta XPU — Stage 12 FFI contract tests.
//
// These exercise the *stable C ABI* directly (not through the compiler): the
// version handshake an external port checks, and — as the later Stage-12
// increments land — registering a module + params and dispatching via the
// versioned launch entry point. The contract these pin is documented in
// docs/gpu/xpu/CajetaXPU-FFI.md and declared in runtime/native/cajeta_xpu_abi.h.
//

#include "gtest/gtest.h"

#include <string>

#include "cajeta_xpu_abi.h"   // the single source of truth being tested
#include "cajeta/xpu/vulkan/VulkanDriver.h"

namespace {

// Inc 1 — the header macro and the runtime agree on the ABI version, and an
// external caller can query it before dispatching. This is the handshake that
// lets a downstream port detect a runtime built against a different contract.
TEST(XpuAbiContractTests, abiVersionIsStampedAndQueryable) {
    EXPECT_EQ(__cajeta_xpu_abi_version(), CAJETA_XPU_ABI_VERSION);
    EXPECT_GE(__cajeta_xpu_abi_version(), 1);
}

// Stage 11/12 host spec-constant override — the ABI version was bumped to 2 when
// __cajeta_xpu_launch_v3 (carrying specCount/specValues) landed. The spec-override
// surface is a DURABLE capability from v2 on, so a downstream port requires
// ABI >= 2 to use it — asserted as >= (not ==) so a later, backward-compatible
// bump does not falsely fail. The current-version tripwire lives in the test
// below.
TEST(XpuAbiContractTests, abiVersionBumpedForSpecOverride) {
    EXPECT_GE(CAJETA_XPU_ABI_VERSION, 2);
    EXPECT_GE(__cajeta_xpu_abi_version(), 2);
}

// ABI v3 — the Tier-B device geometry appended to CajetaXpuRawDevice
// (ldsBytesPerBlock, memory clock/bus, grid/block clamps, … — see
// specs/device-geometry-parameterization-spec.md §2.2). This is the
// CURRENT-VERSION tripwire: a hard `==` so the NEXT ABI bump forces a conscious
// update here (append a test, document the new version's surface). A downstream
// port requiring the Tier-B geometry checks ABI >= 3. The referenced field ties
// this version pin to the actual struct surface it stands for.
TEST(XpuAbiContractTests, abiVersionBumpedForTierBGeometry) {
    EXPECT_EQ(CAJETA_XPU_ABI_VERSION, 3);
    EXPECT_EQ(__cajeta_xpu_abi_version(), 3);
    CajetaXpuRawDevice d = {};
    (void) d.ldsBytesPerBlock;   // the v3 append must exist in the ABI struct
}

// Inc 1 — the parameter-kind values are the frozen contract: they must hold
// their wire numbers exactly (append-only, never renumbered), since the launch
// site, the runtime, and any external marshaller all encode against them.
TEST(XpuAbiContractTests, paramKindWireValuesAreFrozen) {
    EXPECT_EQ(CAJETA_XPU_KP_SCALAR,       0);
    EXPECT_EQ(CAJETA_XPU_KP_BUFFER,       1);
    EXPECT_EQ(CAJETA_XPU_KP_TEXTURE,      2);
    EXPECT_EQ(CAJETA_XPU_KP_SAMPLER,      3);
    EXPECT_EQ(CAJETA_XPU_KP_ACCEL,        4);
    EXPECT_EQ(CAJETA_XPU_KP_IMAGE,        5);
    EXPECT_EQ(CAJETA_XPU_KP_BUFFER_ARRAY, 6);
}

// Inc 3 — per-launch device targeting: a deviceId beyond any real device count
// is a *defined* no-op (a diagnostic, never UB), validated before any dispatch.
// This is independent of which backend the process selected, since the
// out-of-range check returns before touching a kernel. The deviceId 0/-1
// (default-device) path runs the kernel and is proven end-to-end by the CPU
// dispatch suite — deviceId 0 falls through to the same dispatch as the
// __cajeta_xpu_launch shim's deviceId -1.
TEST(XpuAbiContractTests, deviceIdOutOfRangeIsDefinedNoOp) {
    void* argv[1] = { nullptr };
    testing::internal::CaptureStderr();
    __cajeta_xpu_launch_v2("xpu_abi_no_such_kernel",
                           /*grid=*/1, 1, 1, /*block=*/1, 1, 1,
                           /*sharedBytes=*/0, argv,
                           /*streamHandle=*/0, /*deviceId=*/1000000);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("out of range"), std::string::npos) << err;
}

}  // namespace

// apple-vulkan 1.1.5 — the compiler-side driver and the runtime bitcode are
// compiled by different compilers with different include paths, so they can
// disagree about whether Vulkan is available. On macOS both silently stubbed for
// every release to date. Pin them together.
extern "C" int32_t __cajeta_xpu_vk_built(void);

TEST(XpuAbiContractTests, vulkanAvailabilityAgreesAcrossCompilerAndRuntime) {
    EXPECT_EQ(__cajeta_xpu_vk_built() != 0,
              ::cajeta::xpu::vulkan::VulkanDriver::builtWithVulkan());
}
