//
// A8: @Component / @Repository / @TestComponent registration and
// DI graph validation.
//
// Two layers are exercised here:
//
//   1. Registration (parse-time). A class annotated with one of the
//      three DI annotations is added to the process-global component
//      registry (CajetaModule::getComponentClasses) with its name
//      qualifier, profile list, and test-component flag captured.
//
//   2. Validation (post-parse). resolveDependencyGraph walks the
//      registry under the active profile, applies @TestComponent
//      overrides, builds an adjacency list from each component's
//      @Inject fields, and throws on:
//         - missing implementation → CAJETA_ERROR_MISSING_COMPONENT
//         - circular dependency    → CAJETA_ERROR_DI_CYCLE
//         - ambiguous resolution   → CAJETA_ERROR_DI_AMBIGUOUS
//
// Tests use compile-for-inspection (no codegen, no JIT) so the
// resolver is invoked explicitly and exceptions can be caught
// directly. The dependency-graph state itself is not consumed by
// any caller yet — A9 will read it for get_X / make_X synthesis.
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

#include <algorithm>
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
              / ("cajeta_a8_" + std::to_string(rng()));
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
                 / ("cajeta_a8_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// Find the descriptor for a class by short type name in the
// process-global component registry. Returns nullptr if absent.
CajetaModule::ComponentDescriptorPtr findDescriptor(const std::string& shortName) {
    for (auto& d : CajetaModule::getComponentClasses()) {
        if (d && d->klass && d->klass->getQName()
                && d->klass->getQName()->getTypeName() == shortName) {
            return d;
        }
    }
    return nullptr;
}

} // namespace

// Bare @Component class registers with no name qualifier, no
// profile list, and isTestComponent=false. The defaults span every
// optional field on the descriptor.
TEST(ComponentRegistrationTests, bareComponentRegisters) {
    auto src =
        "package test;\n"
        "@Component public class Database {\n"
        "    public Database() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Database");
    auto desc = findDescriptor("Database");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->name, "");
    EXPECT_TRUE(desc->profiles.empty());
    EXPECT_FALSE(desc->isTestComponent);
}

// @Repository registers as an ordinary component — the sibling
// annotation has identical DI semantics in v1.
TEST(ComponentRegistrationTests, repositoryRegistersAsComponent) {
    auto src =
        "package test;\n"
        "@Repository public class UserRepo {\n"
        "    public UserRepo() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.UserRepo");
    auto desc = findDescriptor("UserRepo");
    ASSERT_NE(desc, nullptr);
    EXPECT_FALSE(desc->isTestComponent);
}

// @TestComponent registers with isTestComponent=true so the
// resolver can drop it outside test compilations and prefer it
// inside them. Captures the same metadata as @Component otherwise.
TEST(ComponentRegistrationTests, testComponentSetsFlag) {
    auto src =
        "package test;\n"
        "@TestComponent public class StubDb {\n"
        "    public StubDb() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.StubDb");
    auto desc = findDescriptor("StubDb");
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(desc->isTestComponent);
}

// @Component(name = "...") captures the qualifier. The DI resolver
// reads desc->name during disambiguation when multiple components
// share the same type.
TEST(ComponentRegistrationTests, componentNameQualifierCaptured) {
    auto src =
        "package test;\n"
        "@Component(name = \"disk\") public class DiskPersister {\n"
        "    public DiskPersister() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.DiskPersister");
    auto desc = findDescriptor("DiskPersister");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->name, "disk");
}

// @Profile annotations are collected into the descriptor's profile
// list. The resolver uses this list to filter under the active
// profile setting.
TEST(ComponentRegistrationTests, profileAnnotationsCaptured) {
    auto src =
        "package test;\n"
        "@Component @Profile(\"prod\") public class PostgresDb {\n"
        "    public PostgresDb() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.PostgresDb");
    auto desc = findDescriptor("PostgresDb");
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(desc->profiles.size(), 1u);
    EXPECT_EQ(desc->profiles[0], "prod");
}

// A class with no DI annotation isn't a component. The registry
// stays untouched. Catches an over-broad classifier in the visitor.
TEST(ComponentRegistrationTests, plainClassNotRegistered) {
    auto src =
        "package test;\n"
        "public class PlainOldClass {\n"
        "    public PlainOldClass() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.PlainOldClass");
    EXPECT_EQ(findDescriptor("PlainOldClass"), nullptr);
}

// resolveDependencyGraph passes for a self-contained component with
// no injected dependencies. The empty-graph case must not throw.
TEST(ComponentRegistrationTests, resolveAcceptsLeafComponent) {
    auto src =
        "package test;\n"
        "@Component public class Database {\n"
        "    public Database() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Database");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
}

// A satisfied @Inject (target type has a matching @Component)
// resolves cleanly. No throw means the edge was built and the graph
// is acyclic.
TEST(ComponentRegistrationTests, resolveAcceptsSatisfiedInject) {
    auto src =
        "package test;\n"
        "@Component public class Database {\n"
        "    public Database() { return; }\n"
        "}\n"
        "@Component public class UserService {\n"
        "    @Inject Database db;\n"
        "    public UserService() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.UserService");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
}

// Missing implementation: a class @Injects a type with no
// corresponding @Component. The error code is
// CAJETA_ERROR_MISSING_COMPONENT.
TEST(ComponentRegistrationTests, missingImplementationRejected) {
    auto src =
        "package test;\n"
        "public class Database {\n"   // not a @Component
        "    public Database() { return; }\n"
        "}\n"
        "@Component public class UserService {\n"
        "    @Inject Database db;\n"
        "    public UserService() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.UserService");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
}

// Cycle detection is implemented (DFS over the resolved edges,
// throwing CAJETA_ERROR_DI_CYCLE on a back-edge), but exercising
// it via parsed sources is blocked on two prerequisite issues:
//
//   - A two-class A↔B cycle requires forward-referencing a class
//     name in the first class's @Inject field type — currently
//     rejected by #208's visitFieldDeclaration unknown-type guard.
//   - A self-cycle (class Node { @Inject Node next; }) crashes
//     parse-time layout (CajetaClass::generatePrototype on a
//     self-referential field type).
//
// Both deserve their own fixes. The cycle-test belongs with the
// fix that lifts one of those restrictions; until then, the
// resolver's cycle path is verified by code-inspection only.
//
// (Tracking note: revisit in A12 alongside inheritance walks.)

// Ambiguous resolution via a name qualifier that matches nothing:
// a sole @Component(name="real") candidate exists, but the
// consumer's @Inject(name = "missing") asks for a different name.
// The resolver finds candidates but can't pick one matching the
// requested name → ambiguous error.
TEST(ComponentRegistrationTests, ambiguousNamedInjectRejected) {
    auto src =
        "package test;\n"
        "@Component(name = \"real\") public class Item {\n"
        "    public Item() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(name = \"missing\") Item it;\n"
        "    public Service() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Service");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_DI_AMBIGUOUS";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_DI_AMBIGUOUS");
    }
}

// @Profile filtering removes a component from the active set.
// With activeProfile = "prod" (the default), a component scoped to
// "staging" is excluded, and an @Inject for that type fires the
// missing-impl error.
TEST(ComponentRegistrationTests, profileFilteredOutTriggersMissing) {
    auto src =
        "package test;\n"
        "@Component @Profile(\"staging\") public class Database {\n"
        "    public Database() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject Database db;\n"
        "    public Service() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Service");
    CajetaModule::setActiveProfile("prod");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
    // Reset profile so subsequent tests see the default.
    CajetaModule::setActiveProfile("prod");
}

// @TestComponent participates in the DI graph when activeProfile is
// "test". Without test mode, the same @TestComponent is filtered
// out — verified by the missing-impl error in the next test.
TEST(ComponentRegistrationTests, testComponentIncludedInTestMode) {
    auto src =
        "package test;\n"
        "@TestComponent public class StubDb {\n"
        "    public StubDb() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject StubDb db;\n"
        "    public Service() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Service");
    CajetaModule::setActiveProfile("test");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    CajetaModule::setActiveProfile("prod");
}

// Same source as above, but the active profile is "prod". The
// @TestComponent is filtered out, leaving the @Inject unsatisfied.
TEST(ComponentRegistrationTests, testComponentExcludedInProdMode) {
    auto src =
        "package test;\n"
        "@TestComponent public class StubDb {\n"
        "    public StubDb() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject StubDb db;\n"
        "    public Service() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.Service");
    CajetaModule::setActiveProfile("prod");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
}

// --- Unit 1 (di-profile-selection §2): @TestComponent masks a same-
// --- interface @Component under --profile=test, by shared interface. ---

namespace {
// Short type name of the resolved target for a consumer's first
// @Inject field ("" if unresolved). Used to assert which provider won.
std::string resolvedTargetName(const std::string& consumerShort) {
    auto d = findDescriptor(consumerShort);
    if (!d || d->resolvedFields.empty()) return "";
    auto& rd = d->resolvedFields[0];
    if (!rd.target || !rd.target->klass || !rd.target->klass->getQName()) return "";
    return rd.target->klass->getQName()->getTypeName();
}
} // namespace

// In test mode a @TestComponent implementing an interface masks the
// non-test @Component implementing that same interface: @Inject Sink
// resolves to the double, not ambiguous.
TEST(ComponentRegistrationTests, testComponentMasksSameInterfaceComponent) {
    auto src =
        "package test;\n"
        "public interface Sink {\n"
        "    public int32 tag();\n"
        "}\n"
        "@Component public class RealSink implements Sink {\n"
        "    public RealSink() { return; }\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "@TestComponent public class FakeSink implements Sink {\n"
        "    public FakeSink() { return; }\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Sink s;\n"
        "    public App() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.App");
    CajetaModule::setActiveProfile("test");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    EXPECT_EQ(resolvedTargetName("App"), "FakeSink");
    CajetaModule::setActiveProfile("prod");
}

// Same source, prod profile: the @TestComponent is filtered out and
// the real interface impl is wired.
TEST(ComponentRegistrationTests, prodKeepsRealInterfaceComponent) {
    auto src =
        "package test;\n"
        "public interface Sink {\n"
        "    public int32 tag();\n"
        "}\n"
        "@Component public class RealSink implements Sink {\n"
        "    public RealSink() { return; }\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "@TestComponent public class FakeSink implements Sink {\n"
        "    public FakeSink() { return; }\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Sink s;\n"
        "    public App() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.App");
    CajetaModule::setActiveProfile("prod");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    EXPECT_EQ(resolvedTargetName("App"), "RealSink");
}

// Multiple real impls of one interface plus a @TestComponent: in test
// mode all reals are masked and the double is the sole provider (no
// DI_AMBIGUOUS despite three would-be candidates).
TEST(ComponentRegistrationTests, multipleRealImplsAllMaskedInTest) {
    auto src =
        "package test;\n"
        "public interface Sink {\n"
        "    public int32 tag();\n"
        "}\n"
        "@Component public class DiskSink implements Sink {\n"
        "    public DiskSink() { return; }\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "@Component public class NetSink implements Sink {\n"
        "    public NetSink() { return; }\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "@TestComponent public class FakeSink implements Sink {\n"
        "    public FakeSink() { return; }\n"
        "    public int32 tag() { return 3; }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Sink s;\n"
        "    public App() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.App");
    CajetaModule::setActiveProfile("test");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    EXPECT_EQ(resolvedTargetName("App"), "FakeSink");
    CajetaModule::setActiveProfile("prod");
}

// A @TestComponent that implements no interface masks nothing: an
// unrelated real interface impl still resolves in test mode. Guards
// against over-masking.
TEST(ComponentRegistrationTests, noInterfaceDoubleMasksNothing) {
    auto src =
        "package test;\n"
        "public interface Sink {\n"
        "    public int32 tag();\n"
        "}\n"
        "@Component public class RealSink implements Sink {\n"
        "    public RealSink() { return; }\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "@TestComponent public class StubOnly {\n"
        "    public StubOnly() { return; }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Sink s;\n"
        "    public App() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.App");
    CajetaModule::setActiveProfile("test");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    EXPECT_EQ(resolvedTargetName("App"), "RealSink");
    CajetaModule::setActiveProfile("prod");
}
