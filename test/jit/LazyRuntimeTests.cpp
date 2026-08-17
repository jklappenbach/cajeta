// lazy-codegen Unit 3 (spec 4.4) — lazy emission against the REAL runtime
// surface: heap allocation, drops, vtable globals, stdlib instantiations.
//
// LazyEmitterTests proved delivery on static arithmetic; these fixtures pull in
// everything the whole-module-set passes exist for. A body that heap-allocates
// references its class's vtable global and a drop thunk — neither is a
// CajetaMethod, so neither can cascade through the method index alone. This
// suite is what forces the live-definition fallback to exist.

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/CajetaDefinitionGenerator.h"
#include "cajeta/jit/CajetaLazyEmitter.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"

#include <filesystem>
#include <fstream>
#include <string>

using cajeta::CajetaDefinitionGenerator;
using cajeta::CajetaModulePtr;
using cajeta::CajetaSymbolIndex;
using cajeta::Compiler;
using cajeta::setLazyCodegenEnabled;

namespace {

struct ModeGuard {
    bool saved = cajeta::lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

const char* kSource = R"(package test;

import cajeta.collection.ArrayList;

public class Box {
    int32 v;

    public Box(int32 v) {
        this.v = v;
    }

    public int32 get() {
        return this.v;
    }
}

public class Runner {
    public static int32 useHeap() {
        Box b = heap Box(7);
        return b.get();
    }

    public static int32 useList() {
        ArrayList<int32> xs = heap ArrayList<int32>();
        xs.add(35);
        return xs.get(0);
    }
}
)";

CajetaModulePtr compileSource(Compiler& compiler, const std::string& stem) {
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta-lazyrt-" + stem);
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

cajeta::MethodPtr methodFor(Compiler& compiler, const std::string& cls,
                            const std::string& shortName) {
    for (auto& m : hostModuleSet(compiler))
        for (auto& method : m->getAllMethods()) {
            if (!method) continue;
            const std::string sym = method->getLlvmSymbolName();
            if (sym.find(cls) != std::string::npos
                && sym.find("::" + shortName + "(") != std::string::npos)
                return method;
        }
    return {};
}

std::string symbolFor(Compiler& compiler, const std::string& cls,
                      const std::string& shortName) {
    auto m = methodFor(compiler, cls, shortName);
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
        auto& jd = out.jit->getMainJITDylib();
        auto gen = std::make_unique<CajetaDefinitionGenerator>(
            index,
            [&jit = *out.jit](llvm::orc::ThreadSafeModule tsm,
                              llvm::orc::JITDylib& target) -> llvm::Error {
                return jit.addIRModule(target, std::move(tsm));
            });
        out.generator = gen.get();
        jd.addGenerator(std::move(gen));
        // The native runtime the JIT'd code calls into lives in this test
        // binary; AFTER ours, so a host symbol can never shadow a generatable
        // body (the sl_add/libbsd class).
        jd.addGenerator(llvm::cantFail(
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                out.jit->getDataLayout().getGlobalPrefix())));
        return out;
    }

    // Never cantFail a lookup in a test: the error TEXT is the diagnosis
    // (spec 5.3), and an abort throws it away along with the process.
    int32_t call(const std::string& sym) {
        auto addr = jit->lookup(sym);
        if (!addr) {
            ADD_FAILURE() << "lookup failed for " << sym << ":\n"
                          << llvm::toString(addr.takeError());
            return INT32_MIN;
        }
        return addr->toPtr<int32_t (*)()>()();
    }
};

} // namespace

// 3.1.1 — a body that heap-allocates and drops. The ctor, the vtable global,
// and the drop thunk all arrive through cascades; the drop thunk in particular
// is synthesized DURING generateCode and is not a CajetaMethod, so this fails
// with "Symbols not found: __cajeta_stack_*_drop" until the live-definition
// fallback exists.
TEST(LazyRuntimeTests, heapAllocAndDropRunLazily) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Heap");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = symbolFor(compiler, "test.Runner", "useHeap");
    ASSERT_FALSE(sym.empty());

    auto lazy = LazyJit::create(index);
    EXPECT_EQ(lazy.call(sym), 7);
}

// A stdlib template instantiation's bodies materialize lazily end to end.
TEST(LazyRuntimeTests, stdlibInstantiationRunsLazily) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "List");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string sym = symbolFor(compiler, "test.Runner", "useList");
    ASSERT_FALSE(sym.empty());

    auto lazy = LazyJit::create(index);
    EXPECT_EQ(lazy.call(sym), 35);
}

// 3.1.2 — two entry points sharing one instantiation and one class: the shared
// vtable/thunk/instantiation symbols must resolve to ONE definition, not
// duplicate-define on the second delivery.
TEST(LazyRuntimeTests, sharedInstantiationDoesNotDuplicateDefine) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Shared");
    CajetaSymbolIndex index;
    index.build(hostModuleSet(compiler));

    const std::string heapSym = symbolFor(compiler, "test.Runner", "useHeap");
    const std::string listSym = symbolFor(compiler, "test.Runner", "useList");
    ASSERT_FALSE(heapSym.empty());
    ASSERT_FALSE(listSym.empty());

    auto lazy = LazyJit::create(index);
    EXPECT_EQ(lazy.call(listSym), 35);
    EXPECT_EQ(lazy.call(heapSym), 7);
    // And again — everything now resolves from ORC's own tables.
    EXPECT_EQ(lazy.call(listSym), 35);
}

// 3.1.3 — pin equivalence over the lazy set. The JIT-merge path pins drop
// thunks to weak_odr because llvm::Linker discards unreferenced linkonce_odr
// definitions. Snapshots are DELIVERED to ORC, never Linker-merged, so the
// hazard cannot fire — assert the two properties that make that true:
// a consumer's snapshot carries the thunk only as a declaration (nothing for
// a linker to discard), and a snapshot OF the thunk keeps its definition
// through the whole snapshot tail.
TEST(LazyRuntimeTests, dropThunkSnapshotsCannotHitTheLinkerDiscard) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    Compiler compiler;
    compileSource(compiler, "Pin");
    cajeta::MethodPtr method = methodFor(compiler, "test.Runner", "useHeap");
    ASSERT_TRUE(method);

    auto tsm = cajeta::emitMethodModule(method);
    ASSERT_TRUE(bool(tsm)) << llvm::toString(tsm.takeError());

    std::string thunkName;
    tsm->withModuleDo([&](llvm::Module& m) {
        for (llvm::Function& f : m) {
            if (!f.getName().starts_with("__cajeta")
                || !f.getName().ends_with("_drop"))
                continue;
            EXPECT_TRUE(f.isDeclaration())
                << f.getName().str()
                << " rode along as a definition in a consumer's snapshot";
            if (thunkName.empty()) thunkName = f.getName().str();
        }
    });
    ASSERT_FALSE(thunkName.empty())
        << "useHeap's snapshot references no drop thunk — fixture rotted";

    // The definition itself lives where generateCode synthesized it.
    llvm::GlobalValue* liveThunk = nullptr;
    for (auto& cm : hostModuleSet(compiler)) {
        if (llvm::Module* lm = cm ? cm->getLlvmModule() : nullptr)
            if (llvm::GlobalValue* gv = lm->getNamedValue(thunkName))
                if (!gv->isDeclaration()) { liveThunk = gv; break; }
    }
    ASSERT_NE(liveThunk, nullptr) << "no live definition of " << thunkName;

    auto thunkTsm = cajeta::snapshotLiveDefinition(liveThunk);
    ASSERT_TRUE(bool(thunkTsm)) << llvm::toString(thunkTsm.takeError());
    thunkTsm->withModuleDo([&](llvm::Module& m) {
        llvm::Function* f = m.getFunction(thunkName);
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(f->isDeclaration())
            << "the kept thunk definition did not survive the snapshot tail";
    });
}
