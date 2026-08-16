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

// @Repository registers as an ordinary component — the sibling
// annotation has identical DI semantics in v1.

// @TestComponent registers with isTestComponent=true so the
// resolver can drop it outside test compilations and prefer it
// inside them. Captures the same metadata as @Component otherwise.

// @Component(name = "...") captures the qualifier. The DI resolver
// reads desc->name during disambiguation when multiple components
// share the same type.

// @Profile annotations are collected into the descriptor's profile
// list. The resolver uses this list to filter under the active
// profile setting.

// A class with no DI annotation isn't a component. The registry
// stays untouched. Catches an over-broad classifier in the visitor.

// resolveDependencyGraph passes for a self-contained component with
// no injected dependencies. The empty-graph case must not throw.

// A satisfied @Inject (target type has a matching @Component)
// resolves cleanly. No throw means the edge was built and the graph
// is acyclic.

// Missing implementation: a class @Injects a type with no
// corresponding @Component. The error code is
// CAJETA_ERROR_MISSING_COMPONENT.

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

// Same source as above, but the active profile is "prod". The
// @TestComponent is filtered out, leaving the @Inject unsatisfied.

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

// --- Unit 2 (di-profile-selection §3.3): @Profile multi-value (any-of). ---

namespace {
bool profilesContain(const CajetaModule::ComponentDescriptorPtr& d,
                     const std::string& p) {
    return d && std::find(d->profiles.begin(), d->profiles.end(), p)
                    != d->profiles.end();
}
} // namespace

// Array-literal form @Profile({"dev","test"}) captures every listed name
// (StringList under the implicit "value" key).
TEST(ComponentRegistrationTests, profileListCapturedFromArrayForm) {
    auto src =
        "package test;\n"
        "@Component @Profile({\"dev\", \"test\"}) public class MultiDb {\n"
        "    public MultiDb() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.MultiDb");
    auto desc = findDescriptor("MultiDb");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->profiles.size(), 2u);
    EXPECT_TRUE(profilesContain(desc, "dev"));
    EXPECT_TRUE(profilesContain(desc, "test"));
}

// Repeated @Profile annotations still accumulate (the pre-existing
// single-string path must keep working).
TEST(ComponentRegistrationTests, repeatedProfileStillCaptured) {
    auto src =
        "package test;\n"
        "@Component @Profile(\"dev\") @Profile(\"test\") public class RepeatDb {\n"
        "    public RepeatDb() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.RepeatDb");
    auto desc = findDescriptor("RepeatDb");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->profiles.size(), 2u);
    EXPECT_TRUE(profilesContain(desc, "dev"));
    EXPECT_TRUE(profilesContain(desc, "test"));
}

// Any-of filtering: a component @Profile({"dev","test"}) is included
// under either "dev" or "test", and excluded under "prod" (the @Inject
// then goes unsatisfied).
TEST(ComponentRegistrationTests, profileListFiltersUnderEither) {
    auto src =
        "package test;\n"
        "public interface Store {\n"
        "    public int32 kind();\n"
        "}\n"
        "@Component @Profile({\"dev\", \"test\"}) public class MemStore implements Store {\n"
        "    public MemStore() { return; }\n"
        "    public int32 kind() { return 1; }\n"
        "}\n"
        "@Component public class App {\n"
        "    @Inject Store s;\n"
        "    public App() { return; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.App");
    CajetaModule::setActiveProfile("dev");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    CajetaModule::setActiveProfile("test");
    EXPECT_NO_THROW(CajetaModule::resolveDependencyGraph());
    CajetaModule::setActiveProfile("prod");
    try {
        CajetaModule::resolveDependencyGraph();
        FAIL() << "expected CAJETA_ERROR_MISSING_COMPONENT under prod";
    } catch (Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MISSING_COMPONENT");
    }
    CajetaModule::setActiveProfile("prod");
}

// --- Unit 3 (di-profile-selection §3): shipped cajeta.aot annotation types. ---

// Using the annotations with the shipped declarations imported compiles
// and captures their profile / test-component data. (Imports are lenient —
// an unresolved import is ignored — so this guards that the shipped types
// coexist with the by-short-name recognition, not import resolution.)

// Bare short-name usage (no import) still compiles and captures — shipping
// the declarations must not break the recognize-by-short-name path.
