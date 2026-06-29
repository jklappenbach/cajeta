//
// U7a.1 — ORC mechanism spike for the shared-stdlib-JITDylib model (fork B).
//
// Proves, in isolation from the production JIT path, that one persistent LLJIT
// can host a single shared "stdlib" JITDylib whose definitions resolve from
// multiple independent "user" JITDylibs via addToLinkOrder — and, critically,
// that the stdlib's global constructors run EXACTLY ONCE even though several
// user dylibs link against it and each is initialize()'d separately.
//
// This locks the init/lifecycle model U7b/U7c will build on:
//   - materialize stdlib once into StdlibJD; initialize(StdlibJD) once,
//   - per compile: fresh UserJD, addToLinkOrder(StdlibJD), initialize(UserJD),
//   - lookup + run the user entry; stdlib symbols resolve cross-dylib by name.
//
#include <gtest/gtest.h>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Error.h"

#include <atomic>
#include <memory>

namespace {

using namespace llvm;
using namespace llvm::orc;

// Host counter the stdlib's global ctor bumps. Exposed to JIT'd code as an
// absolute symbol so resolution doesn't depend on the test binary being
// linked with -rdynamic (the production path uses a process-symbol generator
// for libc; here we want a self-contained, robust binding).
std::atomic<int> g_spikeCtorRuns{0};
extern "C" void spike_bump_counter() { g_spikeCtorRuns.fetch_add(1); }

// Build the shared "stdlib" module in its own context:
//   i32  sl_add(i32 a, i32 b) { return a + b; }
//   internal void stdlib_ctor() { spike_bump_counter(); }   (in llvm.global_ctors)
ThreadSafeModule buildStdlibModule() {
    auto ctx = std::make_unique<LLVMContext>();
    auto m = std::make_unique<Module>("spike_stdlib", *ctx);
    IRBuilder<> b(*ctx);
    Type* i32 = Type::getInt32Ty(*ctx);
    Type* voidTy = Type::getVoidTy(*ctx);

    // i32 sl_add(i32, i32)
    auto* addTy = FunctionType::get(i32, {i32, i32}, false);
    auto* add = Function::Create(addTy, Function::ExternalLinkage, "sl_add", *m);
    {
        auto* bb = BasicBlock::Create(*ctx, "entry", add);
        b.SetInsertPoint(bb);
        auto args = add->arg_begin();
        Value* a = &*args++;
        Value* bb2 = &*args;
        b.CreateRet(b.CreateAdd(a, bb2));
    }

    // declare void @spike_bump_counter()
    auto* bumpTy = FunctionType::get(voidTy, {}, false);
    auto* bump = Function::Create(bumpTy, Function::ExternalLinkage,
                                  "spike_bump_counter", *m);

    // internal void stdlib_ctor() { spike_bump_counter(); }
    auto* ctorTy = FunctionType::get(voidTy, {}, false);
    auto* ctor = Function::Create(ctorTy, Function::InternalLinkage,
                                  "stdlib_ctor", *m);
    {
        auto* bb = BasicBlock::Create(*ctx, "entry", ctor);
        b.SetInsertPoint(bb);
        b.CreateCall(bumpTy, bump);
        b.CreateRetVoid();
    }
    appendToGlobalCtors(*m, ctor, 65535);

    return ThreadSafeModule(std::move(m), std::move(ctx));
}

// Build a "user" module in its own context:
//   declare i32 @sl_add(i32, i32)
//   i32 <entry>(i32 x) { return sl_add(x, addend); }
ThreadSafeModule buildUserModule(const char* entryName, int addend) {
    auto ctx = std::make_unique<LLVMContext>();
    auto m = std::make_unique<Module>(entryName, *ctx);
    IRBuilder<> b(*ctx);
    Type* i32 = Type::getInt32Ty(*ctx);

    auto* addTy = FunctionType::get(i32, {i32, i32}, false);
    auto* slAdd = Function::Create(addTy, Function::ExternalLinkage, "sl_add", *m);

    auto* entryTy = FunctionType::get(i32, {i32}, false);
    auto* entry = Function::Create(entryTy, Function::ExternalLinkage, entryName, *m);
    auto* bb = BasicBlock::Create(*ctx, "entry", entry);
    b.SetInsertPoint(bb);
    Value* x = &*entry->arg_begin();
    Value* r = b.CreateCall(addTy, slAdd,
                            {x, ConstantInt::get(i32, addend)});
    b.CreateRet(r);

    return ThreadSafeModule(std::move(m), std::move(ctx));
}

class SharedStdlibDylibSpike : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        InitializeNativeTargetAsmParser();
    }
};

TEST_F(SharedStdlibDylibSpike, sharedStdlibResolvesAcrossUserDylibsCtorRunsOnce) {
    ExitOnError exitOnErr;
    g_spikeCtorRuns.store(0);

    auto jit = exitOnErr(LLJITBuilder().create());
    auto& ES = jit->getExecutionSession();

    // --- shared stdlib dylib, materialized + initialized ONCE ---
    auto& stdlibJD = exitOnErr(jit->createJITDylib("StdlibJD"));
    // Bind the host counter symbol absolutely inside StdlibJD so the ctor
    // resolves it without a process-symbol generator.
    exitOnErr(stdlibJD.define(absoluteSymbols({
        { ES.intern("spike_bump_counter"),
          ExecutorSymbolDef(ExecutorAddr::fromPtr(&spike_bump_counter),
                            JITSymbolFlags::Exported | JITSymbolFlags::Callable) } })));
    exitOnErr(jit->addIRModule(stdlibJD, buildStdlibModule()));
    exitOnErr(jit->initialize(stdlibJD));   // runs stdlib_ctor exactly here
    EXPECT_EQ(g_spikeCtorRuns.load(), 1) << "stdlib ctor must run on StdlibJD init";

    // --- user dylib #1 links against the shared stdlib ---
    auto& u1 = exitOnErr(jit->createJITDylib("U1"));
    u1.addToLinkOrder(stdlibJD);
    exitOnErr(jit->addIRModule(u1, buildUserModule("user_call_1", 100)));
    exitOnErr(jit->initialize(u1));
    auto sym1 = exitOnErr(jit->lookup(u1, "user_call_1"));
    auto fn1 = sym1.toPtr<int(int)>();
    EXPECT_EQ(fn1(5), 105) << "U1 must resolve sl_add from the shared StdlibJD";

    // --- user dylib #2 links against the SAME shared stdlib ---
    auto& u2 = exitOnErr(jit->createJITDylib("U2"));
    u2.addToLinkOrder(stdlibJD);
    exitOnErr(jit->addIRModule(u2, buildUserModule("user_call_2", 200)));
    exitOnErr(jit->initialize(u2));
    auto sym2 = exitOnErr(jit->lookup(u2, "user_call_2"));
    auto fn2 = sym2.toPtr<int(int)>();
    EXPECT_EQ(fn2(5), 205) << "U2 must resolve sl_add from the SAME StdlibJD";

    // The crux: even though two user dylibs link against StdlibJD and each was
    // initialize()'d, the stdlib global ctor ran exactly once (at StdlibJD init).
    EXPECT_EQ(g_spikeCtorRuns.load(), 1)
        << "stdlib ctor must NOT re-run when user dylibs that link it initialize";

    // Same shared definition backs both users (identical sl_add address).
    auto slAddSym = exitOnErr(jit->lookup(stdlibJD, "sl_add"));
    EXPECT_NE(slAddSym.getValue(), 0u);
}

} // namespace
