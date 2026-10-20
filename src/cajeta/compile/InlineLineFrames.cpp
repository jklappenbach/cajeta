#include "InlineLineFrames.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace cajeta {

    namespace {

        // Must match CajetaShadowStack / CajetaShadowFrame in the runtime (cajeta_rt_core.c,
        // cajeta_prof_abi.h): frames[CAJETA_SHADOW_MAX] of {desc, line}, then int32 top.
        constexpr uint64_t kShadowMax = 512;

        struct Shapes {
            llvm::StructType* frame = nullptr;
            llvm::StructType* stack = nullptr;
            llvm::GlobalVariable* cur = nullptr;
            llvm::FunctionCallee slow;
            // Siblings of cajeta's field and array-element types (CajetaModule::ensureTbaaNodes),
            // so shadow-stack traffic does not alias program data or the cached pointer.
            llvm::MDNode* curTag = nullptr;
            llvm::MDNode* topTag = nullptr;
            llvm::MDNode* frameTag = nullptr;
        };

        Shapes shapesFor(llvm::Module& m) {
            auto& ctx = m.getContext();
            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
            Shapes s;
            s.frame = llvm::StructType::get(ctx, {ptrTy, i32});
            s.stack = llvm::StructType::get(
                ctx, {llvm::ArrayType::get(s.frame, kShadowMax), i32, i32});
            s.cur = m.getGlobalVariable("__cajeta_shadow_cur", true);
            if (!s.cur) {
                s.cur = new llvm::GlobalVariable(
                    m, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr,
                    "__cajeta_shadow_cur", nullptr,
                    llvm::GlobalValue::GeneralDynamicTLSModel);
            }
            s.slow = m.getOrInsertFunction("__cajeta_shadow_cur_slow", ptrTy);
            // It only fills __cajeta_shadow_cur and is idempotent, so a stale read of the cache
            // just takes the slow path again; claiming no visible writes lets frames share loads.
            if (auto* sf = llvm::dyn_cast<llvm::Function>(s.slow.getCallee())) {
                sf->addFnAttr(llvm::Attribute::NoUnwind);
                sf->addFnAttr(llvm::Attribute::WillReturn);
                sf->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
            }
            llvm::MDBuilder mdb(ctx);
            llvm::MDNode* charTy =
                mdb.createTBAAScalarTypeNode("omnipotent char", mdb.createTBAARoot("Cajeta TBAA"));
            auto tag = [&](const char* name) {
                llvm::MDNode* t = mdb.createTBAAScalarTypeNode(name, charTy);
                return mdb.createTBAAStructTagNode(t, t, 0);
            };
            s.curTag = tag("cajeta shadow pointer");
            s.topTag = tag("cajeta shadow top");
            s.frameTag = tag("cajeta shadow frame");
            return s;
        }

        /** Emits the running context's shadow stack before `at`: the cached pointer, or the slow path. */
        llvm::Value* emitStack(const Shapes& s, llvm::Instruction* at) {
            llvm::IRBuilder<> b(at);
            auto* ptrTy = llvm::PointerType::get(b.getContext(), 0);
            llvm::LoadInst* cached = b.CreateLoad(ptrTy, s.cur, "shadow.cur");
            cached->setMetadata(llvm::LLVMContext::MD_tbaa, s.curTag);
            llvm::Value* isNull = b.CreateICmpEQ(cached, llvm::ConstantPointerNull::get(ptrTy));
            llvm::BasicBlock* head = cached->getParent();
            llvm::MDNode* unlikely = llvm::MDBuilder(b.getContext()).createBranchWeights(1, 4096);
            llvm::Instruction* thenTerm =
                llvm::SplitBlockAndInsertIfThen(isNull, at->getIterator(), false, unlikely);
            llvm::IRBuilder<> tb(thenTerm);
            llvm::Value* slow = tb.CreateCall(s.slow, {}, "shadow.slow");
            b.SetInsertPoint(at);
            llvm::PHINode* phi = b.CreatePHI(ptrTy, 2, "shadow");
            phi->addIncoming(cached, head);
            phi->addIncoming(slow, thenTerm->getParent());
            return phi;
        }

        /** Emits `if (cond) { body }` before `at`, with the body built by `fill`. */
        template <typename Fill>
        void emitIf(llvm::Value* cond, llvm::Instruction* at, Fill fill) {
            llvm::Instruction* term = llvm::SplitBlockAndInsertIfThen(cond, at->getIterator(), false);
            llvm::IRBuilder<> b(term);
            fill(b);
        }

        void expandEnter(const Shapes& s, llvm::CallInst* call) {
            llvm::Value* desc = call->getArgOperand(0);
            llvm::Value* st = emitStack(s, call);
            llvm::IRBuilder<> b(call);
            llvm::Type* i32 = b.getInt32Ty();
            llvm::Value* topPtr = b.CreateStructGEP(s.stack, st, 1, "shadow.top.ptr");
            llvm::LoadInst* top = b.CreateLoad(i32, topPtr, "shadow.top");
            top->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            llvm::Value* inCap = b.CreateICmpULT(top, b.getInt32(kShadowMax));
            emitIf(inCap, call, [&](llvm::IRBuilder<>& tb) {
                llvm::Value* idx = tb.CreateZExt(top, tb.getInt64Ty());
                llvm::Value* fr = tb.CreateInBoundsGEP(
                    s.stack, st, {tb.getInt32(0), tb.getInt32(0), idx}, "shadow.frame");
                tb.CreateStore(desc, tb.CreateStructGEP(s.frame, fr, 0))
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
                tb.CreateStore(tb.getInt32(0), tb.CreateStructGEP(s.frame, fr, 1))
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
            });
            b.SetInsertPoint(call);
            b.CreateStore(b.CreateAdd(top, b.getInt32(1)), topPtr)
                ->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            call->eraseFromParent();
        }

        void expandMark(const Shapes& s, llvm::CallInst* call) {
            llvm::Value* line = call->getArgOperand(0);
            llvm::Value* st = emitStack(s, call);
            llvm::IRBuilder<> b(call);
            llvm::Type* i32 = b.getInt32Ty();
            llvm::LoadInst* top = b.CreateLoad(i32, b.CreateStructGEP(s.stack, st, 1), "shadow.top");
            top->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            llvm::Value* below = b.CreateSub(top, b.getInt32(1));
            llvm::Value* inCap = b.CreateICmpULT(below, b.getInt32(kShadowMax));
            emitIf(inCap, call, [&](llvm::IRBuilder<>& tb) {
                llvm::Value* idx = tb.CreateZExt(below, tb.getInt64Ty());
                llvm::Value* fr = tb.CreateInBoundsGEP(
                    s.stack, st, {tb.getInt32(0), tb.getInt32(0), idx}, "shadow.frame");
                tb.CreateStore(line, tb.CreateStructGEP(s.frame, fr, 1))
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
            });
            call->eraseFromParent();
        }

        void expandLeave(const Shapes& s, llvm::CallInst* call) {
            llvm::Value* st = emitStack(s, call);
            llvm::IRBuilder<> b(call);
            llvm::Type* i32 = b.getInt32Ty();
            llvm::Value* topPtr = b.CreateStructGEP(s.stack, st, 1, "shadow.top.ptr");
            llvm::LoadInst* top = b.CreateLoad(i32, topPtr, "shadow.top");
            top->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            llvm::Value* positive = b.CreateICmpSGT(top, b.getInt32(0));
            emitIf(positive, call, [&](llvm::IRBuilder<>& tb) {
                tb.CreateStore(tb.CreateSub(top, tb.getInt32(1)), topPtr)
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            });
            call->eraseFromParent();
        }

    } // namespace

    void inlineLineFrames(llvm::Module& m) {
        const char* env = std::getenv("CAJETA_INLINE_LINE_FRAMES");
        if (env && std::strcmp(env, "0") == 0) return;
        llvm::Function* enter = m.getFunction("__cajeta_line_enter");
        llvm::Function* mark = m.getFunction("__cajeta_line_mark");
        llvm::Function* leave = m.getFunction("__cajeta_line_leave");
        if (!enter && !mark && !leave) return;
        std::vector<std::pair<llvm::CallInst*, int>> calls;
        for (llvm::Function& f : m) {
            if (f.isDeclaration()) continue;
            // The runtime's own definitions stay calls; only their callers are expanded.
            if (&f == enter || &f == mark || &f == leave) continue;
            for (llvm::BasicBlock& bb : f)
                for (llvm::Instruction& i : bb)
                    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&i)) {
                        llvm::Function* callee = c->getCalledFunction();
                        if (!callee) continue;
                        if (callee == enter && c->arg_size() == 1) calls.push_back({c, 0});
                        else if (callee == mark && c->arg_size() == 1) calls.push_back({c, 1});
                        else if (callee == leave && c->arg_size() == 0) calls.push_back({c, 2});
                    }
        }
        if (calls.empty()) return;
        Shapes s = shapesFor(m);
        for (auto& [c, kind] : calls) {
            if (kind == 0) expandEnter(s, c);
            else if (kind == 1) expandMark(s, c);
            else expandLeave(s, c);
        }
    }

}
