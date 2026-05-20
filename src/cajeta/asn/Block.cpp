//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
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
            if (insertBB && insertBB->getTerminator()) break;
            child->generateCode(module);
        }

        if (m) {
            // GetInsertBlock can be null in degenerate cases; bail out
            // safely. Terminator present means a return/throw already
            // exited this block — no further IR may be emitted at the
            // current insert point.
            llvm::BasicBlock* insertBB = builder ? builder->GetInsertBlock() : nullptr;
            if (insertBB && !insertBB->getTerminator()) {
                m->emitTopFrameDrops(module);
            }
            m->popDropFrame();
        }
        return nullptr;
    }
}
