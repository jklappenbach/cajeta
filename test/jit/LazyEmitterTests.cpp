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

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/TargetSelect.h"

#include <filesystem>
#include <fstream>
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

// The fixture method's symbol, from the index — never hand-spelled.
std::string symbolFor(Compiler& compiler, const std::string& shortName) {
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods()) {
            if (!method) continue;
            const std::string sym = method->getLlvmSymbolName();
            if (sym.find("test.Calc") != std::string::npos
                && sym.find("::" + shortName + "(") != std::string::npos)
                return sym;
        }
    return {};
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
            [&jit = *out.jit](const MethodPtr& method,
                              llvm::orc::JITDylib& jd) -> llvm::Error {
                auto tsm = cajeta::emitMethodModule(method);
                if (!tsm) return tsm.takeError();
                return jit.addIRModule(jd, std::move(*tsm));
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
