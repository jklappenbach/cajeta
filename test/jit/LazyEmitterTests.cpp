// lazy-codegen 2.1.1 / 2.1.3 / 2.2.3 — on-demand emission, end to end.
//
// The proof the capability rests on: compile a fixture, run NO eager codegen,
// install the generator on a real LLJIT, look a symbol up, CALL it, and get the
// right answer. Everything before this tested decisions; this tests delivery.

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/CajetaDefinitionGenerator.h"
#include "cajeta/jit/CajetaLazyEmitter.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/method/Method.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

using cajeta::CajetaDefinitionGenerator;
using cajeta::CajetaModulePtr;
using cajeta::CajetaSymbolIndex;
using cajeta::Compiler;
using cajeta::MethodPtr;
using cajeta::setLazyCodegenEnabled;

namespace {

struct ModeGuard {
    bool saved = cajeta::lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

// A static method keeps the fixture free of instances, vtables, and drops —
// the runtime surface belongs to Units 3-5, not to the delivery mechanism.
const char* kSource = R"(package test;

public class Calc {
    public static int32 base() {
        return 40;
    }

    public static int32 answer() {
        return base() + 2;
    }
}
)";

CajetaModulePtr compileSource(Compiler& compiler, const std::string& stem) {
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta-lazyemit-" + stem);
    auto pkgDir = base / "src" / "main" / "cajeta" / "test";
    std::filesystem::create_directories(pkgDir);
    {
        std::ofstream out(pkgDir / (stem + ".cajeta"));
        out << kSource;
    }
    auto archive = base / "archive";
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((pkgDir / (stem + ".cajeta")).string(),
                                   (base / "src" / "main" / "cajeta").string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

std::list<CajetaModulePtr> hostModuleSet(Compiler& compiler) {
    std::list<CajetaModulePtr> mods = compiler.getModules();
    if (auto stdlib = cajeta::CajetaModule::getStdlibModule()) {
        bool present = false;
        for (auto& m : mods) if (m == stdlib) { present = true; break; }
        if (!present) mods.push_back(stdlib);
    }
    return mods;
}

// The fixture method itself, from the module set — never hand-spelled.
MethodPtr methodFor(Compiler& compiler, const std::string& shortName) {
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods()) {
            if (!method) continue;
            const std::string sym = method->getLlvmSymbolName();
            if (sym.find("test.Calc") != std::string::npos
                && sym.find("::" + shortName + "(") != std::string::npos)
                return method;
        }
    return {};
}

std::string symbolFor(Compiler& compiler, const std::string& shortName) {
    MethodPtr m = methodFor(compiler, shortName);
    return m ? m->getLlvmSymbolName() : std::string();
}

struct LazyJit {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    CajetaDefinitionGenerator* generator = nullptr;

    static LazyJit create(CajetaSymbolIndex& index) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        LazyJit out;
        out.jit = llvm::cantFail(llvm::orc::LLJITBuilder().create());
        auto gen = std::make_unique<CajetaDefinitionGenerator>(
            index,
            [&jit = *out.jit](llvm::orc::ThreadSafeModule tsm,
                              llvm::orc::JITDylib& jd) -> llvm::Error {
                return jit.addIRModule(jd, std::move(tsm));
            });
        out.generator = gen.get();
        out.jit->getMainJITDylib().addGenerator(std::move(gen));
        return out;
    }

    int32_t call(const std::string& sym) {
        auto addr = llvm::cantFail(out().lookup(sym));
        return addr.toPtr<int32_t (*)()>()();
    }

    llvm::orc::LLJIT& out() { return *jit; }
};

} // namespace

// 3.2.3 — the snapshot is pruned to the kept definition's reference closure.
// Both properties are checked by an independent walk over the delivered
// module, so the test cannot share a bug with the extractor:
//   (1) every definition is transitively referenced from the kept symbol;
//   (2) every declaration has at least one use.
// Before pruning this fails on both counts: the clone carries every
// local-linkage global in the stdlib module and a declaration for each of
// its ~11k functions.
TEST(LazyEmitterTests, snapshotIsPrunedToTheKeptClosure) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Pruned");
    MethodPtr method = methodFor(compiler, "base");
    ASSERT_TRUE(method);

    auto tsm = cajeta::emitMethodModule(method);
    ASSERT_TRUE(bool(tsm)) << llvm::toString(tsm.takeError());

    const std::string sym = method->getLlvmSymbolName();
    tsm->withModuleDo([&](llvm::Module& m) {
        llvm::Function* kept = m.getFunction(sym);
        ASSERT_NE(kept, nullptr);
        ASSERT_FALSE(kept->isDeclaration());

        llvm::SmallPtrSet<const llvm::GlobalValue*, 32> closure;
        llvm::SmallPtrSet<const llvm::Constant*, 32> seen;
        llvm::SmallVector<const llvm::GlobalValue*, 32> work{kept};
        closure.insert(kept);
        std::function<void(const llvm::Constant*)> scan =
            [&](const llvm::Constant* c) {
                if (!seen.insert(c).second) return;
                if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(c)) {
                    if (closure.insert(gv).second) work.push_back(gv);
                    return;
                }
                for (const llvm::Use& u : c->operands())
                    if (auto* op = llvm::dyn_cast<llvm::Constant>(u.get()))
                        scan(op);
            };
        while (!work.empty()) {
            const llvm::GlobalValue* gv = work.pop_back_val();
            for (const llvm::Use& u : gv->operands())
                if (auto* op = llvm::dyn_cast<llvm::Constant>(u.get()))
                    scan(op);
            if (auto* fn = llvm::dyn_cast<llvm::Function>(gv))
                for (const llvm::BasicBlock& bb : *fn)
                    for (const llvm::Instruction& inst : bb)
                        for (const llvm::Use& u : inst.operands())
                            if (auto* op =
                                    llvm::dyn_cast<llvm::Constant>(u.get()))
                                scan(op);
        }

        size_t unreachedDefs = 0, unusedDecls = 0;
        std::string sampleDef, sampleDecl;
        for (const llvm::GlobalValue& gv : m.global_values()) {
            if (gv.isDeclaration()) {
                if (gv.use_empty()) {
                    ++unusedDecls;
                    if (sampleDecl.empty()) sampleDecl = gv.getName().str();
                }
            } else if (!closure.count(&gv)) {
                ++unreachedDefs;
                if (sampleDef.empty()) sampleDef = gv.getName().str();
            }
        }
        EXPECT_EQ(unreachedDefs, 0u)
            << unreachedDefs << " definitions outside the kept closure, e.g. "
            << sampleDef;
        EXPECT_EQ(unusedDecls, 0u)
            << unusedDecls << " declarations with no uses, e.g. " << sampleDecl;
    });
}

// 2.1.1 — a symbol never eagerly emitted resolves through the generator and
// calling it returns the right value.
TEST(LazyEmitterTests, lookupGeneratesBodyAndCallReturnsRightValue) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Direct");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = symbolFor(compiler, "base");
    ASSERT_FALSE(sym.empty());

    auto lazy = LazyJit::create(index);
    EXPECT_EQ(lazy.call(sym), 40);
    EXPECT_GE(lazy.generator->generatedCount(), 1u);
}

// Spec 3.4 — materializing one body references another that was never emitted;
// the nested reference resolves through the same path.
TEST(LazyEmitterTests, cascadeResolvesCalleeThroughTheGenerator) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Cascade");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string answer = symbolFor(compiler, "answer");
    ASSERT_FALSE(answer.empty());

    auto lazy = LazyJit::create(index);
    // Only `answer` is requested; `base` must arrive via the cascade.
    EXPECT_EQ(lazy.call(answer), 42);
    EXPECT_GE(lazy.generator->generatedCount(), 2u)
        << "the callee did not come through the generator";
}

// 2.1.3, live half — a second lookup of the same symbol is served from the
// JIT's own tables: one definition, and the generator is not re-entered.
TEST(LazyEmitterTests, secondLookupDoesNotRegenerate) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Repeat");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = symbolFor(compiler, "base");
    ASSERT_FALSE(sym.empty());

    auto lazy = LazyJit::create(index);
    EXPECT_EQ(lazy.call(sym), 40);
    const size_t after = lazy.generator->generatedCount();
    EXPECT_EQ(lazy.call(sym), 40);
    EXPECT_EQ(lazy.generator->generatedCount(), after)
        << "a resolved symbol re-entered the generator";
}

// 6.1.2 (spec 5.3) — a method the index KNOWS but generateCode cannot give a
// body (an interface method: pure declaration by construction — @Native was
// the first candidate but gets a bridge body, measured writing this test)
// reports the method by name and says WHY. The string is deliberately unlike
// ORC's "Symbols not found" so a log reader can tell "the compiler failed to
// produce this" from "nobody has ever heard of it".
TEST(LazyEmitterTests, indexedMethodWithoutABodyNamesItselfAndTheReason) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    const char* src = R"(package test;

public interface Hollow {
    int32 outside();
}
)";
    auto base = std::filesystem::temp_directory_path() / "cajeta-lazyemit-hollow";
    auto pkgDir = base / "src" / "main" / "cajeta" / "test";
    std::filesystem::create_directories(pkgDir);
    {
        std::ofstream out(pkgDir / "Hollow.cajeta");
        out << src;
    }
    auto archive = base / "archive";
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((pkgDir / "Hollow.cajeta").string(),
                                   (base / "src" / "main" / "cajeta").string(),
                                   archive.string());
    compiler.compile(m);

    MethodPtr method;
    for (auto& mod : hostModuleSet(compiler))
        for (auto& cand : mod->getAllMethods()) {
            if (!cand) continue;
            const std::string sym = cand->getLlvmSymbolName();
            if (sym.find("test.Hollow") != std::string::npos
                && sym.find("::outside(") != std::string::npos)
                method = cand;
        }
    ASSERT_NE(nullptr, method.get());

    auto tsm = cajeta::emitMethodModule(method);
    ASSERT_FALSE(bool(tsm));
    const std::string msg = llvm::toString(tsm.takeError());
    EXPECT_NE(msg.find(method->getLlvmSymbolName()), std::string::npos)
        << "the failure does not name the method: " << msg;
    EXPECT_NE(msg.find("left no body"), std::string::npos)
        << "the failure does not say why: " << msg;
    EXPECT_EQ(msg.find("Symbols not found"), std::string::npos)
        << "indistinguishable from an ordinary missing symbol: " << msg;
}
