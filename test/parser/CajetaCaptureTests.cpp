// Unit tests for CajetaCapture — the per-binding-site synthetic
// capture type that backs first-class capture conversion (see
// cajeta-docs/CaptureConversion.md). Phase 1.1 is purely the type
// machinery; no behavior wiring yet. These tests pin the contract:
//
//   - Two captures with the same bound have distinct IDs (different
//     binding sites produce different captures, which is the load-
//     bearing property for the type checker).
//   - The capture remembers the bound it was given.
//   - `isCapture()` distinguishes captures from plain classes and
//     wildcard sentinels.
//   - The factory variants (extends / super / unbounded) produce
//     captures with the appropriate bound-direction slot populated.

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaCapture.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::CajetaCapture;
using cajeta::CajetaCapturePtr;
using cajeta::CajetaClass;
using cajeta::CajetaClassPtr;
using cajeta::CajetaModulePtr;
using cajeta::CajetaType;
using cajeta::CajetaTypePtr;
using cajeta::Compiler;

namespace {

// Compile a minimal source into a module so we have a CajetaModule
// available to hand to the CajetaCapture factories. Captures aren't
// supposed to be module-scoped state, but the constructor takes a
// module pointer (inherited from CajetaClass) so we route a real
// module through.
CajetaModulePtr compileTrivial(Compiler& compiler) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_capture_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    auto path = base / "test" / "Animal.cajeta";
    std::ofstream(path) << "package test;\npublic class Animal {}\n";
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_capture_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(path.string(),
                                   base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

} // namespace

// Two captures created with the SAME bound get DIFFERENT IDs. This is
// the foundation of capture identity: each binding site allocates a
// fresh capture, so even structurally identical wildcards across two
// distinct locals don't unify.
TEST(CajetaCaptureTests, sameUpperBoundProducesDistinctCaptures) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap1 = CajetaCapture::forExtendsBound(module, animal);
    auto cap2 = CajetaCapture::forExtendsBound(module, animal);
    ASSERT_NE(cap1, nullptr);
    ASSERT_NE(cap2, nullptr);
    EXPECT_NE(cap1->getCaptureId(), cap2->getCaptureId())
        << "fresh captures must carry distinct IDs even when bound matches";
    EXPECT_NE(cap1.get(), cap2.get())
        << "captures are not interned by bound; each call is a fresh allocation";
}

// An extends-bounded capture stores the upper bound, not the lower.
TEST(CajetaCaptureTests, extendsCaptureCarriesUpperBound) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forExtendsBound(module, animal);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->getUpperBound().get(), animal.get());
    EXPECT_EQ(cap->getLowerBound(), nullptr);
}

// A super-bounded capture stores the lower bound, not the upper. The
// upper bound for `? super B` is conceptually Object (or top); v1
// leaves it null and the type checker projects to Object when needed.
TEST(CajetaCaptureTests, superCaptureCarriesLowerBound) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forSuperBound(module, animal);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->getLowerBound().get(), animal.get());
    EXPECT_EQ(cap->getUpperBound(), nullptr);
}

// An unbounded capture carries neither bound — reads project to the
// top type and writes are rejected outright (modulo capture identity).
TEST(CajetaCaptureTests, unboundedCaptureHasNoBounds) {
    Compiler compiler;
    auto module = compileTrivial(compiler);

    auto cap = CajetaCapture::forUnbounded(module);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->getUpperBound(), nullptr);
    EXPECT_EQ(cap->getLowerBound(), nullptr);
}

// `isCapture()` distinguishes captures from regular class types.
// A plain `Animal` class isn't a capture; a fresh capture is.
TEST(CajetaCaptureTests, isCaptureDistinguishesFromClass) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forExtendsBound(module, animal);

    // The plain Animal class is not a capture. We test via dynamic
    // cast because isCapture() is declared on CajetaCapture only — the
    // base CajetaClass doesn't expose it.
    EXPECT_EQ(std::dynamic_pointer_cast<CajetaCapture>(animal), nullptr)
        << "Animal should not be a capture";
    EXPECT_NE(std::dynamic_pointer_cast<CajetaCapture>(cap), nullptr)
        << "a CajetaCapturePtr should round-trip through dynamic_pointer_cast";
    EXPECT_TRUE(cap->isCapture());
}
