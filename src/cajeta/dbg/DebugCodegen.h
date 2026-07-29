//
// Debugger CP5 codegen helpers. When --debug-info is on, these emit the calls
// that build the per-fiber debug frame chain in the runtime:
//
//   __cajeta_dbg_frame_enter(func)              — at each method prologue
//   __cajeta_dbg_frame_leave()                  — on every return path
//   __cajeta_dbg_local(name, type, slot, facets)— at each named local/param
//
// Each is a no-op unless module->getFlags().debugInfo is set (so the main,
// non-debug build is untouched) and the builder/runtime function resolve.
// Centralized here so the scattered emission sites (Method prologue/epilogue,
// ReturnStatement, LocalVariableDeclaration) stay one-liners — mirrors
// Block.cpp's emitDebugSafepoint.
//
#pragma once

#include <string>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/dbg/MemoryFacets.h"
#include "llvm/IR/Value.h"

namespace cajeta::dbg {

    // Push a debug frame for `func` (the cajeta-mangled enclosing function name).
    // Returns the frame NODE (to be stored in an entry-block slot) or null
    // when frames are off — the paired leave consumes it.
    llvm::Value* emitDbgFrameEnter(cajeta::CajetaModulePtr module,
                                   const std::string& func);

    // Pop the current debug frame. Call beside the scope/drop teardown on every
    // return path.
    // Node-paired (9.1): leave loads the frame node the prolog's enter stored
    // in `nodeSlot` and unlinks exactly that node from its own chain.
    void emitDbgFrameLeave(cajeta::CajetaModulePtr module, llvm::Value* nodeSlot);

    // Register a named local/parameter in the current debug frame. `slot` is the
    // local's alloca (primitives: holds the value; objects: holds the heap ptr).
    // `facets` (CP7-1b) carries the allocation class + ownership role, classified
    // at the call site from the Field/FormalParameter, and is passed through to
    // the runtime as two bytes for the IDE ownership/allocation view.
    // `dropEntry` (CP7-1c) is the owner's drop-chain entry pointer (Field/
    // FormalParameter::getDropEntry()) or null for a non-owner; the runtime
    // reads its `active` flag at a stop to derive the lifetime state.
    void emitDbgLocal(cajeta::CajetaModulePtr module, const std::string& name,
                      const std::string& type, llvm::Value* slot,
                      MemoryFacets facets, llvm::Value* dropEntry);

    // external-debug §3: serialize the compiler's DbgLocTable into `module` as a
    // constant array, plus a global ctor registering it with the runtime, so an
    // external debugger can resolve loc_id <-> (file, line) with no compiler
    // present. Call ONCE, at end of codegen, after every safepoint has claimed
    // its id. No-op unless --debug-info=full or the table is empty.
    void emitDbgLocTable(cajeta::CajetaModulePtr module);

} // namespace cajeta::dbg
