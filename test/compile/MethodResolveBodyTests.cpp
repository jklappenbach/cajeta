// xref-lint-emission-gap Unit 2 — body resolution, extracted and idempotent.
//
// `Method::resolveBody` is the body type-resolver pass lifted out of
// `generateCode` so lint can run it without codegen. Unit 2 is a pure
// refactor, and its safety property is byte-identity of the xref export
// (pinned by XrefRelationCoverage's build-vs-lint checks and by the export's
// own determinism pin). What THIS file pins is the property that makes Unit 3
// safe: the walk happens at most once per method, verified by counter rather
// than by reading the code (plan 2.1.2, 2.3.2).
//
// Why a counter and not a spy: in a process that lints and then compiles, the
// second call must be a no-op. "I read generateCode and it only calls it once"
// is exactly the kind of claim that let the original defect ship.
#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::Method;

namespace {

// Compile one source in-process and hand back its module, so the SAME Method
// objects can be driven again.
CajetaModulePtr compileInProcess(Compiler& compiler, const std::string& source,
                                 const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_rb_" + std::to_string(rng()));
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
    std::ofstream(full) << source;

    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_rb_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

const char* kSource =
    "package test;\n"
    "public class Counted {\n"
    "    int32 v;\n"
    "    public Counted() { this.v = 0; }\n"
    "    public void bump() { this.v = this.v + 1; }\n"
    "    public int32 value() { return this.v; }\n"
    "    public static int32 run() {\n"
    "        Counted c = heap Counted();\n"
    "        c.bump();\n"
    "        return c.value();\n"
    "    }\n"
    "}\n";

} // namespace

// 2.3.2 / 2.1.2 — resolveBody walks a body once, and only once.
//
// Driven directly against the parsed module's own Method objects, which is
// what makes the SECOND call meaningful: a fresh compile would build fresh
// Methods and legitimately walk again. `Compiler::compile(module)` parses and
// resolves signatures without entering codegen — exactly the state lint is in
// when Unit 3 asks it to resolve bodies.
TEST(MethodResolveBody, ABodyIsWalkedOnceAndTheSecondCallIsANoOp) {
    Compiler compiler;
    auto m = compileInProcess(compiler, kSource, "test.Counted");
    ASSERT_NE(m, nullptr);

    // Every method the class declares, in declaration order.
    std::vector<cajeta::MethodPtr> bodies;
    for (auto& [_, klass] : m->getStructures()) {
        if (!klass) continue;
        for (auto& [__, method] : klass->getMethods())
            if (method) bodies.push_back(method);
    }
    ASSERT_FALSE(bodies.empty()) << "the parsed module exposed no methods";

    const int64_t start = Method::bodyResolveWalks();
    for (auto& method : bodies) {
        try { method->resolveBody(m); } catch (...) {
            // A body that cannot resolve in isolation is Unit 3's problem
            // (3.1.3 makes it per-method best-effort). The counter is the
            // measurement either way — an attempted body counts as walked.
        }
    }
    const int64_t firstPass = Method::bodyResolveWalks() - start;
    EXPECT_GT(firstPass, 0)
        << "resolveBody walked nothing for a class with " << bodies.size()
        << " methods — the extraction is dead code";
    EXPECT_LE(firstPass, static_cast<int64_t>(bodies.size()))
        << "more walks than methods: a body was resolved more than once on "
           "the FIRST pass";

    // The second pass must be free. This is the property Unit 3 depends on:
    // once lint has resolved a body, codegen in the same process must not pay
    // for it again, nor re-record the edges it emitted.
    const int64_t beforeSecond = Method::bodyResolveWalks();
    for (auto& method : bodies) {
        try { method->resolveBody(m); } catch (...) {}
        EXPECT_TRUE(method->isBodyResolved())
            << "a method reports its body unresolved after resolveBody";
    }
    EXPECT_EQ(Method::bodyResolveWalks(), beforeSecond)
        << "a body was resolved a second time — lint-then-compile would pay "
           "for the walk twice and could double-record Unit 3's edges";
}
