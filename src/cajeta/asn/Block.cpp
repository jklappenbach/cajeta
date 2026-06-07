//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"
#include "cajeta/dbg/DebugLocTable.h"

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
        if (auto method = module->getCurrentMethod()) {
            function = method->getLlvmSymbolName();
        }
        int32_t locId = dbg::globalDbgLocTable().add(
            module->getSourcePath(),
            statement->getSourceLine(),
            statement->getSourceColumn(),
            function);

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
        bool debugInfo = module->getFlags().debugInfo;
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
            // CP2: statement-boundary safepoint before each statement.
            if (debugInfo) emitDebugSafepoint(module, child);
            child->generateCode(module);
        }

        if (m) {
            // GetInsertBlock can be null in degenerate cases; bail out
            // safely. Terminator present means a return/throw already
            // exited this block — no further IR may be emitted at the
            // current insert point.
            llvm::BasicBlock* insertBB = builder ? builder->GetInsertBlock() : nullptr;
            if (insertBB && !insertBB->hasTerminator()) {
                m->emitTopFrameDrops(module);
            }
            m->popDropFrame();
        }
        return nullptr;
    }
}
