// Scope names: `@Scope` publishes, `@Component(scope)` and `@Inject(scope)`
// consume, and the resolver checks every name (component-scopes plan 1.1).

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"
#include "../xpu/KernelLoweringProbe.h"

#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta::Exception;
using cajeta::xpu::probe::compileForInspection;

namespace {

struct Failure {
    std::string id;
    std::string message;
};

// Resolves the graph over what `compiler` parsed and returns the failure, or an
// empty id when it resolved.
Failure resolveFailure() {
    try {
        CajetaModule::resolveDependencyGraph();
    } catch (Exception& e) {
        return { e.getErrorId(), e.getMessage() };
    }
    return {};
}

Failure parseAndResolve(const std::string& src, const std::string& fq) {
    Compiler compiler;
    try {
        compileForInspection(compiler, src, fq, "scopename");
    } catch (Exception& e) {
        return { e.getErrorId(), e.getMessage() };
    }
    return resolveFailure();
}

const char* kPublisher =
    "package test;\n"
    "public class Pipe {\n"
    "    @Scope(\"Request\")\n"
    "    public void run() { return; }\n";

std::string withPublisher(const std::string& rest) {
    return std::string(kPublisher) + "}\n" + rest;
}

CajetaModule::ComponentDescriptorPtr descriptorOf(const std::string& shortName) {
    for (auto& d : CajetaModule::getComponentClasses()) {
        if (d && d->klass && d->klass->getQName()->getTypeName() == shortName) {
            return d;
        }
    }
    return nullptr;
}

} // namespace

// plan 1.1.1: an unknown name fails and lists what is published.
TEST(ScopeNameTests, unknownNameListsPublished) {
    auto f = parseAndResolve(withPublisher(
        "@Component(scope = \"Nope\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"), "test.Pipe");
    EXPECT_EQ(f.id, "CAJETA_ERROR_UNKNOWN_SCOPE");
    EXPECT_NE(f.message.find("\"Nope\""), std::string::npos) << f.message;
    EXPECT_NE(f.message.find("Request"), std::string::npos) << f.message;
    EXPECT_NE(f.message.find("Singleton"), std::string::npos) << f.message;
}

// plan 1.1.1: a published name resolves, and the descriptor records it.
TEST(ScopeNameTests, publishedNameResolves) {
    Compiler compiler;
    compileForInspection(compiler, withPublisher(
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"), "test.Pipe", "scopename");
    auto f = resolveFailure();
    EXPECT_EQ(f.id, "") << f.message;
    auto d = descriptorOf("Cart");
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->scope, nullptr);
    EXPECT_EQ(d->scope->name, "Request");
    EXPECT_EQ(d->scope->package, "test");
    EXPECT_EQ(d->scope->kind, CajetaModule::ScopePublication::Kind::Method);
}

// plan 1.1.2: a name two packages publish fails unqualified, naming both.
TEST(ScopeNameTests, clashNeedsQualification) {
    Compiler compiler;
    compileForInspection(compiler,
        "package alpha;\n"
        "public class P { @Scope(\"Request\") public void run() { return; } }\n",
        "alpha.P", "scopename");
    compileForInspection(compiler,
        "package beta;\n"
        "public class Q { @Scope(\"Request\") public void run() { return; } }\n",
        "beta.Q", "scopename");
    compileForInspection(compiler,
        "package gamma;\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n",
        "gamma.Cart", "scopename");
    auto f = resolveFailure();
    EXPECT_EQ(f.id, "CAJETA_ERROR_AMBIGUOUS_SCOPE");
    EXPECT_NE(f.message.find("alpha.Request"), std::string::npos) << f.message;
    EXPECT_NE(f.message.find("beta.Request"), std::string::npos) << f.message;
}

// plan 1.1.2: the qualified name picks one publisher.
TEST(ScopeNameTests, qualifiedNameResolves) {
    Compiler compiler;
    compileForInspection(compiler,
        "package alpha;\n"
        "public class P { @Scope(\"Request\") public void run() { return; } }\n",
        "alpha.P", "scopename");
    compileForInspection(compiler,
        "package beta;\n"
        "public class Q { @Scope(\"Request\") public void run() { return; } }\n",
        "beta.Q", "scopename");
    compileForInspection(compiler,
        "package gamma;\n"
        "@Component(scope = \"beta.Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n",
        "gamma.Cart", "scopename");
    auto f = resolveFailure();
    EXPECT_EQ(f.id, "") << f.message;
    auto d = descriptorOf("Cart");
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->scope, nullptr);
    EXPECT_EQ(d->scope->package, "beta");
}

// Several methods in one package may open the same scope.
TEST(ScopeNameTests, severalAnchorsOpenOneScope) {
    Compiler compiler;
    compileForInspection(compiler,
        "package test;\n"
        "public class P {\n"
        "    @Scope(\"Request\") public void a() { return; }\n"
        "    @Scope(\"Request\") public void b() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n", "test.P", "scopename");
    auto f = resolveFailure();
    EXPECT_EQ(f.id, "") << f.message;
}

// A name may not be a method scope and an instance scope at once.
TEST(ScopeNameTests, mixedKindsFail) {
    auto f = parseAndResolve(
        "package test;\n"
        "@Scope(\"Request\") public class P {\n"
        "    public P() { return; }\n"
        "    @Scope(\"Request\") public void a() { return; }\n"
        "}\n", "test.P");
    EXPECT_EQ(f.id, "CAJETA_ERROR_DUPLICATE_SCOPE");
    EXPECT_NE(f.message.find("test.Request"), std::string::npos) << f.message;
}

// plan 1.1.3: `allocate` is retired, and the message gives the new spelling.
TEST(ScopeNameTests, allocateIsRetired) {
    auto f = parseAndResolve(
        "package test;\n"
        "@Component public class Dep { public Dep() { return; } }\n"
        "@Component public class Service {\n"
        "    @Inject(allocate = ALLOCATE_TRANSIENT) Dep d;\n"
        "    public Service() { return; }\n"
        "}\n", "test.Service");
    EXPECT_EQ(f.id, "CAJETA_ERROR_ALLOCATE_RETIRED");
    EXPECT_NE(f.message.find("scope = \"Transient\""), std::string::npos) << f.message;
}

// plan 1.1.5: "Call" has no meaning on a field.
TEST(ScopeNameTests, callOnFieldFails) {
    auto f = parseAndResolve(
        "package test;\n"
        "@Component public class Dep { public Dep() { return; } }\n"
        "@Component public class Service {\n"
        "    @Inject(scope = \"Call\") Dep d;\n"
        "    public Service() { return; }\n"
        "}\n", "test.Service");
    EXPECT_EQ(f.id, "CAJETA_ERROR_CALL_SCOPE_FIELD");
}

// plan 1.1.6: a site may not override a scope the component declares.
TEST(ScopeNameTests, siteConflictsWithDeclaredScope) {
    auto f = parseAndResolve(withPublisher(
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject(scope = \"Transient\") Cart c;\n"
        "    public Checkout() { return; }\n"
        "}\n"), "test.Pipe");
    EXPECT_EQ(f.id, "CAJETA_ERROR_SCOPE_CONFLICT");
    EXPECT_NE(f.message.find("Cart"), std::string::npos) << f.message;
}

// plan 1.1.6: an explicit Singleton is declared too.
TEST(ScopeNameTests, siteConflictsWithDeclaredSingleton) {
    auto f = parseAndResolve(
        "package test;\n"
        "@Component(scope = \"Singleton\") public class Dep { public Dep() { return; } }\n"
        "@Component public class Service {\n"
        "    @Inject(scope = \"Owner\") Dep d;\n"
        "    public Service() { return; }\n"
        "}\n", "test.Service");
    EXPECT_EQ(f.id, "CAJETA_ERROR_SCOPE_CONFLICT");
}

// A site naming the component's own scope agrees with it.
TEST(ScopeNameTests, siteRepeatingTheScopeAgrees) {
    auto f = parseAndResolve(withPublisher(
        "@Component(scope = \"Request\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n"
        "@Component(scope = \"Request\") public class Checkout {\n"
        "    @Inject(scope = \"Request\") Cart c;\n"
        "    public Checkout() { return; }\n"
        "}\n"), "test.Pipe");
    EXPECT_EQ(f.id, "") << f.message;
}

// Owner and Call are site-relative, so a component cannot declare them.
TEST(ScopeNameTests, componentCannotDeclareOwner) {
    auto f = parseAndResolve(
        "package test;\n"
        "@Component(scope = \"Owner\") public class Dep { public Dep() { return; } }\n",
        "test.Dep");
    EXPECT_EQ(f.id, "CAJETA_ERROR_SCOPE_SITE_ONLY");
}

// plan 1.1.7: @Scope on a field is refused.
TEST(ScopeNameTests, scopeOnAFieldFails) {
    auto f = parseAndResolve(
        "package test;\n"
        "public class P {\n"
        "    @Scope(\"Request\") int32 x;\n"
        "    public P() { x = 0; return; }\n"
        "}\n", "test.P");
    EXPECT_EQ(f.id, "CAJETA_ERROR_SCOPE_PLACEMENT");
}

// @Scope on a constructor is refused: a constructor is not an activation a
// component outlives meaningfully, and the instance is not yet current.
TEST(ScopeNameTests, scopeOnAConstructorFails) {
    auto f = parseAndResolve(
        "package test;\n"
        "public class P {\n"
        "    @Scope(\"Request\") public P() { return; }\n"
        "}\n", "test.P");
    EXPECT_EQ(f.id, "CAJETA_ERROR_SCOPE_PLACEMENT");
}

// A class-level publication is an instance scope.
TEST(ScopeNameTests, classPublicationIsInstanceScope) {
    Compiler compiler;
    compileForInspection(compiler,
        "package test;\n"
        "@Scope(\"Session\") public class Session {\n"
        "    public Session() { return; }\n"
        "}\n"
        "@Component(scope = \"Session\") public class Cart {\n"
        "    public Cart() { return; }\n"
        "}\n", "test.Session", "scopename");
    auto f = resolveFailure();
    EXPECT_EQ(f.id, "") << f.message;
    auto d = descriptorOf("Cart");
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->scope, nullptr);
    EXPECT_EQ(d->scope->kind, CajetaModule::ScopePublication::Kind::Instance);
}

// `within` must name a published scope.
TEST(ScopeNameTests, withinMustBePublished) {
    auto f = parseAndResolve(
        "package test;\n"
        "public class P {\n"
        "    @Scope(value = \"Message\", within = \"Connection\")\n"
        "    public void onMessage() { return; }\n"
        "}\n", "test.P");
    EXPECT_EQ(f.id, "CAJETA_ERROR_UNKNOWN_SCOPE");
    EXPECT_NE(f.message.find("\"Connection\""), std::string::npos) << f.message;
}
