// Shared drop-function backfill (jit-drop-backfill spec §2).
//
// Drop thunks are synthesized lazily into the owning type's module when a
// consumer's codegen drops a value of the type — so a consumer module can be
// left holding a bare extern declaration whose definition never fired
// (indirect stdlib instantiations are the canonical case). The backfill pass
// scans modules for undefined `__cajeta[_stack]_<type>_drop` declarations and
// synthesizes exactly those. The AOT incremental path has run this scan
// inline for a while; these tests pin the extracted, shared implementation
// that the JIT pipeline calls too.
//
// In-process (Compiler + the standard codegen fixpoint), mirroring
// JitTestHelper's pipeline: the tests need the compiled CajetaClass instances
// and their LLVM modules, not on-disk artifacts.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/DropBackfill.h"
#include "cajeta/type/CajetaClass.h"

namespace {

namespace fs = std::filesystem;
using namespace cajeta;

// Write each (relPath → source) under a fresh temp root; removed on scope exit.
struct TempTree {
    fs::path root;
    explicit TempTree(const std::map<std::string, std::string>& files) {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_dropbf_" + std::to_string(rng()));
        for (auto& [rel, src] : files) {
            fs::path p = root / rel;
            fs::create_directories(p.parent_path());
            std::ofstream out(p);
            out << src;
        }
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// Parse every file under the tree (Debug mode, like buildJit). The pass under
// test only needs registered CajetaClass instances with LLVM modules —
// getOrCreate{Stack,}DropFunction resolves the types it needs lazily — so the
// full codegen fixpoint is deliberately NOT run here (it recompiles the whole
// prelude per test and dominates suite time).
void compileTree(Compiler& compiler, const TempTree& tree) {
    fs::path build = tree.root / "build";
    fs::create_directories(build);
    compiler.setMode(CompilerMode::Debug);
    for (auto& entry : fs::recursive_directory_iterator(tree.root)) {
        if (entry.path().extension() != ".cajeta") continue;
        auto m = compiler.createModule(entry.path().string(),
                                       tree.root.string(), build.string());
        compiler.compile(m);
    }
    CajetaModule::resolveDependencyGraph();
}

CajetaClassPtr classFor(const std::string& canonical) {
    auto& canon = CajetaType::getCanonicalMap();
    auto it = canon.find(canonical);
    if (it == canon.end()) return nullptr;
    return std::dynamic_pointer_cast<CajetaClass>(it->second);
}

llvm::Module* llvmModuleOf(const std::string& structureName) {
    auto& s2m = CajetaModule::getStructureToModule();
    auto it = s2m.find(structureName);
    if (it == s2m.end()) return nullptr;
    return it->second->getLlvmModule();
}

// Plant a bare extern declaration of `symbol` (void(ptr), the drop-thunk
// signature) in `mod` — the dangling shape a consumer module is left with
// when synthesis never fired.
void declareDropThunk(llvm::Module* mod, const std::string& symbol) {
    auto& ctx = mod->getContext();
    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx),
        {llvm::PointerType::get(ctx, 0)}, /*isVarArg=*/false);
    mod->getOrInsertFunction(symbol, fnTy);
}

const char* kValueClassSource =
    "package test;\n"
    "public class S {\n"
    "    public int32 x;\n"
    "}\n";

const char* kOtherClassSource =
    "package test;\n"
    "public class User {\n"
    "    public static int32 run() {\n"
    "        return 7;\n"
    "    }\n"
    "}\n";

// 1.1.1 — an undefined stack-drop declaration in one module is synthesized
// into the owning type's own module by the pass.
TEST(DropBackfillTests, StackDropDeclarationIsBackfilled) {
    TempTree tree({{"test/S.cajeta", kValueClassSource},
                   {"test/User.cajeta", kOtherClassSource}});
    Compiler compiler;
    compileTree(compiler, tree);

    auto klass = classFor("test.S");
    ASSERT_NE(klass, nullptr);
    llvm::Module* userMod = llvmModuleOf("test.User");
    ASSERT_NE(userMod, nullptr);

    const std::string sym = dropSymbolName("test.S", /*stack=*/true);
    declareDropThunk(userMod, sym);
    ASSERT_TRUE(userMod->getFunction(sym)->isDeclaration());

    auto moduleList = compiler.getModules();  // by-value: bind ONE copy
    std::vector<CajetaModulePtr> scan(moduleList.begin(), moduleList.end());
    backfillDropFunctions(scan, scan);

    llvm::Function* def = klass->getEmitModule()->getLlvmModule()->getFunction(sym);
    ASSERT_NE(def, nullptr);
    EXPECT_FALSE(def->isDeclaration());
}

// 1.1.2 — same contract for the heap family.
TEST(DropBackfillTests, HeapDropDeclarationIsBackfilled) {
    TempTree tree({{"test/S.cajeta", kValueClassSource},
                   {"test/User.cajeta", kOtherClassSource}});
    Compiler compiler;
    compileTree(compiler, tree);

    auto klass = classFor("test.S");
    ASSERT_NE(klass, nullptr);
    llvm::Module* userMod = llvmModuleOf("test.User");
    ASSERT_NE(userMod, nullptr);

    const std::string sym = dropSymbolName("test.S", /*stack=*/false);
    declareDropThunk(userMod, sym);
    ASSERT_TRUE(userMod->getFunction(sym)->isDeclaration());

    auto moduleList = compiler.getModules();  // by-value: bind ONE copy
    std::vector<CajetaModulePtr> scan(moduleList.begin(), moduleList.end());
    backfillDropFunctions(scan, scan);

    llvm::Function* def = klass->getEmitModule()->getLlvmModule()->getFunction(sym);
    ASSERT_NE(def, nullptr);
    EXPECT_FALSE(def->isDeclaration());
}

// 1.1.3 — the mangling the pass scans by is the mangling synthesis emits.
// String-level expectation pinned to the exact symbol observed missing on
// samples/tour (nested template args exercise '<', '>', ',', '.', ':').
TEST(DropBackfillTests, ManglingMatchesSynthesis) {
    EXPECT_EQ(
        "__cajeta_stack_cajeta_lang_Pair_int32_cajeta_lang_Pair_int32_int32___drop",
        dropSymbolName("cajeta.lang.Pair<int32,cajeta.lang.Pair<int32,int32>>",
                       /*stack=*/true));
    EXPECT_EQ(
        "__cajeta_cajeta_lang_Optional_int32__drop",
        dropSymbolName("cajeta.lang.Optional<int32>", /*stack=*/false));
    EXPECT_EQ("__cajeta_stack_test_S_drop",
              dropSymbolName("test.S", /*stack=*/true));

    // Agreement with the synthesis site: the function a real class creates
    // carries exactly the name the helper predicts.
    TempTree tree({{"test/S.cajeta", kValueClassSource}});
    Compiler compiler;
    compileTree(compiler, tree);
    auto klass = classFor("test.S");
    ASSERT_NE(klass, nullptr);
    llvm::Function* fn = klass->getOrCreateStackDropFunction();
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(dropSymbolName("test.S", /*stack=*/true), fn->getName().str());
}

// 2.1.1 (mechanism level) — llvm::Linker lazy-links linkonce_odr: a drop
// thunk defined in a donor module is DISCARDED at merge when nothing copied
// in that link step references it, leaving later consumers' declarations
// dangling ("Symbols not found" at LLJIT materialization — samples/tour).
// pinDropFunctionDefinitions promotes definitions to weak_odr pre-merge so
// they survive regardless of link order. Deterministic by construction: the
// suite-level JIT fixture's pre-fix redness depends on compiler-internal
// module discovery order, so the pin's contract is pinned HERE, in the
// adversarial order (owner donor links first, primary holds no reference).
// Returns whether the thunk is a real definition after the merge.
bool dropThunkSurvivesAdversarialMerge(bool pin) {
    TempTree tree({{"test/S.cajeta", kValueClassSource},
                   {"test/User.cajeta", kOtherClassSource},
                   {"test/Prime.cajeta",
                    "package test;\n"
                    "public class Prime {\n"
                    "    public static int32 run() {\n"
                    "        return 1;\n"
                    "    }\n"
                    "}\n"}});
    Compiler compiler;
    compileTree(compiler, tree);

    auto klass = classFor("test.S");
    if (!klass) { ADD_FAILURE() << "test.S not registered"; return false; }
    // Synthesize the thunk (linkonce_odr, in S's own module) and dangle a
    // declaration in a separate consumer module — the tour shape.
    llvm::Function* def = klass->getOrCreateStackDropFunction();
    if (!def || !def->hasLinkOnceODRLinkage()) {
        ADD_FAILURE() << "expected a linkonce_odr stack-drop synthesis";
        return false;
    }
    const std::string sym = dropSymbolName("test.S", /*stack=*/true);
    llvm::Module* userMod = llvmModuleOf("test.User");
    llvm::Module* ownerMod = klass->getEmitModule()->getLlvmModule();
    llvm::Module* primaryMod = llvmModuleOf("test.Prime");
    if (!userMod || !ownerMod || !primaryMod
        || ownerMod == primaryMod || userMod == primaryMod) {
        ADD_FAILURE() << "unexpected module layout";
        return false;
    }
    declareDropThunk(userMod, sym);
    // Reference the thunk from User code so the declaration is not a dead
    // symbol the linker can drop outright (mirrors the drop-chain push).
    {
        llvm::Function* userFn = userMod->getFunction(sym);
        auto& ctx = userMod->getContext();
        auto* holder = llvm::cast<llvm::Function>(
            userMod->getOrInsertFunction(
                "test_user_scope_holder",
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                        {llvm::PointerType::get(ctx, 0)},
                                        false)).getCallee());
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", holder);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(userFn, {holder->getArg(0)});
        b.CreateRetVoid();
    }

    if (pin) {
        auto moduleList = compiler.getModules();  // by-value: bind ONE copy
        std::vector<CajetaModulePtr> all(moduleList.begin(), moduleList.end());
        pinDropFunctionDefinitions(all);
        EXPECT_TRUE(ownerMod->getFunction(sym)->hasWeakODRLinkage());
    }

    // Merge CLONES: linkModules consumes its source, and these llvm::Modules
    // are held by the process-wide module registries — destroying them
    // poisons every later in-process compile in this test binary.
    auto primaryClone = llvm::CloneModule(*primaryMod);
    if (llvm::Linker::linkModules(*primaryClone, llvm::CloneModule(*ownerMod))
        || llvm::Linker::linkModules(*primaryClone,
                                     llvm::CloneModule(*userMod))) {
        ADD_FAILURE() << "linkModules failed";
        return false;
    }
    llvm::Function* merged = primaryClone->getFunction(sym);
    return merged && !merged->isDeclaration();
}

// Characterization of the failure this pass exists for: without the pin the
// linker discards the donor's linkonce_odr thunk. If LLVM's lazy-linking
// semantics ever change and this starts failing, the pin may be obsolete.
TEST(DropBackfillTests, UnpinnedDropThunkIsDiscardedByLazyLinkMerge) {
    EXPECT_FALSE(dropThunkSurvivesAdversarialMerge(/*pin=*/false));
}

TEST(DropBackfillTests, PinnedDropThunkSurvivesLazyLinkMerge) {
    EXPECT_TRUE(dropThunkSurvivesAdversarialMerge(/*pin=*/true));
}

// 1.1.6 — stale-class guard. The canonical type map is a persistent
// thread-local: in a multi-compile process it still holds classes from
// earlier compiles whose LLVM modules were consumed by that compile's merge.
// A declaration whose matched class does NOT belong to the current compile
// must be SKIPPED — synthesizing into the stale module is a write into freed
// memory (segfaulted cajeta_debug_test's JitHost sequence, 2026-07-20).
// Here the "stale" class is from a prior in-process compile whose modules
// are still alive, so the skip is observable without touching freed memory.
TEST(DropBackfillTests, StaleClassFromEarlierCompileIsSkipped) {
    // Compile #1 registers test.StaleOnly and abandons it. compilerA is kept
    // alive so this test may safely INSPECT the stale module afterwards — in
    // production the stale module is freed, which is exactly why the guard
    // must never dereference it (it only compares pointers).
    TempTree treeA({{"test/StaleOnly.cajeta",
                     "package test;\n"
                     "public class StaleOnly {\n"
                     "    public int32 x;\n"
                     "}\n"}});
    Compiler compilerA;
    compileTree(compilerA, treeA);
    auto stale = classFor("test.StaleOnly");
    ASSERT_NE(stale, nullptr);
    llvm::Module* staleMod = stale->getEmitModule()->getLlvmModule();
    ASSERT_NE(staleMod, nullptr);
    // Compile #2 never mentions StaleOnly, but one of its modules carries a
    // dangling declaration whose name matches the stale map entry.
    TempTree treeB({{"test/User.cajeta", kOtherClassSource}});
    Compiler compilerB;
    compileTree(compilerB, treeB);
    llvm::Module* userMod = llvmModuleOf("test.User");
    ASSERT_NE(userMod, nullptr);
    const std::string sym = dropSymbolName("test.StaleOnly", /*stack=*/true);
    declareDropThunk(userMod, sym);

    auto moduleList = compilerB.getModules();  // by-value: bind ONE copy
    std::vector<CajetaModulePtr> scan(moduleList.begin(), moduleList.end());
    size_t staleFnCountBefore = staleMod->getFunctionList().size();
    backfillDropFunctions(scan, scan);

    // Skipped: nothing synthesized into the stale module, declaration left
    // dangling (the JIT's materialization error is preferable to corruption).
    EXPECT_EQ(staleFnCountBefore, staleMod->getFunctionList().size());
    EXPECT_TRUE(userMod->getFunction(sym)->isDeclaration());
}

// 1.1.4 — a module with no dangling drop declarations is left untouched.
TEST(DropBackfillTests, CleanModuleIsUntouched) {
    TempTree tree({{"test/S.cajeta", kValueClassSource}});
    Compiler compiler;
    compileTree(compiler, tree);

    auto moduleList = compiler.getModules();  // by-value: bind ONE copy
    std::vector<CajetaModulePtr> scan(moduleList.begin(), moduleList.end());
    std::vector<size_t> before;
    for (auto& m : scan)
        before.push_back(m->getLlvmModule()->getFunctionList().size());

    backfillDropFunctions(scan, scan);

    size_t i = 0;
    for (auto& m : scan)
        EXPECT_EQ(before[i++], m->getLlvmModule()->getFunctionList().size());
}

} // namespace
