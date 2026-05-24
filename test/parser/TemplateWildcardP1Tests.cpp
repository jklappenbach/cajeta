// Step 1 of template wildcards (`<?>`). See cajeta-docs/TemplateWildcard.md
// and todo.md for the full staging plan. This test fixture exercises only
// the foundation: wildcard-sentinel registration, the gated parser-site
// behavior, the wildcard short-circuit in CajetaClass::instantiate, and
// the assignability helper. No wildcard-typed locals/fields/returns yet
// — Step 2 (drop-chain ABI) is required before wildcard values can flow
// through real codegen.

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::CajetaClass;
using cajeta::CajetaClassPtr;
using cajeta::CajetaModulePtr;
using cajeta::CajetaType;
using cajeta::CajetaTypePtr;
using cajeta::Compiler;

namespace {

CajetaModulePtr compileSource(Compiler& compiler,
                              const std::string& source,
                              const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_wildp1_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_wildp1_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(),
                                   base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

// RAII wildcard-flag enabler. Tests that need the gated parser path on
// instantiate it; the destructor restores env-var-only behavior.
struct WildcardsOn {
    WildcardsOn() { CajetaType::setWildcardsEnabledForTest(true); }
    ~WildcardsOn() { CajetaType::clearWildcardsTestOverride(); }
};

} // namespace

// The wildcard sentinel is registered during CajetaType::init regardless
// of the feature flag — only the parser-site gate consults the flag.
TEST(TemplateWildcardP1Tests, wildcardSentinelRegisteredAfterInit) {
    Compiler compiler;
    (void)compiler;  // ctor runs CajetaType::init.
    auto wild = CajetaType::wildcardSentinel();
    ASSERT_NE(wild, nullptr);
    EXPECT_TRUE(wild->isWildcard());
    EXPECT_EQ(wild->getQName()->toCanonical(), "?");
}

// Sanity: non-wildcard types report false from isWildcard.
TEST(TemplateWildcardP1Tests, nonWildcardTypesReportFalse) {
    Compiler compiler;
    (void)compiler;
    auto int32Ty = CajetaType::of(std::string("int32"));
    ASSERT_NE(int32Ty, nullptr);
    EXPECT_FALSE(int32Ty->isWildcard());
}

// Instantiating a template with the wildcard sentinel yields an erased
// proxy: same templateOrigin as the source template, typeArguments
// populated with the wildcard, isWildcardInstantiation() true.
TEST(TemplateWildcardP1Tests, instantiateWithWildcardReturnsErasedProxy) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Box");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    ASSERT_TRUE(box->isTemplate());

    auto wild = CajetaType::wildcardSentinel();
    ASSERT_NE(wild, nullptr);
    auto proxy = box->instantiate({wild});
    ASSERT_NE(proxy, nullptr);
    EXPECT_TRUE(proxy->isWildcardInstantiation());
    EXPECT_EQ(proxy->getTemplateOrigin().get(), box.get());
    // Canonical includes the `?` suffix and lives under module structures.
    EXPECT_NE(proxy->getQName()->toCanonical().find("?"), std::string::npos);
}

// Wildcard instantiation is cached per (template, args) like concrete
// instantiations — a second call returns the same proxy pointer.
TEST(TemplateWildcardP1Tests, wildcardProxyIsCachedAcrossInstantiateCalls) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Holder<T> {\n"
        "    T value;\n"
        "    public Holder(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Holder");
    auto holder = module->getStructures()["test.Holder"];
    ASSERT_NE(holder, nullptr);

    auto wild = CajetaType::wildcardSentinel();
    auto first = holder->instantiate({wild});
    auto second = holder->instantiate({wild});
    EXPECT_EQ(first.get(), second.get());
}

// Step 1 assignability: a concrete instantiation `Box<int32>` is
// assignable to the same template's wildcard instantiation `Box<?>`.
TEST(TemplateWildcardP1Tests, sameTemplateConcreteAssignableToWildcard) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Box");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);

    auto int32Ty = CajetaType::of(std::string("int32"));
    ASSERT_NE(int32Ty, nullptr);
    auto boxOfInt = box->instantiate({int32Ty});
    ASSERT_NE(boxOfInt, nullptr);

    auto wild = CajetaType::wildcardSentinel();
    auto boxWild = box->instantiate({wild});
    ASSERT_NE(boxWild, nullptr);

    EXPECT_TRUE(CajetaClass::isAssignableToWildcard(boxOfInt, boxWild));
}

// Step 1 assignability rejects different templates: `Holder<int32>` is
// NOT assignable to `Box<?>` because their templateOrigin differs.
TEST(TemplateWildcardP1Tests, differentTemplateRejectedByAssignability) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n"
        "public class Holder<T> {\n"
        "    T value;\n"
        "    public Holder(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Box");
    auto box = module->getStructures()["test.Box"];
    auto holder = module->getStructures()["test.Holder"];
    ASSERT_NE(box, nullptr);
    ASSERT_NE(holder, nullptr);

    auto int32Ty = CajetaType::of(std::string("int32"));
    auto holderOfInt = holder->instantiate({int32Ty});
    auto wild = CajetaType::wildcardSentinel();
    auto boxWild = box->instantiate({wild});

    EXPECT_FALSE(CajetaClass::isAssignableToWildcard(holderOfInt, boxWild));
}

// Step 3 cache invariant 1 — wildcard instantiation does NOT collide
// with concrete instantiations of the same template. Box<int32> and
// Box<?> live under distinct canonical keys in module structures.
TEST(TemplateWildcardP1Tests, wildcardCacheBucketDistinctFromConcrete) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Box");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);

    auto int32Ty = CajetaType::of(std::string("int32"));
    auto boxOfInt = box->instantiate({int32Ty});
    auto boxWild = box->instantiate({CajetaType::wildcardSentinel()});

    ASSERT_NE(boxOfInt, nullptr);
    ASSERT_NE(boxWild, nullptr);
    EXPECT_NE(boxOfInt.get(), boxWild.get());
    EXPECT_NE(boxOfInt->getQName()->toCanonical(),
              boxWild->getQName()->toCanonical());
}

// Step 3 cache invariant 2 — partial-wildcard instantiations remain
// distinct from each other AND from the all-wildcard form. Pair<?,int32>,
// Pair<int32,?>, and Pair<?,?> each get their own cache bucket because
// the canonical key includes each arg position. (Variance — when these
// become assignable in either direction — lands with Step 6.)
TEST(TemplateWildcardP1Tests, partialWildcardCacheBucketsDistinct) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Pair<A, B> {\n"
        "    A first;\n"
        "    B second;\n"
        "    public Pair(A a, B b) { this.first = a; this.second = b; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Pair");
    auto pair = module->getStructures()["test.Pair"];
    ASSERT_NE(pair, nullptr);

    auto wild = CajetaType::wildcardSentinel();
    auto int32Ty = CajetaType::of(std::string("int32"));

    auto wildLeft  = pair->instantiate({wild,    int32Ty});
    auto wildRight = pair->instantiate({int32Ty, wild});
    auto bothWild  = pair->instantiate({wild,    wild});

    EXPECT_NE(wildLeft.get(),  wildRight.get());
    EXPECT_NE(wildLeft.get(),  bothWild.get());
    EXPECT_NE(wildRight.get(), bothWild.get());
    EXPECT_NE(wildLeft->getQName()->toCanonical(),
              wildRight->getQName()->toCanonical());
}

// Assignability also rejects a non-wildcard instantiation as the target
// — `Box<?>` is the *only* legal wildcard-typed destination.
TEST(TemplateWildcardP1Tests, nonWildcardTargetRejected) {
    WildcardsOn flag;
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Box");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);

    auto int32Ty = CajetaType::of(std::string("int32"));
    auto int64Ty = CajetaType::of(std::string("int64"));
    auto boxOfInt = box->instantiate({int32Ty});
    auto boxOfLong = box->instantiate({int64Ty});
    EXPECT_FALSE(CajetaClass::isAssignableToWildcard(boxOfInt, boxOfLong));
}
