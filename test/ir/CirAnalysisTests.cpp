//
// Unit 3 (cajeta-ir plan) — specialization analysis (probes §3.5 -> §3.6).
// Lowers the comparator-sort slice, runs CirSpecializationAnalysis, and asserts:
//   - the directly-supplied-lambda call classifies SPECIALIZE with a request;
//   - a stored/runtime closure -> LEAVE-INDIRECT (no request);
//   - an incomplete invocation set (callee body absent) -> LEAVE-INDIRECT;
//   - an escaping closure parameter -> LEAVE-INDIRECT (hand-built CIR);
//   - the decision is deterministic and structural (no false SPECIALIZE).
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "cajeta/ir/CirBuilder.h"
#include "cajeta/ir/CirAnalysis.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using namespace cajeta::ir;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_cira_" + std::to_string(rng()));
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
                 / ("cajeta_cira_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

const char* kSource =
    "package test;\n"
    "public class S {\n"
    "    public static void each(int32[] a, int32 n, (int32, int32) -> int32 cmp) {\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            if (cmp(a[i], a[i]) > 0) { i = i + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "    }\n"
    "    public static void run(int32[] a, int32 n) {\n"
    "        each(a, n, (x, y) -> {\n"
    "            if (x < y) { return -1; }\n"
    "            if (x > y) { return 1; }\n"
    "            return 0;\n"
    "        });\n"
    "    }\n"
    "    public static void runStored(int32[] a, int32 n, (int32, int32) -> int32 c) {\n"
    "        each(a, n, c);\n"
    "    }\n"
    "}\n";

// Lower one named method of test.S to CIR.
CirFunctionPtr lower(Compiler& compiler, const std::string& method) {
    auto module = compileForInspection(compiler, kSource, "test.S");
    auto klass = module->getStructures()["test.S"];
    if (!klass) return nullptr;
    auto m = findMethod(klass, method);
    return m ? CirBuilder::buildFunction(m) : nullptr;
}

} // namespace

// 3.1.1 — the directly-supplied-lambda call classifies SPECIALIZE + emits a request.
TEST(CirAnalysisTests, directLambdaSpecializes) {
    Compiler compiler;
    auto each = lower(compiler, "each");
    auto run = lower(compiler, "run");
    ASSERT_NE(each, nullptr);
    ASSERT_NE(run, nullptr);

    auto result = CirSpecializationAnalysis::analyze({run, each});
    ASSERT_EQ(result.specialize.size(), 1u)
        << CirSpecializationAnalysis::print(result);
    auto& req = result.specialize[0];
    EXPECT_EQ(req.callee, "each");
    EXPECT_EQ(req.closureParam, "cmp");
    EXPECT_TRUE(has(req.targetFn, "run$lambda")) << req.targetFn;
    EXPECT_GE(req.invocationSites, 1);
    EXPECT_TRUE(result.leaveIndirect.empty())
        << CirSpecializationAnalysis::print(result);
}

// 3.1.2 — a stored/runtime closure (passed by name) -> LEAVE-INDIRECT, no request.
TEST(CirAnalysisTests, storedClosureLeavesIndirect) {
    Compiler compiler;
    auto each = lower(compiler, "each");
    auto runStored = lower(compiler, "runStored");
    ASSERT_NE(each, nullptr);
    ASSERT_NE(runStored, nullptr);

    auto result = CirSpecializationAnalysis::analyze({runStored, each});
    EXPECT_TRUE(result.specialize.empty())
        << CirSpecializationAnalysis::print(result);
    ASSERT_FALSE(result.leaveIndirect.empty());
    EXPECT_TRUE(has(result.leaveIndirect[0].reason, "directly-supplied"))
        << result.leaveIndirect[0].reason;
}

// 3.1.3 — an incomplete invocation set (callee body not in the slice) -> LEAVE-INDIRECT.
TEST(CirAnalysisTests, incompleteInvocationSetLeavesIndirect) {
    Compiler compiler;
    auto run = lower(compiler, "run");
    ASSERT_NE(run, nullptr);

    auto result = CirSpecializationAnalysis::analyze({run});   // `each` absent
    EXPECT_TRUE(result.specialize.empty())
        << CirSpecializationAnalysis::print(result);
    ASSERT_FALSE(result.leaveIndirect.empty());
    EXPECT_TRUE(has(result.leaveIndirect[0].reason, "not in slice"))
        << result.leaveIndirect[0].reason;
}

// 3.1.3 — an escaping closure parameter -> LEAVE-INDIRECT (hand-built CIR: the
// frontend can't express a closure param that is stored/returned).
TEST(CirAnalysisTests, escapingParameterLeavesIndirect) {
    CirType fnTy = CirType::named("(i32,i32)->i32");

    // callee esc(%cmp): invokes cmp once, but ALSO stores it (escape).
    auto esc = std::make_shared<CirFunction>();
    esc->name = "esc";
    auto cmp = cirValue("cmp", fnTy, CirOwnership::Value);
    esc->params = {cmp};
    auto eb = std::make_shared<CirBlock>();
    eb->label = "bb0";
    eb->params = {cmp};
    auto ap = cirInst(CirOp::ApplyClosure);
    ap->operands = {cmp};
    ap->result = cirValue("r", CirType::named("i32"));
    eb->insts.push_back(ap);
    auto slot = cirValue("slot", CirType::named("i32"));
    auto al = cirInst(CirOp::AllocStack); al->result = slot; eb->insts.push_back(al);
    auto st = cirInst(CirOp::Store); st->operands = {slot, cmp};  // cmp escapes
    eb->insts.push_back(st);
    eb->terminator = cirInst(CirOp::Return);
    esc->blocks = {eb};

    // caller passes a known non-capturing make.closure to esc.
    auto caller = std::make_shared<CirFunction>();
    caller->name = "callEsc";
    auto cb = std::make_shared<CirBlock>();
    cb->label = "bb0";
    auto clos = cirValue("clos", fnTy, CirOwnership::Value);
    auto mk = cirInst(CirOp::MakeClosure);
    mk->symbol = "escTarget"; mk->targetKnown = true; mk->result = clos;
    cb->insts.push_back(mk);
    auto call = cirInst(CirOp::Call);
    call->symbol = "esc"; call->operands = {clos};
    cb->insts.push_back(call);
    cb->terminator = cirInst(CirOp::Return);
    caller->blocks = {cb};

    auto result = CirSpecializationAnalysis::analyze({caller, esc});
    EXPECT_TRUE(result.specialize.empty())
        << CirSpecializationAnalysis::print(result);
    ASSERT_FALSE(result.leaveIndirect.empty());
    EXPECT_TRUE(has(result.leaveIndirect[0].reason, "escape"))
        << result.leaveIndirect[0].reason;
}

// 3.3.1 — deterministic + structural: same slice -> identical dump; no false SPECIALIZE.
TEST(CirAnalysisTests, deterministicNoFalseSpecialize) {
    Compiler compiler;
    auto each = lower(compiler, "each");
    auto run = lower(compiler, "run");
    auto runStored = lower(compiler, "runStored");
    ASSERT_NE(each, nullptr);

    std::string a = CirSpecializationAnalysis::print(
        CirSpecializationAnalysis::analyze({run, each, runStored}));
    std::string b = CirSpecializationAnalysis::print(
        CirSpecializationAnalysis::analyze({run, each, runStored}));
    EXPECT_EQ(a, b);

    // exactly one SPECIALIZE (run's lambda), the stored one stays indirect.
    auto result = CirSpecializationAnalysis::analyze({run, each, runStored});
    EXPECT_EQ(result.specialize.size(), 1u) << a;
}
