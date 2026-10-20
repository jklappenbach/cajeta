#include "InlineLineFrames.h"
#include "Optimizer.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
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


        // Longest run merged into one update; longer runs split.
        constexpr size_t kMaxRun = 16;

        bool envOff(const char* name) {
            const char* v = std::getenv(name);
            return v && std::strcmp(v, "0") == 0;
        }

        /** One merged step: a run of enters or leaves, or a single mark. */
        struct Op {
            int kind = 0;   // 0 enter, 1 mark, 2 leave
            std::vector<llvm::CallInst*> calls;
            int from = -1;   // the op whose resulting top still holds here, or -1 to load it
        };

        struct Late {
            const Shapes& s;
            llvm::Value* st = nullptr;
            llvm::Value* topPtr = nullptr;

            llvm::Value* loadTop(llvm::IRBuilder<>& b) {
                llvm::LoadInst* t = b.CreateLoad(b.getInt32Ty(), topPtr, "shadow.top");
                t->setVolatile(true);
                t->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
                return t;
            }

            void storeTop(llvm::IRBuilder<>& b, llvm::Value* v) {
                llvm::StoreInst* st0 = b.CreateStore(v, topPtr);
                st0->setVolatile(true);
                st0->setMetadata(llvm::LLVMContext::MD_tbaa, s.topTag);
            }

            void writeFrame(llvm::IRBuilder<>& b, llvm::Value* idx32, llvm::Value* desc) {
                llvm::Value* idx = b.CreateZExt(idx32, b.getInt64Ty());
                llvm::Value* fr = b.CreateInBoundsGEP(
                    s.stack, st, {b.getInt32(0), b.getInt32(0), idx}, "shadow.frame");
                b.CreateStore(desc, b.CreateStructGEP(s.frame, fr, 0))
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
                b.CreateStore(b.getInt32(0), b.CreateStructGEP(s.frame, fr, 1))
                    ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
            }

            /** Pushes every frame of the run at its first call; returns the new top. */
            llvm::Value* enter(const Op& op, llvm::Value* known) {
                llvm::CallInst* at = op.calls.front();
                llvm::IRBuilder<> b(at);
                llvm::Value* t = known ? known : loadTop(b);
                uint32_t k = (uint32_t) op.calls.size();
                if (k == 1) {
                    emitIf(b.CreateICmpULT(t, b.getInt32(kShadowMax)), at, [&](llvm::IRBuilder<>& tb) {
                        writeFrame(tb, t, op.calls[0]->getArgOperand(0));
                    });
                } else {
                    llvm::Value* fits = b.CreateICmpULE(t, b.getInt32(kShadowMax - k));
                    llvm::Instruction *thenT = nullptr, *elseT = nullptr;
                    llvm::MDNode* likely = llvm::MDBuilder(b.getContext()).createBranchWeights(4096, 1);
                    llvm::SplitBlockAndInsertIfThenElse(fits, at->getIterator(), &thenT, &elseT, likely);
                    llvm::IRBuilder<> tb(thenT);
                    for (uint32_t i = 0; i < k; ++i)
                        writeFrame(tb, tb.CreateAdd(t, tb.getInt32(i)), op.calls[i]->getArgOperand(0));
                    for (uint32_t i = 0; i < k; ++i) {
                        llvm::IRBuilder<> eb(elseT);
                        llvm::Value* ti = eb.CreateAdd(t, eb.getInt32(i));
                        emitIf(eb.CreateICmpULT(ti, eb.getInt32(kShadowMax)), elseT, [&](llvm::IRBuilder<>& fb) {
                            writeFrame(fb, ti, op.calls[i]->getArgOperand(0));
                        });
                    }
                }
                b.SetInsertPoint(at);
                llvm::Value* n = b.CreateAdd(t, b.getInt32(k));
                storeTop(b, n);
                return n;
            }

            /** Pops the run's frames at its first call, never below zero; returns the new top. */
            llvm::Value* leave(const Op& op, llvm::Value* known) {
                llvm::IRBuilder<> b(op.calls.front());
                llvm::Value* t = known ? known : loadTop(b);
                llvm::Value* k = b.getInt32((uint32_t) op.calls.size());
                llvm::Value* floor = b.CreateSelect(b.CreateICmpSGT(t, b.getInt32(0)), b.getInt32(0), t);
                llvm::Value* n = b.CreateSelect(b.CreateICmpSGT(t, k), b.CreateSub(t, k), floor);
                storeTop(b, n);
                return n;
            }

            /** Records the mark's line in the top frame; returns the (unchanged) top. */
            llvm::Value* mark(const Op& op, llvm::Value* known) {
                llvm::CallInst* at = op.calls.front();
                llvm::IRBuilder<> b(at);
                llvm::Value* t = known ? known : loadTop(b);
                llvm::Value* below = b.CreateSub(t, b.getInt32(1));
                llvm::Value* line = at->getArgOperand(0);
                emitIf(b.CreateICmpULT(below, b.getInt32(kShadowMax)), at, [&](llvm::IRBuilder<>& tb) {
                    llvm::Value* idx = tb.CreateZExt(below, tb.getInt64Ty());
                    llvm::Value* fr = tb.CreateInBoundsGEP(
                        s.stack, st, {tb.getInt32(0), tb.getInt32(0), idx}, "shadow.frame");
                    tb.CreateStore(line, tb.CreateStructGEP(s.frame, fr, 1))
                        ->setMetadata(llvm::LLVMContext::MD_tbaa, s.frameTag);
                });
                return t;
            }
        };

        bool breaksRun(const llvm::Instruction& i) {
            auto* cb = llvm::dyn_cast<llvm::CallBase>(&i);
            return cb && !llvm::isa<llvm::IntrinsicInst>(cb);
        }

        /** True for a block that only traps: no call but intrinsics, ending in unreachable. */
        bool trapsOnly(const llvm::BasicBlock* bb) {
            if (!llvm::isa<llvm::UnreachableInst>(bb->getTerminator())) return false;
            for (const llvm::Instruction& i : *bb)
                if (breaksRun(i)) return false;
            return true;
        }

        /** The block a run may continue into from `bb`: its one live successor, reached only from `bb`. */
        llvm::BasicBlock* chainNext(llvm::BasicBlock* bb) {
            auto* br = llvm::dyn_cast<llvm::BranchInst>(bb->getTerminator());
            if (!br) return nullptr;
            llvm::BasicBlock* next = br->getSuccessor(0);
            if (br->isConditional()) {
                llvm::BasicBlock* other = br->getSuccessor(1);
                if (trapsOnly(next)) std::swap(next, other);
                if (!trapsOnly(other) || next == other) return nullptr;
            }
            return next->getSinglePredecessor() == bb ? next : nullptr;
        }

        /** Appends a chain's line calls as runs (same kind, adjacent but for non-call code), noting
         *  in `endOp` which op's top holds at each block's end, for successors with one predecessor. */
        void opsOf(const std::vector<llvm::BasicBlock*>& chain, llvm::Function* enterFn, llvm::Function* markFn,
                   llvm::Function* leaveFn, std::vector<Op>& ops, std::map<const llvm::BasicBlock*, int>& endOp) {
            Op run;
            int cur = -1;
            if (const llvm::BasicBlock* p = chain.front()->getSinglePredecessor()) {
                auto it = endOp.find(p);
                if (it != endOp.end()) cur = it->second;
            }
            std::vector<const llvm::BasicBlock*> waiting;
            auto flush = [&]() {
                if (run.calls.empty()) return;
                run.from = cur;
                ops.push_back(std::move(run));
                run = Op();
                cur = (int) ops.size() - 1;
                for (const llvm::BasicBlock* w : waiting) endOp[w] = cur;
                waiting.clear();
            };
            for (llvm::BasicBlock* bb : chain) {
                for (llvm::Instruction& i : *bb) {
                    auto* c = llvm::dyn_cast<llvm::CallInst>(&i);
                    llvm::Function* callee = c ? c->getCalledFunction() : nullptr;
                    int kind = !callee ? -1
                             : callee == enterFn && c->arg_size() == 1 ? 0
                             : callee == markFn && c->arg_size() == 1 ? 1
                             : callee == leaveFn && c->arg_size() == 0 ? 2 : -1;
                    if (kind >= 0) {
                        bool constDesc = kind != 0 || llvm::isa<llvm::Constant>(c->getArgOperand(0));
                        if (!run.calls.empty() && (run.kind != kind || kind == 1 || !constDesc
                                                   || run.calls.size() >= kMaxRun))
                            flush();
                        run.kind = kind;
                        run.calls.push_back(c);
                        if (kind == 1) flush();
                    } else if (breaksRun(i)) {
                        flush();
                        cur = -1;
                    }
                }
                if (run.calls.empty()) endOp[bb] = cur;
                else waiting.push_back(bb);
            }
            flush();
        }

        llvm::Instruction* firstNonAlloca(llvm::BasicBlock& entry) {
            for (llvm::Instruction& i : entry)
                if (!llvm::isa<llvm::AllocaInst>(i) && !llvm::isa<llvm::PHINode>(i)) return &i;
            return entry.getTerminator();
        }

        bool referencesShadowGlobal(const llvm::Module& m) {
            for (const llvm::GlobalVariable& g : m.globals()) {
                llvm::StringRef n = g.getName();
                if (n.contains("__cajeta_shadow_cur") || n.contains("__cajeta_main_shadow")
                    || n.contains("__cajeta_current_fiber"))
                    return true;
            }
            return false;
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


    bool deferLineFrames(llvm::Module& m) {
        if (envOff("CAJETA_INLINE_LINE_FRAMES") || envOff("CAJETA_LINE_FRAME_COALESCE")) return false;
        llvm::Function* fns[] = {m.getFunction("__cajeta_line_enter"), m.getFunction("__cajeta_line_mark"),
                                 m.getFunction("__cajeta_line_leave")};
        bool any = false;
        for (llvm::Function* f : fns) {
            if (!f) continue;
            if (!f->isDeclaration()) return false;
            any = true;
        }
        if (!any || referencesShadowGlobal(m)) return false;
        for (llvm::Function* f : fns) {
            if (!f) continue;
            f->addFnAttr(llvm::Attribute::NoUnwind);
            f->addFnAttr(llvm::Attribute::WillReturn);
            f->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
        }
        return true;
    }

    void coalesceLineFrames(llvm::Module& m, llvm::TargetMachine* tm) {
        llvm::Function* enterFn = m.getFunction("__cajeta_line_enter");
        llvm::Function* markFn = m.getFunction("__cajeta_line_mark");
        llvm::Function* leaveFn = m.getFunction("__cajeta_line_leave");
        if (!enterFn && !markFn && !leaveFn) return;
        Shapes s = shapesFor(m);
        std::vector<llvm::Function*> merged;
        for (llvm::Function& f : m) {
            if (f.isDeclaration()) continue;
            std::vector<Op> ops;
            std::map<const llvm::BasicBlock*, int> endOp;
            std::vector<llvm::BasicBlock*> order;
            for (llvm::BasicBlock* bb : llvm::ReversePostOrderTraversal<llvm::Function*>(&f)) order.push_back(bb);
            for (llvm::BasicBlock& bb : f) order.push_back(&bb);
            std::set<llvm::BasicBlock*> seen;
            for (llvm::BasicBlock* head : order) {
                if (seen.count(head)) continue;
                std::vector<llvm::BasicBlock*> chain;
                for (llvm::BasicBlock* bb = head; bb && seen.insert(bb).second; bb = chainNext(bb))
                    chain.push_back(bb);
                opsOf(chain, enterFn, markFn, leaveFn, ops, endOp);
            }
            if (ops.empty()) continue;
            bool coalesced = false;
            Late late{s};
            late.st = emitStack(s, firstNonAlloca(f.getEntryBlock()));
            llvm::IRBuilder<> eb(llvm::cast<llvm::Instruction>(late.st)->getParent()->getFirstNonPHI());
            late.topPtr = eb.CreateStructGEP(s.stack, late.st, 1, "shadow.top.ptr");
            std::vector<llvm::Value*> tops(ops.size(), nullptr);
            for (size_t i = 0; i < ops.size(); ++i) {
                const Op& op = ops[i];
                llvm::Value* known = op.from >= 0 ? tops[op.from] : nullptr;
                tops[i] = op.kind == 0 ? late.enter(op, known)
                        : op.kind == 1 ? late.mark(op, known) : late.leave(op, known);
                for (llvm::CallInst* c : op.calls) c->eraseFromParent();
                coalesced |= op.calls.size() > 1;
            }
            if (coalesced) merged.push_back(&f);
        }
        fuseFunctions(merged, tm);
    }

}
