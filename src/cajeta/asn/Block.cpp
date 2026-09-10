//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "LocalVariableDeclaration.h"
#include "Statement.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "../type/Scope.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/LineInfoCodegen.h"

namespace cajeta {

    // Emit a call to __cajeta_dbg_safepoint(loc_id) ahead of `statement` so the
    // in-process debugger can poll for breakpoints at statement boundaries; loc_id
    // indexes the DbgLocTable. No-op without the helper, or after a terminator.
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
        if (auto method = module->getCurrentMethod()) {
            function = method->getLlvmSymbolName();
            // The declaring class's file, remapped — the raw source path is
            // absolute (not reproducible) and empty for every stdlib statement.
            if (auto parent = method->getParent()) {
                file = parent->getDeclaringFile();
            }
        }
        if (file.empty()) file = module->remappedSourcePath();
        // A ranged module claims ids from its own range, so an edit elsewhere never
        // shifts this module's baked constants. fileLineFor clamps a snippet->file
        // correction that overshoots into a negative, un-matchable line.
        int dbgLine = dbg::fileLineFor(module, statement->getSourceLine());
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

    // Emit every child statement in order, bracketed by this block's drop frame,
    // its arena mark/reset, and the name bindings it shadows — each of which is
    // torn down at the closing `}` unless a return/throw already left the block.
    llvm::Value* Block::generateCode(CajetaModulePtr module) {
        // Each `{ ... }` is its own drop frame: locals declared inside register
        // into it and fire LIFO at the closing `}`. A terminator mid-block means a
        // return/throw already fired every frame, so here we only pop.
        auto m = module->getCurrentMethod();
        if (m) m->pushDropFrame();

        // True only for the ENTRY's root block: a nested block's declarations are
        // ordinary locals even when they shadow a session-binding name.
        struct TopLevelGuard {
            CajetaModulePtr mod;
            bool saved;
            TopLevelGuard(CajetaModulePtr mod)
                : mod(mod),
                  saved(mod->setScriptEntryTopLevel(
                      mod->consumeScriptRootBlockPending())) {}
            ~TopLevelGuard() { mod->setScriptEntryTopLevel(saved); }
        } topLevelGuard(module);

        auto* builder = module->getBuilder();

        // Bracket the body with an arena mark/reset so non-escaping owned locals
        // are bump-reclaimed in O(1) at the closing `}`. Gated on a DIRECT
        // arena-eligible declaration: gating on m->usesArena() cost ~2.3x.
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
        // EITHER flag: `safepoints` exists so statement boundaries can be had
        // without debugInfo's registry retention, but debug info without them is
        // useless — deriving it HERE is what keeps the two from desyncing.
        bool safepoints = module->getFlags().safepoints
                       || module->getFlags().debugInfo;
        bool lineInfo = module->getFlags().lineInfo;
        // Checkpoint the move log: a block ending in return/throw never reaches the
        // join, so its moves retract, while a fallthrough block keeps them.
        // break/continue emit plain branches and deliberately do NOT retract.
        auto linScope = module->getScopeStack().peek();
        size_t moveMark = linScope ? linScope->moveLogSize() : 0;
        // Block-scoped NAMES: there is one Scope per method, so a local declared
        // here would rebind the name for the rest of the method. Snapshot and
        // restore ONLY shadowing names — analyses past the body look fresh ones up.
        vector<pair<string, FieldPtr>> shadowed;
        if (linScope) {
            for (auto& child : children) {
                auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(child);
                if (!lvd) continue;
                for (auto& d : lvd->getVariableDeclarators()) {
                    if (!d) continue;
                    const string& name = d->getIdentifier();
                    if (FieldPtr prior = linScope->localBinding(name)) {
                        shadowed.emplace_back(name, prior);
                    }
                }
            }
        }
        // Mark the cell's trailing expression, the candidate for `Out[N]`. Done at
        // the AST because the synthesizer splices token text before a type exists.
        // The candidate is the statement before a SYNTHESIZED trailing `return 0;`.
        const AbstractSyntaxNode* resultCandidate = nullptr;
        if (module->isScriptUnit() && module->hasScriptSyntheticTail()
                && m && m->getName() == scriptEntryName()
                && m->getBlock().get() == this && children.size() >= 2) {
            auto& last = children[children.size() - 2];
            if (dynamic_pointer_cast<ExpressionStatement>(last)) {
                resultCandidate = last.get();
            }
        }

        for (auto child: children) {
            // Stop once the BB has a terminator: anything after a return/throw is
            // dead code, and emitting into a terminated BB would land instructions
            // after the terminator, which the verifier rejects.
            llvm::BasicBlock* insertBB = builder
                ? builder->GetInsertBlock() : nullptr;
            if (insertBB && insertBB->hasTerminator()) break;
            // Mark the shadow frame's line BEFORE the safepoint: a debugger stopped
            // at one reads the shadow stack, and a later mark would render the
            // PREVIOUS statement's line. A script unit marks in HOST coordinates.
            int markLine = child->getSourceLine();
            if (module->isScriptUnit()) {
                markLine = module->mapScriptLine(markLine);
                module->setScriptCurrentHostLine(markLine);
            } else {
                // A generic's body is re-parsed from a snippet, so this token's
                // line is a SNIPPET line; a script unit is never an instantiation.
                markLine = dbg::fileLineFor(module, markLine);
            }
            if (lineInfo) dbg::emitLineMark(module, markLine);
            if (safepoints) emitDebugSafepoint(module, child);
            if (resultCandidate && child.get() == resultCandidate) {
                module->setScriptResultPending(true);
            }
            child->generateCode(module);
        }

        if (linScope) {
            llvm::BasicBlock* bb = builder ? builder->GetInsertBlock() : nullptr;
            llvm::Instruction* term = (bb && bb->hasTerminator())
                ? bb->getTerminator() : nullptr;
            bool exited = term && (llvm::isa<llvm::ReturnInst>(term)
                                   || llvm::isa<llvm::UnreachableInst>(term));
            // ThrowStatement ends its BB with `unreachable` and parks the insert
            // point in a fresh empty BB — the same "this path never joins" signal.
            if (!exited && bb && term == nullptr && bb->empty()
                    && bb->hasNPredecessors(0)
                    && bb != &bb->getParent()->getEntryBlock()) {
                exited = true;
            }
            if (exited) {
                // Inside the script entry these marks feed the SESSION write-back:
                // a `return` terminates the unit rather than joining it, so
                // retracting would erase facts a later unit's compile must see.
                bool scriptEntry = false;
                if (module->isScriptUnit()) {
                    auto cm = module->getCurrentMethod();
                    scriptEntry = cm && cm->getName() == scriptEntryName();
                }
                if (!scriptEntry) {
                    linScope->retractMovesSince(moveMark);
                }
            }
        }

        if (m) {
            // A terminator means a return/throw already left; emit nothing more.
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
        // Undo this block's bindings last, once the drop/arena teardown has
        // finished consulting the scope; reversed, so a repeated name unwinds fully.
        if (linScope) {
            for (auto it = shadowed.rbegin(); it != shadowed.rend(); ++it) {
                linScope->restoreBinding(it->first, it->second);
            }
        }
        return nullptr;
    }
}
