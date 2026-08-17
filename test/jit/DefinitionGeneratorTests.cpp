// lazy-codegen 2.1 — the definition generator's decision logic.
//
// The ORC glue (tryToGenerate) is a thin adapter; what needs pinning is what it
// DECIDES: which requested symbols are ours, which we must leave alone, and
// whether the mode gate is honoured. Those are tested here without standing up
// an ExecutionSession, so the suite stays cheap enough to run on every change.
//
// 2.1.1's end-to-end "a lookup resolves and the call returns the right value"
// needs a host wired to the generator — that is 2.2.3, and it is tested there.

#include "gtest/gtest.h"

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/CajetaDefinitionGenerator.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/method/Method.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using cajeta::CajetaDefinitionGenerator;
using cajeta::CajetaModulePtr;
using cajeta::CajetaSymbolIndex;
using cajeta::Compiler;
using cajeta::MethodPtr;
using cajeta::lazyCodegenEnabled;
using cajeta::setLazyCodegenEnabled;

namespace {

struct ModeGuard {
    bool saved = lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

CajetaModulePtr compileSource(Compiler& compiler, const std::string& stem) {
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta-defgen-" + stem);
    auto pkgDir = base / "src" / "main" / "cajeta" / "test";
    std::filesystem::create_directories(pkgDir);
    {
        std::ofstream out(pkgDir / (stem + ".cajeta"));
        out << "package test;\n\npublic class " << stem << " {\n"
            << "    int32 value;\n\n"
            << "    public int32 doubled() {\n        return this.value * 2;\n    }\n"
            << "}\n";
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

// A symbol the index certainly holds, taken from the index itself rather than
// spelled by hand — a hand-spelled mangled name would rot on any naming change
// and would be testing my transcription, not the generator.
std::string anyIndexedSymbol(Compiler& compiler, const CajetaSymbolIndex&) {
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            if (method && !method->getLlvmSymbolName().empty())
                return method->getLlvmSymbolName();
    return {};
}

} // namespace

// 2.1.2 — a symbol we do not know is not ours. The generator must decline so
// the lookup falls through to the process-symbol generator and, failing that,
// reports the ordinary missing-symbol error.
TEST(DefinitionGeneratorTests, declinesSymbolsItDoesNotKnow) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Alpha");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    CajetaDefinitionGenerator gen(index);
    auto claimed = gen.resolve({"__not_a_cajeta_symbol", "malloc", ""});
    EXPECT_TRUE(claimed.empty())
        << "claiming a symbol we cannot emit turns a clean fall-through into a "
           "failed materialization";
}

// 2.1.2 (other half) — a symbol we DO know is ours, and resolves to the method
// that would have been eagerly emitted under that name.
TEST(DefinitionGeneratorTests, claimsIndexedSymbols) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Beta");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = anyIndexedSymbol(compiler, index);
    ASSERT_FALSE(sym.empty()) << "the fixture indexed nothing";

    CajetaDefinitionGenerator gen(index);
    auto claimed = gen.resolve({sym});
    ASSERT_EQ(claimed.size(), 1u);
    EXPECT_EQ(claimed[0], index.find(sym));
}

// A mixed set must be split, not decided wholesale — ORC hands over everything
// still unresolved, most of which belongs to somebody else.
TEST(DefinitionGeneratorTests, splitsAMixedLookupSet) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Gamma");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = anyIndexedSymbol(compiler, index);
    ASSERT_FALSE(sym.empty());

    CajetaDefinitionGenerator gen(index);
    auto claimed = gen.resolve({"memcpy", sym, "__nope"});
    ASSERT_EQ(claimed.size(), 1u);
    EXPECT_EQ(claimed[0], index.find(sym));
}

// 2.1.5 applied — with the mode off the generator claims nothing at all, so the
// eager path is untouched and Unit 2 can land dark.
TEST(DefinitionGeneratorTests, claimsNothingWhenEager) {
    ModeGuard guard;
    setLazyCodegenEnabled(false);

    Compiler compiler;
    compileSource(compiler, "Delta");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = anyIndexedSymbol(compiler, index);
    ASSERT_FALSE(sym.empty());

    CajetaDefinitionGenerator gen(index);
    EXPECT_TRUE(gen.resolve({sym}).empty())
        << "the generator fired while eager — Unit 2 must be inert by default";
}

// 2.1.3 — repeat resolution is stable. generateCode() is idempotent (measured),
// so a second claim is harmless, but it must still name the SAME method; an
// index that answered differently on the second call would hand ORC a duplicate
// definition of something already materialized.
TEST(DefinitionGeneratorTests, repeatedResolutionIsStable) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Epsilon");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = anyIndexedSymbol(compiler, index);
    ASSERT_FALSE(sym.empty());

    CajetaDefinitionGenerator gen(index);
    auto first = gen.resolve({sym});
    auto second = gen.resolve({sym});
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0], second[0]);
}

// 2.2.2 asserted end-to-end — when ORC's real lookup machinery drives
// tryToGenerate, the emit callback runs INSIDE the compiler gate. This is the
// wiring test the direct CompilerGateTests cannot cover: a generator that
// forgot to route emission through the gate passes those and fails here.
// The emit probe defines nothing, so the lookup itself fails afterwards with
// the ordinary missing-symbol error — consumed, because the probe is the test.
TEST(DefinitionGeneratorTests, orcLookupRunsEmitUnderTheGate) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Zeta");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));
    const std::string sym = anyIndexedSymbol(compiler, index);
    ASSERT_FALSE(sym.empty());

    auto epc = llvm::cantFail(
        llvm::orc::SelfExecutorProcessControl::Create());
    llvm::orc::ExecutionSession session(std::move(epc));
    auto& jd = session.createBareJITDylib("gate-probe");

    bool emitRan = false;
    bool gateHeld = false;
    jd.addGenerator(std::make_unique<cajeta::CajetaDefinitionGenerator>(
        index,
        [&](const cajeta::MethodPtr& method, llvm::orc::JITDylib&)
                -> llvm::Error {
            emitRan = true;
            gateHeld = cajeta::CompilerGate::heldByThisThread();
            EXPECT_EQ(method, index.find(sym));
            return llvm::Error::success();
        }));

    auto result = session.lookup(
        {{&jd, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}},
        session.intern(sym));
    llvm::consumeError(result.takeError());

    EXPECT_TRUE(emitRan) << "ORC never reached the generator";
    EXPECT_TRUE(gateHeld) << "emission ran outside the compiler gate";
    llvm::cantFail(session.endSession());
}
