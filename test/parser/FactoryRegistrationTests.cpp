//
// @Factory discovery + descriptor (AspectModel.md § @Factory, R1–R4).
//
// Parse-time layer only: a @Factory class's provider methods are
// recognized and described — provided type (= return type), @Inject
// param edges, assisted (unmarked) params, and method scope — without
// any graph resolution or codegen (those are Units 3/4). Resolution
// keys on the method *signature*, never the body (R1).
//
// A @Factory class is ALSO registered as a plain @Component so its
// @Inject collaborators resolve and it is itself an injectable
// singleton (R3's assisted case injects the factory).
//
// Tests use compile-for-inspection (parse + prototype, no codegen) so
// the registries can be read directly.
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

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_factory_" + std::to_string(rng()));
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
                 / ("cajeta_factory_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

CajetaModule::FactoryDescriptorPtr findFactory(const std::string& shortName) {
    for (auto& f : CajetaModule::getFactoryClasses()) {
        if (f && f->klass && f->klass->getQName()
                && f->klass->getQName()->getTypeName() == shortName) {
            return f;
        }
    }
    return nullptr;
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

// One provider on a factory descriptor, by provided short type name.
const CajetaModule::FactoryProvider* providerFor(
        const CajetaModule::FactoryDescriptorPtr& f,
        const std::string& providedShort) {
    if (!f) return nullptr;
    for (auto& p : f->providers) {
        if (p.providedType && p.providedType->getQName()
                && p.providedType->getQName()->getTypeName() == providedShort) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

// A factory whose provider method's only param is @Inject is a
// *provider* (all-injected): the descriptor records one provider with
// the right provided type, one injected param edge, no assisted params,
// and the default Singleton scope.
TEST(FactoryRegistrationTests, allInjectedProvider) {
    auto src =
        "package test;\n"
        "public class Pool { public Pool() { return; } }\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.ConnFactory");
    auto f = findFactory("ConnFactory");
    ASSERT_NE(f, nullptr);
    auto* prov = providerFor(f, "Connection");
    ASSERT_NE(prov, nullptr);
    EXPECT_FALSE(prov->hasAssisted);
    ASSERT_EQ(prov->params.size(), 1u);
    EXPECT_TRUE(prov->params[0].injected);
    EXPECT_EQ(prov->scope, CajetaModule::AllocateMode::Singleton);
}

// An unmarked param is assisted (caller-supplied). The provider keeps
// the @Inject param as an edge and flips hasAssisted, so the consumer
// will inject the *factory* and call it (R3).
TEST(FactoryRegistrationTests, assistedParamFlagsFactoryInjection) {
    auto src =
        "package test;\n"
        "public class Pool { public Pool() { return; } }\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make(@Inject Pool pool, int32 tenantId) {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.ConnFactory");
    auto f = findFactory("ConnFactory");
    ASSERT_NE(f, nullptr);
    auto* prov = providerFor(f, "Connection");
    ASSERT_NE(prov, nullptr);
    EXPECT_TRUE(prov->hasAssisted);
    ASSERT_EQ(prov->params.size(), 2u);
    EXPECT_TRUE(prov->params[0].injected);   // @Inject Pool
    EXPECT_FALSE(prov->params[1].injected);  // assisted int32
}

// @Transient on the provider method sets the method scope (R4); the
// default (no scope annotation) stays Singleton — covered above.
TEST(FactoryRegistrationTests, transientScopeOnMethod) {
    auto src =
        "package test;\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    @Transient Connection make() {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.ConnFactory");
    auto f = findFactory("ConnFactory");
    ASSERT_NE(f, nullptr);
    auto* prov = providerFor(f, "Connection");
    ASSERT_NE(prov, nullptr);
    EXPECT_EQ(prov->scope, CajetaModule::AllocateMode::Transient);
    EXPECT_FALSE(prov->hasAssisted);
    EXPECT_TRUE(prov->params.empty());
}

// The @Factory class is also registered as an injectable @Component
// (so its @Inject collaborators resolve and consumers can inject the
// factory itself for the assisted case). The descriptor back-points to
// that component.
TEST(FactoryRegistrationTests, factoryIsAlsoComponent) {
    auto src =
        "package test;\n"
        "public class Connection { public Connection() { return; } }\n"
        "@Factory public class ConnFactory {\n"
        "    Connection make() {\n"
        "        Connection c = heap Connection();\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.ConnFactory");
    auto f = findFactory("ConnFactory");
    ASSERT_NE(f, nullptr);
    auto comp = findComponent("ConnFactory");
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(f->selfComponent, comp);
}

// A plain class with no @Factory annotation registers no factory.
// Catches an over-broad classifier.
TEST(FactoryRegistrationTests, plainClassNotFactory) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    int32 make() { return 1; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Plain");
    EXPECT_EQ(findFactory("Plain"), nullptr);
}
