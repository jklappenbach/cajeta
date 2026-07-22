//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "LocalVariableDeclaration.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"
#include "../type/Scope.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/LineInfoCodegen.h"

namespace cajeta {

    // Debugger CP2: when --debug-info is on, emit a call to
    // __cajeta_dbg_safepoint(loc_id) before a statement so the in-process
    // debugger can poll for breakpoints at each statement boundary. loc_id
    // indexes the global DbgLocTable, which maps it back to {file,line,col,fn}.
    // No-op if the runtime helper can't be resolved or the insert block is
    // already terminated.
    static void emitDebugSafepoint(CajetaModulePtr module,
                                   const AbstractSyntaxNodePtr& statement) {
        llvm::IRBuilder<>* builder = module->getBuilder();
        if (!builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb || bb->hasTerminator()) return;
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_dbg_safepoint");
        if (!fn) return;

        std::string function;
        std::string file;
        int lineDelta = 0;   // 9.2: snippet -> file line for instantiations
        if (auto method = module->getCurrentMethod()) {
            lineDelta = method->getDbgLineDelta();
            // Class-template instantiations carry the delta on the class.
            if (lineDelta == 0)
                if (auto owner = method->getParent())
                    lineDelta = owner->getDbgLineDelta();
            function = method->getLlvmSymbolName();
            // The declaring class's file, remapped — same source as #FrameDesc
            // (external-debug §6). getSourcePath() was the raw ABSOLUTE path,
            // which is not reproducible across build roots (§3.1.3) and is
            // EMPTY for every stdlib statement (one module, no source path).
            if (auto parent = method->getParent()) {
                file = parent->getDeclaringFile();
            }
        }
        if (file.empty()) file = module->remappedSourcePath();
        // Ranged modules (JIT path) claim ids from their own range so an
        // edit elsewhere never shifts this module's baked constants; the
        // dense allocator remains for AOT/lint (and range overflow).
        const int dbgLine = statement->getSourceLine() + lineDelta;
        int32_t locId = module->takeDbgLocId();
        if (locId >= 0) {
            dbg::globalDbgLocTable().setAt(
                locId, dbg::DbgLoc{file, dbgLine,
                                   statement->getSourceColumn(), function});
        } else {
            locId = dbg::globalDbgLocTable().add(
                file,
                dbgLine,
                statement->getSourceColumn(),
                function);
        }

        llvm::Value* arg = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(*module->getLlvmContext()),
            static_cast<uint64_t>(locId));
        builder->CreateCall(fn, {arg});
    }

    llvm::Value* Block::generateCode(CajetaModulePtr module) {
        // Block-scoped drops: each `{ ... }` is its own drop frame. Locals
        // declared inside register into this frame; at the closing `}` the
        // frame's entries fire (LIFO) before the frame is popped. If the
        // block ran to a terminator (return/throw) mid-way through, the
        // terminating statement already fired ALL frames' drops on its
        // way out — we observe the terminator here and skip the fire,
        // just dropping the frame off the stack.
        auto m = module->getCurrentMethod();
        if (m) m->pushDropFrame();

        auto* builder = module->getBuilder();

        // Frame-arena (frame-arena-plan U2): bracket this block's body with an
        // arena mark/reset so non-escaping owned concat / primitive-array locals
        // declared inside are bump-allocated and reclaimed in O(1) at the closing
        // `}` (per-iteration for a loop body). The reset fires on the same
        // normal-exit edge as the drop chain (early return/throw already unwind via
        // emitOwnerDrops; an un-reset frame is reclaimed by an enclosing scope's
        // reset — never a UAF since arena objects don't escape).
        //
        // Gate on whether THIS block directly declares an arena-eligible local, not
        // merely on m->usesArena() (any arena local anywhere in the method). Arena
        // allocations only ever happen at a declaration's initializer (see
        // Method::arenaWalk), and a declaration is always a direct child of its
        // enclosing block — so this scan is exact: a per-iteration arena local lives
        // in the loop-body block and still gets its per-iteration reset, while a hot
        // inner loop that allocates nothing pays zero. The old usesArena() gate
        // wrapped EVERY block (incl. allocation-free inner loops) in a mark/reset
        // CALL pair, a ~2.3x regression on integer-array code (e.g. fannkuch's
        // perm/perm1/count swap loops).
        bool blockHasArenaAlloc = false;
        if (m && m->usesArena()) {
            for (auto& child : children) {
                auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(child);
                if (!lvd) continue;
                for (auto& d : lvd->getVariableDeclarators()) {
                    if (d && m->isArenaEligibleLocal(d->getIdentifier())) {
                        blockHasArenaAlloc = true;
                        break;
                    }
                }
                if (blockHasArenaAlloc) break;
            }
        }
        llvm::Value* arenaMark = nullptr;
        if (blockHasArenaAlloc && builder && builder->GetInsertBlock()
                && !builder->GetInsertBlock()->hasTerminator()) {
            if (llvm::Function* markFn = module->getRuntimeFunction("__cajeta_arena_mark")) {
                arenaMark = builder->CreateCall(markFn, {}, "arena.mark");
            }
        }
        bool debugInfo = module->getFlags().debugInfo;
        bool lineInfo = module->getFlags().lineInfo;
        // title-tracking §3.1.5 — checkpoint the move log: a block whose
        // codegen ends in a return/throw never reaches the join, so the
        // moves it introduced retract (`try { put(#key); return r; } ...
        // put(#key)` — dynamically exclusive paths). A fallthrough block
        // keeps them: moved on any joining path = moved after the join.
        // break/continue emit plain branches and deliberately do NOT
        // retract (their moves can reach post-loop code).
        auto linScope = module->getScopeStack().peek();
        size_t moveMark = linScope ? linScope->moveLogSize() : 0;
        for (auto child: children) {
            // Stop emitting once the current BB has a terminator —
            // anything after a return / throw / break / continue is
            // dead code, and emitting into a terminated BB lands the
            // instructions AFTER the terminator (invalid IR; LLVM
            // verify rejects with "Terminator found in the middle of
            // a basic block"). Hit when a test harness appends a
            // fallback `return 0;` after a body that already returns,
            // or when user code intentionally writes `return X;
            // unused();` for documentation.
            llvm::BasicBlock* insertBB = builder
                ? builder->GetInsertBlock() : nullptr;
            if (insertBB && insertBB->hasTerminator()) break;
            // U3: mark the current shadow frame's line at each statement
            // boundary. BEFORE the safepoint, not after: a debugger stopped AT a
            // safepoint reads the shadow stack to render the frame, and if the
            // mark had not run yet the frame would still carry the PREVIOUS
            // statement's line — `cjbreak F.cajeta:14` would stop and `cjstack`
            // would report :13 (external-debug §5.1).
            if (lineInfo) dbg::emitLineMark(module, child->getSourceLine());
            // CP2: statement-boundary safepoint before each statement.
            if (debugInfo) emitDebugSafepoint(module, child);
            child->generateCode(module);
        }

        if (linScope) {
            llvm::BasicBlock* bb = builder ? builder->GetInsertBlock() : nullptr;
            llvm::Instruction* term = (bb && bb->hasTerminator())
                ? bb->getTerminator() : nullptr;
            bool exited = term && (llvm::isa<llvm::ReturnInst>(term)
                                   || llvm::isa<llvm::UnreachableInst>(term));
            // ThrowStatement ends its BB with `unreachable` then parks the
            // insert point in a fresh predecessor-less `after_throw` BB (so
            // trailing dead code still has a home). An empty, unreached,
            // non-entry insert BB at block end is that same "this path never
            // joins" signal.
            if (!exited && bb && term == nullptr && bb->empty()
                    && bb->hasNPredecessors(0)
                    && bb != &bb->getParent()->getEntryBlock()) {
                exited = true;
            }
            if (exited) {
                linScope->retractMovesSince(moveMark);
            }
        }

        if (m) {
            // GetInsertBlock can be null in degenerate cases; bail out
            // safely. Terminator present means a return/throw already
            // exited this block — no further IR may be emitted at the
            // current insert point.
            llvm::BasicBlock* insertBB = builder ? builder->GetInsertBlock() : nullptr;
            if (insertBB && !insertBB->hasTerminator()) {
                m->emitTopFrameDrops(module);
                // Reclaim this block's arena objects (after drops, before frame pop).
                if (arenaMark) {
                    if (llvm::Function* resetFn =
                            module->getRuntimeFunction("__cajeta_arena_reset")) {
                        builder->CreateCall(resetFn, {arenaMark});
                    }
                }
            }
            m->popDropFrame();
        }
        return nullptr;
    }
}
