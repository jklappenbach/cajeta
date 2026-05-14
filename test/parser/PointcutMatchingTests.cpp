//
// A3: pointcut matching pass.
//
// CajetaModule::resolveAdviceMatches walks every registered @Aspect
// class's advice methods, resolves each pointcut's `.class` argument,
// classifies the pointcut as marker-annotation or type-based, then
// pushes an AdviceMatch onto every user method that matches. A4+
// consumes the per-method match list at codegen.
//
// Tests verify the cached matches via Method::getMatchingAdvice on
// the AST side — no JIT needed, no advice IR yet (that's A4).
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;
using cajeta::MethodPtr;
using cajeta::AdviceKind;
using cajeta::PointcutShape;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_a3_" + std::to_string(rng()));
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
                 / ("cajeta_a3_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    CajetaModule::resolveAdviceMatches();
    return m;
}

// Find a method named `methodName` on the class at the given
// canonical name in the module. Returns nullptr if not found.
MethodPtr findMethod(CajetaModulePtr module,
                     const std::string& classCanonical,
                     const std::string& methodName) {
    auto klass = module->getStructures()[classCanonical];
    if (!klass) return nullptr;
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == methodName) return m;
    }
    return nullptr;
}

} // namespace

// Marker-annotation pointcut: `@Before(Audited.class)` advises every
// method annotated `@Audited`. Audited is declared via @interface,
// which the visitor doesn't register as a class — the resolver
// falls through to MarkerAnnotation mode. Only the annotated method
// gets a match.
TEST(PointcutMatchingTests, markerAnnotationMatchesOnlyAnnotatedMethods) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AuditAspect {\n"
        "    @Before(Audited.class)\n"
        "    public static int32 onCall() { return 0; }\n"
        "}\n"
        "public class Target {\n"
        "    @Audited public static int32 watched() { return 1; }\n"
        "    public static int32 ignored() { return 2; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Target");
    auto watched = findMethod(module, "test.Target", "watched");
    ASSERT_NE(watched, nullptr);
    auto& watchedMatches = watched->getMatchingAdvice();
    ASSERT_EQ(watchedMatches.size(), 1u);
    EXPECT_EQ(watchedMatches[0].kind, AdviceKind::Before);
    EXPECT_EQ(watchedMatches[0].shape, PointcutShape::MarkerAnnotation);
    EXPECT_EQ(watchedMatches[0].adviceMethod->getName(), "onCall");
    EXPECT_EQ(watchedMatches[0].aspectClass->getQName()->getTypeName(),
              "AuditAspect");

    auto ignored = findMethod(module, "test.Target", "ignored");
    ASSERT_NE(ignored, nullptr);
    EXPECT_TRUE(ignored->getMatchingAdvice().empty());
}

// Type-based pointcut: `@Before(PaymentProcessor.class)` where
// PaymentProcessor is a regular class. Every method on that class
// gets a match; methods on other classes don't.
TEST(PointcutMatchingTests, typeBasedMatchesEveryMethodOnTargetClass) {
    auto src =
        "package test;\n"
        "public class PaymentProcessor {\n"
        "    public static int32 charge() { return 0; }\n"
        "    public static int32 refund() { return 0; }\n"
        "}\n"
        "public class Other {\n"
        "    public static int32 unrelated() { return 0; }\n"
        "}\n"
        "@Aspect public class TraceAspect {\n"
        "    @Before(PaymentProcessor.class)\n"
        "    public static int32 trace() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.PaymentProcessor");
    auto charge = findMethod(module, "test.PaymentProcessor", "charge");
    auto refund = findMethod(module, "test.PaymentProcessor", "refund");
    auto unrelated = findMethod(module, "test.Other", "unrelated");
    ASSERT_NE(charge, nullptr);
    ASSERT_NE(refund, nullptr);
    ASSERT_NE(unrelated, nullptr);

    ASSERT_EQ(charge->getMatchingAdvice().size(), 1u);
    EXPECT_EQ(charge->getMatchingAdvice()[0].shape, PointcutShape::Type);
    EXPECT_EQ(charge->getMatchingAdvice()[0].kind, AdviceKind::Before);

    ASSERT_EQ(refund->getMatchingAdvice().size(), 1u);
    EXPECT_EQ(refund->getMatchingAdvice()[0].shape, PointcutShape::Type);

    EXPECT_TRUE(unrelated->getMatchingAdvice().empty());
}

// Multiple aspects on one method. Each advice fires an independent
// match — A4+ codegen will compose them by order. Here we verify
// the cache simply collects all matches; @Order resolution is A7.
TEST(PointcutMatchingTests, multipleAspectsAdviseOneMethod) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AspectA {\n"
        "    @Before(Audited.class)\n"
        "    public static int32 aBefore() { return 0; }\n"
        "}\n"
        "@Aspect public class AspectB {\n"
        "    @After(Audited.class)\n"
        "    public static int32 bAfter() { return 0; }\n"
        "}\n"
        "public class Target {\n"
        "    @Audited public static int32 hot() { return 1; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Target");
    auto hot = findMethod(module, "test.Target", "hot");
    ASSERT_NE(hot, nullptr);
    auto& matches = hot->getMatchingAdvice();
    ASSERT_EQ(matches.size(), 2u);
    // Order is by aspect declaration order, then advice-method
    // declaration order — A7's @Order layer will permute later.
    bool sawA = false, sawB = false;
    for (auto& m : matches) {
        if (m.adviceMethod->getName() == "aBefore"
                && m.kind == AdviceKind::Before) sawA = true;
        if (m.adviceMethod->getName() == "bAfter"
                && m.kind == AdviceKind::After) sawB = true;
    }
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
}

// Every advice form classifies correctly. One aspect with all five
// advice annotations pointing at the same marker. Each user method
// match records the right AdviceKind.
TEST(PointcutMatchingTests, allFiveAdviceKindsClassify) {
    auto src =
        "package test;\n"
        "@interface Traced { }\n"
        "@Aspect public class FullAspect {\n"
        "    @Before(Traced.class)         public static int32 b() { return 0; }\n"
        "    @After(Traced.class)          public static int32 a() { return 0; }\n"
        "    @Around(Traced.class)         public static int32 ar() { return 0; }\n"
        "    @AfterReturning(Traced.class) public static int32 ret() { return 0; }\n"
        "    @AfterThrowing(Traced.class)  public static int32 thr() { return 0; }\n"
        "}\n"
        "public class Target {\n"
        "    @Traced public static int32 hot() { return 1; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Target");
    auto hot = findMethod(module, "test.Target", "hot");
    ASSERT_NE(hot, nullptr);
    auto& matches = hot->getMatchingAdvice();
    ASSERT_EQ(matches.size(), 5u);
    // Collect kinds and confirm all five present.
    bool seen[5] = { false, false, false, false, false };
    for (auto& m : matches) {
        switch (m.kind) {
            case AdviceKind::Before:         seen[0] = true; break;
            case AdviceKind::After:          seen[1] = true; break;
            case AdviceKind::Around:         seen[2] = true; break;
            case AdviceKind::AfterReturning: seen[3] = true; break;
            case AdviceKind::AfterThrowing:  seen[4] = true; break;
        }
    }
    for (bool s : seen) EXPECT_TRUE(s);
}

// Aspect methods don't get advised. The matching pass skips the
// aspect's own class when walking candidates — `aBefore` itself
// shouldn't accumulate matches even if it happens to satisfy a
// pointcut.
TEST(PointcutMatchingTests, aspectMethodsAreSkipped) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class SelfAdvising {\n"
        "    @Before(Audited.class)\n"
        "    @Audited\n"                       // self-annotated, but still skipped
        "    public static int32 trace() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.SelfAdvising");
    auto trace = findMethod(module, "test.SelfAdvising", "trace");
    ASSERT_NE(trace, nullptr);
    EXPECT_TRUE(trace->getMatchingAdvice().empty());
}
