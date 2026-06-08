// Unit tests for CajetaCapture — the per-binding-site synthetic
// capture type that backs first-class capture conversion (see
// docs/CaptureConversion.md). Phase 1.1 is purely the type
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

// Phase 1.2 — captureProject projects an extends-bounded capture to
// its upper bound. Mirrors the bounded-wildcard case (captureProject
// already handles `? extends B` sentinels by returning B). This lets
// chained member access through a capture-typed receiver resolve
// against the bound's surface (e.g., a `b.get()` whose return type is
// capture#N projects to Animal so `.tag()` resolves on Animal).
TEST(CajetaCaptureTests, captureProjectExtendsCaptureReturnsUpperBound) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forExtendsBound(module, animal);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    auto projected = cajeta::CajetaType::captureProject(asType);
    EXPECT_EQ(projected.get(), animal.get())
        << "extends-bounded capture should project to its upper bound";
}

// A super-bounded capture has no upper bound that's safe to project
// to in v1 — reads through `? super B` formally produce Object,
// which cajeta doesn't model as a projection target here. Leave the
// super capture unchanged (the call site can decide how to handle it).
TEST(CajetaCaptureTests, captureProjectSuperCaptureReturnsItself) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forSuperBound(module, animal);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    auto projected = cajeta::CajetaType::captureProject(asType);
    EXPECT_EQ(projected.get(), asType.get())
        << "super-bounded capture has no read-projection target; "
        << "captureProject should return it unchanged";
}

// An unbounded capture also has no projection target — same shape as
// the unbounded wildcard sentinel case that captureProject already
// leaves alone.
TEST(CajetaCaptureTests, captureProjectUnboundedCaptureReturnsItself) {
    Compiler compiler;
    auto module = compileTrivial(compiler);

    auto cap = CajetaCapture::forUnbounded(module);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    auto projected = cajeta::CajetaType::captureProject(asType);
    EXPECT_EQ(projected.get(), asType.get())
        << "unbounded capture has no projection target; should return itself";
}

// Phase 2.0 — captures register in the wildcard-info side table so
// every existing wildcard-aware code path (isWildcardInstantiation,
// substitution-stable hashes, PECS check, alias publish in
// buildVirtualTable) treats Box<capture#N> identically to
// Box<? extends Animal>. The capture retains its per-binding
// identity via the unique qName.
TEST(CajetaCaptureTests, extendsCaptureReportsAsWildcardWithBound) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forExtendsBound(module, animal);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    EXPECT_TRUE(asType->isWildcard())
        << "extends capture must appear as a wildcard to existing predicates";
    EXPECT_EQ(asType->wildcardKind(), cajeta::CajetaType::WildcardKind::Extends);
    EXPECT_EQ(asType->wildcardBound().get(), animal.get());
}

// Super captures register as super-kind wildcards with the lower
// bound recorded as the wildcard bound.
TEST(CajetaCaptureTests, superCaptureReportsAsWildcardWithBound) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap = CajetaCapture::forSuperBound(module, animal);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    EXPECT_TRUE(asType->isWildcard());
    EXPECT_EQ(asType->wildcardKind(), cajeta::CajetaType::WildcardKind::Super);
    EXPECT_EQ(asType->wildcardBound().get(), animal.get());
}

// Unbounded captures register as unbounded wildcards with null bound.
TEST(CajetaCaptureTests, unboundedCaptureReportsAsUnboundedWildcard) {
    Compiler compiler;
    auto module = compileTrivial(compiler);

    auto cap = CajetaCapture::forUnbounded(module);
    CajetaTypePtr asType = std::static_pointer_cast<CajetaType>(cap);
    EXPECT_TRUE(asType->isWildcard());
    EXPECT_EQ(asType->wildcardKind(), cajeta::CajetaType::WildcardKind::Unbounded);
    EXPECT_EQ(asType->wildcardBound(), nullptr);
}

// Two extends-captures with the same bound have DIFFERENT canonical
// names — that's how identity is preserved through the wildcard-info
// table. The wildcard sentinel infrastructure caches by canonical,
// so distinct canonicals give distinct identities.
TEST(CajetaCaptureTests, distinctCapturesHaveDistinctCanonicals) {
    Compiler compiler;
    auto module = compileTrivial(compiler);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto cap1 = CajetaCapture::forExtendsBound(module, animal);
    auto cap2 = CajetaCapture::forExtendsBound(module, animal);
    ASSERT_NE(cap1->getQName(), nullptr);
    ASSERT_NE(cap2->getQName(), nullptr);
    EXPECT_NE(cap1->getQName()->toCanonical(), cap2->getQName()->toCanonical())
        << "two fresh captures must have distinct canonical names so the "
        << "wildcard-info table stores them as distinct entries";
}
