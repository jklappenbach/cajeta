// lazy-codegen Unit 1 (plan 1.1) — the symbol index.
//
// The index is what replaces eager emission: instead of calling generateCode()
// on all 12,379 methods a session knows, index them by the symbol name they
// WOULD be emitted under, and generate a body when the JIT asks for its symbol.
// If a key here is not byte-identical to the name the eager path emits, the
// generator silently never fires and every lookup falls through to "Symbols not
// found" — so 1.1.2 is the load-bearing test, not 1.1.1.

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Module.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using cajeta::CajetaModulePtr;
using cajeta::CajetaSymbolIndex;
using cajeta::Compiler;

namespace {

// Compile one source into a fresh compiler and hand back its module set, the
// same set the eager fixpoint walks.
CajetaModulePtr compileSource(Compiler& compiler, const std::string& source,
                              const std::string& stem) {
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta-symindex-" + stem);
    auto pkgDir = base / "src" / "main" / "cajeta" / "test";
    std::filesystem::create_directories(pkgDir);
    auto full = pkgDir / (stem + ".cajeta");
    {
        std::ofstream out(full);
        out << source;
    }
    auto archive = base / "archive";
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(),
                                   (base / "src" / "main" / "cajeta").string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

// The set the JIT hosts codegen. Both hosts append the stdlib by hand; measured
// 2026-08-16 it is already in getModules() (147 entries, 146 distinct), so this
// dedupes rather than adds. Kept so the test set matches the hosts' exactly.
std::list<CajetaModulePtr> hostModuleSet(Compiler& compiler) {
    std::list<CajetaModulePtr> mods = compiler.getModules();
    if (auto stdlib = cajeta::CajetaModule::getStdlibModule()) {
        bool present = false;
        for (auto& m : mods) if (m == stdlib) { present = true; break; }
        if (!present) mods.push_back(stdlib);
    }
    return mods;
}

const char* kSource = R"(package test;

public class Widget {
    int32 value;

    public int32 doubled() {
        return this.value * 2;
    }

    public int32 plus(int32 n) {
        return this.value + n;
    }
}
)";

} // namespace

// 1.1.1 — every method the eager loop would emit is indexed, exactly once.
TEST(SymbolIndexTests, everyMethodIndexedExactlyOnce) {
    Compiler compiler;
    compileSource(compiler, kSource, "Widget");

    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    size_t methods = 0;
    for (auto& m : hostModuleSet(compiler)) {
        for (auto& method : m->getAllMethods()) {
            ++methods;
            const std::string sym = method->getLlvmSymbolName();
            ASSERT_FALSE(sym.empty())
                << "a method with no symbol name can never be looked up";
            EXPECT_EQ(index.find(sym), method)
                << "not indexed under its own symbol name: " << sym;
        }
    }
    EXPECT_GT(methods, 0u) << "the fixture compiled nothing";
    EXPECT_EQ(index.size(), methods)
        << "index size must equal the method count — a collision would make one "
           "method unreachable through the generator";
}

// 1.1.2 — THE load-bearing one. The key must equal the name actually emitted.
// Compared against the llvm::Function in the module, not against a second call
// to getLlvmSymbolName(), which would only prove the function is deterministic.
TEST(SymbolIndexTests, indexKeyIsByteIdenticalToEmittedFunctionName) {
    Compiler compiler;
    compileSource(compiler, kSource, "WidgetEmit");

    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    // Eager-emit exactly as the JIT hosts do today.
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->getLlvmFunctionType();
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->generateCode();

    // Codegen instantiates templates, which DEFINE NEW METHODS (spec 3.5) —
    // `TakeStream<String>::unwrap` appears only now. A generator serving a
    // stale index would miss exactly these, so refresh before checking.
    index.build(hostModuleSet(compiler));

    size_t checked = 0;
    for (auto& m : hostModuleSet(compiler)) {
        llvm::Module* lm = m->getLlvmModule();
        if (!lm) continue;
        for (auto& method : m->getAllMethods()) {
            const std::string sym = method->getLlvmSymbolName();
            llvm::Function* fn = lm->getFunction(sym);
            if (!fn) continue;   // not emitted into this module; 1.1.1 covers indexing
            ++checked;
            EXPECT_EQ(fn->getName().str(), sym)
                << "emitted name and index key diverge";
            EXPECT_EQ(index.find(sym), method)
                << "the emitted symbol does not resolve back to its method: " << sym;
        }
    }
    EXPECT_GT(checked, 0u)
        << "nothing was emitted — the test proves nothing about naming";
}

// 1.1.2b — a name the index does not know must miss, not return something.
TEST(SymbolIndexTests, unknownSymbolMisses) {
    Compiler compiler;
    compileSource(compiler, kSource, "WidgetMiss");

    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    EXPECT_EQ(index.find("__definitely_not_a_cajeta_method"), nullptr);
    EXPECT_EQ(index.find(""), nullptr);
}

// 1.1.3 — the index covers a session-sized world. Count is asserted; build time
// is REPORTED, not thresholded (perf acceptance is by review, never by a test).
TEST(SymbolIndexTests, indexCoversTheStdlibWorld) {
    Compiler compiler;
    compileSource(compiler, kSource, "WidgetWorld");

    auto t0 = std::chrono::steady_clock::now();
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // The stdlib alone is ~11,015 methods; a bare user class pulls in the
    // primed world. Deliberately loose — this pins "the index sees the world",
    // not an exact count that drifts with every stdlib change.
    EXPECT_GT(index.size(), 1000u)
        << "the index is not seeing the primed stdlib";
    std::fprintf(stderr, "[symbol-index] %zu entries in %lld ms\n",
                 index.size(), (long long) ms);
}

// 1.1.2c — spec 3.5. Codegen instantiates templates and so defines methods that
// did not exist when the index was built; a rebuild must pick them up without
// dropping what was already there. Found by 1.1.2 while it was being written:
// `cajeta.lang.stream.TakeStream<cajeta.lang.String>::unwrap` was emitted but
// unindexed, which through a generator is an unexplained "Symbols not found".
TEST(SymbolIndexTests, rebuildPicksUpMethodsCodegenCreated) {
    Compiler compiler;
    compileSource(compiler, kSource, "WidgetGrow");

    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));
    const size_t beforeCodegen = index.size();

    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->getLlvmFunctionType();
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->generateCode();

    index.build(hostModuleSet(compiler));

    EXPECT_GE(index.size(), beforeCodegen)
        << "a rebuild dropped entries — it must be additive, since the JIT may "
           "already have resolved through the ones that vanished";
}

// 3.2.3 follow-on (spec 3.5) — the generator cannot re-run build(): it only
// holds the index. refresh() alone must surface methods codegen created, and
// stay a cheap no-op when nothing was instantiated since.
TEST(SymbolIndexTests, refreshAloneSurfacesMethodsCodegenCreated) {
    Compiler compiler;
    compileSource(compiler, kSource, "WidgetRefresh");

    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));
    const size_t beforeCodegen = index.size();

    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->getLlvmFunctionType();
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods())
            method->generateCode();

    index.refresh();
    const size_t afterRefresh = index.size();
    EXPECT_GT(afterRefresh, beforeCodegen)
        << "codegen instantiated nothing new, or refresh() failed to see it — "
           "either way this test lost its subject";

    index.refresh();
    EXPECT_EQ(index.size(), afterRefresh)
        << "a refresh with no new instantiations changed the index";
}
