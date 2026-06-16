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

namespace {

// Inc 1 — the header macro and the runtime agree on the ABI version, and an
// external caller can query it before dispatching. This is the handshake that
// lets a downstream port detect a runtime built against a different contract.
TEST(XpuAbiContractTests, abiVersionIsStampedAndQueryable) {
    EXPECT_EQ(__cajeta_xpu_abi_version(), CAJETA_XPU_ABI_VERSION);
    EXPECT_GE(__cajeta_xpu_abi_version(), 1);
}

// Stage 11/12 host spec-constant override — the ABI version was bumped to 2 when
// __cajeta_xpu_launch_v3 (carrying specCount/specValues) landed. Pins the value
// so a downstream port can require >= 2 to use the spec-override surface.
TEST(XpuAbiContractTests, abiVersionBumpedForSpecOverride) {
    EXPECT_EQ(CAJETA_XPU_ABI_VERSION, 2);
    EXPECT_EQ(__cajeta_xpu_abi_version(), 2);
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
