//
// @Factory graph integration (AspectModel.md § @Factory, R1–R4 / Unit 3).
//
// resolveDependencyGraph consults @Factory providers alongside
// @Component ctors:
//   - all-injected provider  => the PRODUCT type is injectable; @Inject T
//                               resolves to the provider.
//   - assisted provider      => the product is NOT injectable; the consumer
//                               injects the @Factory type itself (R3).
//   - both a @Component ctor and a factory provide one type => ambiguity.
//   - missing @Inject-param target / factory-param cycles => errors.
//
// compile-for-inspection (parse + prototype), then resolveDependencyGraph
// is invoked explicitly so exceptions / resolved state are observable.
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta::CajetaModulePtr;
using cajeta::Exception;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_fgraph_" + std::to_string(rng()));
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
                 / ("cajeta_fgraph_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

CajetaModule::ComponentDescriptorPtr findComponent(const std::string& shortName) {
    for (auto& d : CajetaModule::getComponentClasses()) {
        if (d && d->klass && d->klass->getQName()
                && d->klass->getQName()->getTypeName() == shortName) {
            return d;
        }
    }
    return nullptr;
}

std::string klassShort(const cajeta::CajetaClassPtr& k) {
    return (k && k->getQName()) ? k->getQName()->getTypeName() : std::string();
}

} // namespace

// 3.1.1 — an all-injected provider makes its product injectable. The
// consumer's @Inject Connection resolves to the ConnFactory provider
// (not a missing-impl error), and its provider @Inject Pool resolves to
// the Pool @Component.
TEST(FactoryGraphTests, allInjectedProviderResolvesProduct) {
    auto src =
        "package test;\n"
        "@Component public class Pool { public Pool() { return; } }\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class Consumer {\n"
        "    @Inject Connection conn;\n"
        "    public Consumer() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Consumer");
    ASSERT_NO_THROW(CajetaModule::resolveDependencyGraph());
    auto consumer = findComponent("Consumer");
    ASSERT_NE(consumer, nullptr);
    ASSERT_EQ(consumer->resolvedFields.size(), 1u);
    auto& rd = consumer->resolvedFields[0];
    EXPECT_EQ(rd.target, nullptr);              // not a ctor target
    ASSERT_NE(rd.factory, nullptr);             // resolved via a factory provider
    EXPECT_EQ(klassShort(rd.factory->klass), "ConnFactory");
}

// 3.1.2 — an assisted provider does NOT make the product injectable;
// the consumer injects the @Factory type itself (resolved as the
// factory's component), and calls it with assisted args (R3).
TEST(FactoryGraphTests, assistedFactoryInjectsFactoryType) {
    auto src =
        "package test;\n"
        "@Component public class Pool { public Pool() { return; } }\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool, int32 tenant) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class Consumer {\n"
        "    @Inject ConnFactory factory;\n"
        "    public Consumer() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Consumer");
    ASSERT_NO_THROW(CajetaModule::resolveDependencyGraph());
    auto consumer = findComponent("Consumer");
    ASSERT_NE(consumer, nullptr);
    ASSERT_EQ(consumer->resolvedFields.size(), 1u);
    auto& rd = consumer->resolvedFields[0];
    EXPECT_EQ(rd.factory, nullptr);             // injected the factory itself, as a component
    ASSERT_NE(rd.target, nullptr);
    EXPECT_EQ(klassShort(rd.target->klass), "ConnFactory");
}

// 3.1.2b — the product of an assisted-only factory is not injectable;
// a consumer that @Injects the product gets a missing-impl error.
TEST(FactoryGraphTests, assistedProductNotInjectable) {
    auto src =
        "package test;\n"
        "@Component public class Pool { public Pool() { return; } }\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool, int32 tenant) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class Consumer {\n"
        "    @Inject Connection conn;\n"
        "    public Consumer() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Consumer");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
}

// 3.1.3 — a type provided by BOTH a @Component ctor and an all-injected
// @Factory provider is ambiguous.
TEST(FactoryGraphTests, ambiguousComponentAndFactory) {
    auto src =
        "package test;\n"
        "@Component public class Widget { public Widget() { return; } }\n"
        "@Factory public class WidgetFactory {\n"
        "    Widget make() {\n"
        "        Widget w = heap Widget();\n"
        "        return w;\n"
        "    }\n"
        "}\n"
        "@Component public class Consumer {\n"
        "    @Inject Widget w;\n"
        "    public Consumer() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Consumer");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_DI_AMBIGUOUS";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_DI_AMBIGUOUS");
    }
}

// 3.1.3 — an all-injected provider whose @Inject param has no provider
// fires a missing-impl error (the factory's own dependency is unmet).
TEST(FactoryGraphTests, missingFactoryParamRejected) {
    auto src =
        "package test;\n"
        "public class Pool { public Pool() { return; } }\n"   // NOT a @Component
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n"
        "@Component public class Consumer {\n"
        "    @Inject Connection conn;\n"
        "    public Consumer() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Consumer");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
}

// 3.1.3 — a factory provider whose @Inject param is its own product is
// a self-cycle (the product depends on itself). The DFS catches it.
TEST(FactoryGraphTests, factoryParamCycleDetected) {
    auto src =
        "package test;\n"
        "public class A { public A() { return; } }\n"
        "@Factory public class FA {\n"
        "    A make(@Inject A a) {\n"
        "        A x = heap A();\n"
        "        return x;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.FA");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_DI_CYCLE";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_DI_CYCLE");
    }
}
