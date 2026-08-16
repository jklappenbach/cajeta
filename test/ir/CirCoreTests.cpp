//
// Unit 1 (cajeta-ir plan) — CIR core data structures + textual dump + verify.
// Pure unit tests: build CIR by hand, no codegen / JIT / type-registry
// dependency. Traces spec §2.1-2.5.
//

#include <gtest/gtest.h>

#include "cajeta/ir/Cir.h"
#include "cajeta/ir/CirPrinter.h"
#include "cajeta/ir/CirVerifier.h"

using namespace cajeta::ir;

namespace {

    // Build the closed-slice-shaped demo function by hand:
    //
    //   fn demo<T>(%a: T[] borrowed, %n: i32) -> () {
    //   bb0(%a, %n):
    //       %cmp = make.closure cmpNat, [] : (T,T)->i32   ; known
    //       %x = const.int 0 : i32
    //       %y = const.int 1 : i32
    //       %c = apply.closure %cmp(%x, %y) : i32
    //       return
    //   }
    //
    // Exercises: params with ownership, typed insts, a non-capturing
    // make.closure (known target), an apply.closure, and a terminator.
    CirFunctionPtr buildDemo() {
        auto fn = std::make_shared<CirFunction>();
        fn->name = "demo";
        fn->genericParams = {"T"};

        auto a = cirValue("a", CirType::named("T[]"), CirOwnership::Borrowed);
        auto n = cirValue("n", CirType::named("i32"));
        fn->params = {a, n};
        fn->returnType = CirType::voidType();

        auto bb0 = std::make_shared<CirBlock>();
        bb0->label = "bb0";
        bb0->params = {a, n};

        auto cmp = cirValue("cmp", CirType::named("(T,T)->i32"));
        auto mk = cirInst(CirOp::MakeClosure);
        mk->symbol = "cmpNat";
        mk->targetKnown = true;
        mk->result = cmp;
        bb0->insts.push_back(mk);

        auto x = cirValue("x", CirType::named("i32"));
        auto ci0 = cirInst(CirOp::ConstInt);
        ci0->intConst = 0;
        ci0->result = x;
        bb0->insts.push_back(ci0);

        auto y = cirValue("y", CirType::named("i32"));
        auto ci1 = cirInst(CirOp::ConstInt);
        ci1->intConst = 1;
        ci1->result = y;
        bb0->insts.push_back(ci1);

        auto c = cirValue("c", CirType::named("i32"));
        auto ap = cirInst(CirOp::ApplyClosure);
        ap->operands = {cmp, x, y};
        ap->result = c;
        bb0->insts.push_back(ap);

        auto ret = cirInst(CirOp::Return);
        bb0->terminator = ret;

        fn->blocks = {bb0};
        return fn;
    }

    bool contains(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }

} // namespace

// 1.1.1 — dump round-trips the structure (names, types, ownership kinds, the
// make.closure / apply.closure shape).
TEST(CirCoreTests, dumpRoundTripsStructure) {
    auto fn = buildDemo();
    std::string text = CirPrinter::print(*fn);

    EXPECT_TRUE(contains(text, "fn demo<T>(")) << text;
    EXPECT_TRUE(contains(text, "%a: T[] borrowed")) << text;   // ownership shown
    EXPECT_TRUE(contains(text, "%n: i32")) << text;            // value ownership = absence
    EXPECT_TRUE(contains(text, "-> ()")) << text;              // void return
    EXPECT_TRUE(contains(text, "bb0(%a, %n):")) << text;
    EXPECT_TRUE(contains(text, "%cmp = make.closure cmpNat, []")) << text;
    EXPECT_TRUE(contains(text, "known")) << text;              // target-known annotation
    EXPECT_TRUE(contains(text, "%x = const.int 0 : i32")) << text;
    EXPECT_TRUE(contains(text, "%c = apply.closure %cmp(%x, %y) : i32")) << text;
    EXPECT_TRUE(contains(text, "return")) << text;
}

// 1.1.1 — ownership words: owned distinct from borrowed distinct from value.

// 1.1.2 — a block without a terminator is caught.

// 1.1.2 — a well-formed function has no errors.

// 1.1.2 — SSA single-definition: a value used as two instruction results is caught.

// 1.1.2 — branch block-parameter arity mismatch is caught.
TEST(CirCoreTests, verifyCatchesBranchArityMismatch) {
    auto fn = std::make_shared<CirFunction>();
    fn->name = "brfn";
    auto p = cirValue("p", CirType::named("i32"));
    fn->params = {p};

    auto entry = std::make_shared<CirBlock>();
    entry->label = "bb0";
    auto br = cirInst(CirOp::Br);
    br->successors.push_back(CirSuccessor{"bb1", {}, std::nullopt});  // passes 0 args
    entry->terminator = br;

    auto target = std::make_shared<CirBlock>();
    target->label = "bb1";
    target->params = {cirValue("q", CirType::named("i32"))};          // expects 1 param
    target->terminator = cirInst(CirOp::Return);

    fn->blocks = {entry, target};
    auto errs = CirVerifier::verify(*fn);
    ASSERT_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.find("arity") != std::string::npos ||
                             e.find("param") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}
